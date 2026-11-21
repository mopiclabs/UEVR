"""Per-frame GPU from pass-marker captures, play phase only: for each run, per family (L, R, T) the distribution of the
named GDF passes, the unnamed compute and the family total per frame, and the same on the captured frames whose present
interval is a hitch (> 2x the captured frames' median). Shows whether GDF updates spike in either eye.
  python frame_gdf.py <label=run dir> [...]"""
import glob, json, os, sys
from collections import defaultdict
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm, categories_lib as cl

GDF = "GDF refresh (heightfield/page compose)"
UNN = "Compute, unnamed (PSO made before injection)"
for spec in sys.argv[1:]:
    label, run = spec.split("=", 1)
    ph = {p["name"]: p["qpc_ns"] for p in json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))["phases"]}
    t0, t1 = ph["play"], ph.get("leave") or ph.get("exit") or 1 << 62
    d = os.path.join(run, "passmarkers")
    stages = cl.load_names(d)
    frames = {}  # present q (ns) -> {"dur": ms, "gpu": {(fam, shape, pso): ms}}
    shape_w = defaultdict(float)
    for p in sorted(glob.glob(os.path.join(d, "capture-*.bin"))):
        r = pm.analyze(p, lambda q, f: int(q / f * 1e9) if t0 <= q / f * 1e9 < t1 else None)
        if not r or "error" in r:
            continue
        for q, x in r["phases"].items():
            frames[q] = {"dur": x["dur"] * 1000.0, "gpu": dict(x["gpu"])}
            for (fam, sh, pso), v in x["gpu"].items():
                shape_w[sh] += v
    cl.set_sizes(shape_w)
    rec = []
    for q, x in sorted(frames.items()):
        tot, gdf, unn = defaultdict(float), defaultdict(float), defaultdict(float)
        for (fam, sh, pso), v in x["gpu"].items():
            c = cl.categorize(cl.name_of(stages, pso), sh)
            tot[fam] += v
            if c == GDF:
                gdf[fam] += v
            elif c == UNN:
                unn[fam] += v
        rec.append((q, x["dur"], tot, gdf, unn))
    med = float(np.median([r[1] for r in rec]))
    hit = [r for r in rec if r[1] > 2 * med]
    print(f"\n{label}: captured play frames {len(rec)}, median interval {med:.1f} ms, hitches (>2x) {len(hit)}")
    for fam in ("L", "R", "T"):
        if not any(r[2].get(fam) for r in rec):
            continue
        for name, idx in (("total", 2), ("GDF named", 3), ("unnamed", 4)):
            v = np.array([r[idx].get(fam, 0.0) for r in rec]); h = np.array([r[idx].get(fam, 0.0) for r in hit])
            print(f"   {fam} {name:<10} mean {v.mean():6.3f}  p50 {np.median(v):6.3f}  p99 {np.percentile(v, 99):6.3f}  max {v.max():6.3f}"
                  f"  frames >1 ms {int((v > 1).sum()):4d} | on hitches mean {h.mean() if len(h) else float('nan'):6.3f} max {h.max() if len(h) else float('nan'):6.3f}")
    top = sorted(rec, key=lambda r: -sum(r[3].values()))[:5]
    print("   largest GDF frames (route s: L/R/T GDF ms, interval ms): " + ", ".join(
        f"{(r[0] - t0) / 1e9:.0f}: {r[3].get('L', 0):.2f}/{r[3].get('R', 0):.2f}/{r[3].get('T', 0):.2f} ({r[1]:.0f})" for r in top))
