"""Per-process GPU engine utilisation once a second from the Windows "GPU Engine" counters, read through PDH directly
(read only, no admin, nothing injected). Replaces gpu_proc_sampler.ps1, whose Get-Counter call took ~35 s per sample
while a game ran (R7): one PDH query with a wildcard instance is collected once a second and read as one array.

Writes <out>.csv: time,qpc_ns,pid,process,luid,engtype,util (util summed over that process's engines of that type
and adapter; rows with util 0 are left out) and <out>.ticks.csv: qpc_ns,collect_ms,instances (what each tick cost).
qpc_ns is time.perf_counter_ns(), the clock of UEVR's perf CSVs and threadsample.py.
Stops after --seconds or when <out>.stop exists.

  python gpu_pdh_sampler.py --out cells\\gpu-<label>-<time> [--seconds 1200] [--interval 1.0]"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import time

pdh = ctypes.WinDLL("pdh")
k32 = ctypes.WinDLL("kernel32")
PDH_FMT_DOUBLE = 0x00000200
PDH_FMT_NOCAP100 = 0x00008000
PDH_MORE_DATA = 0x800007D2


class FMTVALUE(ctypes.Structure):
    _fields_ = [("CStatus", wt.DWORD), ("pad", wt.DWORD), ("doubleValue", ctypes.c_double)]


class ITEM(ctypes.Structure):
    _fields_ = [("szName", wt.LPWSTR), ("FmtValue", FMTVALUE)]


INST = re.compile(r"^pid_(\d+)_luid_(0x[0-9a-fA-F]+_0x[0-9a-fA-F]+)_phys_\d+_eng_\d+_engtype_(.*)$")


def proc_name(pid, cache):
    if pid in cache:
        return cache[pid]
    name = "?"
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if h:
        buf = ctypes.create_unicode_buffer(520)
        n = wt.DWORD(520)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            name = os.path.splitext(os.path.basename(buf.value))[0]
        k32.CloseHandle(h)
    cache[pid] = name
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=1200)
    ap.add_argument("--interval", type=float, default=1.0)
    a = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    q = wt.HANDLE()
    c = wt.HANDLE()
    if pdh.PdhOpenQueryW(None, 0, ctypes.byref(q)) != 0:
        raise SystemExit("PdhOpenQuery failed")
    st = pdh.PdhAddEnglishCounterW(q, "\\GPU Engine(*)\\Utilization Percentage", 0, ctypes.byref(c))
    if st != 0:
        raise SystemExit("PdhAddEnglishCounter failed 0x%x" % (st & 0xFFFFFFFF))
    pdh.PdhCollectQueryData(q)
    names = {}
    end = time.time() + a.seconds
    with open(a.out + ".csv", "w", newline="") as f, open(a.out + ".ticks.csv", "w", newline="") as ft:
        f.write("time,qpc_ns,pid,process,luid,engtype,util\n")
        ft.write("qpc_ns,collect_ms,instances\n")
        nxt = time.perf_counter() + a.interval
        while time.time() < end and not os.path.exists(a.out + ".stop"):
            delay = nxt - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            nxt += a.interval
            t0 = time.perf_counter_ns()
            if pdh.PdhCollectQueryData(q) != 0:
                continue
            size = wt.DWORD(0)
            count = wt.DWORD(0)
            st = pdh.PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, ctypes.byref(size), ctypes.byref(count), None)
            if (st & 0xFFFFFFFF) != PDH_MORE_DATA or size.value == 0:
                continue
            buf = ctypes.create_string_buffer(size.value)
            st = pdh.PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, ctypes.byref(size), ctypes.byref(count), buf)
            if st != 0:
                continue
            items = ctypes.cast(buf, ctypes.POINTER(ITEM))
            agg = {}
            for i in range(count.value):
                it = items[i]
                v = it.FmtValue.doubleValue
                if it.FmtValue.CStatus not in (0, 1) or v <= 0:
                    continue
                m = INST.match(it.szName or "")
                if not m:
                    continue
                key = (int(m.group(1)), m.group(2), m.group(3))
                agg[key] = agg.get(key, 0.0) + v
            t1 = time.perf_counter_ns()
            stamp = time.strftime("%H:%M:%S")
            for (pid, luid, eng), v in sorted(agg.items()):
                f.write("%s,%d,%d,%s,%s,%s,%.2f\n" % (stamp, t0, pid, proc_name(pid, names), luid, eng, v))
            ft.write("%d,%.1f,%d\n" % (t0, (t1 - t0) / 1e6, count.value))
            f.flush()
            ft.flush()
    pdh.PdhCloseQuery(q)


if __name__ == "__main__":
    main()
