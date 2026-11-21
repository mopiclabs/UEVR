"""PSOs by GPU time (passmarkers.py data), with shader entry names when the run has psos.tsv + names.tsv
(dxilnames.py). Default: the right family in the plain phase against the left family and the SECONDARY phase.
  python psotable.py <run dir> [--top 40] [--fam R] [--phase 0] [--md out.md] [--flat]
  --by name : sum by shader name instead of by PSO"""
import argparse, csv, glob, os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm

ap = argparse.ArgumentParser(); ap.add_argument("run"); ap.add_argument("--top", type=int, default=40)
ap.add_argument("--window", type=int, default=10); ap.add_argument("--groups", type=int, default=2); ap.add_argument("--guard", type=float, default=2)
ap.add_argument("--fam", default="R"); ap.add_argument("--phase", type=int, default=0); ap.add_argument("--md", default="")
ap.add_argument("--flat", action="store_true"); ap.add_argument("--by", default="pso")
a = ap.parse_args()
pm.FLAT = a.flat
d = os.path.join(a.run, "passmarkers")

# PSO -> name from psos.tsv (pso, stage, hash, size) and names.tsv (hash, functions, strings, debug)
names = {}
if os.path.exists(os.path.join(d, "names.tsv")) and os.path.exists(os.path.join(d, "psos.tsv")):
    fn = {}
    with open(os.path.join(d, "names.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            fn[r["hash"]] = r["functions"] or r["strings"][:40] or r["hash"][:8]
    stages = defaultdict(dict)
    with open(os.path.join(d, "psos.tsv"), encoding="utf-8") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 3:
                stages[int(p[0], 16)][p[1]] = fn.get(p[2], p[2][:8])
    for pso, st in stages.items():
        if "C" in st:
            names[pso] = "CS " + st["C"]
        else:
            names[pso] = " / ".join(f"{k}S {st[k]}" for k in ("V", "M", "P") if k in st)


def phase_of(q, freq):
    if a.groups <= 0:
        return 0
    sec = q / freq; w = int(sec // a.window)
    return None if sec - w * a.window < a.guard else int(w % (a.groups + 1))


agg = defaultdict(lambda: {"frames": 0, "gpu": defaultdict(float), "rhi": defaultdict(float)})
shape = {}
for p in glob.glob(os.path.join(d, "capture-*.bin")):
    r = pm.analyze(p, phase_of)
    if not r or "error" in r: continue
    for ph, x in r["phases"].items():
        agg[ph]["frames"] += x["frames"]
        for f in ("gpu", "rhi"):
            for (fam, sh, pso), v in x[f].items():
                key = (names.get(pso, f"pso {pso:x}") if a.by == "name" else (names.get(pso, f"pso {pso:x}") + " @ " + sh) if a.by == "nameshape" else pso)
                agg[ph][f][(fam, key)] += v
                shape.setdefault(key, defaultdict(float))[sh] += v


def g(ph, f, fam, k):
    A = agg[ph]; return A[f][(fam, k)] / A["frames"] if A["frames"] else 0


out = []
def emit(s):
    print(s); out.append(s)

P = a.phase
keys = {k for (fam, k) in agg[P]["gpu"] if fam == a.fam}
rows = sorted(keys, key=lambda k: -g(P, "gpu", a.fam, k))
tot = sum(g(P, "gpu", a.fam, k) for k in keys)
named = sum(g(P, "gpu", a.fam, k) for k in keys if (a.by == "name" and not str(k).startswith("pso ")) or (a.by != "name" and k in names))
emit(f"{a.fam} family GPU (phase {P}) {tot:.2f} ms/f over {len(keys)} {'names' if a.by == 'name' else 'PSOs'}, named {named:.2f}; "
     f"frames per phase {[agg[p]['frames'] for p in sorted(agg)]}")
emit(f"| {'shader' if a.by == 'name' else 'pso'} | name / main shape | {a.fam} gpu | L gpu | R/L | R gpu sec | {a.fam} rhi | L rhi |")
emit("|---|---|---|---|---|---|---|---|")
cum = 0
for k in rows[: a.top]:
    sh = max(shape[k].items(), key=lambda kv: kv[1])[0] if k in shape else "?"
    rg, lg = g(P, "gpu", a.fam, k), g(P, "gpu", "L", k); cum += rg
    label = k if a.by in ("name", "nameshape") else f"{k:x}"
    desc = sh if a.by in ("name", "nameshape") else f"{names.get(k, '?')} ({sh})"
    emit(f"| {label} | {desc} | {rg:.3f} | {lg:.3f} | {rg / lg if lg else float('inf'):.2f} | {g(1, 'gpu', 'R', k):.3f} | "
         f"{g(P, 'rhi', a.fam, k):.3f} | {g(P, 'rhi', 'L', k):.3f} |")
emit(f"top {a.top}: {cum:.2f} ms/f of {tot:.2f}")
if a.md:
    open(a.md, "w", encoding="utf-8").write("\n".join(out) + "\n")
