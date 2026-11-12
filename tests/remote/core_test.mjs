// The page's reading of the plugin's state and files (src/remote/assets/core.js),
// against a stand-in bridge: a SPARSE state (only what differs from the preset,
// absent = the preset's `pv`) and files under remote/<rid>/.
//
//   node tests/remote/core_test.mjs
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const src = fs.readFileSync(path.join(ROOT, "src/remote/assets/core.js"), "utf8");

let fails = 0;
const check = (name, ok, detail = "") => { console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail ? " -- " + detail : ""}`); if (!ok) fails++; };

function boot(files) {
    const fetched = [];
    let listener = null;
    const timers = [];
    const window = {
        schwungRemote: {
            component: "synth",
            onParamChange: (f) => { listener = f; },
            setParam: () => {},
        },
    };
    const ctx = {
        window, console, Date, Object, Math, JSON, parseFloat, parseInt, isNaN, String, Array,
        URLSearchParams, location: { search: "" },
        setTimeout: (f, ms) => { timers.push({ f, ms }); return timers.length; },
        clearTimeout: () => {},
        fetch: (url) => {
            fetched.push(url);
            const m = /remote\/(?:(\d+)\/)?([a-z_]+)\.json/.exec(url);
            const body = m && files[(m[1] ?? "") + "/" + m[2]];
            return Promise.resolve({ ok: !!body, json: () => Promise.resolve(body) });
        },
    };
    ctx.window.window = ctx.window;
    vm.createContext(ctx);
    vm.runInContext(src.replace("})(window);", "})(window);"), ctx);
    const C = ctx.window.SurgeCore;
    C.start();
    return { C, push: (p) => listener(p), fetched, timers, flush: () => new Promise((r) => setImmediate(r)) };
}

const pp = {
    rev: 3, preset: 5, preset_name: "Quiz", category: "Leads",
    macros: [{ key: "macro1", label: "M1", pv: 0.25 }],
    params: [
        { k: "filter1_cutoff", vt: 2, min: -60, max: 70, pv: 0.6 },
        { k: "env1_attack", vt: 2, min: -8, max: 5, pv: 0.1 },
        { k: "b_osc1_pitch", vt: 2, min: -7, max: 7, pv: 0.5 },
    ],
    routings: [],
};

{
    const t = boot({ "0/patch_params": pp, "0/presets_index": { categories: [] }, "0/waves": { scenes: [] }, "0/filters": { scenes: [] } });
    // what the manager sends first: the preset browser's fields, no rid
    t.push({ "synth:preset": "5", "synth:preset_name": "Quiz" });
    await t.flush();
    check("no file is read before the state names its directory", t.fetched.length === 0, t.fetched.join(" "));

    // the whole state: two edits over the preset
    t.push({ "synth:preset": "5", "synth:preset_name": "Quiz", "synth:sparse": "1", "synth:rid": "0",
             "synth:filter1_cutoff": "0.9", "synth:macro1": "0.75" });
    await t.flush(); await t.flush();
    check("files come from remote/<rid>/", t.fetched.length === 4 && t.fetched.every((u) => u.includes("/remote/0/")), t.fetched.map((u) => u.replace(/\?.*/, "")).join(" "));
    check("an edited value comes from the state", t.C.val("filter1_cutoff") === 0.9, String(t.C.val("filter1_cutoff")));
    check("an absent value is the preset's", t.C.val("env1_attack") === 0.1 && t.C.val("b_osc1_pitch") === 0.5, `${t.C.val("env1_attack")} ${t.C.val("b_osc1_pitch")}`);
    check("an edited macro comes from the state", t.C.val("macro1") === 0.75);

    // a single-key update (a knob turned on the device): nothing else moves
    t.push({ "synth:env1_attack": "0.4" });
    await t.flush();
    check("a single-key update leaves the rest", t.C.val("env1_attack") === 0.4 && t.C.val("filter1_cutoff") === 0.9);

    // the next whole state no longer carries filter1_cutoff (turned back to the
    // preset's value) and still carries env1_attack
    t.push({ "synth:preset": "5", "synth:preset_name": "Quiz", "synth:sparse": "1", "synth:rid": "0", "synth:env1_attack": "0.4" });
    await t.flush();
    check("a key a whole state drops goes back to the preset's", t.C.val("filter1_cutoff") === 0.6 && t.C.val("macro1") === 0.25,
          `${t.C.val("filter1_cutoff")} ${t.C.val("macro1")}`);
    check("a key a whole state keeps stays", t.C.val("env1_attack") === 0.4);

    // our own write still landing is not overwritten by a state taken before it
    t.C.S.pending["b_osc1_pitch"] = Date.now();
    t.C.S.vals["b_osc1_pitch"] = 0.8;
    t.push({ "synth:sparse": "1", "synth:rid": "0" });
    await t.flush();
    check("a write still landing is left alone", t.C.val("b_osc1_pitch") === 0.8);

    // the slot was reloaded: a new instance, a new directory
    const before = t.fetched.length;
    t.push({ "synth:sparse": "1", "synth:rid": "1" });
    await t.flush();
    check("a new rid re-reads every file from its directory", t.fetched.length === before + 4 && t.fetched.slice(before).every((u) => u.includes("/remote/1/")));
}

{
    // an older plugin: no rid, a dense state, files in remote/ itself
    const t = boot({ "/patch_params": { ...pp, params: pp.params.map(({ pv, ...p }) => p), macros: [{ key: "macro1", label: "M1" }] } });
    t.push({ "synth:filter1_cutoff": "0.3", "synth:env1_attack": "0.2", "synth:b_osc1_pitch": "0.5" });
    await t.flush();
    check("an older plugin's files are not read before the fallback", t.fetched.length === 0);
    t.timers.find((x) => x.ms === 1500).f();
    await t.flush(); await t.flush();
    check("an older plugin's files come from remote/", t.fetched.length === 4 && t.fetched.every((u) => /\/remote\/[a-z_]+\.json/.test(u)));
    check("a dense state's values, read before the metadata, are numbers", t.C.val("filter1_cutoff") === 0.3 && t.C.val("env1_attack") === 0.2,
          `${typeof t.C.val("filter1_cutoff")} ${t.C.val("filter1_cutoff")}`);
}

console.log(fails ? `\n${fails} failed` : "\nall passed");
process.exit(fails ? 1 : 0);
