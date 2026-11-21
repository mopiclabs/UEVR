"""What the long present intervals (> 2x / 3x the run's median) look like in perf-frames.csv, play phase only: per run
the hitch count by the dominant field of the hitch frame (game-thread tick, render-thread blocked, present call,
none of these = GPU or elsewhere) and the median of each field on hitch frames vs all frames.
  python hitch_kinds.py <label=run dir> [...]"""
import csv, json, os, sys
import numpy as np

for spec in sys.argv[1:]:
    label, run = spec.split("=", 1)
    ph = {p["name"]: p["qpc_ns"] for p in json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))["phases"]}
    t0, t1 = ph["play"], ph.get("leave") or ph.get("exit") or 1 << 62
    rows = [r for r in csv.DictReader(open(os.path.join(run, "perf-frames.csv"), encoding="utf-8", errors="replace")) if r["kind"] == "P"]
    rows.sort(key=lambda r: int(r["qpc_ns"]))
    f = lambda r, k: float(r.get(k) or 0)
    iv = [((int(b["qpc_ns"]) - int(a["qpc_ns"])) / 1e6, b) for a, b in zip(rows, rows[1:]) if t0 <= int(b["qpc_ns"]) < t1]
    med = float(np.median([v for v, _ in iv]))
    fields = ["tick_us", "rt_blocked_us", "present_us", "uevr_rt_us", "uevr_gt_us", "ticks"]
    allm = {k: float(np.median([f(r, k) for _, r in iv])) for k in fields}
    for mult in (2, 3):
        hs = [(v, r) for v, r in iv if v > mult * med]
        kinds = {"game tick": 0, "rt blocked": 0, "present": 0, "other (GPU?)": 0}
        for v, r in hs:
            ms = {"game tick": f(r, "tick_us") / 1e3, "rt blocked": f(r, "rt_blocked_us") / 1e3, "present": f(r, "present_us") / 1e3}
            k = max(ms, key=ms.get)
            kinds[k if ms[k] > 0.5 * v else "other (GPU?)"] += 1
        hm = {k: float(np.median([f(r, k) for _, r in hs])) if hs else float("nan") for k in fields}
        print(f"{label} >{mult}x ({mult * med:.0f} ms): {len(hs)} | " + ", ".join(f"{k} {n}" for k, n in kinds.items())
              + " | median on hitch / all: " + ", ".join(f"{k} {hm[k]:.0f}/{allm[k]:.0f}" for k in fields))
