"""Watch one game process through its exit: when its exit code is set (ExitProcess called) and when its handle is
signaled (the process is really gone). Times are QPC ns (time.perf_counter_ns(), the clock gamepilot writes its
phases in) plus unix ns, so they line up with pilot-status.json's "exit" phase (the menu quit).

    python exitwatch.py <process name without .exe> <out.json> [--find-timeout 600] [--watch-timeout 2400]

Polls every 20 ms. Writes out.json when the process is gone or the watch times out (the harness ends a game still
alive 30 s after it started exiting, after taking a dump).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
SYNCHRONIZE = 0x00100000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
k32.CloseHandle.argtypes = [wt.HANDLE]


def find(name):
    arr = (wt.DWORD * 4096)()
    needed = wt.DWORD()
    if not psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(needed)):
        return None
    want = name.lower() + ".exe"
    for pid in arr[: needed.value // ctypes.sizeof(wt.DWORD)]:
        if not pid:
            continue
        h = k32.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            continue
        buf = ctypes.create_unicode_buffer(1024)
        n = wt.DWORD(len(buf))
        ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
        if ok and os.path.basename(buf.value).lower() == want:
            return pid, h, buf.value
        k32.CloseHandle(h)
    return None


def stamp():
    return {"qpc_ns": time.perf_counter_ns(), "unix_ns": time.time_ns()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("out")
    ap.add_argument("--find-timeout", type=float, default=600)
    ap.add_argument("--watch-timeout", type=float, default=2400)
    a = ap.parse_args()
    res = {"process": a.name, "started": stamp()}
    t0 = time.monotonic()
    got = None
    while time.monotonic() - t0 < a.find_timeout:
        got = find(a.name)
        if got:
            break
        time.sleep(0.25)
    if not got:
        res["error"] = "process never appeared"
    else:
        pid, h, path = got
        res.update(pid=pid, path=path, found=stamp())
        t1 = time.monotonic()
        code = wt.DWORD()
        while time.monotonic() - t1 < a.watch_timeout:
            if "exit_code_set" not in res and k32.GetExitCodeProcess(h, ctypes.byref(code)) and code.value != STILL_ACTIVE:
                res["exit_code_set"] = stamp()
                res["exit_code"] = "0x%08X" % code.value
            if k32.WaitForSingleObject(h, 0) == 0:
                res["gone"] = stamp()
                if "exit_code_set" not in res and k32.GetExitCodeProcess(h, ctypes.byref(code)):
                    res["exit_code"] = "0x%08X" % code.value
                break
            time.sleep(0.02)
        else:
            res["error"] = "still alive at the end of the watch"
        k32.CloseHandle(h)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
