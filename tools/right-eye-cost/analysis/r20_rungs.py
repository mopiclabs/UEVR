"""R20 ladder: one run's numbers over its measured play (from play + --settle to leave), split by 10 s window phase
when --phases 2 (phase = floor(qpc s / 10) % 2: 0 plain, 1 right eye skipped; first --guard s of a window left out).

Frames: UEVR's perf-frames.csv P rows (frames, plus its ticks and presents columns), or for a run without UEVR the
DXGI Present starts of the game's process from the logman trace (tools\\etw_present.py), mapped to QPC through the
pilot's clock pair. GPU: the harness's nvidia-smi gpu.csv (utilization.gpu, clock) and the PDH sampler's per-process
3D utilisation (game, runtime service). Threads: threadsample.py cycles per thread class (analysis\\analyze.classify).
Per phase: fps; GPU busy %; GPU work per frame = game 3D % x frame time (and nvidia-smi busy x frame time); game /
render / RHI thread CPU ms per frame; each also per 10 s block, with a 95 % interval over the blocks (between-run
comparisons) and paired plain/skip window differences (in-run).

  python r20_rungs.py --run <run dir> --side <r20-side dir> [--cell <cells dir>] [--settle 30] [--phases 2]
      [--present-ratio R]   (A: presents per rendered frame, taken from B(fg))"""
import argparse
import csv
import datetime
import glob
import json
import math
import os
import statistics
import subprocess
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import classify  # noqa: E402

T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23, 11: 2.20, 12: 2.18,
       13: 2.16, 14: 2.14, 15: 2.13, 16: 2.12, 17: 2.11, 18: 2.10, 19: 2.09, 20: 2.09, 25: 2.06, 30: 2.04}
FILETIME_UNIX = 116444736000000000


def t95(n):
    return T95.get(n, 2.0 if n > 30 else T95[min(T95, key=lambda k: abs(k - n))])


def ci(vals):
    vals = [v for v in vals if v is not None]
    if len(vals) < 2:
        return (statistics.mean(vals) if vals else float("nan")), float("nan"), len(vals)
    m = statistics.mean(vals)
    return m, t95(len(vals) - 1) * statistics.stdev(vals) / math.sqrt(len(vals)), len(vals)


def etw_presents(etl, clock):
    out = etl + ".csv"
    if not os.path.exists(out):
        subprocess.run(["tracerpt", etl, "-o", out, "-of", "CSV", "-y"], check=True, stdout=subprocess.DEVNULL)
    by_pid = defaultdict(list)
    with open(out, newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        head = [h.strip() for h in next(r)]
        i_id, i_pid, i_t, i_n = head.index("Event ID"), head.index("PID"), head.index("Clock-Time"), head.index("Event Name")
        for row in r:
            if len(row) <= i_t or row[i_n].strip() != "Microsoft-Windows-DXGI":
                continue
            try:
                eid = int(row[i_id])
            except ValueError:
                continue
            if eid not in (42, 55):
                continue
            pid = row[i_pid].strip()
            pid = int(pid, 16) if pid.startswith("0x") else int(pid)
            unix_ns = (int(row[i_t]) - FILETIME_UNIX) * 100
            by_pid[pid].append(clock["qpc_ns"] + (unix_ns - clock["unix_ns"]))
    if not by_pid:
        return [], 0
    pid = max(by_pid, key=lambda p: len(by_pid[p]))
    return sorted(by_pid[pid]), pid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--side", default="")
    ap.add_argument("--cell", default="")
    ap.add_argument("--settle", type=float, default=30)
    ap.add_argument("--phases", type=int, default=1)
    ap.add_argument("--guard", type=float, default=2)
    ap.add_argument("--window", type=float, default=10)
    ap.add_argument("--present-ratio", type=float, default=0)
    ap.add_argument("--pm-csv", default="", help="PresentMon CSV decoded from the run's logman trace (instead of tracerpt)")
    ap.add_argument("--no-etw", action="store_true", help="no frame count from the trace (blade18's traces lose the game's events)")
    ap.add_argument("--etw-check", action="store_true", help="with --pm-csv on a UEVR run: also count the trace's presents per block")
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    st = json.load(open(os.path.join(a.run, "pilot-status.json"), encoding="utf-8-sig"))
    clock = st["clock"]
    # the recipe's measured segment (the longest one if several), from --settle s after its start
    segs = [s for s in st.get("segments", []) if s.get("start_qpc_ns") and s.get("end_qpc_ns")]
    seg = max(segs, key=lambda s: s["end_qpc_ns"] - s["start_qpc_ns"])
    t0 = seg["start_qpc_ns"] + int(a.settle * 1e9)
    t1 = seg["end_qpc_ns"]
    W = a.window

    def phase_of(q):
        s = q / 1e9
        w = int(s // W)
        if a.phases > 1 and s - w * W < a.guard:
            return None, w
        return (w % a.phases if a.phases > 1 else 0), w

    # frames per 10 s block (block = window index; phase from it)
    frames = defaultdict(float)
    ticks = defaultdict(float)
    presents = defaultdict(float)
    xr_new = defaultdict(float)
    wait_ms = defaultdict(list)
    end_ms = defaultdict(list)
    psecs = defaultdict(float)
    pint = defaultdict(list)
    etw_count = defaultdict(int)
    src = ""
    pf = os.path.join(a.run, "perf.csv")
    level = defaultdict(lambda: defaultdict(int))
    have_perf = os.path.exists(pf) and sum(1 for _ in open(pf, encoding="utf-8")) > 5
    if have_perf:
        # UEVR's 1 Hz perf log: a row covers (qpc_ns - window_ms, qpc_ns]; a row counts for a block only when it lies
        # inside the block (after the guard for window runs)
        src = "UEVR perf.csv (1 Hz): ticks = rendered frames"
        for r in csv.DictReader(open(pf, encoding="utf-8")):
            try:
                q = int(r["qpc_ns"])
                dur = float(r["window_ms"]) / 1000
            except (ValueError, KeyError):
                continue
            qs = q - int(dur * 1e9)
            if not (t0 <= qs and q <= t1):
                continue
            p, w = phase_of(qs)
            if p is None or int(q / 1e9 // W) != w:
                continue
            psecs[w] += dur
            pint[w].append((qs, q))
            ticks[w] += float(r.get("ticks") or 0)
            presents[w] += float(r.get("presents") or 0)
            xr_new[w] += float(r.get("xr_new") or 0)
            frames[w] += float(r.get("ticks") or 0)
            if float(r.get("wait_calls") or 0) > 0:
                wait_ms[w].append(float(r.get("wait_avg_ms") or 0))
                end_ms[w].append(float(r.get("end_avg_ms") or 0))
        if a.etw_check and a.pm_csv:
            by_pid = defaultdict(list)
            freq = clock.get("qpc_freq", 10000000)
            for r in csv.DictReader(open(a.pm_csv, encoding="utf-8", errors="replace")):
                v = r.get("CPUStartQPC") or r.get("TimeInQPC") or ""
                if v.strip().isdigit():
                    by_pid[r.get("ProcessID", "?")].append(int(v) * 1_000_000_000 // freq)
            if by_pid:
                eq = sorted(by_pid[max(by_pid, key=lambda p: len(by_pid[p]))])
                import bisect
                for w, ivs in pint.items():
                    etw_count[w] = sum(bisect.bisect_right(eq, hi) - bisect.bisect_right(eq, lo) for lo, hi in ivs)
            src += " + trace presents for the check"
        lpf = os.path.join(a.run, "perf-frames.csv")
        if os.path.exists(lpf):
            for r in csv.DictReader(open(lpf, encoding="utf-8")):
                if r.get("kind") != "P" or r.get("level_applied") in (None, ""):
                    continue
                q = int(r["qpc_ns"])
                if t0 <= q < t1:
                    p, w = phase_of(q)
                    if p is not None:
                        level[p][(r.get("level_applied"), r.get("sp_applied"), r.get("upscaler_applied"))] += 1
    else:
        if a.pm_csv:
            # PresentMon --etl_file --qpc_time over the logman trace: one row per present, CPU start as QPC ticks
            by_pid = defaultdict(list)
            freq = clock.get("qpc_freq", 10000000)
            for r in csv.DictReader(open(a.pm_csv, encoding="utf-8", errors="replace")):
                v = r.get("CPUStartQPC") or r.get("TimeInQPC") or ""
                if not v.strip().isdigit():
                    continue
                by_pid[r.get("ProcessID", "?")].append(int(v) * 1_000_000_000 // freq)
            pid = max(by_pid, key=lambda p: len(by_pid[p])) if by_pid else "?"
            qs = sorted(by_pid.get(pid, []))
            src = "DXGI presents (logman trace, decoded by PresentMon), pid %s" % pid
        elif not a.no_etw:
            etl = os.path.join(a.side, "dxgi.etl")
            qs, pid = etw_presents(etl, clock)
            src = "DXGI Present starts (ETW), pid %d" % pid
        else:
            qs, src = [], "no frame source (--no-etw)"
        for q in qs:
            if t0 <= q < t1:
                p, w = phase_of(q)
                if p is not None:
                    presents[w] += 1
                    frames[w] += 1
    # seconds counted per block
    secs = {}
    for w in set(frames):
        if have_perf:
            secs[w] = psecs[w]
            continue
        lo, hi = max(t0, int(w * W * 1e9)), min(t1, int((w + 1) * W * 1e9))
        if a.phases > 1:
            lo = max(lo, int((w * W + a.guard) * 1e9))
        secs[w] = max(0.0, (hi - lo) / 1e9)
    if not frames:
        # no frame source (a run without UEVR whose trace lost the game's events): rate signals only, per 10 s block
        src += "; no frames: rate signals only"
        for w in range(int(t0 / 1e9 // W), int(t1 / 1e9 // W) + 1):
            lo, hi = max(t0, int(w * W * 1e9)), min(t1, int((w + 1) * W * 1e9))
            if hi - lo >= 3e9:
                frames[w] = 0
                secs[w] = (hi - lo) / 1e9
    blocks = sorted(w for w in frames if secs.get(w, 0) >= 3)

    # nvidia-smi busy per block
    nv = defaultdict(list)
    clk = defaultdict(list)
    gcsv = os.path.join(a.run, "gpu.csv")
    if os.path.exists(gcsv):
        for r in csv.reader(open(gcsv, encoding="utf-8")):
            if not r or r[0].startswith("timestamp"):
                continue
            try:
                dt = datetime.datetime.strptime(r[0].strip(), "%Y/%m/%d %H:%M:%S.%f")
            except ValueError:
                continue
            q = clock["qpc_ns"] + int(dt.timestamp() * 1e9) - clock["unix_ns"]
            if t0 <= q < t1:
                p, w = phase_of(q)
                if p is not None:
                    nv[w].append(float(r[1]))
                    clk[w].append(float(r[2]))
    # PDH per-process 3D per block
    g3d = defaultdict(lambda: defaultdict(list))
    scsv = os.path.join(a.side, "gpu.csv")
    if a.side and os.path.exists(scsv):
        per_tick = defaultdict(lambda: defaultdict(float))
        rows3d = [r for r in csv.DictReader(open(scsv, encoding="utf-8"))
                  if r["engtype"] == "3D" and float(r["util"]) <= 150]  # a new instance's first PDH value can be garbage (1.8e14 once in run a)
        # only the game's adapter (blade18 has the iGPU too: Steam's web helper and the Mopic display's adapter)
        by_luid = defaultdict(float)
        for r in rows3d:
            if r["process"].startswith("b1-"):
                by_luid[r["luid"]] += float(r["util"])
        game_luid = max(by_luid, key=by_luid.get) if by_luid else None
        for r in rows3d:
            if r["luid"] != game_luid:
                continue
            q = int(r["qpc_ns"])
            who = "game" if r["process"].startswith("b1-") else ("service" if "openxr" in r["process"] or "monado" in r["process"] else "other")
            per_tick[q][who] += float(r["util"])
        for q, d in per_tick.items():
            if t0 <= q < t1:
                p, w = phase_of(q)
                if p is not None:
                    for who in ("game", "service", "other"):
                        g3d[w][who].append(d.get(who, 0.0))
    # thread CPU per block
    cpu = defaultdict(lambda: defaultdict(float))
    if a.cell and os.path.exists(os.path.join(a.cell, "samples.csv")):
        meta = json.load(open(os.path.join(a.cell, "meta.json")))
        hz = meta["tsc_hz"]
        cls = {}
        for r in csv.DictReader(open(os.path.join(a.cell, "threads.csv"), encoding="utf-8", errors="replace")):
            cls[r["tid"]] = classify(r.get("name") or "", r.get("start") or "")
        for r in csv.DictReader(open(os.path.join(a.cell, "samples.csv"), encoding="utf-8")):
            q = int(r["qpc_ns"])
            if not (t0 <= q < t1):
                continue
            p, w = phase_of(q)
            if p is None:
                continue
            # with UEVR's perf rows as the clock, only the seconds those rows cover count (secs = their sum)
            if have_perf and not any(lo < q <= hi for lo, hi in pint.get(w, ())):
                continue
            cpu[w][cls.get(r["tid"], "Other")] += int(r["cycles"]) / hz * 1000  # ms

    rows = {}
    for w in blocks:
        s = secs[w]
        fps = frames[w] / s
        d = {"phase": phase_of(w * W * 1e9 + (a.guard + 0.5) * 1e9)[0], "fps": fps, "frame_ms": 1000 / fps if fps else None,
             "ticks_s": ticks[w] / s if ticks[w] else None, "presents_s": presents[w] / s,
             "xr_new_s": xr_new[w] / s if xr_new[w] else None,
             "wait_ms": statistics.mean(wait_ms[w]) if wait_ms.get(w) else None,
             "end_ms": statistics.mean(end_ms[w]) if end_ms.get(w) else None}
        if etw_count.get(w):
            d["etw_presents_s"] = etw_count[w] / s
        if a.present_ratio and not have_perf:
            d["rendered_fps"] = fps / a.present_ratio
        base_fps = d.get("rendered_fps") or (d["ticks_s"] if d["ticks_s"] else fps)
        d["base_fps"] = base_fps
        fr = 1000 / base_fps if base_fps else None  # ms per rendered frame
        if not fps:
            d["fps"] = None
            d["presents_s"] = None
        if nv.get(w):
            d["nv_busy"] = statistics.mean(nv[w])
            d["nv_clock"] = statistics.mean(clk[w])
            if fr:
                d["nv_work_ms"] = d["nv_busy"] / 100 * fr
        if g3d.get(w):
            for who in ("game", "service", "other"):
                d["3d_" + who] = statistics.mean(g3d[w][who])
            if fr:
                d["game_work_ms"] = d["3d_game"] / 100 * fr
                d["idle_ms"] = (1 - (d["3d_game"] + d["3d_service"] + d["3d_other"]) / 100) * fr
        if cpu.get(w):
            for c in ("GameThread", "RenderThread", "RHIThread"):
                if fr:
                    d[c + "_ms_f"] = cpu[w].get(c, 0.0) / s * fr / 1000
                d[c + "_busy"] = cpu[w].get(c, 0.0) / s / 10  # % of one core
        rows[w] = d
    keys = ["fps", "rendered_fps", "ticks_s", "presents_s", "etw_presents_s", "xr_new_s", "wait_ms", "end_ms", "nv_busy", "nv_clock", "nv_work_ms", "3d_game", "3d_service",
            "3d_other", "game_work_ms", "idle_ms", "GameThread_ms_f", "RenderThread_ms_f", "RHIThread_ms_f",
            "GameThread_busy", "RenderThread_busy", "RHIThread_busy"]
    print("%s: %s; play %.0f s after settle, %d blocks" % (os.path.basename(a.run), src, (t1 - t0) / 1e9, len(blocks)))
    summary = {}
    for p in sorted(set(d["phase"] for d in rows.values())):
        sel = [rows[w] for w in blocks if rows[w]["phase"] == p]
        summary[p] = {}
        line = []
        for k in keys:
            m, h, n = ci([d.get(k) for d in sel])
            if n:
                summary[p][k] = (m, h, n)
                line.append("%s %.3f±%.3f" % (k, m, h) if not math.isnan(h) else "%s %.3f" % (k, m))
        print("  phase %s (%d blocks): %s" % (p, len(sel), "; ".join(line)))
        if level.get(p):
            top = sorted(level[p].items(), key=lambda kv: -kv[1])[:4]
            print("    AutoRes applied (level, screen %, upscaler): " + ", ".join("%s x%d" % (k, v) for k, v in top))
    if a.phases == 2:
        print("  paired windows (skip w+1 minus plain w):")
        for k in keys:
            dd = [rows[w + 1][k] - rows[w][k] for w in blocks if rows[w]["phase"] == 0 and (w + 1) in rows and k in rows[w] and k in rows[w + 1]]
            if len(dd) >= 2:
                m, h, n = ci(dd)
                print("    %-18s %+.3f [%+.3f, %+.3f] (n %d)" % (k, m, m - h, m + h, n))
    if a.json:
        json.dump({"src": src, "summary": {str(p): v for p, v in summary.items()}, "blocks": {str(w): rows[w] for w in blocks}},
                  open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
