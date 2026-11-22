"""R22: does either eye freeze or go stale in the skip windows? From the eye sampler's eyes.csv (t_ns, per-eye strip mean,
per-eye change since the previous sample l_diff / r_diff, L/R correlation), per 10 s window phase (0 = even = skip,
1 = odd = keep, first --guard s left out): samples, each eye's median change, the share of samples where one eye stood
still (change < --still) while the other moved (> --moving), and the median L/R correlation. A frozen or stale eye
shows as a high one-eye-still share in the skip phase against the keep phase.

  python eyes_freeze.py <run dir> [--window 10 --guard 2 --still 0.05 --moving 0.5]"""
import argparse
import csv
import os
import statistics
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--window", type=float, default=10)
    ap.add_argument("--guard", type=float, default=2)
    ap.add_argument("--still", type=float, default=0.05)
    ap.add_argument("--moving", type=float, default=0.5)
    a = ap.parse_args()
    rows = defaultdict(list)
    for r in csv.DictReader(open(os.path.join(a.run, "eyes.csv"), encoding="utf-8")):
        if not r.get("l_diff") or not r.get("r_diff"):
            continue
        s = int(r["t_ns"]) / 1e9
        w = int(s // a.window)
        if s - w * a.window < a.guard:
            continue
        rows[w % 2].append((float(r["l_diff"]), float(r["r_diff"]), float(r["lr_corr"]) if r.get("lr_corr") else None))
    print(os.path.basename(a.run))
    for ph in sorted(rows):
        v = rows[ph]
        l = [x[0] for x in v]
        r = [x[1] for x in v]
        l_still = sum(1 for x in v if x[0] < a.still and x[1] > a.moving) / len(v)
        r_still = sum(1 for x in v if x[1] < a.still and x[0] > a.moving) / len(v)
        both_still = sum(1 for x in v if x[0] < a.still and x[1] < a.still) / len(v)
        corr = [x[2] for x in v if x[2] is not None]
        print("  phase %d (%s): %d samples; median change L %.2f R %.2f; left still while right moves %.2f %%, right still while left moves %.2f %%, both still %.2f %%; median L/R corr %s" % (
            ph, "skip" if ph == 0 else "keep", len(v), statistics.median(l), statistics.median(r), 100 * l_still, 100 * r_still,
            100 * both_still, "%.3f" % statistics.median(corr) if corr else "-"))


if __name__ == "__main__":
    main()
