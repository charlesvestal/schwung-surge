/* Surge XT Remote UI -- core: the bridge, the state, the files.
 *
 * What the page knows and where it comes from:
 *
 *   values      Schwung Manager pushes the module's `state`, flattened, as
 *               `synth:<key>` = normalised 0..1 text, plus preset/preset_name/
 *               octave/bpm/mpe fields and the six mod slots. The state holds
 *               only the parameters and macros that DIFFER from the loaded
 *               preset (`sparse` = 1; the host keeps 16 KB of it): a key a whole
 *               push does not carry is at the preset's value, which
 *               patch_params.json gives as `pv`. A push carrying `sparse` is the
 *               whole state; a single-key update is not.
 *   files       under remote/<rid>/, one directory per plugin instance, named
 *               by the state's `rid` (two Surge slots must not read each
 *               other's files). A plugin that sends no rid wrote remote/.
 *   metadata    patch_params.json: every parameter's key, names, group,
 *               control type, range, default, preset value (`pv`) and Surge's
 *               displayInfo, the macro labels, wavetable names and the patch's
 *               modulation routings. Rebuilt after a preset load or a type
 *               change; carries a rev and the preset name.
 *   presets     presets_index.json, in display order by category.
 *   pictures    waves.json (rendered oscillator cycles) and filters.json
 *               (measured filter responses), rebuilt after a relevant edit.
 *
 * Writes go through schwungRemote.setParam with the value in the form the
 * plugin's set_param takes: Surge's natural units for floats and plain ints,
 * the option TEXT for enumerated parameters, 0..1 for macros.
 */
(function (root) {
    "use strict";
    var remote = root.schwungRemote;
    var comp = (remote && remote.component) || "synth";
    var PFX = comp + ":";
    var query = new URLSearchParams(location.search);
    var MODULE_ID = query.get("module") || "surge";
    var ASSETS = "/api/remote-ui/module-assets/" + encodeURIComponent(MODULE_ID) + "/remote/";
    var rid = null, filesStarted = false;   // this instance's directory under remote/
    var META = root.SURGE_META || {};
    var FMT = root.SurgeFormat;

    var S = {
        vals: {},            // key -> normalised number (params, macros) or string (module fields)
        params: null,        // patch_params.json
        full: null,          // the latest whole (sparse) state push: key -> raw text
        byKey: {},           // key -> param entry
        presets: null,       // presets_index.json
        waves: null, filters: null,
        connected: false,
        pending: {},         // key -> ts of an unconfirmed local write
        rev: { params: 0, waves: 0, filters: 0 },
        preset: null, presetName: "", category: "",
        listeners: [],       // fn(event, detail)
    };

    function emit(ev, detail) { S.listeners.forEach(function (f) { try { f(ev, detail); } catch (e) { console.error(e); } }); }
    function on(fn) { S.listeners.push(fn); }

    function ctName(p) { return (META.ctrltypes && META.ctrltypes[p.ct]) || ""; }

    /* ---- values ------------------------------------------------------------ */
    function val(key) { var v = S.vals[key]; return v === undefined ? null : v; }
    function natural(key) { var p = S.byKey[key], v = val(key); return p && v !== null ? FMT.natural(p, v) : null; }
    function text(key) { var p = S.byKey[key], v = val(key); return p && v !== null ? FMT.display(p, v, ctName(p)) : (v === null ? "--" : String(v)); }

    /* ---- outbound ----------------------------------------------------------- */
    var throttles = {};
    function rawSet(key, value) {
        if (!remote) return;
        var now = Date.now(), t = throttles[key];
        if (t && now - t.last < 33) {
            t.value = value;
            if (!t.timer) t.timer = setTimeout(function () { t.timer = null; t.last = Date.now(); remote.setParam(PFX + key, t.value); }, 33 - (now - t.last));
            return;
        }
        throttles[key] = { last: now, timer: null, value: value };
        remote.setParam(PFX + key, value);
    }

    /* Set a Surge parameter from a normalised value: local first, then the
     * device, in the units set_param expects. */
    function setNorm(key, n) {
        var p = S.byKey[key];
        if (!p) return;
        n = n < 0 ? 0 : n > 1 ? 1 : n;
        if (p.vt !== 2) {
            /* integers snap; enumerated ones travel as their option text */
            var i = FMT.natural(p, n); n = FMT.normalised(p, i);
            S.vals[key] = n; S.pending[key] = Date.now();
            if (p.o && p.o.length && !isFilterSubtype(p)) rawSet(key, p.o[i - p.min]);
            else rawSet(key, String(i));
        } else {
            S.vals[key] = n; S.pending[key] = Date.now();
            rawSet(key, FMT.natural(p, n).toFixed(6));
        }
        emit("value", key);
        schedulePictures(p);
    }
    /* filter subtypes are plain ints to the plugin (no options in chain_params) */
    function isFilterSubtype(p) { return ctName(p) === "ct_filtersubtype"; }

    function setMacro(i, n) { n = n < 0 ? 0 : n > 1 ? 1 : n; var k = "macro" + (i + 1); S.vals[k] = n; S.pending[k] = Date.now(); rawSet(k, n.toFixed(6)); emit("value", k); }
    function setModule(key, value) { S.vals[key] = value; rawSet(key, String(value)); emit("value", key); }
    function selectPreset(i) { S.vals.preset = i; S.pending.preset = Date.now(); rawSet("preset", String(i)); emit("preset", i); expectPatch(); }

    /* ---- inbound ------------------------------------------------------------- */
    var typeChanged = false;
    function onParams(params) {
        var touched = [], presetMoved = false;
        var whole = Object.prototype.hasOwnProperty.call(params, PFX + "sparse");
        if (whole) S.full = {};
        for (var full in params) {
            if (!Object.prototype.hasOwnProperty.call(params, full) || full.indexOf(PFX) !== 0) continue;
            var key = full.substring(PFX.length), raw = params[full];
            if (whole) S.full[key] = raw;
            if (key === "rid") { ridSeen(raw); continue; }
            var p = S.byKey[key];
            if (p || /^macro[1-8]$/.test(key)) {
                var n = parseFloat(raw);
                if (isNaN(n)) continue;
                if (S.pending[key] && Date.now() - S.pending[key] < 600 && Math.abs((S.vals[key] || 0) - n) > 1e-6) continue; // our own write is still landing
                delete S.pending[key];
                S.vals[key] = n; touched.push(key);
            } else {
                var before = S.vals[key];
                S.vals[key] = raw;
                if (key === "preset" || key === "preset_name") { if (String(before) !== String(raw)) presetMoved = true; }
                touched.push(key);
            }
        }
        if (whole) touched = touched.concat(applyBase());
        if (!S.connected && touched.length) { S.connected = true; emit("connected"); }
        if (touched.length) emit("values", touched);
        if (presetMoved) { emit("preset", S.vals.preset); expectPatch(); }
    }

    /* ---- files ------------------------------------------------------------------- */
    function fetchJSON(name) {
        var dir = ASSETS + (rid !== null ? rid + "/" : "");
        return fetch(dir + name + "?t=" + Date.now(), { cache: "no-store" }).then(function (r) { return r.ok ? r.json() : null; }).catch(function () { return null; });
    }
    /* Values out of the last whole state over the preset's: what the state
     * carries, else `pv`. A key with one of our writes still landing is left
     * alone. Returns the keys that moved. */
    function applyBase() {
        if (!S.full || !S.params) return [];
        var moved = [], now = Date.now();
        function put(k, pv) {
            var has = Object.prototype.hasOwnProperty.call(S.full, k);
            var n = has ? parseFloat(S.full[k]) : pv;
            if (n === undefined || n === null || isNaN(n)) return;
            if (S.pending[k] && now - S.pending[k] < 600) return;
            if (S.vals[k] !== n) { S.vals[k] = n; moved.push(k); }
        }
        S.params.params.forEach(function (p) { put(p.k, p.pv); });
        (S.params.macros || []).forEach(function (m) { put(m.key, m.pv); });
        return moved;
    }
    function indexParams(j) {
        S.params = j; S.byKey = {};
        j.params.forEach(function (p) {
            S.byKey[p.k] = p;
            /* a value that arrived before its parameter was known came in as text */
            if (typeof S.vals[p.k] === "string") { var n = parseFloat(S.vals[p.k]); if (!isNaN(n)) S.vals[p.k] = n; }
        });
        S.preset = j.preset; S.presetName = j.preset_name; S.category = j.category;
        S.rev.params = j.rev;
        return applyBase();
    }
    function loadParams() {
        return fetchJSON("patch_params.json").then(function (j) {
            if (j && j.params) { var moved = indexParams(j); emit("params", j); if (moved.length) emit("values", moved); }
            return j;
        });
    }
    function loadAll() { filesStarted = true; loadParams(); loadPresets(); loadWaves(); loadFilters(); }
    /* the state names this instance's directory; a new one (the slot was
     * reloaded) means every file is someone else's until re-read */
    function ridSeen(raw) {
        var r = parseInt(raw, 10);
        if (isNaN(r) || r < 0 || (r === rid && filesStarted)) return;   // -1: the plugin writes no files
        rid = r;
        loadAll();
    }
    function loadPresets() { return fetchJSON("presets_index.json").then(function (j) { if (j && j.categories) { S.presets = j; emit("presets", j); } return j; }); }
    function loadWaves() { return fetchJSON("waves.json").then(function (j) { if (j && j.scenes) { S.waves = j; emit("waves", j); } return j; }); }
    function loadFilters() { return fetchJSON("filters.json").then(function (j) { if (j && j.scenes) { S.filters = j; emit("filters", j); } return j; }); }

    /* After a preset change the plugin rebuilds its files over the next few
     * hundred ms (it formats them on the audio thread, a slice per block); the
     * page re-reads until patch_params.json names the preset the state names,
     * on a widening schedule, then stops. */
    var expectTimer = null, expectStep = 0;
    var EXPECT_MS = [400, 900, 1600, 2600, 4000];
    function expectPatch() {
        clearTimeout(expectTimer); expectStep = 0;
        var tick = function () {
            loadParams().then(function (j) {
                var want = S.vals.preset_name;
                var ok = j && (!want || j.preset_name === want);
                loadWaves(); loadFilters();
                if (!ok && ++expectStep < EXPECT_MS.length) expectTimer = setTimeout(tick, EXPECT_MS[expectStep]);
            });
        };
        expectTimer = setTimeout(tick, EXPECT_MS[0]);
    }

    /* Oscillator and filter edits change their pictures; the plugin redraws
     * them within a few blocks, the page fetches a little after the last edit.
     * A type change also re-shapes parameters, so the metadata is re-read too. */
    var picTimer = null, picBits = 0;
    function schedulePictures(p) {
        var ctn = ctName(p);
        var isType = ctn === "ct_osctype" || ctn === "ct_fxtype" || ctn === "ct_filtertype" || ctn === "ct_wstype" || ctn === "ct_lfotype";
        if (isType) picBits |= 7;
        else if (p.g === 2) picBits |= 1;      // cg_OSC
        else if (p.g === 4) picBits |= 2;      // cg_FILTER
        else return;
        clearTimeout(picTimer);
        picTimer = setTimeout(function () {
            var b = picBits; picBits = 0;
            if (b & 4) loadParams();
            if (b & 1) loadWaves();
            if (b & 2) loadFilters();
            if (b & 4) setTimeout(function () { loadParams(); loadWaves(); loadFilters(); }, 900);   // the type switch lands on the audio thread
        }, 450);
    }

    function refreshAll() { if (remote) rawSet("remote_refresh", "1"); setTimeout(function () { loadParams(); loadPresets(); loadWaves(); loadFilters(); }, 600); }

    /* ---- boot ----------------------------------------------------------------------- */
    function start() {
        if (remote) remote.onParamChange(onParams);
        /* the files wait for the state to name their directory; a plugin that
         * never does (an older build) wrote them to remote/ itself */
        setTimeout(function () { if (!filesStarted) loadAll(); }, 1500);
    }

    root.SurgeCore = {
        S: S, META: META, on: on, emit: emit, start: start, remote: remote, PFX: PFX,
        val: val, natural: natural, text: text, ctName: ctName,
        setNorm: setNorm, setMacro: setMacro, setModule: setModule, selectPreset: selectPreset, refreshAll: refreshAll,
        loadParams: loadParams, loadPresets: loadPresets, loadWaves: loadWaves, loadFilters: loadFilters,
    };
})(window);
