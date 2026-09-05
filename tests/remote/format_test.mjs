// The page's port of Parameter::get_display against Surge's own text.
//
// patch_params.json (written by the plugin on the device, or captured from it)
// carries, for every float parameter that the port formats, five reference
// strings Surge produced at 0, 25, 50, 75 and 100 % ("chk"), and for every
// integer parameter its option texts. This test formats the same points with
// the port and reports every disagreement.
//
//   node tests/remote/format_test.mjs [path/to/patch_params.json]
import fs from "node:fs";
import vm from "node:vm";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const assets = path.join(here, "../../src/remote/assets");
const file = process.argv[2] || path.join(here, "fixtures/patch_params.json");
if (!fs.existsSync(file)) { console.log("no patch_params.json at " + file + " (capture one from a running module first)"); process.exit(0); }

const ctx = {}; ctx.window = ctx; ctx.globalThis = ctx; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(assets, "surge-meta.js"), "utf8"), ctx);
vm.runInContext(fs.readFileSync(path.join(assets, "surge-format.js"), "utf8"), ctx);
const FMT = ctx.SurgeFormat, META = ctx.SURGE_META;
const J = JSON.parse(fs.readFileSync(file, "utf8"));

let checked = 0, bad = 0;
const byType = {};
for (const p of J.params) {
    const ctn = META.ctrltypes[p.ct] || String(p.ct);
    if (p.vt === 2 && p.chk) {
        p.chk.forEach((want, i) => {
            checked++;
            const got = FMT.display(p, i / 4, ctn);
            if (norm(got) !== norm(want)) { bad++; byType[ctn] = (byType[ctn] || 0) + 1; if (bad <= 40) console.log(`${p.k.padEnd(28)} ${ctn.padEnd(34)} @${i * 25}%  port="${got}"  surge="${want}"`); }
        });
    }
}
function norm(s) { return String(s).replace(/\s+/g, " ").trim().replace(/-0\.00/g, "0.00"); }
console.log(`${checked} reference points, ${bad} disagreements`);
if (bad) console.log("by control type:", byType);
process.exit(bad ? 1 : 0);
