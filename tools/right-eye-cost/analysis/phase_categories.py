"""GPU ms per frame by category, per window phase and family (L and R), from pass-marker captures of an in-run A/B
(phase = floor(qpc s / N) % (groups + 1); the first --guard seconds of each window left out).
  python phase_categories.py <run> [--window 10 --groups 1 --names share,refresh --guard 2] [--top 14]"""
import argparse, glob, os, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm, categories_lib as cl

ap = argparse.ArgumentParser(); ap.add_argument("run"); ap.add_argument("--window", type=int, default=10)
ap.add_argument("--groups", type=int, default=1); ap.add_argument("--names", default="share,refresh")
ap.add_argument("--guard", type=float, default=2); ap.add_argument("--top", type=int, default=14)
a = ap.parse_args()
d = os.path.join(a.run, "passmarkers")
names = a.names.split(",")
def phase_of(q, f):
    sec = q / f; w = int(sec // a.window)
    return None if sec - w * a.window < a.guard else int(w % (a.groups + 1))
raw = defaultdict(float); frames = defaultdict(int); shape_w = defaultdict(float)
for p in glob.glob(os.path.join(d, "capture-*.bin")):
    r = pm.analyze(p, phase_of)
    if not r or "error" in r:
        continue
    for ph, x in r["phases"].items():
        frames[ph] += x["frames"]
        for (fam, sh, pso), v in x["gpu"].items():
            raw[(ph, fam, sh, pso)] += v; shape_w[sh] += v
cl.set_sizes(shape_w)
stages = cl.load_names(d)
cat = defaultdict(float)
for (ph, fam, sh, pso), v in raw.items():
    cat[(ph, fam, cl.categorize(cl.name_of(stages, pso), sh))] += v / max(1, frames[ph])
phases = sorted(frames)
print(f"{os.path.basename(a.run)}: frames " + ", ".join(f"{names[p] if p < len(names) else p} {frames[p]}" for p in phases))
for fam in ("L", "R", "T"):
    cats = sorted({c for (p, f, c) in cat if f == fam}, key=lambda c: -max(cat[(p, fam, c)] for p in phases))
    if not cats:
        continue
    print(f"\nfamily {fam} | " + " | ".join(names[p] if p < len(names) else str(p) for p in phases))
    for c in cats[: a.top]:
        print(f"   {c[:55]:<55} " + " ".join(f"{cat[(p, fam, c)]:>7.3f}" for p in phases))
    print(f"   {'total':<55} " + " ".join(f"{sum(v for (pp, f, _), v in cat.items() if pp == p and f == fam):>7.3f}" for p in phases))
