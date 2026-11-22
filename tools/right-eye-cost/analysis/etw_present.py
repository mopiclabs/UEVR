"""Presents per second from a DXGI-only ETW trace (a game without UEVR has no other frame counter on blade18:
PresentMon 2.5.1 drops events there even with only the runtime provider). Recorded by r20_ladder.ps1 with the
built-in logman, file mode, only Microsoft-Windows-DXGI (keyword 0x2 = the Present tasks), no download:
    logman create trace R20dxgi -p Microsoft-Windows-DXGI 0x2 0x4 -o <etl> -bs 1024 -nb 64 512 -ft 1 -ets

Decodes the ETL with the built-in tracerpt (CSV), takes IDXGISwapChain::Present starts (event 42) and Present1 /
PresentMultiplaneOverlay starts (55) per process, and prints per process: presents, span, mean presents/s, and the
per-second counts inside [--from, --to] (unix seconds, from the harness's segment times) when given.

  python etw_present.py <etl> [--pid N] [--from UNIX_S --to UNIX_S] [--csv out.csv]"""
import argparse
import collections
import csv
import os
import subprocess
import tempfile

FILETIME_UNIX = 116444736000000000  # 100 ns ticks from 1601 to 1970


def decode(etl):
    out = os.path.join(tempfile.mkdtemp(prefix="etwp"), "dxgi.csv")
    subprocess.run(["tracerpt", etl, "-o", out, "-of", "CSV", "-y"], check=True, stdout=subprocess.DEVNULL)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("etl")
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=0)
    ap.add_argument("--csv", default="")
    a = ap.parse_args()
    path = decode(a.etl)
    by_pid = collections.defaultdict(list)
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        head = [h.strip() for h in next(r)]
        i_id, i_pid, i_t = head.index("Event ID"), head.index("PID"), head.index("Clock-Time")
        i_name = head.index("Event Name")
        for row in r:
            if len(row) <= i_t or row[i_name].strip() != "Microsoft-Windows-DXGI":
                continue
            try:
                eid = int(row[i_id])
            except ValueError:
                continue
            if eid not in (42, 55):
                continue
            pid = int(row[i_pid].strip(), 16) if row[i_pid].strip().startswith("0x") else int(row[i_pid])
            t = (int(row[i_t]) - FILETIME_UNIX) / 1e7
            by_pid[pid].append(t)
    rows = []
    for pid, ts in sorted(by_pid.items(), key=lambda kv: -len(kv[1])):
        ts.sort()
        span = ts[-1] - ts[0] if len(ts) > 1 else 0
        print("pid %d: %d presents over %.1f s = %.2f/s" % (pid, len(ts), span, (len(ts) - 1) / span if span > 0 else 0))
        if (a.pid == 0 or a.pid == pid) and a.t1 > a.t0:
            inside = [t for t in ts if a.t0 <= t < a.t1]
            per_s = collections.Counter(int(t) for t in inside)
            secs = list(range(int(a.t0), int(a.t1)))
            counts = [per_s.get(s, 0) for s in secs]
            if counts:
                print("  [%.0f, %.0f): %d presents in %d s = %.2f/s; per-second min %d max %d" % (
                    a.t0, a.t1, len(inside), len(secs), len(inside) / len(secs), min(counts), max(counts)))
            rows += [(pid, s, c) for s, c in zip(secs, counts)]
    if a.csv and rows:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["pid", "unix_s", "presents"])
            w.writerows(rows)


if __name__ == "__main__":
    main()
