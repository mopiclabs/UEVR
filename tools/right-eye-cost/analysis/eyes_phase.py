"""Eye equality per window phase of an in-run A/B (SBS + EyeSampler runs): eyes.csv samples (left/right mean luma,
L-R correlation) and binocular-report.json frames (detail mismatch, colour difference), split by
phase = floor(t s / N) % (groups + 1), samples within --guard s of a window start left out, play phase only.

  python eyes_phase.py <run dir> [--window 10 --groups 1 --guard 2 --names plain,hold]
"""
import argparse
import csv
import json
import os
import re
import statistics
from collections import defaultdict


def med(v):
    return statistics.median(v) if v else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--window", type=int, default=10)
    ap.add_argument("--groups", type=int, default=1)
    ap.add_argument("--guard", type=float, default=2)
    ap.add_argument("--settle", type=float, default=10)
    ap.add_argument("--names", default="plain,hold")
    a = ap.parse_args()
    names = a.names.split(",")
    st = json.load(open(os.path.join(a.run, "pilot-status.json"), encoding="utf-8-sig"))
    ph = {p["name"]: p["qpc_ns"] for p in st.get("phases", [])}
    t0 = ph.get("play", 0) + a.settle * 1e9
    t1 = ph.get("leave") or ph.get("exit") or float("inf")

    def phase(t):
        if not (t0 <= t < t1):
            return None
        s = t / 1e9
        w = int(s // a.window)
        if s - w * a.window < a.guard:
            return None
        return w % (a.groups + 1)

    acc = defaultdict(lambda: defaultdict(list))
    p = os.path.join(a.run, "eyes.csv")
    if os.path.exists(p):
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                try:
                    t = int(r["t_ns"])
                    lm, rm = float(r["l_mean"]), float(r["r_mean"])
                except (ValueError, KeyError):
                    continue
                k = phase(t)
                if k is None or lm < 5:
                    continue
                acc[k]["rl"].append(rm / lm)
                if r.get("lr_corr"):
                    acc[k]["corr"].append(float(r["lr_corr"]))
    p = os.path.join(a.run, "binocular-report.json")
    if os.path.exists(p):
        for fr in json.load(open(p, encoding="utf-8")).get("frames", []):
            m = re.match(r"(\d+)", fr.get("file", ""))
            if not m or fr.get("luma_left", 0) < 5:
                continue
            k = phase(int(m.group(1)))
            if k is None:
                continue
            acc[k]["mis"].append(fr["mismatch_pct"])
            acc[k]["col"].append(fr["color_diff"])
            acc[k]["bl"].append(fr["luma_right"] / fr["luma_left"])
    print(f"{os.path.basename(a.run)}: N={a.window}s groups {a.groups} guard {a.guard}s")
    print(f"   {'phase':<10}{'R/L luma (eyes.csv)':>22}{'L-R corr':>10}{'n':>6}{'mismatch %':>12}{'colour':>8}{'R/L (bino)':>12}{'n':>5}")
    for k in sorted(acc):
        x = acc[k]
        print(f"   {(names[k] if k < len(names) else k):<10}{med(x['rl']):>22.3f}{med(x['corr']):>10.3f}{len(x['rl']):>6}"
              f"{med(x['mis']):>12.2f}{med(x['col']):>8.2f}{med(x['bl']):>12.3f}{len(x['mis']):>5}")


if __name__ == "__main__":
    main()
