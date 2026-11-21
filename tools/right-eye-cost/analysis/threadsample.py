"""Per-thread CPU sampler for a game process, from outside it (no admin rights, nothing injected).

Waits for <exe>, then every --interval seconds writes, for every thread of it that ran or switched since the last
tick, its CPU time (QueryThreadCycleTime, exact TSC cycles), context switches, state and wait reason
(NtQuerySystemInformation). Per tick it also writes every logical processor's busy share (its idle cycles) and the
effective CPU clock (PDH "% Processor Performance" x "Processor Frequency").

Output (in --out):
  samples.csv   qpc_ns,tid,cycles,ctxsw,state,wait_reason   (deltas since the previous tick of that thread)
  ticks.csv     qpc_ns,dt_ns,proc_cycles,threads,cpu_mhz,busy_p,busy_e,busy_all  (busy_* = sum over cores of busy share)
  cores.csv     qpc_ns,cpu,busy                                (one row per logical processor per tick)
  threads.csv   tid,name,start,first_qpc_ns,last_qpc_ns       (written at exit; name = GetThreadDescription)
  meta.json     pid, exe, tsc_hz, logical processors and their efficiency classes
qpc_ns is time.perf_counter_ns(): the clock of UEVR's perf CSVs and PresentMon's --qpc_time.

python threadsample.py --exe b1-Win64-Shipping.exe --out <dir> [--interval 0.25] [--wait 1800] [--seconds 0]
"""

import argparse
import csv
import ctypes
import ctypes.wintypes as wt
import json
import os
import bisect
import re
import sys
import threading
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
pdh = ctypes.WinDLL("pdh")

THREAD_QUERY_INFORMATION = 0x0040
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
STATUS_INFO_LENGTH_MISMATCH = 0xC0000004
SYNCHRONIZE = 0x00100000

k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetCurrentThread.restype = wt.HANDLE
k32.QueryThreadCycleTime.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_uint64)]
k32.QueryProcessCycleTime.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_uint64)]
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
k32.GetThreadDescription.restype = ctypes.c_long
k32.LocalFree.argtypes = [ctypes.c_void_p]
k32.QueryIdleProcessorCycleTime.argtypes = [ctypes.POINTER(wt.ULONG), ctypes.POINTER(ctypes.c_uint64)]
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.K32EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
k32.K32GetModuleBaseNameW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
k32.K32GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
module_paths = {}   # base name -> full path, filled by modules()


class MODULEINFO(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]


k32.K32GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODULEINFO), wt.DWORD]
ntdll.NtQuerySystemInformation.argtypes = [ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
ntdll.NtQuerySystemInformation.restype = ctypes.c_ulong
ntdll.NtQueryInformationThread.argtypes = [wt.HANDLE, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
ntdll.NtQueryInformationThread.restype = ctypes.c_ulong


def find_pid(exe, min_threads=20):
    """The pid of the process named exe (case-insensitive) with the most threads, once one has min_threads (a game's
    launcher stub of the same name, as Hogwarts Legacy's, has one), or None."""
    best, most = None, 0
    for pid, name, threads in iter_processes(want_threads=True):
        if name.lower() == exe.lower() and len(threads) > most:
            best, most = pid, len(threads)
    return best if most >= min_threads else None


_buf = ctypes.create_string_buffer(1 << 20)


def query_processes():
    global _buf
    ret = ctypes.c_ulong(0)
    while True:
        status = ntdll.NtQuerySystemInformation(5, _buf, len(_buf), ctypes.byref(ret))
        if status == STATUS_INFO_LENGTH_MISMATCH:
            _buf = ctypes.create_string_buffer(max(len(_buf) * 2, ret.value + 65536))
            continue
        if status != 0:
            raise OSError(f"NtQuerySystemInformation 0x{status:08x}")
        return _buf.raw[: ret.value]


def iter_processes(want_threads=True, only_pid=None):
    """(pid, image name, [(tid, ctxsw, state, wait_reason), ...]) per process, x64 layouts."""
    raw = query_processes()
    off = 0
    while True:
        nxt = int.from_bytes(raw[off:off + 4], "little")
        nthreads = int.from_bytes(raw[off + 4:off + 8], "little")
        name_len = int.from_bytes(raw[off + 56:off + 58], "little")
        name_ptr = int.from_bytes(raw[off + 64:off + 72], "little")
        pid = int.from_bytes(raw[off + 80:off + 88], "little")
        name = ""
        if name_ptr and name_len:
            # ImageName.Buffer points into our buffer: turn it into an offset
            base = ctypes.addressof(_buf)
            start = name_ptr - base
            if 0 <= start < len(raw):
                name = raw[start:start + name_len].decode("utf-16-le", "replace")
        threads = []
        if want_threads and (only_pid is None or pid == only_pid):
            t = off + 256
            for _ in range(nthreads):
                tid = int.from_bytes(raw[t + 48:t + 56], "little")
                ctxsw = int.from_bytes(raw[t + 64:t + 68], "little")
                state = int.from_bytes(raw[t + 68:t + 72], "little")
                wait_reason = int.from_bytes(raw[t + 72:t + 76], "little")
                threads.append((tid, ctxsw, state, wait_reason))
                t += 80
        yield pid, name, threads
        if nxt == 0:
            break
        off += nxt


def thread_name(h):
    p = ctypes.c_void_p()
    if k32.GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value:
        s = ctypes.wstring_at(p.value)
        k32.LocalFree(p)
        return s
    return ""


def thread_start(h):
    addr = ctypes.c_void_p()
    if ntdll.NtQueryInformationThread(h, 9, ctypes.byref(addr), ctypes.sizeof(addr), None) == 0:
        return addr.value or 0
    return 0


def modules(pid):
    """[(base, size, name)] of the process's modules, or [] when it can't be read."""
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        return []
    try:
        arr = (ctypes.c_void_p * 2048)()
        need = wt.DWORD()
        if not k32.K32EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 3):
            return []
        out = []
        for i in range(min(need.value // ctypes.sizeof(ctypes.c_void_p), 2048)):
            mi = MODULEINFO()
            nm = ctypes.create_unicode_buffer(260)
            if k32.K32GetModuleInformation(h, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)):
                k32.K32GetModuleBaseNameW(h, arr[i], nm, 260)
                out.append((mi.base or 0, mi.size, nm.value))
                if nm.value not in module_paths:
                    full = ctypes.create_unicode_buffer(1024)
                    if k32.K32GetModuleFileNameExW(h, arr[i], full, 1024):
                        module_paths[nm.value] = full.value
        return out
    finally:
        k32.CloseHandle(h)


def locate(addr, mods):
    for base, size, name in mods:
        if base <= addr < base + size:
            return f"{name}+0x{addr - base:x}"
    return f"0x{addr:x}" if addr else ""


def tsc_hz():
    """TSC cycles per second, from our own thread's cycle counter over a busy 200 ms (best of 3)."""
    h = k32.GetCurrentThread()
    best = 0.0
    for _ in range(3):
        c0, c1 = ctypes.c_uint64(), ctypes.c_uint64()
        k32.QueryThreadCycleTime(h, ctypes.byref(c0))
        t0 = time.perf_counter_ns()
        while time.perf_counter_ns() - t0 < 200_000_000:
            pass
        k32.QueryThreadCycleTime(h, ctypes.byref(c1))
        t1 = time.perf_counter_ns()
        best = max(best, (c1.value - c0.value) / ((t1 - t0) / 1e9))
    return best


def efficiency_classes():
    """{logical processor index: EfficiencyClass} from GetSystemCpuSetInformation (higher = faster cores)."""
    need = wt.ULONG(0)
    k32.GetSystemCpuSetInformation(None, 0, ctypes.byref(need), None, 0)
    buf = ctypes.create_string_buffer(need.value)
    if not k32.GetSystemCpuSetInformation(buf, need.value, ctypes.byref(need), None, 0):
        return {}
    out, off, raw = {}, 0, buf.raw
    while off < need.value:
        size = int.from_bytes(raw[off:off + 4], "little")
        # SYSTEM_CPU_SET_INFORMATION: Size, Type, then CpuSet: Id(4) Group(2) LogicalProcessorIndex(1) CoreIndex(1)
        # LastLevelCacheIndex(1) NumaNodeIndex(1) EfficiencyClass(1)
        lp = raw[off + 14]
        out[lp] = raw[off + 18]
        off += size
    return out


class Pdh:
    def __init__(self):
        self.q = ctypes.c_void_p()
        self.ok = pdh.PdhOpenQueryW(None, 0, ctypes.byref(self.q)) == 0
        self.perf = ctypes.c_void_p()
        self.freq = ctypes.c_void_p()
        if self.ok:
            self.ok = (pdh.PdhAddEnglishCounterW(self.q, "\\Processor Information(_Total)\\% Processor Performance", 0, ctypes.byref(self.perf)) == 0
                       and pdh.PdhAddEnglishCounterW(self.q, "\\Processor Information(_Total)\\Processor Frequency", 0, ctypes.byref(self.freq)) == 0)
            pdh.PdhCollectQueryData(self.q)

    def mhz(self):
        if not self.ok or pdh.PdhCollectQueryData(self.q) != 0:
            return ""

        class V(ctypes.Structure):
            _fields_ = [("status", wt.DWORD), ("value", ctypes.c_double)]

        p, f = V(), V()
        if pdh.PdhGetFormattedCounterValue(self.perf, 0x200, None, ctypes.byref(p)) != 0:
            return ""
        if pdh.PdhGetFormattedCounterValue(self.freq, 0x200, None, ctypes.byref(f)) != 0:
            return ""
        return f"{f.value * p.value / 100:.0f}"


THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
CONTEXT_CONTROL_AMD64 = 0x00100001
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.SuspendThread.restype = wt.DWORD
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.ResumeThread.restype = wt.DWORD
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class StackSampler(threading.Thread):
    """Samples the instruction pointer of the game's main threads (game, render, RHI) at `hz`, plus up to 12 stack
    values that point into a loaded module (heuristic return addresses, read while the thread is suspended for a
    few microseconds). Writes stacks.csv: qpc_ns,tid,rip,callers (hex, ';'-separated)."""

    PATTERN = re.compile(r"^(GameThread|RenderThread|RHIThread)")

    def __init__(self, pid, hz, path, handles):
        super().__init__(daemon=True)
        self.pid, self.period, self.path, self.handles = pid, 1.0 / hz, path, handles
        self.mods = []          # sorted [(base, end)], set by the main loop
        self.stop = threading.Event()
        self.count = 0

    def targets(self):
        try:
            items = list(self.handles.items())
        except RuntimeError:
            return []
        out = [tid for tid, e in items if e[3] and self.PATTERN.match(e[3])]
        if items and not any(e[3] == "GameThread" for _, e in items):
            out.append(items[0][0])   # UE4's unnamed main thread = the game thread
        return out

    def run(self):
        hproc = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, False, self.pid)
        raw = (ctypes.c_byte * (1232 + 16))()
        ctx = (ctypes.addressof(raw) + 15) & ~15
        stack = ctypes.create_string_buffer(1024)
        got = ctypes.c_size_t()
        th = {}
        with open(self.path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["qpc_ns", "tid", "rip", "callers"])
            nxt = time.perf_counter()
            while not self.stop.is_set():
                nxt += self.period
                d = nxt - time.perf_counter()
                if d > 0:
                    time.sleep(d)
                bases = [b for b, _ in self.mods]
                for tid in self.targets():
                    h = th.get(tid)
                    if h is None:
                        h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                        th[tid] = h
                    if not h:
                        continue
                    if k32.SuspendThread(h) == 0xFFFFFFFF:
                        continue
                    try:
                        ctypes.c_uint32.from_address(ctx + 0x30).value = CONTEXT_CONTROL_AMD64
                        ok = k32.GetThreadContext(h, ctx)
                        rip = ctypes.c_uint64.from_address(ctx + 0xF8).value if ok else 0
                        rsp = ctypes.c_uint64.from_address(ctx + 0x98).value if ok else 0
                        n = 0
                        if rsp and k32.ReadProcessMemory(hproc, rsp, stack, 1024, ctypes.byref(got)):
                            n = got.value
                    finally:
                        k32.ResumeThread(h)
                    q = time.perf_counter_ns()
                    callers = []
                    for i in range(0, n - 7, 8):
                        v = int.from_bytes(stack.raw[i:i + 8], "little")
                        j = bisect.bisect_right(bases, v) - 1
                        if j >= 0 and v < self.mods[j][1]:
                            callers.append(f"{v:x}")
                            if len(callers) >= 12:
                                break
                    w.writerow([q, tid, f"{rip:x}", ";".join(callers)])
                    self.count += 1
        for h in th.values():
            if h:
                k32.CloseHandle(h)
        k32.CloseHandle(hproc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--pid", type=int, default=0, help="sample this pid instead of the newest process named --exe")
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=float, default=0.25)
    ap.add_argument("--wait", type=float, default=1800, help="seconds to wait for the process to appear")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = until the process exits)")
    ap.add_argument("--stack-hz", type=float, default=0, help="also sample the game/render/RHI threads' instruction pointers (stacks.csv)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    t_end_wait = time.monotonic() + a.wait
    pid = a.pid or None
    while pid is None:
        pid = find_pid(a.exe)
        if pid is None:
            if time.monotonic() > t_end_wait:
                print(f"threadsample: {a.exe} never appeared", file=sys.stderr)
                return 2
            time.sleep(1.0)
    hproc = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, False, pid)
    print(f"threadsample: {a.exe} pid {pid}", flush=True)

    hz = tsc_hz()
    ncpu = os.cpu_count()
    eff = efficiency_classes()
    top = max(eff.values()) if eff else 0
    meta = {"pid": pid, "exe": a.exe, "tsc_hz": hz, "interval": a.interval, "cpus": ncpu, "efficiency_class": eff,
            "start_qpc_ns": time.perf_counter_ns(), "start_unix": time.time()}
    pdhq = Pdh()

    handles = {}   # tid -> [handle, last_cycles, last_ctxsw, name, start, first, last, named_check]
    mods, mods_at = [], 0.0
    stacks = None
    if a.stack_hz > 0:
        stacks = StackSampler(pid, a.stack_hz, os.path.join(a.out, "stacks.csv"), handles)
        stacks.start()
    samples = open(os.path.join(a.out, "samples.csv"), "w", newline="")
    ticks = open(os.path.join(a.out, "ticks.csv"), "w", newline="")
    cores = open(os.path.join(a.out, "cores.csv"), "w", newline="")
    sw, tw, cw = csv.writer(samples), csv.writer(ticks), csv.writer(cores)
    sw.writerow(["qpc_ns", "tid", "cycles", "ctxsw", "state", "wait_reason"])
    tw.writerow(["qpc_ns", "dt_ns", "proc_cycles", "threads", "cpu_mhz", "busy_p", "busy_e", "busy_all"])
    cw.writerow(["qpc_ns", "cpu", "busy"])

    idle_n = wt.ULONG(ctypes.sizeof(ctypes.c_uint64) * ncpu)
    idle_prev = (ctypes.c_uint64 * ncpu)()
    k32.QueryIdleProcessorCycleTime(ctypes.byref(idle_n), idle_prev)
    proc_prev = ctypes.c_uint64()
    k32.QueryProcessCycleTime(hproc, ctypes.byref(proc_prev))
    t_prev = time.perf_counter_ns()
    t_stop = time.monotonic() + a.seconds if a.seconds > 0 else None
    next_tick = time.monotonic()
    first_pass = True
    try:
        while True:
            next_tick += a.interval
            delay = next_tick - time.monotonic()
            if delay > 0:
                if k32.WaitForSingleObject(hproc, int(delay * 1000)) == 0:
                    break
            elif k32.WaitForSingleObject(hproc, 0) == 0:
                break
            if t_stop is not None and time.monotonic() > t_stop:
                break

            threads = []
            for p, _n, th in iter_processes(only_pid=pid):
                if p == pid:
                    threads = th
                    break
            now = time.perf_counter_ns()
            if time.monotonic() - mods_at > 10:
                mods, mods_at = modules(pid), time.monotonic()
                if stacks is not None:
                    stacks.mods = sorted((b, b + sz) for b, sz, _ in mods)
            seen = set()
            for tid, ctxsw, state, wait_reason in threads:
                seen.add(tid)
                e = handles.get(tid)
                if e is None:
                    h = k32.OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, False, tid) or \
                        k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                    if not h:
                        continue
                    c = ctypes.c_uint64()
                    k32.QueryThreadCycleTime(h, ctypes.byref(c))
                    start = thread_start(h)
                    e = [h, c.value, ctxsw, thread_name(h), start, now, now, time.monotonic(), ""]
                    handles[tid] = e
                    # a thread new since the last tick: its cycles so far count as this tick's (not at attach)
                    if not first_pass:
                        sw.writerow([now, tid, c.value, 0, state, wait_reason])
                    continue
                c = ctypes.c_uint64()
                if not k32.QueryThreadCycleTime(e[0], ctypes.byref(c)):
                    continue
                dc, ds = c.value - e[1], (ctxsw - e[2]) & 0xFFFFFFFF
                e[1], e[2], e[6] = c.value, ctxsw, now
                if dc or ds:
                    sw.writerow([now, tid, dc, ds, state, wait_reason])
                if not e[3] and time.monotonic() - e[7] > 2:
                    e[3], e[7] = thread_name(e[0]), time.monotonic()
                if not e[8] and mods:
                    e[8] = locate(e[4], mods)
            idle = (ctypes.c_uint64 * ncpu)()
            k32.QueryIdleProcessorCycleTime(ctypes.byref(idle_n), idle)
            dt = now - t_prev
            span = hz * dt / 1e9
            busy_p = busy_e = busy_all = 0.0
            for i in range(ncpu):
                b = max(0.0, min(1.0, 1.0 - (idle[i] - idle_prev[i]) / span)) if span > 0 else 0.0
                cw.writerow([now, i, f"{b:.3f}"])
                busy_all += b
                if eff.get(i, top) == top:
                    busy_p += b
                else:
                    busy_e += b
            idle_prev = idle
            pc = ctypes.c_uint64()
            k32.QueryProcessCycleTime(hproc, ctypes.byref(pc))
            tw.writerow([now, dt, pc.value - proc_prev.value, len(threads), pdhq.mhz(), f"{busy_p:.2f}", f"{busy_e:.2f}", f"{busy_all:.2f}"])
            proc_prev, t_prev = pc, now
            first_pass = False
    finally:
        if stacks is not None:
            stacks.stop.set()
            stacks.join(5)
            meta["stack_samples"] = stacks.count
        meta["modules"] = [[b, sz, n] for b, sz, n in mods]
        meta["module_paths"] = module_paths
        with open(os.path.join(a.out, "threads.csv"), "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["tid", "name", "start", "first_qpc_ns", "last_qpc_ns"])
            for tid, e in handles.items():
                w.writerow([tid, e[3], e[8] or locate(e[4], mods), e[5], e[6]])
                k32.CloseHandle(e[0])
        meta["end_qpc_ns"] = time.perf_counter_ns()
        with open(os.path.join(a.out, "meta.json"), "w") as f:
            json.dump(meta, f, indent=1)
        samples.close()
        ticks.close()
        cores.close()
    print("threadsample: done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
