"""Fixed-per-view vs per-pixel GPU cost, from two VR runs at two eye sizes (the second with ~2x the pixels): per
category and per shader name, c1 = ms/frame at the small size, c2 at the large one, fixed ~= (k*c1 - c2)/(k-1) with
k = pixel ratio (2*c1 - c2 for k = 2), pixel part = c1 - fixed. Plain phase (0) of each run, one family.
A third run at the small size (--noise) gives the same-size spread.
  python sizecost.py <small run> <large run> [--fam L] [--groups-small 2 --groups-large 2] [--noise <run> --groups-noise 0]
     [--top 25] [--md out.md]"""
import argparse, glob, os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm
import categories_lib as cl

ap = argparse.ArgumentParser(); ap.add_argument("small"); ap.add_argument("large")
ap.add_argument("--fam", default="L"); ap.add_argument("--fam-large", default=""); ap.add_argument("--groups-small", type=int, default=2); ap.add_argument("--groups-large", type=int, default=2)
ap.add_argument("--noise", default=""); ap.add_argument("--groups-noise", type=int, default=0)
ap.add_argument("--window", type=int, default=10); ap.add_argument("--guard", type=float, default=2)
ap.add_argument("--top", type=int, default=25); ap.add_argument("--md", default=""); ap.add_argument("--flat", action="store_true")
a = ap.parse_args()
pm.FLAT = a.flat


def load(run, groups, fam=None):
    fam = fam or a.fam
    d = os.path.join(run, "passmarkers")

    def phase_of(q, freq):
        if groups <= 0:
            return 0
        sec = q / freq; w = int(sec // a.window)
        return None if sec - w * a.window < a.guard else int(w % (groups + 1))
    raw = defaultdict(float); frames = 0; shape_w = defaultdict(float)
    for p in glob.glob(os.path.join(d, "capture-*.bin")):
        r = pm.analyze(p, phase_of)
        if not r or "error" in r or 0 not in r["phases"]:
            continue
        x = r["phases"][0]
        frames += x["frames"]
        for (f, sh, pso), v in x["gpu"].items():
            shape_w[sh] += v
            if f == fam:
                raw[(sh, pso)] += v
    cl.set_sizes(shape_w)
    stages = cl.load_names(d)
    cats = defaultdict(float); names = defaultdict(float)
    for (sh, pso), v in raw.items():
        n = cl.name_of(stages, pso)
        c = cl.categorize(n, sh)
        cats[c] += v / frames
        key = re.sub(r"\s+", " ", n).strip() if n and n.strip() else f"(unnamed) {c}"
        names[key] += v / frames
    m = re.match(r"(\d+)x(\d+)", cl.INT or "0x0")
    return {"frames": frames, "int": cl.INT, "px": int(m.group(1)) * int(m.group(2)), "cats": cats, "names": names}


S = load(a.small, a.groups_small); L = load(a.large, a.groups_large, a.fam_large or a.fam)
N = load(a.noise, a.groups_noise) if a.noise else None
k = L["px"] / S["px"] if S["px"] else 2.0
out = []


def emit(s=""):
    print(s); out.append(s)


emit(f"family {a.fam} (large run: {a.fam_large or a.fam}); small {os.path.basename(a.small)} INT {S['int']} ({S['frames']} frames), large {os.path.basename(a.large)} "
     f"INT {L['int']} ({L['frames']} frames); pixel ratio k = {k:.3f}" + (f"; noise {os.path.basename(a.noise)} INT {N['int']} ({N['frames']} frames)" if N else ""))
fixed = lambda c1, c2: (k * c1 - c2) / (k - 1)
for title, key in (("category", "cats"), ("shader name", "names")):
    keys = set(S[key]) | set(L[key])
    rows = sorted(keys, key=lambda x: -fixed(S[key].get(x, 0), L[key].get(x, 0)))
    emit(f"\n### by {title} (GPU ms per frame; fixed = (k*c1 - c2)/(k-1); scaling = c2/c1, 1 = fixed, {k:.2f} = per pixel)")
    emit("| " + title + " | c1 small | c2 large | c2/c1 | fixed | per-pixel part at small |" + (" noise c1' |" if N else ""))
    emit("|---|---|---|---|---|---|" + ("---|" if N else ""))
    shown = [x for x in rows if max(S[key].get(x, 0), L[key].get(x, 0)) >= 0.02]
    tot1 = tot2 = 0
    for x in (shown if title == "category" else shown[:a.top] + [x for x in shown[a.top:] if fixed(S[key].get(x, 0), L[key].get(x, 0)) < -0.1][:5]):
        c1, c2 = S[key].get(x, 0), L[key].get(x, 0)
        f = fixed(c1, c2)
        emit(f"| {x[:110]} | {c1:.3f} | {c2:.3f} | {c2 / c1 if c1 else float('inf'):.2f} | {f:+.3f} | {c1 - f:.3f} |" +
             (f" {N[key].get(x, 0):.3f} |" if N else ""))
    c1, c2 = sum(S[key].values()), sum(L[key].values())
    emit(f"| total | {c1:.3f} | {c2:.3f} | {c2 / c1:.2f} | {fixed(c1, c2):+.3f} | {c1 - fixed(c1, c2):.3f} |" +
         (f" {sum(N[key].values()):.3f} |" if N else ""))
if a.md:
    open(a.md, "w", encoding="utf-8").write("\n".join(out) + "\n")
