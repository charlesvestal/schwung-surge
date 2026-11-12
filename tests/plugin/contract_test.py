#!/usr/bin/env python3
"""The plugin's contract with the chain host, checked against a real dsp.so.

Runs tests/plugin/plugin_ops (every plugin call from one SCHED_FIFO 70 thread,
as the host's SPI callback makes them) and checks:

  state-size     a fresh preset's state, an edited one, and one with every
                 parameter moved: under 16 KB, or refused (-1), never cut off
  retype-restore a state whose oscillator and FX types differ from the preset's
                 restores every value -- including one that equals the PRESET's
                 value but not the new type's default -- through a fresh
                 process, compact and pretty-printed as the host writes it
  instances      two live instances write separate remote/<rid>/ files
  threads        the file worker runs SCHED_OTHER on cores 0-2
  legacy-state   a state saved by the current release (every Scene A key)
                 still restores

    scripts/test_plugin.sh            (builds in Docker and runs this)
    contract_test.py --ops plugin_ops --so dsp.so --data surge-data [--ref other_dsp.so]

--ref runs retype-restore against another build too and reports, without
failing, what it does -- the reviewed build lost FX values that way.
--legacy names a build of the current release for legacy-state.
"""
import argparse, json, os, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("--ops", required=True)
ap.add_argument("--so", required=True)
ap.add_argument("--data", required=True, help="surge-data directory")
ap.add_argument("--work", default="/tmp/surge-contract")
ap.add_argument("--ref", help="another dsp.so to run retype-restore against, for comparison")
ap.add_argument("--legacy", help="a dsp.so of the current release, for legacy-state")
A = ap.parse_args()

FAILS = []
def check(name, ok, detail=""):
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))
    if not ok: FAILS.append(name)

def moddir(tag):
    d = os.path.join(A.work, tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    os.symlink(os.path.abspath(A.data), os.path.join(d, "surge-data"))
    return d

def run(so, md, *ops):
    p = subprocess.run([A.ops, so, md] + list(ops), capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout, p.stderr)
        raise SystemExit("plugin_ops failed (%d)" % p.returncode)
    return p.stdout.splitlines(), p.stderr

def w(name): return os.path.join(A.work, name)

def state_sizes(lines): return {l.split()[1]: int(l.split()[2]) for l in lines if l.startswith("STATE ")}

def tsv(path):
    out = {}
    for line in open(path):
        k, _, v = line.rstrip("\n").partition("\t")
        out[k] = v
    return out

def same(a, b):
    if a == b: return True
    try:
        x, y = float(a), float(b)
        return abs(x - y) <= 1e-4 * max(1.0, abs(x), abs(y))
    except ValueError:
        return False

def diff_values(a, b, keys):
    return [(k, a.get(k), b.get(k)) for k in keys if not same(a.get(k), b.get(k))]

os.makedirs(A.work, exist_ok=True)

# ---- discovery: the preset, its types, and the parameter table ----------------
PRESET = 200
md = moddir("discover")
run(A.so, md, "create:A", "set:preset=%d" % PRESET, "render:300", "sleep:1200", "state:" + w("s_fresh.json"))
fresh = json.load(open(w("s_fresh.json")))
pp = json.load(open(os.path.join(md, "remote", str(fresh["rid"]), "patch_params.json")))
P = {p["k"]: p for p in pp["params"]}
keys = [p["k"] for p in pp["params"]] + ["macro%d" % i for i in range(1, 9)] + \
       ["mod_%d_%s" % (i, f) for i in range(6) for f in ("enable", "source", "dest", "amount")] + ["preset", "preset_name"]
open(w("keys.txt"), "w").write("\n".join(keys) + "\n")

def current_option(k):
    p = P[k]
    return p["o"][round(p["pv"] * (p["max"] - p["min"]))]

fx_now, osc_now = current_option("fx0_type"), current_option("osc1_type")
FX = next(t for t in ("Reverb 2", "Delay", "Chorus") if t != fx_now and t in P["fx0_type"]["o"])
OSC = next(t for t in ("FM2", "Wavetable", "Sine") if t != osc_now and t in P["osc1_type"]["o"])
print("preset %d %r: fx0 %s -> %s, osc1 %s -> %s" % (PRESET, pp["preset_name"], fx_now, FX, osc_now, OSC))

# the retyped units' ranges, from a run that makes the same switches
md = moddir("discover2")
run(A.so, md, "create:A", "set:preset=%d" % PRESET, "render:300", "set:fx0_type=" + FX, "set:osc1_type=" + OSC,
    "render:300", "sleep:1200")
P2 = {p["k"]: p for p in json.load(open(os.path.join(md, "remote", "0", "patch_params.json")))["params"]}

def natural(p, n):
    """the value set_param takes for normalised n"""
    if p["vt"] == 2: return "%.6f" % (p["min"] + n * (p["max"] - p["min"]))
    i = round(n * (p["max"] - p["min"]))
    return p["o"][i] if p.get("o") and p["ct"] != 0 else str(int(p["min"]) + i)

edits, edge = [], None
for unit in ("fx0_p", "osc1_param"):
    n = 0
    for k in sorted((k for k in P2 if k.startswith(unit)), key=lambda k: int(k[len(unit):])):
        p = P2[k]
        if p["ct"] == 0 or p["vt"] != 2 or p["max"] <= p["min"]: continue
        def_n = (p["def"] - p["min"]) / (p["max"] - p["min"])
        if edge is None and unit == "fx0_p" and abs(P[k]["pv"] - def_n) > 0.05 and n >= 2:
            # equal to the PRESET's value, not the new type's default: saved only
            # because its unit was retyped
            edge = k
            edits.append("set:%s=%s" % (k, natural(p, P[k]["pv"])))
        else:
            edits.append("set:%s=%s" % (k, natural(p, 0.21 + 0.13 * n)))
        n += 1
edits += ["set:b_filter1_cutoff=" + natural(P["b_filter1_cutoff"], 0.33), "set:macro3=0.7",
          "set:mod_2_enable=On", "set:mod_2_source=lfo1", "set:mod_2_dest=filter1_cutoff", "set:mod_2_amount=0.4"]
print("edits: %d (edge case: %s)" % (len(edits), edge or "none found"))

# ---- state-size -------------------------------------------------------------------
check("state-size fresh preset", 0 < os.path.getsize(w("s_fresh.json")) < 16000 and fresh.get("sparse") == 1,
      "%d bytes" % os.path.getsize(w("s_fresh.json")))

def retype_restore(so, tag):
    md = moddir(tag + "-a")
    lines, _ = run(so, md, "create:A", "set:preset=%d" % PRESET, "render:300", "set:fx0_type=" + FX,
                   "set:osc1_type=" + OSC, "render:200", *edits, "render:300", "sleep:600",
                   "state:" + w(tag + "_S.json"), "getkeys:%s:%s" % (w("keys.txt"), w(tag + "_A.tsv")))
    size = state_sizes(lines)[w(tag + "_S.json")]
    S = json.load(open(w(tag + "_S.json")))
    json.dump(S, open(w(tag + "_S_pretty.json"), "w"), indent=2)   # what the host writes to the slot file
    out = {"size": size, "state": S}
    for form in ("S", "S_pretty"):
        md = moddir(tag + "-b")
        run(so, md, "create:B", "render:200", "restore:" + w("%s_%s.json" % (tag, form)), "render:300",
            "getkeys:%s:%s" % (w("keys.txt"), w("%s_%s_B.tsv" % (tag, form))), "state:" + w(tag + "_" + form + "_again.json"))
        out[form] = diff_values(tsv(w(tag + "_A.tsv")), tsv(w("%s_%s_B.tsv" % (tag, form))), keys)
    return out

r = retype_restore(A.so, "new")
check("state-size edited", 0 < r["size"] < 16000, "%d bytes, %d keys" % (r["size"], len(r["state"])))
check("retype-restore compact", not r["S"], "%d values differ %s" % (len(r["S"]), r["S"][:4]))
check("retype-restore pretty-printed", not r["S_pretty"], "%d values differ %s" % (len(r["S_pretty"]), r["S_pretty"][:4]))
if edge:
    check("retype-restore keeps a value equal to the preset's", edge in r["state"], edge)
again = json.load(open(w("new_S_again.json")))
check("restored state re-saves identically", {k: v for k, v in again.items() if k != "rid"} ==
      {k: v for k, v in r["state"].items() if k != "rid"})

if A.ref:
    rr = retype_restore(A.ref, "ref")
    print("INFO reference build: state %d bytes; after restore %d values differ: %s" %
          (rr["size"], len(rr["S"]), " ".join(k for k, _, _ in rr["S"])))

# every parameter that can move, moved -- floats and option lists, both scenes,
# every FX slot switched on: whatever comes back must be whole JSON under 16 KB,
# or a refusal; never a cut-off blob
md = moddir("overflow")
on = ["set:fx%d_type=%s" % (s, FX) for s in range(16)]
lines, _ = run(A.so, md, "create:A", "set:preset=%d" % PRESET, "render:200", *on, "render:200", "sleep:1500")
PO = {p["k"]: p for p in json.load(open(os.path.join(md, "remote", "0", "patch_params.json")))["params"]}
moves = []
for k, p in PO.items():
    if p["ct"] == 0 or p["max"] <= p["min"] or k.endswith("_type"): continue
    if p["vt"] == 2: moves.append("set:%s=%s" % (k, natural(p, 0.123 if abs(p["pv"] - 0.123) > 0.01 else 0.877)))
    elif p.get("o") and len(p["o"]) > 1:
        cur = round(p["pv"] * (p["max"] - p["min"]))
        moves.append("set:%s=%s" % (k, p["o"][1 if cur != 1 else 0]))
lines, _ = run(A.so, md, "create:A", "set:preset=%d" % PRESET, "render:200", *on, "render:200", *moves, "render:200",
               "state:" + w("s_over.json"), "state:" + w("s_cap.json") + ":200", "set:preset=%d" % (PRESET + 1),
               "render:200", "state:" + w("s_after.json"))
sz = state_sizes(lines)
n = sz[w("s_over.json")]
whole = n == -1 or (0 < n < 16000 and isinstance(json.load(open(w("s_over.json"))), dict))
check("state-size everything moved: whole and under 16 KB, or refused", whole,
      "%d params moved, 16 FX on: %s" % (len(moves), "refused" if n == -1 else "%d bytes" % n))
check("state-size small buffer is refused, not cut", sz[w("s_cap.json")] == -1)
check("state-size fits again after a preset load", 0 < sz[w("s_after.json")] < 16000, "%d bytes" % sz[w("s_after.json")])

# ---- instances + threads ----------------------------------------------------------------
md = moddir("two")
lines, _ = run(A.so, md, "create:A", "create:B", "use:B", "set:preset=300", "render:300", "sleep:1200",
               "use:A", "state:" + w("two_A.json"), "use:B", "state:" + w("two_B.json"), "threads",
               "destroy:A", "destroy:B")
sa, sb = json.load(open(w("two_A.json"))), json.load(open(w("two_B.json")))
fa = json.load(open(os.path.join(md, "remote", str(sa["rid"]), "patch_params.json")))
fb = json.load(open(os.path.join(md, "remote", str(sb["rid"]), "patch_params.json")))
check("instances get their own files", sa["rid"] != sb["rid"] and fa["preset_name"] == sa["preset_name"] and
      fb["preset_name"] == sb["preset_name"] and sa["preset_name"] != sb["preset_name"],
      "rid %d %r, rid %d %r" % (sa["rid"], fa["preset_name"], sb["rid"], fb["preset_name"]))
threads = [l for l in lines if l.startswith("THREAD ")]
workers = [l for l in threads if " surge-remote " in l]
caller_rt = any("policy=FIFO prio=70" in l for l in threads)
check("threads worker SCHED_OTHER on cores 0-2", len(workers) == 2 and all("policy=OTHER" in l and "cpus=0,1,2" in l for l in workers),
      "%s; caller at FIFO 70: %s" % ("; ".join(workers), caller_rt))
for l in lines:
    if l.startswith("DESTROY "): print("INFO", l)

# ---- legacy-state ----------------------------------------------------------------
if A.legacy:
    md = moddir("legacy")
    run(A.legacy, md, "create:A", "set:preset=%d" % PRESET, "render:200", "set:filter1_cutoff=" + natural(P["filter1_cutoff"], 0.4),
        "set:env1_attack=" + natural(P["env1_attack"], 0.3), "set:osc2_pitch=" + natural(P["osc2_pitch"], 0.6), "render:100",
        "state:" + w("legacy_S.json"), "getkeys:%s:%s" % (w("keys.txt"), w("legacy_A.tsv")))
    md = moddir("legacy-b")
    run(A.so, md, "create:B", "render:200", "restore:" + w("legacy_S.json"), "render:200",
        "getkeys:%s:%s" % (w("keys.txt"), w("legacy_B.tsv")))
    sceneA = [k for k in keys if k in P and P[k]["sc"] == 1] + ["preset_name"]
    d = diff_values(tsv(w("legacy_A.tsv")), tsv(w("legacy_B.tsv")), sceneA)
    check("legacy-state restores", not d, "%d bytes, %d Scene A values differ %s" % (os.path.getsize(w("legacy_S.json")), len(d), d[:4]))

print("\n%d failed" % len(FAILS) if FAILS else "\nall passed")
sys.exit(1 if FAILS else 0)
