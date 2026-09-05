/* Surge XT Remote UI -- value formatting.
 *
 * A port of Parameter::get_display() from Surge's src/common/Parameter.cpp,
 * driven by the displayInfo the plugin exports for every parameter in
 * patch_params.json. Nothing here is a guess about units: each parameter says
 * how Surge itself turns its value into text -- LinearWithScale, ATwoToTheBx
 * (2^(b·v) with a scale, the envelopes, rates and frequencies) or Decibel --
 * and the few types Surge formats by hand (FM ratio, Chow ratio, the toggles
 * and every integer/boolean) come with Surge's own text, either as an options
 * list or as a 65-point table. Pure functions; tests/remote/format_test.mjs
 * loads this file in node.
 */
(function (root, factory) {
    var api = factory();
    if (typeof module === "object" && module.exports) module.exports = api;
    root.SurgeFormat = api;
})(typeof window !== "undefined" ? window : globalThis, function () {
    "use strict";

    var F = {
        kHasCustomMinString: 1, kHasCustomMaxString: 2, kHasCustomDefaultString: 4,
        kHasCustomMinValue: 8, kHasCustomMaxValue: 16, kUnitsAreSemitonesOrKeys: 32,
        kScaleBasedOnIsBiPolar: 64, kSwitchesFromSecToMillisec: 512,
    };
    var DT = { Custom: 0, LinearWithScale: 1, ATwoToTheBx: 2, Decibel: 3, DelegatedToFormatter: 4 };

    /* normalised 0..1 -> Surge's natural value, and back */
    function natural(p, n) {
        if (p.vt !== 2) return Math.round(p.min + n * (p.max - p.min));
        return p.min + n * (p.max - p.min);
    }
    function normalised(p, v) {
        var r = p.max - p.min;
        if (!(r > 0)) return 0;
        var n = (v - p.min) / r;
        return n < 0 ? 0 : n > 1 ? 1 : n;
    }

    /* Parameter::get_extended -- the value with the range extension applied,
     * keyed by control TYPE NAME so the table reads like Surge's switch. */
    var EXT = {
        ct_freq_fm2_offset: function (f) { return 100 * f; }, ct_freq_shift: function (f) { return 100 * f; },
        ct_pitch_semi7bp: function (f) { return 12 * f; }, ct_pitch_semi7bp_absolutable: function (f) { return 12 * f; },
        ct_decibel_extendable: function (f) { return 3 * f; }, ct_bonsai_bass_boost: function (f) { return 3 * f; },
        ct_detuning: function (f) { return 6 * f; },
        ct_decibel_narrow_extendable: function (f) { return 5 * f; },
        ct_decibel_narrow_short_extendable: function (f) { return 2 * f; },
        ct_oscspread: function (f) { return 12 * f; }, ct_oscspread_bipolar: function (f) { return 12 * f; },
        ct_osc_feedback: function (f) { return 8 * f - 4 * f; },
        ct_filter_feedback: function (f) { return 4 * f; }, ct_osc_feedback_negative: function (f) { return 4 * f; },
        ct_lfophaseshuffle: function (f) { return 2 * f - 1; },
        ct_fmratio: function (f) { return f > 16 ? ((f - 16) * 31 / 16 + 1) : -((16 - f) * 31 / 16 + 1); },
        ct_dly_fb_clippingmodes: function (f) { return 2 * f - 1; },
        ct_percent_with_extend_to_bipolar: function (f) { return 2 * f - 1; },
        ct_percent_with_extend_to_bipolar_static_default: function (f) { return 2 * f - 1; },
    };
    function extended(p, ctn, f) { if (!p.xr) return f; var fn = EXT[ctn]; return fn ? fn(f) : f; }

    function fmtFixed(v, dec) { return (Math.abs(v) < 5e-7 ? 0 : v).toFixed(dec); }
    /* Surge's amp_to_db is 18·log2(a) -- its amplitude scale is 2^(dB/18),
     * not the 20·log10 an engineer would guess; 25 % reads -36.00 dB there. */
    function ampToDb(a) { return 18 * Math.log2(a); }
    /* Surge formats in float; keep the arithmetic in float too, or the second
     * decimal of a cutoff readout disagrees with the plugin's own text. */
    var fr = Math.fround;

    /* Parameter::tempoSyncNotationValue */
    function tempoSyncNotation(f) {
        var a = Math.trunc(f), b = f - a;
        if (b >= 0) { b -= 1; a += 1; }
        var q, nn, t;
        if (f >= 1) {
            q = Math.pow(2, f - 1); nn = "whole";
            if (q >= 3) {
                if (Math.abs(q - Math.floor(q + 0.01)) < 0.01) return Math.floor(q + 0.01) + " whole notes";
                return Math.floor(q * 3 / 2 + 0.02) + " whole triplets";
            } else if (q >= 2) { nn = "double whole"; q /= 2; }
            if (q < 1.3) t = "note";
            else if (q < 1.4) {
                t = "triplet";
                if (nn === "whole") nn = "double whole";
                else { q = Math.pow(2, f - 1); return Math.floor(q * 3 / 2 + 0.02) + " whole triplets"; }
            } else t = "dotted";
        } else {
            var d = Math.pow(2, -(a - 2));
            q = Math.pow(2, b + 1);
            if (q < 1.3) t = "note";
            else if (q < 1.4) { t = "triplet"; d = d / 2; }
            else t = "dotted";
            nn = d === 1 ? "whole" : "1/" + Math.trunc(d);
        }
        return nn + " " + t;
    }

    /* The display text for parameter `p` (a patch_params.json entry) at
     * normalised value n, with Surge's control-type name `ctn`. */
    function display(p, n, ctn) {
        if (n === null || n === undefined || isNaN(n)) return "--";
        if (p.vt !== 2) {
            var i = natural(p, n);
            if (p.o && p.o.length) { var idx = i - p.min; return p.o[idx] !== undefined ? p.o[idx] : String(i); }
            return String(i);
        }
        if (p.tbl && p.tbl.length === 65) {
            /* Surge's own text, sampled at 65 points: nearest sample */
            return p.tbl[Math.round(Math.max(0, Math.min(1, n)) * 64)];
        }
        var f = p.min + n * (p.max - p.min);
        var d = p.d || {};
        var u = d.u || "";
        function semitones() { if ((d.f & F.kUnitsAreSemitonesOrKeys) && !p.abs) u = "semitones"; }

        switch (p.dt) {
            case DT.LinearWithScale:
            case DT.DelegatedToFormatter: {
                semitones();
                if (d.f & F.kScaleBasedOnIsBiPolar) { if (!p.bip) f = (f + 1) * 0.5; }
                if (p.ext) f = extended(p, ctn, f);
                if (p.ab && p.abs) { f = d.absF * f; u = d.au || u; }
                var txt = fmtFixed(d.sc * f, d.dec) + (u ? " " + u : "");
                if (f >= p.max && (d.f & F.kHasCustomMaxString)) txt = d.maxL;
                if (f <= p.min && (d.f & F.kHasCustomMinString)) txt = d.minL;
                if (f === p.def && (d.f & F.kHasCustomDefaultString)) txt = d.defL;
                return txt;
            }
            case DT.ATwoToTheBx: {
                if (p.ts && p.tsn) return tempoSyncNotation(d.tsm * f);
                if (p.ext && p.xr) f = extended(p, ctn, f);
                semitones();
                var dval = fr(fr(d.a) * fr(Math.pow(2, fr(fr(f) * fr(d.b))))), dec = d.dec;
                if (d.f & F.kSwitchesFromSecToMillisec) { if (dval < 1) { dval = fr(dval * 1000); u = "ms"; dec = 1; } }
                if (f >= p.max) {
                    if (d.f & F.kHasCustomMaxString) return d.maxL;
                    if (d.f & F.kHasCustomMaxValue) dval = d.maxV;
                }
                if (f <= p.min) {
                    if (d.f & F.kHasCustomMinString) return d.minL;
                    if (d.f & F.kHasCustomMinValue) dval = d.minV;
                }
                return fmtFixed(dval, dec) + (u ? " " + u : "");
            }
            case DT.Decibel:
                return f === 0 ? "-inf dB" : fmtFixed(ampToDb(f), 2) + " dB";
            default: {
                /* Custom float types without a table: FM ratio and Chow ratio,
                 * ported from Surge; anything else shows its natural value. */
                if (ctn === "ct_fmratio") {
                    if (p.abs) { var bpv = (f - 16) / 16, note = 69 + 69 * bpv; return fmtFixed(440 * Math.pow(2, (note - 69) / 12), 2) + " Hz"; }
                    var q = extended(p, ctn, f);
                    if (p.xr && q < 0) return "C : 1 / " + fmtFixed(-q, 2);
                    return "C : " + fmtFixed(q, 2);
                }
                if (ctn === "ct_chow_ratio") return "1 : " + fmtFixed(f, 2);
                return fmtFixed(f, 2) + (u ? " " + u : "");
            }
        }
    }

    /* Where a parameter's zero sits, for drawing a bipolar arc: normalised. */
    function zeroNormalised(p) {
        if (!p.bip) return 0;
        if (p.min < 0 && p.max > 0) return normalised(p, 0);
        return 0.5;
    }

    /* Note names for parameters that support them (ct_midikey etc. via options)
     * and for pitch display when asked. */
    var NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    function noteName(n) { n = Math.round(n); return NOTE[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1); }

    return { natural: natural, normalised: normalised, display: display, zeroNormalised: zeroNormalised, noteName: noteName, tempoSyncNotation: tempoSyncNotation, DT: DT, F: F };
});
