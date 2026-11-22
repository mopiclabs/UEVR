"""Per frame and family: command counts (draw, indirect, dispatch, ...) and RHI recording ms from pass-marker captures,
per window phase (or the whole run with --groups 0); --flat counts a run without eye outputs as one family (L).
For R21: flat B (bpm) against the VR left eye at the same pixel count (vr14's skip windows), to see how much of B -> C's
extra RHI thread time is more commands (the VR view) and how much is the Present hook.

  python family_calls.py <run> [--groups 1 --window 10] [--flat]"""
import argparse
import glob
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--groups", type=int, default=1)
    ap.add_argument("--window", type=int, default=10)
    ap.add_argument("--guard", type=float, default=2)
    ap.add_argument("--flat", action="store_true")
    a = ap.parse_args()
    pm.FLAT = a.flat

    def phase_of(q, f):
        if a.groups <= 0:
            return 0
        sec = q / f
        w = int(sec // a.window)
        return None if sec - w * a.window < a.guard else int(w % (a.groups + 1))

    frames = defaultdict(int)
    calls = defaultdict(lambda: [0] * 6)
    rhi = defaultdict(float)
    gpu = defaultdict(float)
    for p in glob.glob(os.path.join(a.run, "passmarkers", "capture-*.bin")):
        r = pm.analyze(p, phase_of)
        if not r or "error" in r:
            continue
        for ph, x in r["phases"].items():
            frames[ph] += x["frames"]
            for (fam, sh, pso), c in x["calls"].items():
                for i in range(6):
                    calls[(ph, fam)][i] += c[i]
            for (fam, sh, pso), v in x["rhi"].items():
                rhi[(ph, fam)] += v
            for (fam, sh, pso), v in x["gpu"].items():
                gpu[(ph, fam)] += v
    print(os.path.basename(a.run))
    print("| phase | family | frames | draw | indirect | dispatch | rt | barrier | copy | RHI rec ms/f | GPU ms/f |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for (ph, fam) in sorted(calls):
        n = frames[ph] or 1
        c = calls[(ph, fam)]
        print("| %s | %s | %d | %s | %.2f | %.2f |" % (ph, fam, frames[ph], " | ".join("%.0f" % (v / n) for v in c), rhi[(ph, fam)] / n, gpu[(ph, fam)] / n))


if __name__ == "__main__":
    main()
