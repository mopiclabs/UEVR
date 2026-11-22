"""Pass-marker family totals (GPU ms per frame) grouped by the eye output size of each capture: for runs where AutoRes
changes the eye size mid-run (R21 eship). Prints, per eye size, captures, frames, L / R / T GPU, and the top categories
(the category rules' internal / output sizes are set per group).

  python by_eye_size.py <run>"""
import glob
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm  # noqa: E402
import categories_lib as cl  # noqa: E402


def main():
    run = sys.argv[1]
    d = os.path.join(run, "passmarkers")
    groups = defaultdict(lambda: {"caps": 0, "frames": 0, "dur": 0.0, "gpu": defaultdict(float), "raw": defaultdict(float),
                                  "shape_w": defaultdict(float)})
    for p in sorted(glob.glob(os.path.join(d, "capture-*.bin"))):
        r = pm.analyze(p, lambda q, f: 0)
        if not r or "error" in r or 0 not in r["phases"]:
            continue
        g = groups[r["outputs"][0]]
        x = r["phases"][0]
        g["caps"] += 1
        g["frames"] += x["frames"]
        g["dur"] += x["dur"]
        for (fam, sh, pso), v in x["gpu"].items():
            g["gpu"][fam] += v
            g["raw"][(fam, sh, pso)] += v
            g["shape_w"][sh] += v
    stages = cl.load_names(d)
    for eye, g in sorted(groups.items(), key=lambda kv: -kv[1]["frames"]):
        n = g["frames"] or 1
        print("eye output %s: %d captures, %d frames, fps %.1f; GPU L %.2f R %.2f T %.2f = %.2f ms/f" % (
            eye, g["caps"], g["frames"], g["frames"] / g["dur"] if g["dur"] else 0, g["gpu"]["L"] / n, g["gpu"]["R"] / n,
            g["gpu"]["T"] / n, sum(g["gpu"].values()) / n))
        cl.set_sizes(g["shape_w"])
        cat = defaultdict(float)
        for (fam, sh, pso), v in g["raw"].items():
            cat[cl.categorize(cl.name_of(stages, pso), sh)] += v
        print("   (internal %s, output %s)" % (cl.INT, cl.OUT))
        for c, v in sorted(cat.items(), key=lambda kv: -kv[1])[:10]:
            print("   %-58s %.2f" % (c, v / n))


if __name__ == "__main__":
    main()
