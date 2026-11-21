"""Per-thread CPU of measured cells (run_cell.ps1 output), side by side.

For each cell: the measured window is the recipe's play phase from --settle s after its start to the "leave" phase
(pilot-status.json). Frames in it: VR = perf-frames.csv P rows (new engine frames submitted); flat = PresentMon
presents / the harness's presents_per_sim (frame generation's presents counted once).
Per thread class and per top thread: cores = CPU seconds per wall second, ms/f = CPU ms per engine frame,
p90 = 90th percentile of the thread's per-tick busy share (1.00 = never off the CPU in that 250 ms).

python analyze.py <cell dir> [<cell dir> ...] [--settle 30] [--top 12] [--json out.json]
"""

import argparse
import csv
import glob
import json
import os
import re
import sys
from collections import defaultdict

CLASSES = [
    ("GameThread", r"^GameThread$"),
    ("RenderThread", r"^RenderThread"),
    ("RHIThread", r"^RHIThread$"),
    ("RHI other", r"^RHI(Submission|Interrupt)"),
    ("TaskGraph", r"^(TaskGraphThread|Foreground Worker|Background Worker|TaskGraph)"),
    ("Pools", r"^(PoolThread|BackgroundThreadPool|IOThreadPool|ThreadPool|LargeThreadPool)"),
    ("Audio", r"Audio"),
    ("Loading/IO", r"(AsyncLoading|IoDispatcher|IoService|FileSystem|Stream|Zen|Package)"),
    ("Shader/PSO", r"(Shader|PSO|Pipeline)"),
]
MODULE_CLASSES = [
    ("UEVR", r"^(UEVRBackend|LuaVR)\.dll"),
    ("OpenXR client", r"(openxr|monado|mopic)"),
    ("GPU driver", r"^(nvwgf2umx|nvwgf2um|nvoglv64|nvapi64|nvldumdx|amdxc64|igd12um|nvcuda|nvd3dumx)"),
    ("D3D runtime", r"^(d3d12|D3D12Core|dxgi|d3d11|D3D12SDKLayers|dxcore)"),
    ("Streamline/NGX", r"^(sl\.|nvngx|NvLowLatency|_nvngx)"),
]


def classify(name, start):
    for cls, pat in CLASSES:
        if name and re.search(pat, name):
            return cls
    mod = start.split("+")[0] if start else ""
    for cls, pat in MODULE_CLASSES:
        if (mod and re.search(pat, mod, re.I)) or (name and re.search(pat, name, re.I)):
            return cls
    return "Other"


def pctl(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


def load_cell(cell, settle_s):
    meta = json.load(open(os.path.join(cell, "meta.json")))
    hz = meta["tsc_hz"]
    rundir = open(os.path.join(cell, "rundir.txt")).read().strip() if os.path.exists(os.path.join(cell, "rundir.txt")) else ""
    res = {"cell": os.path.basename(cell), "rundir": rundir}
    status = json.load(open(os.path.join(rundir, "pilot-status.json"), encoding="utf-8-sig")) if rundir else {}
    phases = {p["name"]: p["qpc_ns"] for p in status.get("phases", [])}
    if "play" not in phases:
        res["error"] = "no play phase"
        return res
    t0 = phases["play"] + int(settle_s * 1e9)
    t1 = phases.get("leave") or phases.get("exit") or meta.get("end_qpc_ns")
    res["window_s"] = (t1 - t0) / 1e9
    result = json.load(open(os.path.join(rundir, "result.json"), encoding="utf-8-sig"))
    res["verdict"] = result.get("verdict")

    # frames
    frames, kind = 0, ""
    pf = os.path.join(rundir, "perf-frames.csv")
    pm = glob.glob(os.path.join(rundir, "presentmon-*.csv"))
    if os.path.exists(pf):
        kind = "vr"
        with open(pf, newline="") as f:
            for r in csv.DictReader(f):
                if r["kind"] == "P" and t0 <= int(r["qpc_ns"]) < t1:
                    frames += 1
    elif pm:
        kind = "flat"
        pps = 1.0
        try:
            perf = json.load(open(os.path.join(rundir, "perf.json"), encoding="utf-8-sig"))
            pps = (perf["groups"]["gameplay"]["flat"] or {}).get("presents_per_sim") or 1.0
        except Exception:
            pass
        n = 0
        gpu_busy = []
        with open(pm[0], newline="", encoding="utf-8-sig") as f:
            for r in csv.DictReader(f):
                q = int(r["TimeInQPC"]) * 100
                if t0 <= q < t1:
                    n += 1
                    try:
                        gpu_busy.append(float(r["MsGPUBusy"]))
                    except ValueError:
                        pass
        frames = round(n / pps)
        res["presents_per_sim"] = pps
        res["gpu_busy_ms_p50"] = pctl(gpu_busy, 0.5)
    res["kind"], res["frames"] = kind, frames
    res["fps"] = frames / res["window_s"] if res["window_s"] > 0 else 0

    threads = {}
    with open(os.path.join(cell, "threads.csv"), newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            threads[r["tid"]] = (r["name"], r["start"])
    # UE4 leaves the game thread (the process's main thread, the first one listed) unnamed
    if threads and not any(n == "GameThread" for n, _ in threads.values()):
        first = next(iter(threads))
        if not threads[first][0]:
            threads[first] = ("GameThread", threads[first][1])
    per = defaultdict(float)
    ticks_busy = defaultdict(list)   # tid -> busy share per tick
    ctx = defaultdict(int)
    tick_times = set()
    with open(os.path.join(cell, "samples.csv"), newline="") as f:
        for r in csv.DictReader(f):
            q = int(r["qpc_ns"])
            if not (t0 <= q < t1):
                continue
            c = int(r["cycles"])
            per[r["tid"]] += c
            ctx[r["tid"]] += int(r["ctxsw"])
            ticks_busy[r["tid"]].append((q, c))
    mhz, bp, be = [], [], []
    tick_dt = {}
    with open(os.path.join(cell, "ticks.csv"), newline="") as f:
        for r in csv.DictReader(f):
            q = int(r["qpc_ns"])
            if t0 <= q < t1:
                tick_dt[q] = int(r["dt_ns"])
                if r["cpu_mhz"]:
                    mhz.append(float(r["cpu_mhz"]))
                bp.append(float(r["busy_p"]))
                be.append(float(r["busy_e"]))
    ntick = len(tick_dt)
    res["cpu_mhz"] = sum(mhz) / len(mhz) if mhz else 0
    res["busy_p"] = sum(bp) / len(bp) if bp else 0
    res["busy_e"] = sum(be) / len(be) if be else 0
    res["p_cores"] = sum(1 for v in meta["efficiency_class"].values() if v == max(meta["efficiency_class"].values()))
    W = res["window_s"]
    F = max(frames, 1)
    cls_cpu = defaultdict(float)
    rows = []
    for tid, cyc in per.items():
        name, start = threads.get(tid, ("", ""))
        cls = classify(name, start)
        sec = cyc / hz
        cls_cpu[cls] += sec
        shares = [c / hz / (tick_dt.get(q, 250e6) / 1e9) for q, c in ticks_busy[tid]]
        shares += [0.0] * max(0, ntick - len(shares))
        rows.append({"tid": tid, "name": name or start, "class": cls, "cores": sec / W, "ms_per_frame": sec * 1000 / F,
                     "p90": pctl(shares, 0.9), "p50": pctl(shares, 0.5), "ctxsw_per_frame": ctx[tid] / F})
    rows.sort(key=lambda r: -r["cores"])
    res["threads"] = rows
    res["classes"] = {c: {"cores": s / W, "ms_per_frame": s * 1000 / F} for c, s in cls_cpu.items()}
    res["total_cores"] = sum(cls_cpu.values()) / W
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cells", nargs="+")
    ap.add_argument("--settle", type=float, default=30)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--json")
    a = ap.parse_args()
    out = [load_cell(c, a.settle) for c in a.cells]
    for r in out:
        if "error" in r:
            print(f"{r['cell']}: {r['error']}")
            continue
        print(f"\n== {r['cell']}  [{r['kind']} {r['verdict']}] window {r['window_s']:.0f} s, {r['frames']} frames, "
              f"{r['fps']:.1f} fps ({1000 / r['fps'] if r['fps'] else 0:.2f} ms)  CPU {r['cpu_mhz']:.0f} MHz, "
              f"busy P {r['busy_p']:.1f}/{2 * r['p_cores'] if False else r['p_cores']} E {r['busy_e']:.1f}, process {r['total_cores']:.2f} cores"
              + (f", flat GPU busy p50 {r['gpu_busy_ms_p50']:.2f} ms" if r.get("gpu_busy_ms_p50") else ""))
        print(f"   {'class':<16}{'cores':>7}{'ms/f':>8}")
        for c, v in sorted(r["classes"].items(), key=lambda x: -x[1]["cores"]):
            print(f"   {c:<16}{v['cores']:>7.2f}{v['ms_per_frame']:>8.2f}")
        print(f"   {'top thread':<34}{'class':<15}{'cores':>6}{'ms/f':>7}{'p50':>6}{'p90':>6}{'csw/f':>7}")
        for t in r["threads"][: a.top]:
            print(f"   {t['name'][:33]:<34}{t['class']:<15}{t['cores']:>6.2f}{t['ms_per_frame']:>7.2f}{t['p50']:>6.2f}{t['p90']:>6.2f}{t['ctxsw_per_frame']:>7.1f}")
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    sys.exit(main())
