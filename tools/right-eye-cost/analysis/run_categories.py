"""GPU ms per frame by category and family (L, R, T), whole runs side by side, play phase only (pilot's play..leave),
from pass-marker captures. For runs that are one mode each (no in-run windows).
  python run_categories.py <label=run dir> [<label=run dir> ...] [--top 14]"""
import argparse, glob, json, os, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm, categories_lib as cl

ap = argparse.ArgumentParser(); ap.add_argument("runs", nargs="+"); ap.add_argument("--top", type=int, default=14)
a = ap.parse_args()
raw = defaultdict(float); frames = defaultdict(int); shape_w = defaultdict(float); stages = {}
labels = []
for spec in a.runs:
    label, run = spec.split("=", 1); labels.append(label)
    ph = {p["name"]: p["qpc_ns"] for p in json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))["phases"]}
    t0, t1 = ph["play"], ph.get("leave") or ph.get("exit") or 1 << 62
    phase_of = lambda q, f, t0=t0, t1=t1, label=label: label if t0 <= q / f * 1e9 < t1 else None
    d = os.path.join(run, "passmarkers")
    for p in glob.glob(os.path.join(d, "capture-*.bin")):
        r = pm.analyze(p, phase_of)
        if not r or "error" in r:
            continue
        for lab, x in r["phases"].items():
            frames[lab] += x["frames"]
            for (fam, sh, pso), v in x["gpu"].items():
                raw[(lab, fam, sh, pso)] += v; shape_w[sh] += v
    stages[label] = cl.load_names(d)
cl.set_sizes(shape_w)
cat = defaultdict(float)
for (lab, fam, sh, pso), v in raw.items():
    cat[(lab, fam, cl.categorize(cl.name_of(stages[lab], pso), sh))] += v / max(1, frames[lab])
print("frames in play: " + ", ".join(f"{l} {frames[l]}" for l in labels))
for fam in ("L", "R", "T"):
    cats = sorted({c for (l, f, c) in cat if f == fam}, key=lambda c: -max(cat[(l, fam, c)] for l in labels))
    if not cats:
        continue
    print(f"\nfamily {fam} | " + " | ".join(labels))
    for c in cats[: a.top]:
        print(f"   {c[:55]:<55} " + " ".join(f"{cat[(l, fam, c)]:>7.3f}" for l in labels))
    print(f"   {'total':<55} " + " ".join(f"{sum(v for (ll, f, _), v in cat.items() if ll == l and f == fam):>7.3f}" for l in labels))
