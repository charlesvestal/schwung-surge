/* Surge XT Remote UI -- widgets.
 *
 * Touch first. Every control is a Pointer Events surface with touch-action
 * none and pointer capture, tracked PER POINTER so two fingers can work two
 * controls (or both axes of a pad) at once. While a finger is down the value
 * is shown in a bubble above it, because the finger hides the control. Drags
 * are relative -- a knob never jumps to where you touched -- with a fine mode
 * (the Fine switch, or a second finger held anywhere on the panel), double-tap
 * resets to Surge's default, and a long press opens the typed-value sheet.
 * Everything is also keyboard-operable.
 */
(function (root) {
    "use strict";
    var C = root.SurgeCore, FMT = root.SurgeFormat, S = C.S;
    var W = {};

    /* ---- shared: the value bubble and the fine switch --------------------------- */
    var bubble = null;
    function showBubble(x, y, label, text) {
        if (!bubble) { bubble = document.createElement("div"); bubble.className = "bubble"; bubble.setAttribute("aria-hidden", "true"); document.body.appendChild(bubble); }
        bubble.innerHTML = "<b></b><span></span>";
        bubble.firstChild.textContent = label; bubble.lastChild.textContent = text;
        bubble.style.left = Math.max(8, Math.min(window.innerWidth - 8, x)) + "px";
        bubble.style.top = Math.max(8, y - 64) + "px";
        bubble.classList.add("on");
    }
    function moveBubble(x, y, text) { if (!bubble) return; bubble.lastChild.textContent = text; bubble.style.left = Math.max(8, Math.min(window.innerWidth - 8, x)) + "px"; bubble.style.top = Math.max(8, y - 64) + "px"; }
    function hideBubble() { if (bubble) bubble.classList.remove("on"); }
    /* The readout under a knob is 96 px wide: "semitones" does not fit beside a
     * number, so the small readouts abbreviate the unit words. The bubble and
     * the typed-value sheet keep Surge's full text. */
    function vtext(key) { return C.text(key).replace(/ \([^)]*\)$/, "").replace(/ semitones$/, " st").replace(/ octaves$/, " oct").replace(/ cents$/, " ct"); }
    var FINE = { on: false };
    var pointersDown = 0;
    document.addEventListener("pointerdown", function () { pointersDown++; }, true);
    document.addEventListener("pointerup", function () { pointersDown = Math.max(0, pointersDown - 1); }, true);
    document.addEventListener("pointercancel", function () { pointersDown = Math.max(0, pointersDown - 1); }, true);
    function fineNow(e) { return FINE.on || (e && e.shiftKey) || pointersDown > 1; }

    /* ---- the cell: label / control box / value, so a row of mixed controls lines up --
     * The label is always two lines tall, the control box always the same height,
     * the value always one line: knobs, switches and menus in one row share a
     * baseline whatever their labels wrap to. */
    function shell(cls, labelText) {
        var el = document.createElement("div"); el.className = "cell " + cls;
        var lab = document.createElement("div"); lab.className = "label"; lab.textContent = labelText || "";
        var ctl = document.createElement("div"); ctl.className = "ctl";
        var val = document.createElement("div"); val.className = "value";
        el.appendChild(lab); el.appendChild(ctl); el.appendChild(val);
        return { el: el, lab: lab, ctl: ctl, val: val };
    }

    /* ---- relative drag on any element ---------------------------------------------- */
    /* opts: get() -> normalised, set(n), label(), text(), pixels (full range),
     * onTap(), axis: "y" | "x" | "xy" (xy returns {dx,dy}) */
    function bindDrag(el, opts) {
        var active = {};   // pointerId -> {x0,y0,v0,moved,lp}
        el.style.touchAction = "none";
        el.addEventListener("pointerdown", function (e) {
            if (e.button !== 0 && e.pointerType === "mouse") return;
            el.setPointerCapture(e.pointerId);
            var st = { x0: e.clientX, y0: e.clientY, v0: opts.get(), moved: false, lp: null, last: null };
            active[e.pointerId] = st;
            st.lp = setTimeout(function () { if (!st.moved && opts.onLongPress) { st.longed = true; opts.onLongPress(); } }, 550);
            if (opts.label) showBubble(e.clientX, e.clientY, opts.label(), opts.text ? opts.text() : "");
            el.classList.add("dragging");
            e.preventDefault();
        });
        el.addEventListener("pointermove", function (e) {
            var st = active[e.pointerId]; if (!st || st.longed) return;
            var dx = e.clientX - st.x0, dy = e.clientY - st.y0;
            if (!st.moved && Math.abs(dx) + Math.abs(dy) < 3) return;
            if (!st.moved) { st.moved = true; clearTimeout(st.lp); }
            var k = fineNow(e) ? 0.2 : 1;
            var px = opts.pixels || 220;
            var n;
            if (opts.axis === "xy") { opts.setXY(st, dx * k, dy * k); }
            else {
                var d = opts.axis === "x" ? dx : -dy;
                if (opts.axis !== "x" && opts.axis !== "y") d = dx - dy;   // knobs: up or right increases
                n = st.v0 + d / px * k;
                n = n < 0 ? 0 : n > 1 ? 1 : n;
                opts.set(n);
            }
            if (opts.label) moveBubble(e.clientX, e.clientY, opts.text ? opts.text() : "");
        });
        function up(e) {
            var st = active[e.pointerId]; if (!st) return;
            clearTimeout(st.lp);
            delete active[e.pointerId];
            try { el.releasePointerCapture(e.pointerId); } catch (x) {}
            if (!Object.keys(active).length) { el.classList.remove("dragging"); hideBubble(); }
            if (!st.moved && !st.longed) {
                var now = Date.now();
                if (el._lastTap && now - el._lastTap < 320) { el._lastTap = 0; if (opts.onDoubleTap) opts.onDoubleTap(); }
                else { el._lastTap = now; if (opts.onTap) opts.onTap(); }
            }
        }
        el.addEventListener("pointerup", up); el.addEventListener("pointercancel", up);
        el.addEventListener("wheel", function (e) {
            if (!opts.set) return;
            e.preventDefault();
            var step = (e.deltaMode === 1 ? 0.02 : 0.0025) * (fineNow(e) ? 0.2 : 1);
            var n = opts.get() - Math.sign(e.deltaY) * Math.min(1, Math.abs(e.deltaY) / 10 + 1) * step;
            opts.set(n < 0 ? 0 : n > 1 ? 1 : n);
        }, { passive: false });
        el.addEventListener("keydown", function (e) {
            if (!opts.set) return;
            var n = opts.get(), d = 0, big = opts.stepBig || 0.1, sm = opts.step || 0.01;
            switch (e.key) {
                case "ArrowUp": case "ArrowRight": d = e.shiftKey ? sm / 5 : sm; break;
                case "ArrowDown": case "ArrowLeft": d = -(e.shiftKey ? sm / 5 : sm); break;
                case "PageUp": d = big; break; case "PageDown": d = -big; break;
                case "Home": opts.set(0); e.preventDefault(); return;
                case "End": opts.set(1); e.preventDefault(); return;
                case "Backspace": case "Delete": if (opts.onDoubleTap) opts.onDoubleTap(); e.preventDefault(); return;
                case "Enter": if (opts.onLongPress) opts.onLongPress(); e.preventDefault(); return;
                default: return;
            }
            e.preventDefault(); n += d; opts.set(n < 0 ? 0 : n > 1 ? 1 : n);
        });
    }

    /* ---- typed value sheet --------------------------------------------------------- */
    function valueSheet(title, current, onCommit) {
        var back = document.createElement("div"); back.className = "sheet-back";
        var sh = document.createElement("div"); sh.className = "sheet small"; sh.setAttribute("role", "dialog"); sh.setAttribute("aria-label", title);
        sh.innerHTML = '<h3></h3><input type="text" inputmode="decimal" autocomplete="off"><div class="row"><button type="button" class="btn ghost">Cancel</button><button type="button" class="btn primary">Set</button></div>';
        sh.querySelector("h3").textContent = title;
        var inp = sh.querySelector("input"); inp.value = current;
        function close() { back.remove(); }
        sh.querySelector(".ghost").addEventListener("click", close);
        sh.querySelector(".primary").addEventListener("click", function () { onCommit(inp.value); close(); });
        inp.addEventListener("keydown", function (e) { if (e.key === "Enter") { onCommit(inp.value); close(); } if (e.key === "Escape") close(); });
        back.addEventListener("click", function (e) { if (e.target === back) close(); });
        back.appendChild(sh); document.body.appendChild(back);
        setTimeout(function () { inp.focus(); inp.select(); }, 30);
    }

    /* Parse typed text back to a normalised value: a number in Surge's natural
     * units, or for enumerations an option name. */
    function parseTyped(p, txt) {
        txt = String(txt).trim();
        if (p.o && p.o.length) {
            var idx = p.o.findIndex(function (o) { return o.toLowerCase() === txt.toLowerCase(); });
            if (idx < 0) idx = p.o.findIndex(function (o) { return o.toLowerCase().indexOf(txt.toLowerCase()) === 0; });
            if (idx >= 0) return FMT.normalised(p, p.min + idx);
        }
        var num = parseFloat(txt.replace(",", "."));
        if (isNaN(num)) return null;
        var d = p.d || {};
        if (p.dt === FMT.DT.ATwoToTheBx && d.a && d.b) {          // typed in display units (Hz, s): invert 2^
            if (/ms\b/i.test(txt)) num /= 1000;
            var f = Math.log2(num / d.a) / d.b; return FMT.normalised(p, f);
        }
        if (p.dt === FMT.DT.LinearWithScale && d.sc) return FMT.normalised(p, num / d.sc);
        if (p.dt === FMT.DT.Decibel) return FMT.normalised(p, Math.pow(10, num / 20));
        return FMT.normalised(p, num);
    }

    /* ---- knob ------------------------------------------------------------------------ */
    var SVGNS = "http://www.w3.org/2000/svg";
    function svg(tag, a) { var n = document.createElementNS(SVGNS, tag); for (var k in a) n.setAttribute(k, a[k]); return n; }
    function polar(cx, cy, r, deg) { var t = (deg - 90) * Math.PI / 180; return [cx + r * Math.cos(t), cy + r * Math.sin(t)]; }
    function arc(cx, cy, r, a0, a1) { if (a1 < a0) { var t = a0; a0 = a1; a1 = t; } var p0 = polar(cx, cy, r, a0), p1 = polar(cx, cy, r, a1); return "M" + p0[0].toFixed(2) + " " + p0[1].toFixed(2) + " A" + r + " " + r + " 0 " + (a1 - a0 > 180 ? 1 : 0) + " 1 " + p1[0].toFixed(2) + " " + p1[1].toFixed(2); }

    /* A knob bound to parameter `key`. opts.label overrides the name; opts.big. */
    function knob(key, opts) {
        opts = opts || {};
        var p = S.byKey[key];
        var sh = shell("knob-cell" + (opts.big ? " big" : "") + (opts.cls ? " " + opts.cls : ""), "");
        var el = sh.el, lab = sh.lab, valEl = sh.val;
        var k = document.createElement("div"); k.className = "knob"; k.tabIndex = 0; k.setAttribute("role", "slider");
        var sv = svg("svg", { viewBox: "0 0 64 64" });
        sv.innerHTML = '<circle class="well" cx="32" cy="32" r="30"/><path class="track" d="' + arc(32, 32, 26, -135, 135) + '"/><path class="arc" d=""/><path class="mod" d="" visibility="hidden"/><circle class="cap" cx="32" cy="32" r="19"/><line class="ptr" x1="32" y1="30" x2="32" y2="16"/>';
        k.appendChild(sv);
        var arcEl = sv.querySelector(".arc"), ptr = sv.querySelector(".ptr"), modEl = sv.querySelector(".mod");
        sh.ctl.appendChild(k);
        var cur = 0;
        function angle(n) { return -135 + 270 * n; }
        function bind(pp) {
            p = pp;
            lab.textContent = opts.label || (p ? p.s : key);
            k.setAttribute("aria-label", p ? p.n : key);
            if (p) { k.setAttribute("aria-valuemin", 0); k.setAttribute("aria-valuemax", 100); }
        }
        function paint() {
            var n = C.val(key);
            var has = n !== null && p;
            cur = has ? n : 0;
            var a = angle(cur), z = p ? angle(FMT.zeroNormalised(p)) : -135;
            ptr.style.transform = "rotate(" + a + "deg)";
            arcEl.setAttribute("d", arc(32, 32, 26, z, a)); arcEl.style.visibility = has && Math.abs(a - z) > 0.5 ? "" : "hidden";
            valEl.textContent = has ? vtext(key) : "--";
            k.setAttribute("aria-valuenow", Math.round(cur * 100)); k.setAttribute("aria-valuetext", has ? C.text(key) : "--");
            el.classList.toggle("pending", !!S.pending[key]);
            el.classList.toggle("inactive", !!(p && p.dv));
            /* modulation reach: the routings that target this parameter */
            var reach = modReach(key);
            if (reach > 0 && has) { modEl.setAttribute("d", arc(32, 32, 30, angle(Math.max(0, cur - reach)), angle(Math.min(1, cur + reach)))); modEl.setAttribute("visibility", "visible"); }
            else modEl.setAttribute("visibility", "hidden");
        }
        bindDrag(k, {
            get: function () { return cur; }, set: function (n) { C.setNorm(key, n); paint(); },
            pixels: opts.big ? 260 : 200,
            label: function () { return p ? p.n : key; }, text: function () { return C.text(key); },
            onDoubleTap: function () { if (p) { C.setNorm(key, FMT.normalised(p, p.def)); paint(); } },
            onLongPress: function () { if (!p) return; valueSheet(p.n, C.text(key), function (t) { var n = parseTyped(p, t); if (n !== null) { C.setNorm(key, n); paint(); } }); },
        });
        bind(p);
        paint();
        return { el: el, key: key, paint: paint, rebind: function () { bind(S.byKey[key]); paint(); } };
    }

    /* How far the patch's modulation can move a parameter, in normalised units. */
    function modReach(key) {
        var r = 0, j = S.params;
        if (!j || !j.routings) return 0;
        j.routings.forEach(function (m) { if (m.dst === key && !m.muted) r += Math.abs(m.depth); });
        return r;
    }

    /* ---- slider (vertical by default) ------------------------------------------------- */
    function slider(key, opts) {
        opts = opts || {};
        var p = S.byKey[key];
        var el = document.createElement("div"); el.className = "cell slider-cell" + (opts.horizontal ? " h" : "") + (opts.cls ? " " + opts.cls : "");
        var lab = document.createElement("div"); lab.className = "label"; lab.textContent = opts.label || (p ? p.s : key);
        var s = document.createElement("div"); s.className = "slider"; s.tabIndex = 0; s.setAttribute("role", "slider"); s.setAttribute("aria-label", p ? p.n : key);
        s.innerHTML = '<div class="rail"></div><div class="fill"></div><div class="mod"></div><div class="thumb"></div>';
        var fill = s.querySelector(".fill"), thumb = s.querySelector(".thumb"), mod = s.querySelector(".mod");
        var valEl = document.createElement("div"); valEl.className = "value";
        el.appendChild(lab); el.appendChild(s); el.appendChild(valEl);
        var cur = 0;
        function paint() {
            var n = C.val(key), has = n !== null;
            cur = has ? n : 0;
            var z = p ? FMT.zeroNormalised(p) : 0;
            var lo = Math.min(cur, z) * 100, hi = Math.max(cur, z) * 100;
            if (opts.horizontal) { thumb.style.left = (cur * 100) + "%"; fill.style.left = lo + "%"; fill.style.width = (hi - lo) + "%"; }
            else { thumb.style.bottom = (cur * 100) + "%"; fill.style.bottom = lo + "%"; fill.style.height = (hi - lo) + "%"; }
            var reach = modReach(key);
            if (reach > 0) { var a = Math.max(0, cur - reach) * 100, b = Math.min(1, cur + reach) * 100; mod.style.display = ""; if (opts.horizontal) { mod.style.left = a + "%"; mod.style.width = (b - a) + "%"; } else { mod.style.bottom = a + "%"; mod.style.height = (b - a) + "%"; } } else mod.style.display = "none";
            valEl.textContent = has ? vtext(key) : "--";
            s.setAttribute("aria-valuenow", Math.round(cur * 100)); s.setAttribute("aria-valuetext", has ? C.text(key) : "--");
            el.classList.toggle("pending", !!S.pending[key]);
        }
        bindDrag(s, {
            get: function () { return cur; }, set: function (n) { C.setNorm(key, n); paint(); },
            axis: opts.horizontal ? "x" : "y", pixels: opts.horizontal ? 260 : 180,
            label: function () { return p ? p.n : key; }, text: function () { return C.text(key); },
            onDoubleTap: function () { if (p) { C.setNorm(key, FMT.normalised(p, p.def)); paint(); } },
            onLongPress: function () { if (!p) return; valueSheet(p.n, C.text(key), function (t) { var n = parseTyped(p, t); if (n !== null) { C.setNorm(key, n); paint(); } }); },
        });
        paint();
        return { el: el, key: key, paint: paint, rebind: function () { p = S.byKey[key]; lab.textContent = opts.label || (p ? p.s : key); paint(); } };
    }

    /* ---- macro slider: not a Surge parameter, a 0..1 controller ------------------------ */
    function macro(i, labelText) {
        /* a wide horizontal fader: label, rail, value on one 44 px row -- eight of
         * them fill a panel edge to edge and nothing sits over the words */
        var key = "macro" + (i + 1);
        var el = document.createElement("div"); el.className = "macro-row";
        var lab = document.createElement("div"); lab.className = "mlabel"; lab.textContent = (labelText && labelText !== "-") ? labelText : ("Macro " + (i + 1));
        var s = document.createElement("div"); s.className = "slider macro"; s.tabIndex = 0; s.setAttribute("role", "slider"); s.setAttribute("aria-label", lab.textContent);
        s.innerHTML = '<div class="rail"></div><div class="fill"></div><div class="thumb"></div>';
        var valEl = document.createElement("div"); valEl.className = "value";
        el.appendChild(lab); el.appendChild(s); el.appendChild(valEl);
        var cur = 0;
        function paint() { var n = C.val(key); cur = n === null ? 0 : n; s.querySelector(".thumb").style.left = (cur * 100) + "%"; s.querySelector(".fill").style.width = (cur * 100) + "%"; valEl.textContent = Math.round(cur * 100) + " %"; s.setAttribute("aria-valuenow", Math.round(cur * 100)); }
        bindDrag(s, { get: function () { return cur; }, set: function (n) { C.setMacro(i, n); paint(); }, axis: "x", pixels: 300, label: function () { return lab.textContent; }, text: function () { return Math.round(cur * 100) + " %"; }, onDoubleTap: function () { C.setMacro(i, 0); paint(); } });
        paint();
        return { el: el, key: key, paint: paint, setLabel: function (t) { lab.textContent = (t && t !== "-") ? t : ("Macro " + (i + 1)); s.setAttribute("aria-label", lab.textContent); } };
    }

    /* ---- choice: segmented for a few options, a sheet for many ----------------------------- */
    function choice(key, opts) {
        opts = opts || {};
        var p = S.byKey[key];
        var sh = shell("choice-cell" + (opts.cls ? " " + opts.cls : ""), opts.label || (p ? p.s : key));
        var el = sh.el, lab = sh.lab;
        var body = document.createElement("div"); body.className = "choice"; sh.ctl.appendChild(body);
        function options() { return (p && p.o) ? p.o : []; }
        function current() { var n = C.val(key); return p && n !== null ? FMT.natural(p, n) - p.min : -1; }
        function build() {
            body.innerHTML = "";
            var o = options();
            /* a segmented strip only when there are few options AND their words are
             * short enough to sit side by side; chips wrap; everything else is a menu */
            var words = o.join("").length;
            var seg = o.length && o.length <= (opts.maxSeg || 5) && (opts.chips || words <= 28);
            el.classList.remove("seg-cell", "seg2-cell", "pick-cell", "chips-cell");
            el.classList.add(seg ? (opts.chips ? "chips-cell" : (o.length <= 2 ? "seg2-cell" : "seg-cell")) : "pick-cell");
            if (seg) {
                body.className = "choice " + (opts.chips ? "chips" : "seg"); body.setAttribute("role", "radiogroup");
                o.forEach(function (name, i) {
                    var b = document.createElement("button"); b.type = "button"; b.setAttribute("role", "radio");
                    b.textContent = (opts.optionLabels && opts.optionLabels[name]) || (opts.short ? shortOpt(name) : name);
                    b.title = name;
                    b.addEventListener("click", function () { C.setNorm(key, FMT.normalised(p, p.min + i)); paint(); });
                    body.appendChild(b);
                });
            } else {
                body.className = "choice pick";
                var b = document.createElement("button"); b.type = "button"; b.className = "picker";
                b.innerHTML = '<span class="txt"></span><span class="chev">\u2304</span>';
                b.addEventListener("click", function () { pickSheet(lab.textContent, o, current(), function (i) { C.setNorm(key, FMT.normalised(p, p.min + i)); paint(); }); });
                body.appendChild(b);
            }
        }
        function paint() {
            var cur = current();
            if (body.classList.contains("seg") || body.classList.contains("chips")) Array.prototype.forEach.call(body.children, function (b, i) { b.setAttribute("aria-checked", String(i === cur)); });
            else { var t = body.querySelector(".txt"); if (t) t.textContent = cur >= 0 ? options()[cur] : (p ? C.text(key) : "--"); }
            el.classList.toggle("pending", !!S.pending[key]);
        }
        build(); paint();
        return { el: el, key: key, paint: paint, rebind: function () { p = S.byKey[key]; lab.textContent = opts.label || (p ? p.s : key); build(); paint(); } };
    }
    function shortOpt(s) { return s.length > 7 ? s.replace(/[aeiou ]/gi, "").slice(0, 6) : s; }

    /* A full-height picker for long option lists, with search. onPick(index). */
    function pickSheet(title, options, current, onPick, groups) {
        var back = document.createElement("div"); back.className = "sheet-back";
        var sh = document.createElement("div"); sh.className = "sheet"; sh.setAttribute("role", "dialog"); sh.setAttribute("aria-label", title);
        sh.innerHTML = '<div class="sheet-head"><h3></h3><input type="search" placeholder="Search" aria-label="Search options"><button type="button" class="btn ghost close" aria-label="Close">\u2715</button></div><div class="sheet-list" role="listbox"></div>';
        sh.querySelector("h3").textContent = title;
        var list = sh.querySelector(".sheet-list"), q = sh.querySelector("input");
        function close() { back.remove(); }
        function render() {
            list.innerHTML = "";
            var needle = q.value.trim().toLowerCase(), lastGroup = null;
            options.forEach(function (name, i) {
                if (needle && name.toLowerCase().indexOf(needle) < 0) return;
                if (groups && groups[i] !== lastGroup) { lastGroup = groups[i]; var h = document.createElement("div"); h.className = "group"; h.textContent = lastGroup; list.appendChild(h); }
                var b = document.createElement("button"); b.type = "button"; b.setAttribute("role", "option"); b.setAttribute("aria-selected", String(i === current)); b.textContent = name;
                b.addEventListener("click", function () { onPick(i); close(); });
                list.appendChild(b);
            });
        }
        q.addEventListener("input", render);
        sh.querySelector(".close").addEventListener("click", close);
        back.addEventListener("click", function (e) { if (e.target === back) close(); });
        back.appendChild(sh); document.body.appendChild(back); render();
        var sel = list.querySelector('[aria-selected="true"]'); if (sel) sel.scrollIntoView({ block: "center" });
    }

    /* ---- switch (boolean) --------------------------------------------------------------- */
    function toggle(key, opts) {
        opts = opts || {};
        var p = S.byKey[key];
        var sh = shell("toggle-cell" + (opts.cls ? " " + opts.cls : ""), opts.label || (p ? p.s : key));
        var el = sh.el, lab = sh.lab;
        var b = document.createElement("button"); b.type = "button"; b.className = "toggle"; b.setAttribute("role", "switch"); b.setAttribute("aria-checked", "false");
        b.innerHTML = '<span class="knb"></span>'; b.setAttribute("aria-label", lab.textContent);
        sh.ctl.appendChild(b);
        function on() { var n = C.val(key); return n !== null && n > 0.5; }
        b.addEventListener("click", function () { C.setNorm(key, on() ? 0 : 1); paint(); });
        function paint() { b.setAttribute("aria-checked", String(on())); sh.val.textContent = C.val(key) === null ? "--" : (p && p.o ? C.text(key) : (on() ? "On" : "Off")); el.classList.toggle("pending", !!S.pending[key]); }
        paint();
        return { el: el, key: key, paint: paint, rebind: function () { p = S.byKey[key]; lab.textContent = opts.label || (p ? p.s : key); paint(); } };
    }

    /* Pick the widget a parameter wants. */
    function auto(key, opts) {
        var p = S.byKey[key];
        if (!p) return knob(key, opts);
        if (p.vt === 1 || (p.o && p.o.length === 2 && /^(off|on)$/i.test(p.o[0]))) return toggle(key, opts);
        if (p.vt !== 2 && p.o && p.o.length) {
            /* sixty-three voices or a hundred and twenty-seven keys are a knob's
             * job; a menu is for options that are words */
            if (p.o.length > 5 && p.o.every(function (o) { return /^-?\d+(\.\d+)?( .*)?$/.test(o); })) return knob(key, opts);
            return choice(key, opts);
        }
        return knob(key, opts);
    }

    /* ---- XY pad: two parameters on one surface, one or more nodes --------------------------- */
    /* nodes: [{x:key, y:key, label, cls}], draw(ctx, w, h) draws the backdrop */
    function xypad(nodes, opts) {
        opts = opts || {};
        var el = document.createElement("div"); el.className = "xypad" + (opts.cls ? " " + opts.cls : ""); el.style.touchAction = "none";
        var cv = document.createElement("canvas"); cv.className = "xy-canvas"; cv.setAttribute("aria-hidden", "true"); el.appendChild(cv);
        var dots = nodes.map(function (nd, i) {
            var d = document.createElement("div"); d.className = "xy-node " + (nd.cls || ""); d.tabIndex = 0; d.setAttribute("role", "slider"); d.setAttribute("aria-label", nd.label || ("node " + (i + 1)));
            d.innerHTML = '<span class="ring"></span><span class="tag"></span>'; d.querySelector(".tag").textContent = nd.label || "";
            el.appendChild(d); return d;
        });
        var active = {};   // pointerId -> {node, x0,y0, vx0, vy0}
        var INSET = 22;   // half a node: the ring stays whole at the edges
        function span() { return { w: Math.max(1, el.clientWidth - 2 * INSET), h: Math.max(1, el.clientHeight - 2 * INSET) }; }
        function place() {
            var sp = span();
            nodes.forEach(function (nd, i) {
                var x = C.val(nd.x), y = C.val(nd.y);
                var has = x !== null && y !== null;
                dots[i].style.display = has ? "" : "none";
                if (!has) return;
                dots[i].style.left = (INSET + x * sp.w) + "px"; dots[i].style.top = (INSET + (1 - y) * sp.h) + "px";
                dots[i].classList.toggle("low", (1 - y) * sp.h < 8);
                if (nd.off) dots[i].classList.toggle("off", (C.natural(nd.off) || 0) === 0);
                dots[i].setAttribute("aria-valuetext", C.text(nd.x) + " / " + C.text(nd.y));
            });
        }
        function nearest(x, y) {
            var best = -1, bd = 1e9, sp = span();
            nodes.forEach(function (nd, i) {
                var vx = C.val(nd.x), vy = C.val(nd.y); if (vx === null || vy === null) return;
                var dx = INSET + vx * sp.w - x, dy = INSET + (1 - vy) * sp.h - y, d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = i; }
            });
            return best;
        }
        el.addEventListener("pointerdown", function (e) {
            var r = el.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
            var i = nearest(x, y); if (i < 0) return;
            el.setPointerCapture(e.pointerId);
            active[e.pointerId] = { i: i, x0: e.clientX, y0: e.clientY, vx0: C.val(nodes[i].x), vy0: C.val(nodes[i].y) };
            dots[i].classList.add("dragging");
            showBubble(e.clientX, e.clientY, nodes[i].label || "", C.text(nodes[i].x) + "  \u00B7  " + C.text(nodes[i].y));
            e.preventDefault();
        });
        el.addEventListener("pointermove", function (e) {
            var st = active[e.pointerId]; if (!st) return;
            var k = fineNow(e) ? 0.2 : 1, sp = span(), nd = nodes[st.i];
            var nx = st.vx0 + (e.clientX - st.x0) / sp.w * k, ny = st.vy0 - (e.clientY - st.y0) / sp.h * k;
            C.setNorm(nd.x, nx < 0 ? 0 : nx > 1 ? 1 : nx); C.setNorm(nd.y, ny < 0 ? 0 : ny > 1 ? 1 : ny);
            place(); moveBubble(e.clientX, e.clientY, C.text(nd.x) + "  \u00B7  " + C.text(nd.y));
        });
        function up(e) { var st = active[e.pointerId]; if (!st) return; delete active[e.pointerId]; dots[st.i].classList.remove("dragging"); try { el.releasePointerCapture(e.pointerId); } catch (x) {} if (!Object.keys(active).length) hideBubble(); }
        el.addEventListener("pointerup", up); el.addEventListener("pointercancel", up);
        dots.forEach(function (d, i) {
            d.addEventListener("keydown", function (e) {
                var nd = nodes[i], sx = C.val(nd.x), sy = C.val(nd.y), st = e.shiftKey ? 0.002 : 0.01;
                if (e.key === "ArrowRight") C.setNorm(nd.x, Math.min(1, sx + st)); else if (e.key === "ArrowLeft") C.setNorm(nd.x, Math.max(0, sx - st));
                else if (e.key === "ArrowUp") C.setNorm(nd.y, Math.min(1, sy + st)); else if (e.key === "ArrowDown") C.setNorm(nd.y, Math.max(0, sy - st)); else return;
                e.preventDefault(); place();
            });
        });
        function draw() {
            var dpr = window.devicePixelRatio || 1, w = el.clientWidth, h = el.clientHeight;
            if (!w || !h) return;
            if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
            var g = cv.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, w, h);
            if (opts.draw) opts.draw(g, w, h);
            place();
        }
        if (typeof ResizeObserver === "function") new ResizeObserver(draw).observe(el);
        return { el: el, paint: draw, place: place };
    }

    /* ---- envelope: A D S R with draggable corners ------------------------------------------------ */
    function envelope(keys, opts) {
        /* keys: {a, d, s, r, ashape, dshape, rshape, mode} */
        opts = opts || {};
        var el = document.createElement("div"); el.className = "envgraph" + (opts.cls ? " " + opts.cls : ""); el.style.touchAction = "none";
        var cv = document.createElement("canvas"); cv.setAttribute("aria-hidden", "true"); el.appendChild(cv);
        var handles = ["a", "d", "r"].map(function (h) { var d = document.createElement("div"); d.className = "env-handle " + h; d.tabIndex = 0; d.setAttribute("role", "slider"); d.setAttribute("aria-label", { a: "Attack", d: "Decay and sustain", r: "Release" }[h]); el.appendChild(d); return d; });
        var PAD = 14;
        function geo() {
            var w = el.clientWidth - 2 * PAD, h = el.clientHeight - 2 * PAD;
            var a = C.val(keys.a) || 0, d = C.val(keys.d) || 0, s = C.val(keys.s) || 0, r = C.val(keys.r) || 0;
            var wa = 0.04 + a * 0.30, wd = 0.04 + d * 0.30, wh = 0.14, wr = 0.04 + r * 0.30;
            var tot = wa + wd + wh + wr;
            var x0 = PAD, x1 = x0 + wa / tot * w, x2 = x1 + wd / tot * w, x3 = x2 + wh / tot * w, x4 = x3 + wr / tot * w;
            return { w: w, h: h, x0: x0, x1: x1, x2: x2, x3: x3, x4: x4, yb: PAD + h, yt: PAD, ys: PAD + h - s * h, a: a, d: d, s: s, r: r };
        }
        function curve(g, x0, y0, x1, y1, shape) {
            /* Surge's shapes: 0 = fast/convex, 0.5 = linear, 1 = slow/concave (approximated as a quadratic bend) */
            var sh = shape === null ? 0.5 : shape;
            var cx = x0 + (x1 - x0) * (1 - sh), cy = y0 + (y1 - y0) * sh;
            g.quadraticCurveTo(cx, cy, x1, y1);
        }
        function draw() {
            var dpr = window.devicePixelRatio || 1, W = el.clientWidth, H = el.clientHeight;
            if (!W || !H) return;
            if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
            var g = cv.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
            var q = geo(), col = opts.color || getComputedStyle(el).getPropertyValue("--acc") || "#f80";
            g.strokeStyle = "rgba(128,128,128,0.18)"; g.lineWidth = 1; g.beginPath(); for (var i = 1; i < 4; i++) { var yy = Math.round(PAD + q.h * i / 4) + 0.5; g.moveTo(PAD, yy); g.lineTo(W - PAD, yy); } g.stroke();
            var ash = C.val(keys.ashape), dsh = C.val(keys.dshape), rsh = C.val(keys.rshape);
            g.beginPath(); g.moveTo(q.x0, q.yb); curve(g, q.x0, q.yb, q.x1, q.yt, ash); curve(g, q.x1, q.yt, q.x2, q.ys, dsh); g.lineTo(q.x3, q.ys); curve(g, q.x3, q.ys, q.x4, q.yb, rsh);
            g.lineTo(q.x4, q.yb); g.lineTo(q.x0, q.yb); g.closePath();
            var grad = g.createLinearGradient(0, PAD, 0, q.yb); grad.addColorStop(0, col.trim() + "55"); grad.addColorStop(1, col.trim() + "05"); g.fillStyle = grad; g.fill();
            g.beginPath(); g.moveTo(q.x0, q.yb); curve(g, q.x0, q.yb, q.x1, q.yt, ash); curve(g, q.x1, q.yt, q.x2, q.ys, dsh); g.lineTo(q.x3, q.ys); curve(g, q.x3, q.ys, q.x4, q.yb, rsh);
            g.strokeStyle = col.trim(); g.lineWidth = 2.2; g.lineJoin = "round"; g.stroke();
            g.setLineDash([3, 4]); g.strokeStyle = "rgba(128,128,128,0.45)"; g.lineWidth = 1; g.beginPath(); g.moveTo(q.x3 + 0.5, PAD); g.lineTo(q.x3 + 0.5, q.yb); g.stroke(); g.setLineDash([]);
            handles[0].style.left = q.x1 + "px"; handles[0].style.top = q.yt + "px";
            handles[1].style.left = q.x2 + "px"; handles[1].style.top = q.ys + "px";
            handles[2].style.left = q.x4 + "px"; handles[2].style.top = q.yb + "px";
            handles[0].setAttribute("aria-valuetext", C.text(keys.a)); handles[1].setAttribute("aria-valuetext", C.text(keys.d) + " / " + C.text(keys.s)); handles[2].setAttribute("aria-valuetext", C.text(keys.r));
        }
        var active = {};
        el.addEventListener("pointerdown", function (e) {
            var r = el.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top, q = geo();
            var cand = [[q.x1, q.yt, 0], [q.x2, q.ys, 1], [q.x4, q.yb, 2]], best = -1, bd = 1e9;
            cand.forEach(function (c) { var d = (c[0] - x) * (c[0] - x) + (c[1] - y) * (c[1] - y); if (d < bd) { bd = d; best = c[2]; } });
            if (best < 0) return;
            el.setPointerCapture(e.pointerId);
            active[e.pointerId] = { h: best, x0: e.clientX, y0: e.clientY, a0: q.a, d0: q.d, s0: q.s, r0: q.r };
            handles[best].classList.add("dragging");
            showBubble(e.clientX, e.clientY, ["Attack", "Decay \u00B7 Sustain", "Release"][best], best === 1 ? C.text(keys.d) + "  \u00B7  " + C.text(keys.s) : C.text(best === 0 ? keys.a : keys.r));
            e.preventDefault();
        });
        el.addEventListener("pointermove", function (e) {
            var st = active[e.pointerId]; if (!st) return;
            var k = fineNow(e) ? 0.2 : 1, w = el.clientWidth, h = el.clientHeight - 2 * PAD;
            var dx = (e.clientX - st.x0) / (w * 0.33) * k, dy = -(e.clientY - st.y0) / h * k;
            var clamp = function (v) { return v < 0 ? 0 : v > 1 ? 1 : v; };
            if (st.h === 0) C.setNorm(keys.a, clamp(st.a0 + dx));
            else if (st.h === 1) { C.setNorm(keys.d, clamp(st.d0 + dx)); C.setNorm(keys.s, clamp(st.s0 + dy)); }
            else C.setNorm(keys.r, clamp(st.r0 + dx));
            draw();
            moveBubble(e.clientX, e.clientY, st.h === 1 ? C.text(keys.d) + "  \u00B7  " + C.text(keys.s) : C.text(st.h === 0 ? keys.a : keys.r));
        });
        function up(e) { var st = active[e.pointerId]; if (!st) return; delete active[e.pointerId]; handles[st.h].classList.remove("dragging"); try { el.releasePointerCapture(e.pointerId); } catch (x) {} if (!Object.keys(active).length) hideBubble(); }
        el.addEventListener("pointerup", up); el.addEventListener("pointercancel", up);
        if (typeof ResizeObserver === "function") new ResizeObserver(draw).observe(el);
        return { el: el, paint: draw, keys: Object.keys(keys).map(function (k) { return keys[k]; }) };
    }

    /* ---- a generic canvas display that redraws itself ------------------------------------------ */
    function scope(draw, cls) {
        var el = document.createElement("div"); el.className = "scope" + (cls ? " " + cls : "");
        var cv = document.createElement("canvas"); cv.setAttribute("aria-hidden", "true"); el.appendChild(cv);
        function paint() {
            var dpr = window.devicePixelRatio || 1, w = el.clientWidth, h = el.clientHeight;
            if (!w || !h) return;
            if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
            var g = cv.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, w, h);
            draw(g, w, h, el);
        }
        if (typeof ResizeObserver === "function") new ResizeObserver(paint).observe(el);
        return { el: el, paint: paint };
    }

    W.knob = knob; W.slider = slider; W.macro = macro; W.choice = choice; W.toggle = toggle; W.auto = auto;
    W.xypad = xypad; W.envelope = envelope; W.scope = scope; W.pickSheet = pickSheet; W.valueSheet = valueSheet;
    W.FINE = FINE; W.bindDrag = bindDrag; W.hideBubble = hideBubble; W.shell = shell;
    root.SurgeWidgets = W;
})(window);
