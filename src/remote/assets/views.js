/* Surge XT Remote UI -- the views.
 *
 * Nine tabs under a persistent top bar: Play, Presets, Osc, Filter, Env, LFO,
 * Mod, FX, Global. Scene A and Scene B are the same views with a key prefix
 * ("" or "b_") and a colour; the switch in the top bar flips both. Every view
 * is built from patch_params.json, so a parameter that Surge renames or
 * retypes when an oscillator or effect type changes is rebuilt, not relabelled.
 */
(function (root) {
    "use strict";
    var C = root.SurgeCore, W = root.SurgeWidgets, FMT = root.SurgeFormat, S = C.S, META = C.META;

    var V = { scene: 0, tab: "play", lfo: 0, widgets: [], fav: loadFavs(), catFilter: null, q: "", favOnly: false };
    var el = {};

    function loadFavs() { try { return JSON.parse(localStorage.getItem("surge.favs") || "{}") || {}; } catch (e) { return {}; } }
    function saveFavs() { try { localStorage.setItem("surge.favs", JSON.stringify(V.fav)); } catch (e) {} }
    function pfx() { return V.scene ? "b_" : ""; }
    function k(name) { return pfx() + name; }
    function has(key) { return !!S.byKey[key]; }
    function reg(w) { if (w) V.widgets.push(w); return w; }
    function h(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }
    function section(title, tag, cls) {
        var s = h("section", "section" + (cls ? " " + cls : ""));
        var hd = h("h2"); hd.appendChild(document.createTextNode(title)); var sp = h("span", "sp"); hd.appendChild(sp);
        if (tag) hd.appendChild(h("span", "tag", tag));
        s.appendChild(hd); return s;
    }
    function cells(cls) { return h("div", "cells" + (cls ? " " + cls : "")); }
    function isNone(key) { var p = S.byKey[key]; return !p || C.ctName(p) === "ct_none"; }

    /* ---- shell --------------------------------------------------------------------- */
    function buildShell(root) {
        root.innerHTML = "";
        var app = h("div", "app"); el.app = app;
        var top = h("div", "topbar");
        var title = h("div", "title");
        el.name = h("div", "name", "Surge XT"); var sub = h("div", "sub");
        el.dot = h("span", "dot"); el.cat = h("span", "cat", ""); el.status = h("span", "", "connecting");
        sub.appendChild(el.dot); sub.appendChild(el.cat); sub.appendChild(el.status);
        title.appendChild(el.name); title.appendChild(sub);
        var tools = h("div", "tools");
        var sw = h("div", "scenesw"); sw.setAttribute("role", "group"); sw.setAttribute("aria-label", "Scene");
        el.sceneBtns = [0, 1].map(function (i) { var b = h("button", i ? "b" : "a", i ? "Scene B" : "Scene A"); b.type = "button"; b.setAttribute("aria-pressed", String(i === 0)); b.addEventListener("click", function () { setScene(i); }); sw.appendChild(b); return b; });
        var fine = h("button", "btn", "Fine"); fine.type = "button"; fine.setAttribute("aria-pressed", "false"); fine.title = "Fine adjustment for every drag (or hold a second finger down)";
        fine.addEventListener("click", function () { W.FINE.on = !W.FINE.on; fine.setAttribute("aria-pressed", String(W.FINE.on)); });
        var panic = h("button", "btn ghost", "Panic"); panic.type = "button"; panic.title = "All notes off"; panic.addEventListener("click", function () { C.setModule("all_notes_off", 1); });
        var theme = h("button", "btn icon", "\u25D0"); theme.type = "button"; theme.setAttribute("aria-label", "Switch light and dark"); theme.addEventListener("click", toggleTheme);
        tools.appendChild(sw); tools.appendChild(fine); tools.appendChild(panic); tools.appendChild(theme);
        top.appendChild(title); top.appendChild(tools);
        app.appendChild(top);
        var tabs = h("div", "tabs"); tabs.setAttribute("role", "tablist");
        el.tabBtns = TABS.map(function (t) { var b = h("button", "", t.label); b.type = "button"; b.setAttribute("role", "tab"); b.dataset.tab = t.id; b.setAttribute("aria-selected", "false"); b.addEventListener("click", function () { setTab(t.id); }); tabs.appendChild(b); return b; });
        tabs.addEventListener("keydown", function (e) { var i = el.tabBtns.indexOf(document.activeElement); if (i < 0) return; var n = e.key === "ArrowRight" ? i + 1 : e.key === "ArrowLeft" ? i - 1 : -1; if (n < 0) return; n = (n + TABS.length) % TABS.length; el.tabBtns[n].focus(); setTab(TABS[n].id); e.preventDefault(); });
        app.appendChild(tabs);
        el.view = h("div", "view"); el.view.setAttribute("role", "tabpanel"); app.appendChild(el.view);
        var foot = h("div", "foot"); foot.innerHTML = "<span>Drag knobs and faders; drag the handles on envelopes and pads. Double-tap resets, long-press types a value. Fine: the switch, or a second finger held anywhere.</span><span>Surge XT \u00B7 Schwung</span>";
        app.appendChild(foot);
        root.appendChild(app);
        try { var th = localStorage.getItem("surge.theme"); if (th) document.documentElement.setAttribute("data-theme", th); } catch (e) {}
        try { V.tab = localStorage.getItem("surge.tab") || "play"; } catch (e) {}
        var qt = new URLSearchParams(location.search).get("tab"); if (qt && TABS.some(function (t) { return t.id === qt; })) V.tab = qt;
    }
    function toggleTheme() { var cur = document.documentElement.getAttribute("data-theme") === "light" ? "dark" : "light"; if (cur === "dark") document.documentElement.removeAttribute("data-theme"); else document.documentElement.setAttribute("data-theme", cur); try { localStorage.setItem("surge.theme", cur === "dark" ? "" : cur); } catch (e) {} paintAll(); }
    function setScene(i) { V.scene = i; el.sceneBtns.forEach(function (b, j) { b.setAttribute("aria-pressed", String(j === i)); }); el.app.classList.toggle("scene-b", i === 1); render(); }
    var TABS = [
        { id: "play", label: "Play" }, { id: "presets", label: "Presets" }, { id: "osc", label: "Osc" }, { id: "filter", label: "Filter" },
        { id: "env", label: "Env" }, { id: "lfo", label: "LFO" }, { id: "mod", label: "Mod" }, { id: "fx", label: "FX" }, { id: "global", label: "Global" },
    ];
    function setTab(id) { V.tab = id; try { localStorage.setItem("surge.tab", id); } catch (e) {} render(); }

    function refreshHeader() {
        var name = S.vals.preset_name || S.presetName || "Surge XT";
        el.name.textContent = name;
        el.cat.textContent = S.category || S.vals.bank_name || "";
        var pend = Object.keys(S.pending).length;
        el.dot.className = "dot " + (!S.connected ? "" : pend ? "busy" : "on");
        el.status.textContent = !S.connected ? (C.remote ? "waiting for the slot" : "no host") : pend ? "sending" : "in sync";
    }

    /* ---- render the current tab ------------------------------------------------------ */
    function render() {
        V.widgets = [];
        el.tabBtns.forEach(function (b) { b.setAttribute("aria-selected", String(b.dataset.tab === V.tab)); });
        el.view.innerHTML = "";
        if (!S.params) { el.view.appendChild(h("div", "empty", C.remote ? "Reading the patch from the module\u2026" : "No host: open this page from Schwung Manager.")); return; }
        var f = VIEWS[V.tab]; if (f) f(el.view);
        paintAll();
    }
    function paintAll() { V.widgets.forEach(function (w) { if (w.paint) w.paint(); }); refreshHeader(); }
    function repaintKeys(keys) {
        var set = {}; keys.forEach(function (k2) { set[k2] = true; });
        V.widgets.forEach(function (w) {
            if (w.key && set[w.key]) w.paint();
            else if (w.keys && w.keys.some(function (k2) { return set[k2]; })) w.paint();
            else if (w.any) w.paint();
        });
        if (set.preset || set.preset_name) refreshHeader();
    }

    /* ---- drawings --------------------------------------------------------------------- */
    function acc() { return getComputedStyle(el.app).getPropertyValue("--acc").trim() || "#f80"; }
    function grid(g, w, h2, col) { g.strokeStyle = col || "rgba(128,128,128,0.16)"; g.lineWidth = 1; g.beginPath(); for (var i = 1; i < 4; i++) { var y = Math.round(h2 * i / 4) + 0.5; g.moveTo(0, y); g.lineTo(w, y); } for (var j = 1; j < 8; j++) { var x = Math.round(w * j / 8) + 0.5; g.moveTo(x, 0); g.lineTo(x, h2); } g.stroke(); }
    function glow(g, path, col, width) { g.save(); g.shadowColor = col; g.shadowBlur = 10; g.strokeStyle = col; g.lineWidth = width || 2; g.lineJoin = "round"; g.lineCap = "round"; g.beginPath(); path(); g.stroke(); g.restore(); }

    function drawWave(g, w, h2, pts, col) {
        grid(g, w, h2);
        if (!pts || !pts.length) { g.fillStyle = "rgba(128,128,128,0.5)"; g.font = "600 11px " + getComputedStyle(document.body).fontFamily; g.fillText("no picture yet", 10, h2 - 8); return; }
        var mid = h2 / 2, amp = h2 / 2 - 8;
        glow(g, function () { for (var i = 0; i < pts.length; i++) { var x = i / (pts.length - 1) * w, y = mid - Math.max(-1, Math.min(1, pts[i])) * amp; if (i === 0) g.moveTo(x, y); else g.lineTo(x, y); } }, col, 2);
    }
    /* filters.json -> a curve in a 20..20k log / -48..+24 dB frame */
    function drawFilterCurves(g, w, h2, units, cols) {
        grid(g, w, h2, "rgba(128,128,128,0.12)");
        var f0 = Math.log(20), f1 = Math.log(20000);
        var lab = ["100", "1k", "10k"]; g.fillStyle = "rgba(128,128,128,0.55)"; g.font = "600 10px " + getComputedStyle(document.body).getPropertyValue("--mono");
        [100, 1000, 10000].forEach(function (f, i) { var x = (Math.log(f) - f0) / (f1 - f0) * w; g.fillRect(Math.round(x), h2 - 6, 1, 6); g.fillText(lab[i], x + 4, h2 - 4); });
        units.forEach(function (u, i) {
            if (!u || !u.db || !S.filters) return;
            var hz = S.filters.hz;
            glow(g, function () { for (var j = 0; j < hz.length; j++) { var x = (Math.log(hz[j]) - f0) / (f1 - f0) * w, y = h2 * (0.25 + (-u.db[j]) / 72 * 1.0); y = Math.max(2, Math.min(h2 - 2, y)); if (j === 0) g.moveTo(x, y); else g.lineTo(x, y); } }, cols[i], 2.2);
        });
    }
    function lfoShape(type, deform, t) {
        var ph = t - Math.floor(t);
        switch (type) {
            case 0: return Math.sin(2 * Math.PI * ph);
            case 1: return 1 - Math.abs(ph * 4 - 2);
            case 2: return ph < 0.5 + deform * 0.45 ? 1 : -1;
            case 3: return 1 - 2 * ph;
            case 4: return (Math.sin(t * 91.7) * 43758.5453) % 1 * 2 - 1;
            case 5: { var s = Math.floor(t); return ((Math.sin(s * 12.9898) * 43758.5453) % 1) * 2 - 1; }
            case 6: return Math.min(1, ph * 4) * Math.exp(-ph * 3) * 2 - 1;
            case 7: { var st = Math.floor(ph * 16); return ((Math.sin(st * 7.13 + 1) * 43758.5453) % 1) * 2 - 1; }
            default: return Math.sin(2 * Math.PI * ph);
        }
    }

    /* ---- shared building blocks ---------------------------------------------------------- */
    /* opts.labels: {key: label} overrides for a row where Surge's short names repeat */
    function knobs(container, keys, opts) {
        keys.forEach(function (key) {
            if (!has(key) || isNone(key)) return;
            var o = opts ? Object.assign({}, opts) : {};
            if (o.labels) { if (o.labels[key]) o.label = o.labels[key]; delete o.labels; }
            var w = reg(W.auto(key, o)); container.appendChild(w.el);
        });
    }
    function oscTypeStrip(key) {
        var p = S.byKey[key]; var strip = h("div", "typestrip"); strip.setAttribute("role", "radiogroup"); strip.setAttribute("aria-label", "Oscillator type");
        var opts = p && p.o ? p.o : (META.osc_type_names || []);
        var btns = opts.map(function (name, i) {
            var b = h("button"); b.type = "button"; b.setAttribute("role", "radio");
            b.innerHTML = oscGlyph(i) + "<span></span>"; b.lastChild.textContent = name;
            b.addEventListener("click", function () { C.setNorm(key, FMT.normalised(p, p.min + i)); paint(); });
            strip.appendChild(b); return b;
        });
        function paint() { var n = C.val(key), cur = n === null ? -1 : FMT.natural(p, n) - p.min; btns.forEach(function (b, i) { b.setAttribute("aria-checked", String(i === cur)); }); var sel = btns[cur]; if (sel && sel.scrollIntoView) sel.scrollIntoView({ inline: "nearest", block: "nearest" }); }
        return { el: strip, key: key, paint: paint };
    }
    function oscGlyph(i) {
        var d = ["M0 8 L10 2 L10 14 L20 2 L20 14 L30 2 L30 14 L40 8", "M0 8 C5 0 10 0 13 8 S22 16 26 8 S35 0 40 8", "M0 12 L6 2 L12 14 L18 4 L24 12 L30 3 L36 13 L40 8", "M0 8 L4 8 L4 3 L9 3 L9 12 L14 12 L14 6 L19 6 L19 10 L24 10 L24 2 L30 2 L30 14 L36 14 L36 8 L40 8",
            "M0 8 L6 2 L6 14 L12 2 L12 14 L18 2 L18 14 L24 2 L24 14 L30 2 L30 14 L36 2 L36 14 L40 8", "M0 14 L10 2 L20 14 L30 2 L40 14", "M0 8 C10 -4 20 20 30 8 S38 4 40 8", "M0 8 L2 3 L4 13 L6 5 L8 11 L10 2 L12 14 L14 6 L16 10 L18 4 L20 12 L22 6 L24 9 L26 2 L28 14 L30 7 L32 9 L34 5 L36 11 L38 3 L40 8",
            "M0 8 Q10 -6 20 8 T40 8", "M0 14 L8 2 L16 14 L24 2 L32 14 L40 2", "M0 8 L5 8 L5 2 L12 2 L12 14 L20 14 L20 2 L27 2 L27 8 L40 8", "M0 8 C6 2 8 14 14 8 S20 2 26 8 S34 14 40 8", "M0 12 L8 4 L8 12 L16 4 L16 12 L24 4 L24 12 L32 4 L32 12 L40 4", "M0 8 L40 8 M0 3 L40 13", "M0 8 Q20 -8 40 8"];
        return '<svg viewBox="-1 -1 42 18" aria-hidden="true"><path d="' + (d[i] || d[0]) + '" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"/></svg>';
    }

    /* ---- views ------------------------------------------------------------------------------ */
    var VIEWS = {};

    VIEWS.play = function (v) {
        var g1 = h("div", "grid two");
        /* macros: Surge's eight assignable controllers, labelled by the patch */
        var sm = section("Macros", "patch controls", "scene-g");
        var row = h("div", "macros");
        for (var i = 0; i < 8; i++) { var m = S.params.macros && S.params.macros[i]; var w = reg(W.macro(i, m && m.label)); row.appendChild(w.el); }
        sm.appendChild(row);
        /* filters as a pad */
        var sf = section("Filters", V.scene ? "Scene B" : "Scene A");
        sf.appendChild(filterPad().el);
        var quick = cells("center");
        var lbl = {}; lbl[k("filter1_envmod")] = "F1 env depth"; lbl[k("filter2_envmod")] = "F2 env depth";
        knobs(quick, [k("filter1_envmod"), k("filter2_envmod"), k("feedback"), k("volume")], { labels: lbl });
        sf.appendChild(quick);
        g1.appendChild(sm); g1.appendChild(sf);
        v.appendChild(g1);
        var g2 = h("div", "grid two");
        /* both envelopes, drawn and with their four times each */
        var se = section("Envelopes", V.scene ? "Scene B" : "Scene A"); var eg = h("div", "envrow");
        [["env1", "Amp"], ["env2", "Filter"]].forEach(function (e) {
            var col = h("div", "envcol"); col.appendChild(h("div", "sublabel", e[1]));
            col.appendChild(reg(W.envelope({ a: k(e[0] + "_attack"), d: k(e[0] + "_decay"), s: k(e[0] + "_sustain"), r: k(e[0] + "_release"), ashape: k(e[0] + "_attack_shape"), dshape: k(e[0] + "_decay_shape"), rshape: k(e[0] + "_release_shape") }, { cls: "tall" })).el);
            eg.appendChild(col);
        });
        se.appendChild(eg);
        var sp = section("Performance", "keys \u00B7 voices \u00B7 tempo", "scene-g"); var pc = cells("center");
        pc.appendChild(moduleChoice("octave_transpose", "Octave", ["-3", "-2", "-1", "0", "+1", "+2", "+3"], -3).el);
        knobs(pc, [k("pitch"), k("portamento"), "g_polylimit"], { labels: { g_polylimit: "Voices" } });
        pc.appendChild(moduleToggle("mpe_enabled", "MPE").el);
        pc.appendChild(moduleToggle("sync_bpm", "Auto BPM").el); pc.appendChild(moduleNumber("bpm", "BPM", 20, 300).el);
        sp.appendChild(pc);
        g2.appendChild(se); g2.appendChild(sp);
        v.appendChild(g2);
    };

    function filterPad() {
        var cols = [acc(), getComputedStyle(el.app).getPropertyValue("--g").trim()];
        var pad = reg(W.xypad([{ x: k("filter1_cutoff"), y: k("filter1_resonance"), label: "Filter 1", off: k("filter1_type") }, { x: k("filter2_cutoff"), y: k("filter2_resonance"), label: "Filter 2", cls: "b", off: k("filter2_type") }],
            { draw: function (g, w, h2) { var sc = S.filters && S.filters.scenes[V.scene]; drawFilterCurves(g, w, h2, sc || [], [cols[0], getComputedStyle(el.app).getPropertyValue("--b").trim()]); } }));
        pad.any = true;
        return pad;
    }

    VIEWS.presets = function (v) {
        var wrap = h("div", "presets");
        var rail = h("div", "catlist"); rail.setAttribute("role", "listbox"); rail.setAttribute("aria-label", "Categories");
        var main = h("div", "view");
        var bar = h("div", "pbar");
        var q = h("input"); q.type = "search"; q.placeholder = "Search " + (S.presets ? S.presets.count : "") + " presets"; q.value = V.q; q.setAttribute("aria-label", "Search presets");
        q.addEventListener("input", function () { V.q = q.value.trim().toLowerCase(); drawTiles(); });
        var favBtn = h("button", "btn", "\u2605 Favourites"); favBtn.type = "button"; favBtn.setAttribute("aria-pressed", String(V.favOnly)); favBtn.addEventListener("click", function () { V.favOnly = !V.favOnly; favBtn.setAttribute("aria-pressed", String(V.favOnly)); drawTiles(); });
        var rnd = h("button", "btn", "Random"); rnd.type = "button"; rnd.addEventListener("click", function () { var pool = visible(); if (pool.length) C.selectPreset(pool[Math.floor(Math.random() * pool.length)].i); });
        var prev = h("button", "btn ghost", "\u25C2"); prev.type = "button"; prev.setAttribute("aria-label", "Previous preset"); prev.addEventListener("click", function () { step(-1); });
        var next = h("button", "btn ghost", "\u25B8"); next.type = "button"; next.setAttribute("aria-label", "Next preset"); next.addEventListener("click", function () { step(1); });
        bar.appendChild(q); bar.appendChild(favBtn); bar.appendChild(rnd); bar.appendChild(prev); bar.appendChild(next);
        var tiles = h("div", "tiles"); tiles.setAttribute("role", "grid");
        main.appendChild(bar); main.appendChild(tiles);
        wrap.appendChild(rail); wrap.appendChild(main); v.appendChild(wrap);
        var cats = S.presets ? S.presets.categories : [];
        function cur() { var p = parseInt(S.vals.preset, 10); return isNaN(p) ? -1 : p; }
        function all() { var out = []; cats.forEach(function (c) { c.presets.forEach(function (p) { out.push({ i: p.i, n: p.n, cat: c.name, kind: c.kind, fav: p.fav }); }); }); return out; }
        function visible() {
            return all().filter(function (p) {
                if (V.catFilter !== null && p.cat !== V.catFilter) return false;
                if (V.favOnly && !(V.fav[p.n] || p.fav)) return false;
                if (V.q && p.n.toLowerCase().indexOf(V.q) < 0 && p.cat.toLowerCase().indexOf(V.q) < 0) return false;
                return true;
            });
        }
        function step(d) { var list = visible(), c = cur(), idx = list.findIndex(function (p) { return p.i === c; }); var n = list[(idx + d + list.length) % list.length]; if (n) C.selectPreset(n.i); }
        function drawRail() {
            rail.innerHTML = "";
            var allB = h("button", "", ""); allB.type = "button"; allB.innerHTML = "<span>All presets</span><span class='n'></span>"; allB.lastChild.textContent = S.presets ? S.presets.count : ""; allB.setAttribute("aria-current", String(V.catFilter === null));
            allB.addEventListener("click", function () { V.catFilter = null; drawRail(); drawTiles(); }); rail.appendChild(allB);
            var lastKind = null;
            cats.forEach(function (c) {
                if (c.kind !== lastKind) { lastKind = c.kind; rail.appendChild(h("div", "kind", { factory: "Factory", thirdparty: "Third party", user: "Your patches" }[c.kind] || c.kind)); }
                var b = h("button"); b.type = "button"; b.innerHTML = "<span></span><span class='n'></span>"; b.firstChild.textContent = c.name; b.lastChild.textContent = c.presets.length;
                b.setAttribute("aria-current", String(V.catFilter === c.name));
                b.addEventListener("click", function () { V.catFilter = V.catFilter === c.name ? null : c.name; drawRail(); drawTiles(); });
                rail.appendChild(b);
            });
        }
        function drawTiles() {
            tiles.innerHTML = "";
            var list = visible(), c = cur();
            if (!list.length) { tiles.appendChild(h("div", "empty", S.presets ? "Nothing matches." : "The preset list has not arrived yet.")); return; }
            list.forEach(function (p) {
                var t = h("button", "tile" + (p.i === c && S.pending.preset ? " pending" : "")); t.type = "button"; t.setAttribute("aria-current", String(p.i === c));
                t.innerHTML = "<div class='nm'></div><div class='ct'></div>"; t.firstChild.textContent = p.n; t.lastChild.textContent = p.cat;
                t.addEventListener("click", function () { C.selectPreset(p.i); paintTiles(); });
                var star = h("button", "star", (V.fav[p.n] || p.fav) ? "\u2605" : "\u2606"); star.type = "button"; star.setAttribute("aria-pressed", String(!!(V.fav[p.n] || p.fav))); star.setAttribute("aria-label", "Favourite " + p.n);
                star.addEventListener("click", function (e) { e.stopPropagation(); if (V.fav[p.n]) delete V.fav[p.n]; else V.fav[p.n] = 1; saveFavs(); drawTiles(); });
                t.appendChild(star); t.dataset.i = p.i; tiles.appendChild(t);
            });
            var selEl = tiles.querySelector('[aria-current="true"]');
            if (selEl) { var top = selEl.offsetTop - tiles.clientHeight / 2 + selEl.offsetHeight / 2; if (selEl.offsetTop < tiles.scrollTop || selEl.offsetTop + selEl.offsetHeight > tiles.scrollTop + tiles.clientHeight) tiles.scrollTop = Math.max(0, top); }
        }
        function paintTiles() { var c = cur(); Array.prototype.forEach.call(tiles.children, function (t) { if (!t.dataset) return; var on = parseInt(t.dataset.i, 10) === c; t.setAttribute("aria-current", String(on)); t.classList.toggle("pending", on && !!S.pending.preset); }); }
        drawRail(); drawTiles();
        reg({ any: true, paint: paintTiles });
    };

    VIEWS.osc = function (v) {
        var g = h("div", "grid three");
        for (var o = 1; o <= 3; o++) (function (o) {
            var s = section("Oscillator " + o, V.scene ? "B" : "A");
            var typeKey = k("osc" + o + "_type");
            s.appendChild(reg(oscTypeStrip(typeKey)).el);
            var sc = reg(W.scope(function (g2, w, h2) { var wv = S.waves && S.waves.scenes[V.scene] && S.waves.scenes[V.scene][o - 1]; drawWave(g2, w, h2, wv && wv.w, acc()); if (wv && wv.wt) { g2.fillStyle = "rgba(128,128,128,0.7)"; g2.font = "600 10px " + getComputedStyle(document.body).getPropertyValue("--mono"); g2.fillText(wv.wt, 8, 14); } }));
            sc.any = true; s.appendChild(sc.el);
            var c1 = cells("compact"); knobs(c1, [k("osc" + o + "_octave"), k("osc" + o + "_pitch"), k("osc" + o + "_keytrack"), k("osc" + o + "_retrigger")]);
            for (var i = 0; i < 7; i++) knobs(c1, [k("osc" + o + "_param" + i)]);
            s.appendChild(c1);
            g.appendChild(s);
        })(o);
        v.appendChild(g);
        var mix = section("Mixer", "level \u00B7 mute \u00B7 solo \u00B7 route"); var mx = h("div", "mixer");
        [["o1", "Osc 1"], ["o2", "Osc 2"], ["o3", "Osc 3"], ["noise", "Noise"], ["ring12", "Ring 1\u00D72"], ["ring23", "Ring 2\u00D73"], ["pfg", "Pre-filter"]].forEach(function (ch) {
            var col = h("div", "chan");
            var lv = reg(W.slider(k("level_" + ch[0]), { label: ch[1] })); col.appendChild(lv.el);
            if (has(k("mute_" + ch[0]))) { var mini = h("div", "mini"); [["mute", "M"], ["solo", "S"]].forEach(function (m) { var key = k(m[0] + "_" + ch[0]); if (!has(key)) return; var b = h("button", "", m[1]); b.type = "button"; b.setAttribute("aria-pressed", "false"); b.title = m[0]; b.addEventListener("click", function () { C.setNorm(key, C.val(key) > 0.5 ? 0 : 1); paint(); }); function paint() { b.setAttribute("aria-pressed", String(C.val(key) > 0.5)); } reg({ key: key, paint: paint }); mini.appendChild(b); });
                col.appendChild(mini);
                if (has(k("route_" + ch[0]))) { var rw = reg(W.choice(k("route_" + ch[0]), { label: "Route", maxSeg: 3, optionLabels: { "Filter 1": "F1", "Both": "Both", "Filter 2": "F2" } })); rw.el.querySelector(".label").remove(); rw.el.classList.add("route"); col.appendChild(rw.el); } }
            mx.appendChild(col);
        });
        mix.appendChild(mx);
        var com = section("Scene", "pitch \u00B7 play \u00B7 output"); var cc = cells();
        knobs(cc, [k("octave"), k("pitch"), k("portamento"), k("polymode"), k("fm_switch"), k("fm_depth"), k("drift"), k("noisecol"), k("ktrkroot"), k("volume"), k("pan"), k("pan2"), k("send_fx_1"), k("send_fx_2"), k("send_fx_3"), k("send_fx_4")]);
        com.appendChild(cc);
        v.appendChild(mix); v.appendChild(com);
    };

    VIEWS.filter = function (v) {
        var top = section("Filters", V.scene ? "Scene B" : "Scene A");
        top.appendChild(filterPad().el);
        v.appendChild(top);
        var g = h("div", "grid two");
        [1, 2].forEach(function (f) {
            var s = section("Filter " + f, f === 2 ? "" : "");
            var c = cells();
            knobs(c, [k("filter" + f + "_type"), k("filter" + f + "_subtype"), k("filter" + f + "_cutoff"), k("filter" + f + "_resonance"), k("filter" + f + "_envmod"), k("filter" + f + "_keytrack")]);
            if (f === 2) knobs(c, [k("f2_cf_is_offset"), k("f2_link_resonance")]);
            s.appendChild(c); g.appendChild(s);
        });
        v.appendChild(g);
        var sb = section("Filter block", "routing \u00B7 waveshaper \u00B7 amplifier"); var gr = h("div", "groups");
        [["Block", [k("fb_config"), k("f_balance"), k("feedback"), k("lowcut")]], ["Waveshaper", [k("ws_type"), k("ws_drive")]], ["Amplifier", [k("vca_level"), k("vca_velsense")]]].forEach(function (grp) {
            var col = h("div", "envcol"); col.appendChild(h("div", "sublabel", grp[0])); var c = cells(); knobs(c, grp[1]); col.appendChild(c); gr.appendChild(col);
        });
        sb.appendChild(gr); v.appendChild(sb);
    };

    VIEWS.env = function (v) {
        var g = h("div", "grid two");
        [["env1", "Amp envelope"], ["env2", "Filter envelope"]].forEach(function (e) {
            var s = section(e[1]);
            s.appendChild(reg(W.envelope({ a: k(e[0] + "_attack"), d: k(e[0] + "_decay"), s: k(e[0] + "_sustain"), r: k(e[0] + "_release"), ashape: k(e[0] + "_attack_shape"), dshape: k(e[0] + "_decay_shape"), rshape: k(e[0] + "_release_shape") }, { cls: "tall" })).el);
            var c = cells(); knobs(c, [k(e[0] + "_attack"), k(e[0] + "_decay"), k(e[0] + "_sustain"), k(e[0] + "_release")]); s.appendChild(c);
            var c2 = cells(); knobs(c2, [k(e[0] + "_attack_shape"), k(e[0] + "_decay_shape"), k(e[0] + "_release_shape"), k(e[0] + "_mode")]); s.appendChild(c2);
            g.appendChild(s);
        });
        v.appendChild(g);
    };

    VIEWS.lfo = function (v) {
        var strip = h("div", "tabs"); strip.setAttribute("role", "tablist"); strip.setAttribute("aria-label", "LFO");
        var names = []; for (var i = 0; i < 12; i++) names.push(i < 6 ? "LFO " + (i + 1) : "S-LFO " + (i - 5));
        names.forEach(function (n, i) { var b = h("button", "", n); b.type = "button"; b.setAttribute("role", "tab"); b.setAttribute("aria-selected", String(i === V.lfo)); b.addEventListener("click", function () { V.lfo = i; render(); }); strip.appendChild(b); });
        v.appendChild(strip);
        var L = "lfo" + V.lfo + "_";
        var g = h("div", "grid two");
        var s = section(names[V.lfo], V.lfo < 6 ? "per voice" : "per scene");
        var shapeW = reg(W.choice(k(L + "shape"), { label: "Shape", maxSeg: 12, chips: true })); s.appendChild(shapeW.el);
        var phase = 0, last = 0, raf = 0;
        var sc = reg(W.scope(function (g2, w, h2) {
            grid(g2, w, h2);
            var type = C.natural(k(L + "shape")) || 0, rate = C.val(k(L + "rate")) || 0, mag = C.val(k(L + "magnitude")); mag = mag === null ? 1 : mag; var def = (C.val(k(L + "deform")) || 0.5) * 2 - 1;
            var uni = C.val(k(L + "unipolar")) > 0.5;
            var mid = h2 / 2, amp = h2 / 2 - 8, cyc = 1 + rate * 5;
            glow(g2, function () { for (var x = 0; x <= w; x++) { var t = x / w * cyc + phase, y0 = lfoShape(type, def, t) * mag; if (uni) y0 = y0 * 0.5 + 0.5; var y = mid - y0 * amp; if (x === 0) g2.moveTo(x, y); else g2.lineTo(x, y); } }, acc(), 2.2);
            g2.fillStyle = "rgba(128,128,128,0.6)"; g2.font = "600 10px " + getComputedStyle(document.body).getPropertyValue("--mono"); g2.fillText((META.lt_names && META.lt_names[type]) || "", 8, 14);
        }, "tall"));
        sc.any = true; s.appendChild(sc.el);
        function anim(now) { raf = 0; if (V.tab !== "lfo") return; var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches; if (!reduce) { var dt = last ? (now - last) / 1000 : 0; last = now; phase += dt * (0.2 + (C.val(k(L + "rate")) || 0) * 2.5); sc.paint(); raf = requestAnimationFrame(anim); } }
        raf = requestAnimationFrame(anim);
        var c = cells(); knobs(c, [k(L + "rate"), k(L + "phase"), k(L + "magnitude"), k(L + "deform"), k(L + "trigmode"), k(L + "unipolar")]); s.appendChild(c);
        var se = section("LFO envelope", "delay \u00B7 attack \u00B7 hold \u00B7 decay \u00B7 sustain \u00B7 release");
        var ce = cells("compact"); knobs(ce, [k(L + "delay"), k(L + "attack"), k(L + "hold"), k(L + "decay"), k(L + "sustain"), k(L + "release")]); se.appendChild(ce);
        g.appendChild(s); g.appendChild(se); v.appendChild(g);
    };

    VIEWS.mod = function (v) {
        var g = h("div", "grid two");
        var sm = section("Modulation slots", "six routings you set", "scene-g");
        v.appendChild(sm);
        var chain = C.chain || [];
        var srcOpts = (chain.filter(function (c) { return c.key === "mod_0_source"; })[0] || {}).options || META.modsource_names || [];
        var destKeys = S.params.params.map(function (p) { return p.k; });
        var destNames = S.params.params.map(function (p) { return p.n; });
        var destGroups = S.params.params.map(function (p) { return (p.sc === 1 ? "Scene A \u00B7 " : p.sc === 2 ? "Scene B \u00B7 " : "Global \u00B7 ") + (META.control_groups[p.g] || ""); });
        for (var i = 0; i < 6; i++) (function (i) {
            var row = h("div", "modrow");
            var en = h("button", "toggle"); en.type = "button"; en.setAttribute("role", "switch"); en.innerHTML = '<span class="knb"></span>'; en.setAttribute("aria-label", "Slot " + (i + 1) + " enabled");
            en.addEventListener("click", function () { var on = String(S.vals["mod_" + i + "_enable"]) === "1" || S.vals["mod_" + i + "_enable"] === "On"; C.setModule("mod_" + i + "_enable", on ? "Off" : "On"); paint(); });
            var src = h("button", "picker"); src.type = "button"; src.innerHTML = '<span class="txt"></span><span class="chev">\u2304</span>';
            src.addEventListener("click", function () { W.pickSheet("Source", srcOpts, srcIndex(), function (j) { C.setModule("mod_" + i + "_source", srcOpts[j]); paint(); }); });
            var dst = h("button", "picker"); dst.type = "button"; dst.innerHTML = '<span class="txt"></span><span class="chev">\u2304</span>';
            dst.addEventListener("click", function () { W.pickSheet("Destination", destNames, destIndex(), function (j) { C.setModule("mod_" + i + "_dest", destKeys[j]); paint(); }, destGroups); });
            var amt = h("div", "cell slider-cell h"); var sl = h("div", "slider"); sl.tabIndex = 0; sl.setAttribute("role", "slider"); sl.setAttribute("aria-label", "Slot " + (i + 1) + " amount"); sl.innerHTML = '<div class="rail"></div><div class="fill"></div><div class="thumb"></div>'; var av = h("div", "value"); amt.appendChild(sl); amt.appendChild(av);
            function amount() { var a = parseFloat(S.vals["mod_" + i + "_amount"]); return isNaN(a) ? 0 : a; }
            W.bindDrag(sl, { get: function () { return (amount() + 1) / 2; }, set: function (n) { C.setModule("mod_" + i + "_amount", (n * 2 - 1).toFixed(3)); paint(); }, axis: "x", pixels: 260, label: function () { return "Amount"; }, text: function () { return Math.round(amount() * 100) + " %"; }, onDoubleTap: function () { C.setModule("mod_" + i + "_amount", "0"); paint(); } });
            function srcIndex() { var s = S.vals["mod_" + i + "_source"]; var j = srcOpts.indexOf(String(s)); if (j >= 0) return j; var n = parseInt(s, 10); return isNaN(n) ? 0 : n; }
            function destIndex() { var d = S.vals["mod_" + i + "_dest"]; var j = destKeys.indexOf(String(d)); if (j >= 0) return j; var n = parseInt(d, 10); return isNaN(n) ? -1 : n - 1; }
            function paint() {
                var e = S.vals["mod_" + i + "_enable"]; en.setAttribute("aria-checked", String(String(e) === "1" || e === "On"));
                src.querySelector(".txt").textContent = srcOpts[srcIndex()] || "none";
                var di = destIndex(); dst.querySelector(".txt").textContent = di >= 0 && destNames[di] ? destNames[di] : "none";
                var a = amount(), n = (a + 1) / 2; sl.querySelector(".thumb").style.left = (n * 100) + "%"; var lo = Math.min(n, 0.5) * 100, hi = Math.max(n, 0.5) * 100; sl.querySelector(".fill").style.left = lo + "%"; sl.querySelector(".fill").style.width = (hi - lo) + "%"; av.textContent = Math.round(a * 100) + " %";
            }
            reg({ keys: ["mod_" + i + "_enable", "mod_" + i + "_source", "mod_" + i + "_dest", "mod_" + i + "_amount"], paint: paint });
            row.appendChild(en); row.appendChild(src); row.appendChild(dst); row.appendChild(amt); sm.appendChild(row);
        })(i);
        var sr = section("In this patch", "every routing Surge holds", "scene-g");
        var rl = h("div", "routings");
        var routings = S.params.routings || [];
        if (!routings.length) rl.appendChild(h("div", "hint", "This patch has no modulation routings."));
        routings.forEach(function (m) {
            var r = h("div", "routing" + (m.muted ? " muted" : ""));
            var dp = S.byKey[m.dst];
            r.innerHTML = '<span class="src"></span><span class="arrow">\u2192</span><span class="dst"></span><span class="depth"></span>';
            r.querySelector(".src").textContent = m.srcn; r.querySelector(".dst").textContent = dp ? dp.n : m.dst; r.querySelector(".depth").textContent = (m.depth >= 0 ? "+" : "") + Math.round(m.depth * 100) + " %";
            rl.appendChild(r);
        });
        sr.appendChild(rl);
        var smac = section("Macros", "labels from the patch", "scene-g"); var row = h("div", "macros");
        for (var j = 0; j < 8; j++) { var m2 = S.params.macros && S.params.macros[j]; row.appendChild(reg(W.macro(j, m2 && m2.label)).el); }
        smac.appendChild(row);
        g.appendChild(sr); g.appendChild(smac); v.appendChild(g);
    };

    VIEWS.fx = function (v) {
        var order = [0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6, 7, 14, 15];   // fxslot_order: A1-4, B1-4, S1-4, G1-4
        var groups = [["A Insert", order.slice(0, 4), ""], ["B Insert", order.slice(4, 8), "scene-b"], ["Send", order.slice(8, 12), "scene-g"], ["Global", order.slice(12, 16), "scene-g"]];
        /* Four uniform cards per group -- the slot's name and its type -- and under
         * the row one wide strip per ACTIVE slot with its twelve controls in a
         * line. Cards stay a grid of the chain; the controls get the width
         * twelve knobs need. */
        groups.forEach(function (grp) {
            var wrap = h("div", "fxgroup " + grp[2]);
            var g = h("div", "grid four");
            var details = [];
            grp[1].forEach(function (slot) {
                var typeKey = "fx" + slot + "_type";
                var shortName = (META.fxslot_shortnames && META.fxslot_shortnames[slot]) || ("FX " + slot), longName = (META.fxslot_names && META.fxslot_names[slot]) || "";
                var s = section(shortName, longName); s.classList.add("fxslot");
                var head = h("div", "head");
                var tw = reg(W.choice(typeKey, { label: "Type", maxSeg: 0 })); tw.el.querySelector(".label").remove(); head.appendChild(tw.el);
                s.appendChild(head);
                var d = section(shortName, ""); d.classList.add("fxdetail"); var dtag = d.querySelector(".tag") || h("span", "tag"); if (!dtag.parentNode) d.querySelector("h2").appendChild(dtag);
                var c = cells();
                for (var i = 0; i < 12; i++) knobs(c, ["fx" + slot + "_p" + i]);
                /* the send slots (fxslot_send1..4 = 4, 5, 12, 13) have a return level, a global parameter */
                var sendNo = { 4: 1, 5: 2, 12: 3, 13: 4 }[slot];
                if (sendNo) { var rl = {}; rl["g_volume_FX" + sendNo] = "Return"; knobs(c, ["g_volume_FX" + sendNo], { labels: rl }); }
                d.appendChild(c);
                reg({ key: typeKey, paint: function () { var off = (C.natural(typeKey) || 0) === 0; s.classList.toggle("on", !off); d.hidden = off || !c.children.length; dtag.textContent = off ? "" : longName + " \u00B7 " + C.text(typeKey); } });
                g.appendChild(s); details.push(d);
            });
            wrap.appendChild(g); details.forEach(function (d) { wrap.appendChild(d); });
            v.appendChild(wrap);
        });
    };

    VIEWS.global = function (v) {
        var g = h("div", "grid three");
        var s1 = section("Scenes", "", "scene-g"); var c1 = cells(); knobs(c1, ["g_scenemode", "g_scene_active", "g_splitkey", "g_volume", "g_polylimit", "g_character"], { optionLabels: { "0": "A", "1": "B" }, labels: { g_scene_active: "Active scene", g_polylimit: "Voices" } }); s1.appendChild(c1);
        var s2 = section("FX chain", "", "scene-g"); var c2 = cells(); knobs(c2, ["g_fx_bypass", "g_volume_FX1", "g_volume_FX2", "g_volume_FX3", "g_volume_FX4"], { labels: { g_volume_FX1: "Send 1 return", g_volume_FX2: "Send 2 return", g_volume_FX3: "Send 3 return", g_volume_FX4: "Send 4 return" } }); s2.appendChild(c2);
        var s3 = section("MIDI & tempo", "", "scene-g"); var c3 = cells();
        c3.appendChild(moduleChoice("octave_transpose", "Octave", ["-3", "-2", "-1", "0", "+1", "+2", "+3"], -3).el);
        c3.appendChild(moduleToggle("mpe_enabled", "MPE").el); c3.appendChild(moduleNumber("mpe_pitch_bend_range", "MPE bend range", 1, 96).el);
        c3.appendChild(moduleToggle("sync_bpm", "Auto BPM").el); c3.appendChild(moduleNumber("bpm", "BPM", 20, 300).el);
        s3.appendChild(c3);
        g.appendChild(s1); g.appendChild(s2); g.appendChild(s3); v.appendChild(g);
        var s4 = section("Files", "what the module wrote for this page", "scene-g quiet"); var c4 = h("div", "row");
        var rb = h("button", "btn", "Re-read everything"); rb.type = "button"; rb.addEventListener("click", C.refreshAll); c4.appendChild(rb);
        var info = h("div", "hint"); info.textContent = "patch rev " + (S.params ? S.params.rev : "-") + " \u00B7 " + (S.params ? S.params.params.length : 0) + " parameters \u00B7 presets " + (S.presets ? S.presets.count : "-"); c4.appendChild(info);
        s4.appendChild(c4); v.appendChild(s4);
    };

    /* ---- module-level (non-Surge) controls -------------------------------------------------- */
    function moduleChoice(key, label, options, base) {
        var sh = W.shell("choice-cell seg-cell", label), c = sh.el;
        var seg = h("div", "choice seg"); seg.setAttribute("role", "radiogroup");
        var btns = options.map(function (o, i) { var b = h("button", "", o); b.type = "button"; b.setAttribute("role", "radio"); b.addEventListener("click", function () { C.setModule(key, base + i); paint(); }); seg.appendChild(b); return b; });
        sh.ctl.appendChild(seg);
        function paint() { var v = parseInt(S.vals[key], 10); btns.forEach(function (b, i) { b.setAttribute("aria-checked", String(base + i === v)); }); }
        return reg({ el: c, key: key, paint: paint });
    }
    function moduleToggle(key, label) {
        var sh = W.shell("toggle-cell", label), c = sh.el; var b = h("button", "toggle"); b.type = "button"; b.setAttribute("role", "switch"); b.innerHTML = '<span class="knb"></span>'; b.setAttribute("aria-label", label);
        b.addEventListener("click", function () { var on = parseInt(S.vals[key], 10) === 1; C.setModule(key, on ? 0 : 1); paint(); });
        sh.ctl.appendChild(b);
        function paint() { var on = parseInt(S.vals[key], 10) === 1; b.setAttribute("aria-checked", String(on)); sh.val.textContent = S.vals[key] === undefined ? "--" : (on ? "On" : "Off"); }
        return reg({ el: c, key: key, paint: paint });
    }
    function moduleNumber(key, label, min, max) {
        var sh = W.shell("knob-cell", label), c = sh.el;
        var kn = h("div", "knob"); kn.tabIndex = 0; kn.setAttribute("role", "slider"); kn.setAttribute("aria-label", label);
        kn.innerHTML = '<svg viewBox="0 0 64 64"><circle class="well" cx="32" cy="32" r="30"/><path class="track" d="M12.9 51.1 A26 26 0 1 1 51.1 51.1"/><path class="arc" d=""/><circle class="cap" cx="32" cy="32" r="19"/><line class="ptr" x1="32" y1="30" x2="32" y2="16"/></svg>';
        var val = sh.val; sh.ctl.appendChild(kn);
        function cur() { var v = parseFloat(S.vals[key]); return isNaN(v) ? min : v; }
        function paint() { var n = (cur() - min) / (max - min); kn.querySelector(".ptr").style.transform = "rotate(" + (-135 + 270 * n) + "deg)"; var a = -135 + 270 * n, p0 = [32 + 26 * Math.cos((-135 - 90) * Math.PI / 180), 32 + 26 * Math.sin((-135 - 90) * Math.PI / 180)], p1 = [32 + 26 * Math.cos((a - 90) * Math.PI / 180), 32 + 26 * Math.sin((a - 90) * Math.PI / 180)]; kn.querySelector(".arc").setAttribute("d", "M" + p0[0] + " " + p0[1] + " A26 26 0 " + (a + 135 > 180 ? 1 : 0) + " 1 " + p1[0] + " " + p1[1]); val.textContent = Math.round(cur()); }
        W.bindDrag(kn, { get: function () { return (cur() - min) / (max - min); }, set: function (n) { C.setModule(key, Math.round(min + n * (max - min))); paint(); }, pixels: 220, label: function () { return label; }, text: function () { return String(Math.round(cur())); } });
        return reg({ el: c, key: key, paint: paint });
    }

    /* ---- events ------------------------------------------------------------------------------- */
    function start(rootEl) {
        buildShell(rootEl);
        render();
        C.on(function (ev, detail) {
            if (ev === "params") { render(); }
            else if (ev === "presets") { if (V.tab === "presets") render(); }
            else if (ev === "waves" || ev === "filters") { V.widgets.forEach(function (w) { if (w.any) w.paint(); }); }
            else if (ev === "values") { repaintKeys(detail); }
            else if (ev === "value") { repaintKeys([detail]); }
            else if (ev === "preset") { refreshHeader(); V.widgets.forEach(function (w) { if (w.any) w.paint(); }); }
            else if (ev === "connected") { refreshHeader(); }
        });
        if (C.remote && C.remote.getChainParams) C.remote.getChainParams().then(function (cp) { if (Array.isArray(cp)) C.chain = cp; });
    }

    root.SurgeViews = { start: start, render: render, V: V };
})(window);
