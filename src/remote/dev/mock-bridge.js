/* A stand-in for Schwung Manager's schwungRemote bridge and for the plugin's
 * files, so the page can be looked at with no device. It replays a capture:
 * window.SURGE_FIXTURE = { state, patch_params, presets_index, waves, filters }
 * taken from a running module (tests/remote/fixtures/), answers fetch() for
 * the remote/*.json files from it, and echoes writes back into the state the
 * way the manager does (a re-read ~250 ms after the write). Never shipped.
 */
(function () {
    "use strict";
    var FX = window.SURGE_FIXTURE || {};
    var state = Object.assign({}, FX.state || {});
    var params = FX.patch_params || { rev: 1, preset: 0, preset_name: "Init", category: "", macros: [], wavetables: [[], []], params: [], routings: [] };
    var byKey = {}; params.params.forEach(function (p) { byKey[p.k] = p; });
    var listeners = [], refetch = null;
    function emit() { var copy = Object.assign({}, state); listeners.forEach(function (f) { try { f(copy); } catch (e) { console.error(e); } }); }
    function schedule() { clearTimeout(refetch); refetch = setTimeout(emit, 250); }
    window.schwungRemote = {
        component: "synth",
        onParamChange: function (f) { listeners.push(f); setTimeout(emit, 300); },
        getParam: function (k) { return Promise.resolve(state[k]); },
        getHierarchy: function () { return Promise.resolve(null); },
        getChainParams: function () { return Promise.resolve(FX.chain_params || null); },
        resubscribe: function () { schedule(); },
        setParam: function (full, value) {
            var key = full.replace(/^synth:/, ""); value = String(value);
            var p = byKey[key];
            if (p) {
                var n;
                if (p.vt !== 2) { var i = p.o ? p.o.indexOf(value) : -1; if (i < 0) i = parseInt(value, 10) - p.min; n = (p.max > p.min) ? i / (p.max - p.min) : 0; }
                else n = (parseFloat(value) - p.min) / (p.max - p.min);
                state[full] = String(Math.max(0, Math.min(1, n)));
            } else if (/^macro[1-8]$/.test(key)) state[full] = value;
            else if (key === "preset") { state[full] = value; var nm = findPreset(parseInt(value, 10)); if (nm) { state["synth:preset_name"] = nm; params.preset_name = nm; params.preset = parseInt(value, 10); params.rev++; } }
            else if (key === "remote_refresh" || key === "all_notes_off") { /* nothing to change */ }
            else state[full] = value;
            schedule();
        }
    };
    function findPreset(i) { var idx = FX.presets_index; if (!idx) return null; for (var c of idx.categories) for (var p of c.presets) if (p.i === i) return p.n; return null; }
    var realFetch = window.fetch;
    window.fetch = function (url) {
        var m = /remote\/(patch_params|presets_index|waves|filters)\.json/.exec(String(url));
        if (m) {
            var body = m[1] === "patch_params" ? params : FX[m[1]] || null;
            return Promise.resolve(new Response(body ? JSON.stringify(body) : "", { status: body ? 200 : 404, headers: { "Content-Type": "application/json" } }));
        }
        return realFetch.apply(this, arguments);
    };
})();
