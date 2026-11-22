"""Screenshots of a flat game window from outside the game (R20 framing check: no UEVR in run A, so no eye frames
and no pose read). Waits for <exe>'s main window, then every --every seconds grabs the display it is on with the
harness's capture\\burstcap.exe (Desktop Duplication where it works, else Windows.Graphics.Capture: both see an
independent-flip D3D12 window, which BitBlt / PrintWindow did not) and writes the window's client area as
<out>\\<qpc_ns>.png, at 1/--scale size. Stops when the game exits, after --seconds, or when <out>\\stop exists.
The PNGs are game pictures: they stay in the local runs folder, never in the repo.

  python flatshots.py --exe b1-Win64-Shipping.exe --out <dir> [--every 30] [--scale 2] [--wait 900]"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import queue
import struct
import subprocess
import threading
import time

import numpy as np
from PIL import Image

u32 = ctypes.WinDLL("user32")
k32 = ctypes.WinDLL("kernel32")
u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: physical pixels, like burstcap
BURSTCAP = os.path.join(os.environ.get("MOPIC_TEST_DIR", ""), "capture", "burstcap.exe")  # the mopic-test harness


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD)]


def find_pid(exe):
    import csv, io
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % exe, "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for row in csv.reader(io.StringIO(out)):
        if len(row) > 1 and row[0].lower() == exe.lower():
            return int(row[1])
    return 0


def main_window(pid):
    found = []
    PROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h) and u32.GetWindow(h, 4) == 0:  # GW_OWNER
            r = wt.RECT()
            u32.GetClientRect(h, ctypes.byref(r))
            if r.right - r.left >= 640:
                found.append(h)
        return True
    u32.EnumWindows(PROC(cb), 0)
    return found[0] if found else None


def client_on_screen(h):
    r = wt.RECT()
    u32.GetClientRect(h, ctypes.byref(r))
    pt = wt.POINT(0, 0)
    u32.ClientToScreen(h, ctypes.byref(pt))
    return pt.x, pt.y, r.right - r.left, r.bottom - r.top


def monitor_rect(h):
    m = u32.MonitorFromWindow(h, 2)
    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(mi)
    u32.GetMonitorInfoW(m, ctypes.byref(mi))
    r = mi.rcMonitor
    return r.left, r.top, r.right - r.left, r.bottom - r.top


def read_burst(path):
    with open(path, "rb") as f:
        magic, ver, w, h, n = struct.unpack("<4sIIII", f.read(20))
        frames = []
        for _ in range(n):
            grab, present, refreshes, _z = struct.unpack("<qqII", f.read(24))
            px = np.frombuffer(f.read(w * h * 3), np.uint8).reshape(h, w, 3)[:, :, ::-1]
            frames.append((grab, px))
    return frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--every", type=float, default=30)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--wait", type=float, default=900)
    ap.add_argument("--seconds", type=float, default=1800)
    ap.add_argument("--pid", type=int, default=0, help="test mode: this process instead of --exe")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = open(os.path.join(a.out, "flatshots.log"), "a", buffering=1)
    t_end = time.time() + a.wait
    pid = a.pid
    while time.time() < t_end and pid == 0:
        pid = find_pid(a.exe)
        time.sleep(2)
    def alive():
        if a.pid:  # test mode: a given process
            h = k32.OpenProcess(0x100000, False, a.pid)  # SYNCHRONIZE
            gone = not h or k32.WaitForSingleObject(h, 0) == 0
            if h:
                k32.CloseHandle(h)
            return not gone
        return find_pid(a.exe) == pid
    if pid == 0:
        log.write("no %s within %.0f s\n" % (a.exe, a.wait))
        return
    proc = None
    mon = None
    stop_at = time.time() + a.seconds
    nxt = time.time() + a.every
    while time.time() < stop_at and not os.path.exists(os.path.join(a.out, "stop")):
        if not alive():
            log.write("game gone\n")
            break
        time.sleep(max(0.0, nxt - time.time()))
        nxt += a.every
        h = main_window(pid)
        if not h:
            continue
        m = monitor_rect(h)
        if proc is None or m != mon:
            if proc is not None:
                try:
                    proc.stdin.write(b"quit\n"); proc.stdin.flush()
                except OSError:
                    pass
            mon = m
            proc = subprocess.Popen([BURSTCAP, "--rect", "%d,%d,%d,%d" % m, "--scale", str(a.scale)],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            lines = queue.Queue()
            threading.Thread(target=lambda p=proc, q=lines: [q.put(l.decode(errors="replace").strip()) for l in iter(p.stdout.readline, b"")],
                             daemon=True).start()
            try:
                ready = lines.get(timeout=10)  # "ready <source> <w> <h>" or an error
            except queue.Empty:
                ready = "no answer"
            log.write("burstcap on display %s: %s\n" % (m, ready))
            if not ready.startswith("ready"):
                proc = None
                continue
        tmp = os.path.join(a.out, "burst.tmp")
        try:
            proc.stdin.write(("burst 2 %s\n" % tmp).encode()); proc.stdin.flush()
            ans = lines.get(timeout=8)  # a still picture never completes a burst: give up on this one
        except queue.Empty:
            log.write("burst: no new frames within 8 s\n")
            try:
                proc.kill()
            except OSError:
                pass
            proc = None
            continue
        except OSError as e:
            log.write("burstcap failed: %s\n" % e)
            proc = None
            continue
        if not ans.startswith("ok"):
            log.write("burst: %s\n" % ans)
            continue
        cx, cy, cw, ch = client_on_screen(h)
        frames = read_burst(tmp)
        if not frames:
            log.write("burst: %s (no new picture)\n" % ans)
            continue
        grab, px = frames[-1]
        s = a.scale
        x0, y0 = (cx - m[0]) // s, (cy - m[1]) // s
        crop = px[max(0, y0):max(0, y0) + ch // s, max(0, x0):max(0, x0) + cw // s]
        Image.fromarray(np.ascontiguousarray(crop)).save(os.path.join(a.out, "%d.png" % grab))
        log.write("%d %s client %d,%d %dx%d\n" % (grab, ans, cx, cy, cw, ch))
        log.flush()
    if proc is not None:
        try:
            proc.stdin.write(b"quit\n"); proc.stdin.flush()
            proc.wait(5)
        except (OSError, subprocess.TimeoutExpired):
            proc.kill()
    tmp =os.path.join(a.out, "burst.tmp")
    if os.path.exists(tmp):
        os.remove(tmp)


if __name__ == "__main__":
    main()
