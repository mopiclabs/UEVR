"""gamepilot: look at a game window and drive it with keyboard/mouse input.

Used by run-test.ps1 (-Recipe) and interactively by Claude Code to get games from their title screen into
gameplay and back out through their menus. README.md has the main commands, the recipe format and the discovery
workflow; `python gamepilot.py - -h` / `<process> <command> -h` list all commands and options.

<process> is the exe name without ".exe" ("-" for commands that don't touch a game). Coordinates for click/move
are in the last screenshot's pixels (the shot is scaled to --max-width); the mapping is stored next to it.

Keys are sent with SendInput using scan codes, so games that read DirectInput / Raw Input see them.
The game window is brought to the foreground first (input only reaches the focused window).
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import math
import os
import random
import re
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi")

# Physical pixels everywhere (the displays here run at 200% scaling).
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2
except Exception:
    user32.SetProcessDPIAware()

STATE_DIR = os.path.join(os.environ.get("TEMP", "."), "gamepilot")
os.makedirs(STATE_DIR, exist_ok=True)

# ---------------------------------------------------------------------------------------------------------------
# windows

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def process_name(pid):
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wt.DWORD(len(buf))
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return os.path.splitext(os.path.basename(buf.value))[0]
    finally:
        kernel32.CloseHandle(h)
    return None


def window_rect(hwnd):
    rect = wt.RECT()
    # DWMWA_EXTENDED_FRAME_BOUNDS = 9 (without the invisible resize borders)
    if dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(rect), ctypes.sizeof(rect)) != 0:
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


class WINDOWPLACEMENT(ctypes.Structure):
    _fields_ = [("length", wt.UINT), ("flags", wt.UINT), ("showCmd", wt.UINT), ("ptMinPosition", wt.POINT),
                ("ptMaxPosition", wt.POINT), ("rcNormalPosition", wt.RECT)]


def find_window(proc):
    """Largest visible top-level window owned by the process."""
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        name = process_name(pid.value)
        if name and name.lower() == proc.lower():
            l, t, r, b = window_rect(hwnd)
            if user32.IsIconic(hwnd):
                placement = WINDOWPLACEMENT()
                placement.length = ctypes.sizeof(placement)
                if user32.GetWindowPlacement(hwnd, ctypes.byref(placement)):
                    n = placement.rcNormalPosition
                    l, t, r, b = n.left, n.top, n.right, n.bottom
            if r - l > 100 and b - t > 100:
                found.append(((r - l) * (b - t), hwnd))
        return True

    user32.EnumWindows(cb, 0)
    if not found:
        return None
    found.sort(reverse=True)
    return found[0][1]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


def processes():
    """(pid, lower-case exe name) of all processes (Toolhelp32 works without opening the processes)."""
    procs = []
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snap in (0, -1):
        return procs
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            procs.append((entry.th32ProcessID, entry.szExeFile.lower()))
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return procs


def process_names():
    return {name for _, name in processes()}


def process_threads():
    """{pid: thread count} of all processes."""
    counts = {}
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snap in (0, -1):
        return counts
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            counts[entry.th32ProcessID] = entry.cntThreads
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return counts


def process_alive(pid):
    """An exited process stays in the process list while anyone (the harness) holds a handle to it."""
    h = kernel32.OpenProcess(0x00100000 | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)  # SYNCHRONIZE
    if not h:
        return True  # can't tell; assume it's still there
    try:
        return kernel32.WaitForSingleObject(h, 0) == 0x102  # WAIT_TIMEOUT: not signaled, still running
    finally:
        kernel32.CloseHandle(h)


def is_locked():
    """True while Windows shows the lock screen (input and capture don't reach the user desktop then)."""
    if "logonui.exe" in process_names():
        return True

    desk = user32.OpenInputDesktop(0, False, 0)
    if not desk:
        return True
    try:
        name = ctypes.create_unicode_buffer(256)
        needed = wt.DWORD()
        user32.GetUserObjectInformationW(desk, 2, name, ctypes.sizeof(name), ctypes.byref(needed))  # UOI_NAME
        return name.value.lower() != "default"
    finally:
        user32.CloseDesktop(desk)


def focus(hwnd):
    if user32.GetForegroundWindow() == hwnd:
        return True
    fg = user32.GetForegroundWindow()
    cur_tid = kernel32.GetCurrentThreadId()
    fg_tid = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    target_tid = user32.GetWindowThreadProcessId(hwnd, None)
    # An ALT tap lets a background process take the foreground.
    send_keys_raw([(0x38, False, True)], hold_ms=0)
    send_keys_raw([(0x38, False, False)], hold_ms=0)
    if fg_tid:
        user32.AttachThreadInput(cur_tid, fg_tid, True)
    user32.AttachThreadInput(cur_tid, target_tid, True)
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(cur_tid, target_tid, False)
    if fg_tid:
        user32.AttachThreadInput(cur_tid, fg_tid, False)
    time.sleep(0.15)
    return user32.GetForegroundWindow() == hwnd

# ---------------------------------------------------------------------------------------------------------------
# input

INPUT_MOUSE, INPUT_KEYBOARD = 0, 1
KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE = 0x1, 0x2, 0x8
MOUSEEVENTF_MOVE, MOUSEEVENTF_ABSOLUTE, MOUSEEVENTF_VIRTUALDESK = 0x1, 0x8000, 0x4000
MOUSEEVENTF_WHEEL = 0x0800
MOUSE_BUTTONS = {"left": (0x2, 0x4), "right": (0x8, 0x10), "middle": (0x20, 0x40)}

ULONG_PTR = ctypes.c_size_t


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ULONG_PTR)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


def _send(inputs):
    arr = (INPUT * len(inputs))(*inputs)
    user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))


# name -> (scan code, extended)
SCAN = {
    "esc": (0x01, False), "escape": (0x01, False), "enter": (0x1C, False), "return": (0x1C, False),
    "space": (0x39, False), "tab": (0x0F, False), "backspace": (0x0E, False),
    "lshift": (0x2A, False), "shift": (0x2A, False), "lctrl": (0x1D, False), "ctrl": (0x1D, False),
    "lalt": (0x38, False), "alt": (0x38, False),
    "up": (0x48, True), "down": (0x50, True), "left": (0x4B, True), "right": (0x4D, True),
    "home": (0x47, True), "end": (0x4F, True), "pageup": (0x49, True), "pagedown": (0x51, True),
    "insert": (0x52, True), "delete": (0x53, True),
    "f1": (0x3B, False), "f2": (0x3C, False), "f3": (0x3D, False), "f4": (0x3E, False), "f5": (0x3F, False),
    "f6": (0x40, False), "f7": (0x41, False), "f8": (0x42, False), "f9": (0x43, False), "f10": (0x44, False),
    "f11": (0x57, False), "f12": (0x58, False),
    "minus": (0x0C, False), "equals": (0x0D, False), "tilde": (0x29, False), "grave": (0x29, False),
}
for i, ch in enumerate("1234567890"):
    SCAN[ch] = (0x02 + i, False)
for row, start in (("qwertyuiop", 0x10), ("asdfghjkl", 0x1E), ("zxcvbnm", 0x2C)):
    for i, ch in enumerate(row):
        SCAN[ch] = (start + i, False)


def send_keys_raw(events, hold_ms=60):
    """events: [(scan, extended, down)]"""
    for scan, ext, down in events:
        flags = KEYEVENTF_SCANCODE | (KEYEVENTF_EXTENDEDKEY if ext else 0) | (0 if down else KEYEVENTF_KEYUP)
        _send([INPUT(INPUT_KEYBOARD, _INPUTUNION(ki=KEYBDINPUT(0, scan, flags, 0, 0)))])
        if down and hold_ms:
            time.sleep(hold_ms / 1000.0)


def mouse_look(dx, dy, ms):
    """Relative mouse motion of (dx, dy) mickeys spread over `ms` (camera turns: games read these as raw input)."""
    n = max(1, int(ms) // 10)
    for i in range(n):
        sx, sy = dx * (i + 1) // n - dx * i // n, dy * (i + 1) // n - dy * i // n
        _send([INPUT(INPUT_MOUSE, _INPUTUNION(mi=MOUSEINPUT(sx, sy, 0, MOUSEEVENTF_MOVE, 0, 0)))])
        time.sleep(max(0, int(ms)) / 1000.0 / n)


def press(name, hold_ms=80):
    """name, or a chord like 'alt+f4' / 'shift+tab'; 'lmb' / 'rmb' click where the mouse is; 'look:dx,dy' moves
    the mouse by that much (relative, spread over the hold time: a camera turn); 'wheel:N' turns the mouse wheel N
    notches (negative: down / towards the end of a list) where the mouse is."""
    if name.lower().startswith("wheel:"):
        try:
            notches = int(name[6:])
        except ValueError:
            raise SystemExit(f"bad mouse wheel '{name}' (wheel:N, e.g. wheel:-5)")
        for _ in range(abs(notches)):
            delta = (120 if notches > 0 else -120) & 0xFFFFFFFF
            _send([INPUT(INPUT_MOUSE, _INPUTUNION(mi=MOUSEINPUT(0, 0, delta, MOUSEEVENTF_WHEEL, 0, 0)))])
            time.sleep(0.06)
        return
    if name.lower().startswith("look:"):
        try:
            dx, dy = (int(v) for v in name[5:].split(","))
        except ValueError:
            raise SystemExit(f"bad mouse look '{name}' (look:dx,dy, e.g. look:-600,0)")
        mouse_look(dx, dy, hold_ms)
        return
    if name.lower() in ("lmb", "rmb"):
        down, up = MOUSE_BUTTONS["left" if name.lower() == "lmb" else "right"]
        _send([INPUT(INPUT_MOUSE, _INPUTUNION(mi=MOUSEINPUT(0, 0, 0, down, 0, 0)))])
        time.sleep(hold_ms / 1000.0)
        _send([INPUT(INPUT_MOUSE, _INPUTUNION(mi=MOUSEINPUT(0, 0, 0, up, 0, 0)))])
        return
    parts = [p.strip().lower() for p in name.split("+")]
    for p in parts:
        if p not in SCAN:
            raise SystemExit(f"unknown key '{p}' (known: {', '.join(sorted(SCAN))})")
    for p in parts[:-1]:
        send_keys_raw([(SCAN[p][0], SCAN[p][1], True)], hold_ms=20)
    scan, ext = SCAN[parts[-1]]
    send_keys_raw([(scan, ext, True)], hold_ms=hold_ms)
    send_keys_raw([(scan, ext, False)], hold_ms=0)
    for p in reversed(parts[:-1]):
        send_keys_raw([(SCAN[p][0], SCAN[p][1], False)], hold_ms=0)


def mouse_to(x, y):
    vx, vy = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)   # SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN
    vw, vh = user32.GetSystemMetrics(78), user32.GetSystemMetrics(79)   # SM_CXVIRTUALSCREEN / SM_CYVIRTUALSCREEN
    nx = int(round((x - vx) * 65535 / max(1, vw - 1)))
    ny = int(round((y - vy) * 65535 / max(1, vh - 1)))
    _send([INPUT(INPUT_MOUSE, _INPUTUNION(mi=MOUSEINPUT(nx, ny, 0,
          MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, 0, 0)))])


def click_at(x, y, button="left", double=False):
    mouse_to(x, y)
    time.sleep(0.05)
    down, up = MOUSE_BUTTONS[button]
    for _ in range(2 if double else 1):
        _send([INPUT(INPUT_MOUSE, _INPUTUNION(mi=MOUSEINPUT(0, 0, 0, down, 0, 0)))])
        time.sleep(0.06)
        _send([INPUT(INPUT_MOUSE, _INPUTUNION(mi=MOUSEINPUT(0, 0, 0, up, 0, 0)))])
        time.sleep(0.08)

# ---------------------------------------------------------------------------------------------------------------
# screenshots

def mopic_monitor():
    """The Mopic display (EDID manufacturer MPL), where the runtime shows what the viewer sees."""
    import mss
    with mss.MSS() as sct:
        for m in sct.monitors[1:]:
            if "MPL" in (m.get("unique_id") or "").upper() or "MPL" in (m.get("name") or "").upper():
                return m["left"], m["top"], m["left"] + m["width"], m["top"] + m["height"]
    return None


MOPIC_SOURCES = ("mopic", "mopic-sbs")


def sbs_left_eye(img, width, height):
    """A capture of the Mopic display while monado-service runs with MOPIC_MODE=sbs (left eye | right eye, each
    squeezed into half the width) -> the left eye stretched back to width x height (the full display's shape), so
    it lines up with what the checkpoints were recorded on (the woven display, where UI at the zero-parallax plane
    sits where the 2D picture has it)."""
    from PIL import Image
    return img.crop((0, 0, img.width // 2, img.height)).resize((width, height), Image.LANCZOS)


def grab(hwnd, max_width, source="window"):
    """(image, meta) of the game window, or of the Mopic display with source="mopic" ("mopic-sbs": its left eye
    while monado-service shows side by side)."""
    import mss
    from PIL import Image
    l, t, r, b = window_rect(hwnd)
    if source in MOPIC_SOURCES:
        m = mopic_monitor()
        if m is None:
            raise SystemExit("no Mopic display found")
        l, t, r, b = m
    with mss.MSS() as sct:
        raw = sct.grab({"left": l, "top": t, "width": r - l, "height": b - t})
    img = Image.frombytes("RGB", raw.size, raw.bgra, "raw", "BGRX")
    scale = 1.0
    if img.width > max_width:
        scale = max_width / img.width
    if source == "mopic-sbs":
        img = sbs_left_eye(img, max_width if scale != 1.0 else img.width, int(img.height * scale))
    elif scale != 1.0:
        img = img.resize((max_width, int(img.height * scale)), Image.LANCZOS)
    meta = {"rect": [l, t, r, b], "scale": scale, "size": [img.width, img.height],
            "source": source, "window": list(window_rect(hwnd)), "time": time.time()}
    return img, meta

# ---------------------------------------------------------------------------------------------------------------
# measurement: the segments perfreport.py cuts the frame-rate data to. Every source is timed in
# QueryPerformanceCounter time scaled to nanoseconds ("QPC ns"): time.perf_counter_ns() here, steady_clock in UEVR,
# os_monotonic_get_ns in monado, PresentMon's --qpc_time (raw ticks, converted with the frequency stored below).

def qpc_ns():
    return time.perf_counter_ns()


def qpc_freq():
    freq = ctypes.c_int64()
    kernel32.QueryPerformanceFrequency(ctypes.byref(freq))
    return freq.value


class DISPLAY_DEVICEW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("DeviceName", wt.WCHAR * 32), ("DeviceString", wt.WCHAR * 128),
                ("StateFlags", wt.DWORD), ("DeviceID", wt.WCHAR * 128), ("DeviceKey", wt.WCHAR * 128)]


class DEVMODEW(ctypes.Structure):
    # the display variant of the union after dmFields (dmPosition, dmDisplayOrientation, dmDisplayFixedOutput)
    _fields_ = [("dmDeviceName", wt.WCHAR * 32), ("dmSpecVersion", wt.WORD), ("dmDriverVersion", wt.WORD),
                ("dmSize", wt.WORD), ("dmDriverExtra", wt.WORD), ("dmFields", wt.DWORD),
                ("dmPositionX", wt.LONG), ("dmPositionY", wt.LONG), ("dmDisplayOrientation", wt.DWORD),
                ("dmDisplayFixedOutput", wt.DWORD), ("dmColor", ctypes.c_short), ("dmDuplex", ctypes.c_short),
                ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short), ("dmCollate", ctypes.c_short),
                ("dmFormName", wt.WCHAR * 32), ("dmLogPixels", wt.WORD), ("dmBitsPerPel", wt.DWORD),
                ("dmPelsWidth", wt.DWORD), ("dmPelsHeight", wt.DWORD), ("dmDisplayFlags", wt.DWORD),
                ("dmDisplayFrequency", wt.DWORD), ("dmICMMethod", wt.DWORD), ("dmICMIntent", wt.DWORD),
                ("dmMediaType", wt.DWORD), ("dmDitherType", wt.DWORD), ("dmReserved1", wt.DWORD),
                ("dmReserved2", wt.DWORD), ("dmPanningWidth", wt.DWORD), ("dmPanningHeight", wt.DWORD)]


class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD),
                ("szDevice", wt.WCHAR * 32)]


class SYSTEM_POWER_STATUS(ctypes.Structure):
    _fields_ = [("ACLineStatus", ctypes.c_ubyte), ("BatteryFlag", ctypes.c_ubyte), ("BatteryLifePercent", ctypes.c_ubyte),
                ("SystemStatusFlag", ctypes.c_ubyte), ("BatteryLifeTime", wt.DWORD), ("BatteryFullLifeTime", wt.DWORD)]


# monitor handles are pointer-sized
user32.MonitorFromWindow.restype = ctypes.c_void_p
user32.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]
user32.GetMonitorInfoW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]


def displays():
    """Every display on the desktop: GDI name, the adapter that scans it out (on this laptop the Mopic display and
    the internal panel hang off the Intel iGPU, the 240 Hz monitor off the NVIDIA GPU), monitor id (EDID: MPL0291 is
    the Mopic display), current mode."""
    out = []
    i = 0
    while True:
        dev = DISPLAY_DEVICEW()
        dev.cb = ctypes.sizeof(dev)
        if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(dev), 0):
            break
        i += 1
        if not dev.StateFlags & 0x1:  # DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
            continue
        entry = {"device": dev.DeviceName, "adapter": dev.DeviceString, "monitor": "", "primary": bool(dev.StateFlags & 0x4)}
        mon = DISPLAY_DEVICEW()
        mon.cb = ctypes.sizeof(mon)
        if user32.EnumDisplayDevicesW(dev.DeviceName, 0, ctypes.byref(mon), 0):
            parts = mon.DeviceID.split("\\")  # MONITOR\MPL0291\{4d36e96e-...}\0006
            entry["monitor"] = parts[1] if len(parts) > 1 else mon.DeviceID
        mode = DEVMODEW()
        mode.dmSize = ctypes.sizeof(mode)
        if user32.EnumDisplaySettingsW(dev.DeviceName, ctypes.c_uint32(0xFFFFFFFF), ctypes.byref(mode)):  # ENUM_CURRENT_SETTINGS
            entry.update(width=mode.dmPelsWidth, height=mode.dmPelsHeight, refresh_hz=mode.dmDisplayFrequency,
                         left=mode.dmPositionX, top=mode.dmPositionY)
        out.append(entry)
    return out


def window_display(hwnd):
    """The display (see displays()) that has most of the window, plus the window's rect; None if unknown."""
    try:
        hmon = user32.MonitorFromWindow(hwnd, 2)  # MONITOR_DEFAULTTONEAREST
        info = MONITORINFOEXW()
        info.cbSize = ctypes.sizeof(info)
        if not hmon or not user32.GetMonitorInfoW(hmon, ctypes.byref(info)):
            return None
        found = next((d for d in displays() if d["device"] == info.szDevice), {"device": info.szDevice})
        return dict(found, window=list(window_rect(hwnd)))
    except Exception:  # noqa: BLE001 - context only, never fail a step over it
        return None


def power_status():
    """AC or battery (a laptop: the GPU's power limit moves with it)."""
    s = SYSTEM_POWER_STATUS()
    if not kernel32.GetSystemPowerStatus(ctypes.byref(s)):
        return None
    return {"ac": None if s.ACLineStatus == 255 else s.ACLineStatus == 1,
            "battery_pct": None if s.BatteryLifePercent == 255 else s.BatteryLifePercent,
            "saver": s.SystemStatusFlag == 1}


def shot(hwnd, out, max_width, source="window"):
    img, meta = grab(hwnd, max_width, source)
    img.save(out)
    meta["path"] = os.path.abspath(out)
    with open(os.path.join(STATE_DIR, "last_shot.json"), "w") as f:
        json.dump(meta, f)
    return meta


def shot_to_screen(x, y, meta=None):
    """Map a point in a screenshot (default: the last one) to the game window on screen. A Mopic-display shot
    shows the game image letterboxed/pillarboxed into the display, so map through the image's fitted area."""
    if meta is None:
        with open(os.path.join(STATE_DIR, "last_shot.json")) as f:
            meta = json.load(f)
    l, t, r, b = meta["rect"]
    sx, sy = l + x / meta["scale"], t + y / meta["scale"]
    if meta.get("source") not in MOPIC_SOURCES:
        return int(sx), int(sy)
    wl, wt_, wr, wb = meta["window"]
    ww, wh, dw, dh = wr - wl, wb - wt_, r - l, b - t
    fit = min(dw / ww, dh / wh)
    ox, oy = l + (dw - ww * fit) / 2, t + (dh - wh * fit) / 2
    return int(wl + (sx - ox) / fit), int(wt_ + (sy - oy) / fit)

# ---------------------------------------------------------------------------------------------------------------
# checkpoints: a region of a reference screenshot that identifies a screen (a menu title, a HUD element).
# The reference is stored at screenshot scale (the recipe's max_width), so live shots compare 1:1.

MATCH_WIDTH = 64   # compare at this width (grayscale), plenty for menu titles and HUD elements


def mean_color(img):
    from PIL import ImageStat
    return ImageStat.Stat(img.convert("RGB")).mean


def match_score(img, ref, region, margin=8, mode="shape"):
    """How well img's region matches the reference crop (1 = same).

    shape (default): best normalized cross-correlation, allowing a shift of up to `margin` screenshot pixels, and
    the brightness at that alignment has to agree too. Flat references (no texture) use the mean abs difference.
    highlight: for a menu cursor. The highlight bar's colour decides (it pulses and shimmers, which makes the
    correlation useless against the same item unselected); the text only has to roughly line up."""
    from PIL import Image
    if mode == "highlight":
        x, y, w, h = region
        live, want = mean_color(img.crop((x, y, x + w, y + h))), mean_color(ref)
        dist = max(abs(a - b) for a, b in zip(live, want))
        color = 1.0 - max(0.0, dist - 40.0) / 40.0
        return color if _match(img, ref, region, margin, ncc_only=True) >= 0.4 else 0.0
    return _match(img, ref, region, margin)


def _match(img, ref, region, margin, ncc_only=False):
    import numpy as np
    from numpy.lib.stride_tricks import sliding_window_view
    from PIL import Image
    x, y, w, h = region
    k = max(1.0, w / MATCH_WIDTH)
    rw, rh = max(4, int(round(w / k))), max(4, int(round(h / k)))
    kx, ky = w / rw, h / rh                      # both images are sampled on the same grid
    m = max(1, int(math.ceil(margin / k)))
    r = np.asarray(ref.convert("L").resize((rw, rh), Image.BILINEAR, box=(0, 0, w, h)), dtype=np.float64)
    rc = r - r.mean()
    rnorm = math.sqrt(float((rc * rc).sum()))
    flat = rnorm < 3.0 * math.sqrt(rw * rh)     # no texture: compare brightness only
    gray = img.convert("L")
    aw, ah = rw + 2 * m, rh + 2 * m
    mx, my = m * kx, m * ky
    best, best_mad = -math.inf, 0.0
    # Shifts are tried on the downsampled grid; a second pass half a cell over (when cells are more than 2 px)
    # keeps every offset up to `margin` within a quarter cell of a tried one.
    for px in ((0.0, 0.5) if kx > 2 else (0.0,)):
        for py in ((0.0, 0.5) if ky > 2 else (0.0,)):
            bx, by = x - mx + px * kx, y - my + py * ky
            x0, y0 = int(math.floor(bx)), int(math.floor(by))
            x1, y1 = int(math.ceil(bx + w + 2 * mx)), int(math.ceil(by + h + 2 * my))
            area = gray.crop((x0, y0, x1, y1))   # pads outside the image with black
            box = (bx - x0, by - y0, bx - x0 + w + 2 * mx, by - y0 + h + 2 * my)
            a = np.asarray(area.resize((aw, ah), Image.BILINEAR, box=box), dtype=np.float64)
            wins = sliding_window_view(a, (rh, rw))                     # (2m+1, 2m+1, rh, rw)
            # the half-cell pass stays inside +-m cells (its last window would be m + 1/2 cells out)
            wins = wins[:wins.shape[0] - (py > 0), :wins.shape[1] - (px > 0)]
            mads = np.abs(wins - r).mean(axis=(2, 3))
            if flat:
                scores = 1.0 - mads / 64.0
            else:
                wc = wins - wins.mean(axis=(2, 3), keepdims=True)
                wn = np.sqrt((wc * wc).sum(axis=(2, 3)))
                num = (wc * rc).sum(axis=(2, 3))
                scores = np.where(wn > 0, num / np.maximum(wn * rnorm, 1e-9), 0.0)
            i = np.unravel_index(int(np.argmax(scores)), scores.shape)
            if scores[i] > best:
                best, best_mad = float(scores[i]), float(mads[i])
    if ncc_only:
        return best
    # Correlation ignores brightness, so a highlighted menu item (same text on a bright bar) would still match
    # an unselected one; the brightness difference at the best alignment has to be small as well.
    return min(best, 1.0 - max(0.0, best_mad - 20.0) / 40.0)


def load_recipe(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


VAR_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
# key names and free text: a ${name} there always becomes text
TEXT_FIELDS = {"key", "keys", "press", "idle_press", "note", "shot", "phase"}


def parse_vars(pairs):
    """--var name=value (repeatable) -> {name: value}"""
    variables = {}
    for pair in pairs or []:
        name, sep, value = pair.partition("=")
        name = name.strip()
        if not sep or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise SystemExit(f"--var expects name=value, got {pair!r}")
        variables[name] = value
    return variables


def resolve_vars(recipe, given):
    """The recipe's "vars" ({name: default}, null = no default) filled with the --var values:
    ({name: (text, value)}, [problems]). A --var value is read as JSON when it parses (180, [110, 305], true,
    ["hud", "loading"]), else as text; a default is taken as written. Only declared names are vars: a recipe without
    "vars" takes no --var and its steps are never changed (a literal ${...} in them stays)."""
    declared = recipe.get("vars")
    if declared is None:
        declared = {}  # "vars": null is no vars, as run-test.ps1 / run-ladder.ps1 read it
    if not isinstance(declared, dict):
        return {}, ['the recipe\'s "vars" must be an object {name: default}']
    problems = []
    unknown = sorted(n for n in given if n not in declared)
    if unknown:
        problems.append(f"--var {', '.join(unknown)}: the recipe declares no such var"
                        f" (its \"vars\": {', '.join(declared) or 'none'})")
    resolved = {}
    for name, default in declared.items():
        if name in given:
            text = given[name]
            try:
                value = json.loads(text)
            except ValueError:
                value = text
        elif default is None:
            continue  # required: an error only where a step to run uses it
        else:
            value = default
            text = default if isinstance(default, str) else json.dumps(default, ensure_ascii=False)
        resolved[name] = (text, value)
    return resolved, problems


def substitute_vars(value, resolved, declared, unfilled, field=None):
    """${name} of a declared var in the steps' strings -> its value; any other ${...} stays as written. A string
    that is only "${name}" becomes the value itself (a number for "play" / "timeout", a list for "click" /
    "wait_for"), except in key and text fields; inside a longer string the value is text. Declared names without a
    value are added to `unfilled` and left as they are."""
    if isinstance(value, dict):
        return {k: substitute_vars(v, resolved, declared, unfilled, k) for k, v in value.items()}
    if isinstance(value, list):
        return [substitute_vars(v, resolved, declared, unfilled, field) for v in value]
    if not isinstance(value, str) or "${" not in value:
        return value
    whole = VAR_RE.fullmatch(value)
    if whole and whole.group(1) in resolved and field not in TEXT_FIELDS:
        return resolved[whole.group(1)][1]

    def repl(m):
        name = m.group(1)
        if name not in declared:
            return m.group(0)
        if name not in resolved:
            unfilled.add(name)
            return m.group(0)
        return resolved[name][0]
    return VAR_RE.sub(repl, value)


def recipe_steps_with_vars(recipe, given):
    """-> (steps, {name: text}, [problems], [set of unfilled names per step]). Without "vars" the steps are the
    recipe's own list, untouched."""
    steps = recipe.get("steps", [])
    resolved, problems = resolve_vars(recipe, given)
    declared = recipe.get("vars") if isinstance(recipe.get("vars"), dict) else {}
    unfilled = [set() for _ in steps]
    if declared:
        steps = [substitute_vars(s, resolved, declared, unfilled[i]) for i, s in enumerate(steps)]
    return steps, {n: t for n, (t, _) in resolved.items()}, problems, unfilled


def save_recipe(path, recipe):
    """One checkpoint / step per line, so recipes stay readable and diffable."""
    lines = ["{"]
    head = [k for k in recipe if k not in ("checkpoints", "steps")]
    for k in head:
        lines.append(f"  {json.dumps(k)}: {json.dumps(recipe[k], ensure_ascii=False)},")
    cps = recipe.get("checkpoints", {})
    lines.append('  "checkpoints": {')
    lines += [f"    {json.dumps(n)}: {json.dumps(c, ensure_ascii=False)}," for n, c in cps.items()]
    if cps:
        lines[-1] = lines[-1].rstrip(",")
    lines.append("  },")
    steps = recipe.get("steps", [])
    lines.append('  "steps": [')
    lines += [f"    {json.dumps(s, ensure_ascii=False)}," for s in steps]
    if steps:
        lines[-1] = lines[-1].rstrip(",")
    lines += ["  ]", "}", ""]
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    os.replace(tmp, path)


def checkpoint_dir(recipe_path, recipe=None):
    """recipes/<Game>.json keeps its images in recipes/<Game>/ ("images" points a variant at another folder)."""
    if recipe and recipe.get("images"):
        return os.path.join(os.path.dirname(recipe_path), recipe["images"])
    return os.path.splitext(recipe_path)[0]


def checkpoint_score(recipe_path, recipe, name, img):
    from PIL import Image
    cp = recipe["checkpoints"][name]
    ref = Image.open(os.path.join(checkpoint_dir(recipe_path, recipe), cp["image"]))
    if list(ref.size) != list(cp["region"][2:]):
        raise ValueError(f"checkpoint {name}: image is {ref.size[0]}x{ref.size[1]} but the region is "
                         f"{cp['region'][2]}x{cp['region'][3]} (re-save it)")
    return match_score(img, ref, cp["region"], cp.get("margin", 8), cp.get("mode", "shape"))

# ---------------------------------------------------------------------------------------------------------------
# recipes: a recorded way through a game's menus into gameplay and back out, replayed with checkpoints.
# The format (keys, steps, checkpoint modes) is described in README.md.

def recipe_fit(recipe, source):
    """A recipe recorded on the Mopic display replayed from the desktop window (the game without UEVR):
    "window_fit" is where the window's image sits in the Mopic-display screenshot, so checkpoints still match."""
    return recipe.get("window_fit") if source == "window" and recipe.get("source") == "mopic" else None


def recipe_grab(hwnd, recipe, source=None):
    """Screenshot in the recipe's coordinates."""
    source = source or recipe.get("source", "window")
    max_width = recipe.get("max_width", 1280)
    img, meta = grab(hwnd, max_width, source)
    fit = recipe_fit(recipe, source)
    if fit:
        from PIL import Image
        x, y, w, h = fit
        canvas = Image.new("RGB", (max_width, recipe.get("height", 720)))
        canvas.paste(img.resize((w, h), Image.LANCZOS), (x, y))
        img = canvas
    return img, meta


class GameQuit(Exception):
    pass


class GameExiting(Exception):
    """The window went away after the quit started, the process is still shutting down."""


class Failed(Exception):
    def __init__(self, kind, msg):
        super().__init__(msg)
        self.kind = kind


class Pilot:
    def __init__(self, proc, recipe_path, out_dir, status_path, source=None, variables=None):
        self.proc = proc
        self.recipe_path = os.path.abspath(recipe_path)
        self.recipe = load_recipe(recipe_path)
        self.recipe["steps"], self.variables, self.var_problems, self.unfilled_vars = \
            recipe_steps_with_vars(self.recipe, dict(variables or {}))
        self.source = source or self.recipe.get("source", "window")
        self.fit = recipe_fit(self.recipe, self.source)
        self.max_width = self.recipe.get("max_width", 1280)
        self.out_dir = out_dir
        os.makedirs(out_dir, exist_ok=True)
        self.status_path = status_path
        self.logf = open(os.path.join(out_dir, "pilot.log"), "a", encoding="utf-8")
        # clock: a QPC / wall-clock pair taken together, so perfreport.py can place wall-clock data (nvidia-smi)
        # on the QPC time line; phases and segments carry QPC times (segments: the measured steps, see do())
        unix_ns = time.time_ns()
        self.status = {"state": "running", "recipe": self.recipe_path, "step": 0, "desc": "", "phase": "",
                       "exit_expected": False, "reached": [], "error": None, "error_kind": None,
                       "vars": self.variables, "started": time.time(), "updated": time.time(),
                       "clock": {"qpc_ns": qpc_ns(), "unix_ns": unix_ns, "qpc_freq": qpc_freq()},
                       "phases": [], "segments": [], "displays": []}
        try:
            self.status["displays"] = displays()
        except Exception:  # noqa: BLE001 - context only
            pass
        self.rng = random.Random(0)  # re-seeded per step in run()
        self.segment = None       # the open segment (a dict in status["segments"])
        self.segment_spans = []    # its screen captures: (start, end) QPC ns, written to captures.csv at its end
        self.segment_cpu = 0.0
        self.write_status()

    def log(self, msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        self.logf.write(line + "\n")
        self.logf.flush()

    def write_status(self, **kw):
        self.status.update(kw, updated=time.time())
        if not self.status_path:
            return
        tmp = self.status_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.status, f, indent=2, ensure_ascii=True)
        # the replace is refused while someone else has the file open (the harness reading it, a virus scan)
        for attempt in range(20):
            try:
                os.replace(tmp, self.status_path)
                return
            except PermissionError:
                time.sleep(0.05)
        self.log(f"  warning: could not update {self.status_path}")

    def game_running(self):
        exe = self.proc.lower() + ".exe"
        return any(process_alive(pid) for pid, name in processes() if name == exe)

    def window(self):
        """The game window; handles the lock screen and a window that went away."""
        if is_locked():
            raise Failed("locked", "Windows is showing the lock screen")
        hwnd = find_window(self.proc)
        if not hwnd:
            if self.status["exit_expected"] and not self.game_running():
                raise GameQuit()  # quit through the menu as planned
            if self.status["exit_expected"]:
                raise GameExiting()
            if not self.game_running():
                raise Failed("game_exited", "the game exited before the recipe started quitting")
            raise Failed("no_window", "the game has no visible window")
        return hwnd

    def grab(self):
        hwnd = self.window()
        if self.segment is None:
            return recipe_grab(hwnd, self.recipe, self.source)
        # a capture inside a measured segment (play_until grabs the whole 4K Mopic display every loop): its time
        # goes to captures.csv so perfreport.py can tell hitches that overlap one
        start = qpc_ns()
        try:
            return recipe_grab(hwnd, self.recipe, self.source)
        finally:
            self.segment_spans.append((start, qpc_ns()))

    def measure_label(self, step):
        """The segment label of a step, or None. play / play_until steps are measured as "gameplay" unless
        "measure": false; "measure": true or "<label>" makes any other step a segment too (a wait during a movie)."""
        if "phase" in step:
            return None
        m = step.get("measure", "play" in step or "play_until" in step)
        if m is None or m is False:
            return None
        if m is True:
            return "gameplay"
        if isinstance(m, str):
            return m.strip() or None
        raise Failed("recipe", f"\"measure\" must be true, false or a label, got {m!r}")

    def segment_begin(self, label):
        """Open a measured segment: start time, the window's display and the power source, written at once so a
        crash in the middle still leaves the start in the status."""
        hwnd = find_window(self.proc)
        seg = {"label": label, "phase": self.status["phase"], "step": self.status["step"], "start_qpc_ns": None,
               "end_qpc_ns": None, "ok": None, "captures": 0, "capture_s": 0.0, "cpu_s": None,
               "window": window_display(hwnd) if hwnd else None, "power": power_status()}
        self.status["segments"].append(seg)
        self.segment, self.segment_spans, self.segment_cpu = seg, [], time.process_time()
        seg["start_qpc_ns"] = qpc_ns()
        self.write_status()
        self.log(f"  measuring ({label})")

    def segment_end(self, ok):
        """Close the open segment (no-op when none is open)."""
        seg = self.segment
        if seg is None:
            return
        seg["end_qpc_ns"] = qpc_ns()
        self.segment = None
        seg["ok"] = ok
        seg["cpu_s"] = round(time.process_time() - self.segment_cpu, 3)  # the pilot's own CPU use (GPU power budget)
        spans = self.segment_spans
        seg["captures"] = len(spans)
        seg["capture_s"] = round(sum(e - s for s, e in spans) / 1e9, 3)
        if spans:
            index = self.status["segments"].index(seg)
            path = os.path.join(self.out_dir, "captures.csv")
            try:
                new = not os.path.exists(path)
                with open(path, "a", encoding="utf-8", newline="\n") as f:
                    f.write(("segment,start_qpc_ns,end_qpc_ns\n" if new else "")
                            + "".join(f"{index},{s},{e}\n" for s, e in spans))
            except OSError as e:
                self.log(f"  warning: could not write {path}: {e}")
        self.write_status()
        self.log(f"  measured {(seg['end_qpc_ns'] - seg['start_qpc_ns']) / 1e9:.1f} s ({seg['label']}"
                 f"{'' if ok else ', failed'}, {len(spans)} captures)")

    def save_shot(self, name, img=None):
        if img is None:
            img, _ = self.grab()
        safe = "".join("+" if c == "|" else ("_" if c in '<>:"/\\?*' else c) for c in name)
        path = os.path.join(self.out_dir, f"{self.status['step']:02d}-{safe}.png")
        img.save(path)
        return path

    def sleep(self, seconds):
        """Sleep, but notice the game going away."""
        end = time.time() + seconds
        while time.time() < end:
            if not self.game_running():
                self.window()  # raises
            time.sleep(min(0.5, max(0.0, end - time.time())))

    def input_ready(self, strict=True):
        """The game window, focused. If it can't be brought to the foreground, input would go to whatever window
        is in front (the terminal that started the harness...): fail (strict), or return None to skip the input."""
        hwnd = self.window()
        for attempt in range(3):
            if focus(hwnd):
                return hwnd
            time.sleep(0.3)
        if strict:
            raise Failed("focus", "could not bring the game window to the foreground")
        self.log("  warning: could not bring the game window to the foreground, skipping this input")
        return None

    def matches(self, name, img):
        cp = self.recipe["checkpoints"][name]
        return checkpoint_score(self.recipe_path, self.recipe, name, img) >= cp.get("threshold", 0.8)

    def reached(self, name, img, note=""):
        self.status["reached"].append(name)
        self.write_status()
        self.save_shot(name, img)
        self.log(f"  reached {name}{note}")

    def seek(self, name, key, max_presses, gap):
        """Press `key` until the checkpoint shows (menu cursors don't always start at the same item). Cursor
        highlights often pulse, so the screen is sampled several times over `gap` after each press."""
        for i in range(max_presses + 1):
            end = time.time() + gap
            while True:
                img, _ = self.grab()
                if self.matches(name, img):
                    self.reached(name, img, f" after {i} x {key}")
                    return
                if time.time() >= end:
                    break
                self.sleep(0.2)
            if i < max_presses and self.input_ready(strict=False):
                press(key)
        self.save_shot(f"FAILED-{name}", img)
        raise Failed("checkpoint", f"{name} not reached after {max_presses} x {key}")

    def play_until(self, name, timeout, keys, every, hold, while_cp, idle_key, idle_every):
        """Random gameplay keys until the checkpoint shows. With `while`, keys are only pressed while that
        checkpoint (an in-game HUD element) is visible, so they don't act as menu input on result screens."""
        end, last_idle, presses = time.time() + timeout, time.time(), 0
        while True:
            img, _ = self.grab()
            if self.matches(name, img):
                self.segment_end(True)  # gameplay ended at this capture (saving the shot isn't part of it)
                self.reached(name, img, f" after {presses} gameplay keys")
                return
            if time.time() >= end:
                self.save_shot(f"FAILED-{name}", img)
                raise Failed("checkpoint", f"{name} not reached within {timeout} s of gameplay")
            if keys and (not while_cp or self.matches(while_cp, img)):
                if self.input_ready(strict=False):
                    press(self.rng.choice(keys), hold)
                    presses += 1
            elif idle_key and time.time() - last_idle >= idle_every:
                if self.input_ready(strict=False):
                    press(idle_key)
                last_idle = time.time()
            self.sleep(every)

    def to_screen(self, hwnd, x, y):
        """Recipe coordinates (the screenshot the checkpoints are in) -> screen pixels."""
        start = qpc_ns()
        _, meta = grab(hwnd, self.max_width, self.source)
        if self.segment is not None:
            self.segment_spans.append((start, qpc_ns()))
        if self.fit:  # recipe coordinates -> the unfitted window screenshot
            fx, fy, fw, fh = self.fit
            x, y = (x - fx) * meta["size"][0] / fw, (y - fy) * meta["size"][1] / fh
        return shot_to_screen(x, y, meta)

    def click(self, x, y, button="left", double=False, strict=True):
        """Click at recipe coordinates."""
        hwnd = self.input_ready(strict)
        if not hwnd:
            return
        sx, sy = self.to_screen(hwnd, x, y)
        click_at(sx, sy, button, double)

    def move(self, x, y):
        """Move the mouse to recipe coordinates (hover: menus that highlight under the mouse)."""
        hwnd = self.input_ready()
        mouse_to(*self.to_screen(hwnd, x, y))

    def wait_for(self, names, timeout, press_key=None, every=3.0, poll=1.0, click=None):
        """Wait until one of the checkpoints shows; optionally press a key (or click a point) every `every` s
        meanwhile (skipping movies, poking a scene until it reacts). A press only follows a check that failed."""
        names = [names] if isinstance(names, str) else list(names)
        label = "|".join(names)
        end, last_press, best = time.time() + timeout, time.time(), -2.0
        while True:
            img, _ = self.grab()
            for name in names:
                cp = self.recipe["checkpoints"][name]
                score = checkpoint_score(self.recipe_path, self.recipe, name, img)
                best = max(best, score)
                if score >= cp.get("threshold", 0.8):
                    self.reached(name, img, f" (score {score:.2f})")
                    return name
            if time.time() >= end:
                self.save_shot(f"FAILED-{label}", img)
                raise Failed("checkpoint", f"{label} not reached within {timeout} s (best score {best:.2f})")
            if (press_key or click) and time.time() - last_press >= every:
                if click:
                    self.click(click[0], click[1], strict=False)
                elif self.input_ready(strict=False):
                    press(press_key)
                last_press = time.time()
            self.sleep(poll)

    def wait_gone(self, names, timeout, settle=3.0, poll=0.5):
        """Wait until none of the checkpoints has shown for `settle` seconds in a row (a loading screen going away;
        the settle time rides out frames where an animated screen briefly fails its match)."""
        names = [names] if isinstance(names, str) else list(names)
        label = "|".join(names)
        start = time.time()
        end, gone_since = start + timeout, None
        while True:
            img, _ = self.grab()
            if any(self.matches(n, img) for n in names):
                gone_since = None
            else:
                gone_since = gone_since or time.time()
                if time.time() - gone_since >= settle:
                    self.log(f"  {label} gone after {gone_since - start:.0f} s")
                    return
            if time.time() >= end:
                self.save_shot(f"FAILED-gone-{label}", img)
                raise Failed("checkpoint", f"{label} still showing after {timeout} s")
            self.sleep(poll)

    def do(self, step):
        if "note" in step and len(step) == 1:
            return
        if "if" in step:
            img, _ = self.grab()
            if not self.matches(step["if"], img):
                self.log(f"  skipped ({step['if']} not showing)")
                return
        label = self.measure_label(step)
        if label is None:
            self.do_step(step)
            return
        self.segment_begin(label)
        ok = False
        try:
            self.do_step(step)
            ok = True
        finally:
            self.segment_end(ok)

    def do_step(self, step):
        if "phase" in step:
            self.status["phases"].append({"name": step["phase"], "step": self.status["step"], "qpc_ns": qpc_ns()})
            self.write_status(phase=step["phase"], exit_expected=self.status["exit_expected"] or step["phase"] == "exit")
            self.log(f"phase {step['phase']}")
        elif "wait_for" in step:
            self.wait_for(step["wait_for"], step.get("timeout", 30), step.get("press"), step.get("every", 3.0),
                          click=step.get("click"))
        elif "wait_gone" in step:
            self.wait_gone(step["wait_gone"], step.get("timeout", 60), step.get("for", 3.0))
        elif "seek" in step:
            self.seek(step["seek"], step.get("press", "down"), step.get("max", 8), step.get("gap", 1.2))
        elif "play_until" in step:
            self.play_until(step["play_until"], step.get("timeout", 300), step.get("keys", "").split(),
                            step.get("every", 0.35), step.get("hold", 80), step.get("while"),
                            step.get("idle_press"), step.get("idle_every", 3.0))
        elif "key" in step:
            self.input_ready()
            for i in range(step.get("times", 1)):
                press(step["key"], step.get("hold", 80))
                time.sleep(step.get("gap", 300) / 1000.0)
        elif "keys" in step and "play" not in step:
            self.input_ready()
            for n in step["keys"].split():
                press(n, step.get("hold", 80))
                time.sleep(step.get("gap", 350) / 1000.0)
        elif "click" in step:
            self.click(step["click"][0], step["click"][1], step.get("button", "left"), step.get("double", False))
        elif "move" in step:
            self.move(step["move"][0], step["move"][1])
        elif "wait" in step:
            self.sleep(step["wait"])
        elif "play" in step:
            keys, every = step.get("keys", "").split(), step.get("every", 0.4)
            end = time.time() + step["play"]
            while time.time() < end:
                if keys and self.input_ready(strict=False):
                    press(self.rng.choice(keys), step.get("hold", 80))
                self.sleep(every)
            self.segment_end(True)  # the screenshot after the play isn't part of it
            self.save_shot("play")
        elif "expect_exit" in step:
            start = time.time()
            end, next_log = start + step["expect_exit"], start + 2
            while time.time() < end:
                if not self.game_running():
                    raise GameQuit()
                if time.time() >= next_log:
                    exe = self.proc.lower() + ".exe"
                    pids = [pid for pid, name in processes() if name == exe]
                    self.log(f"  still running after {time.time() - start:.0f} s: pids {pids}, alive {[process_alive(p) for p in pids]}")
                    next_log += 2
                time.sleep(0.5)
            try:
                self.save_shot("still-running")
            except (Failed, GameExiting):
                pass  # usually the window is gone already
            raise Failed("exit_timeout", f"the game was still running {step['expect_exit']} s after quitting through the menu")
        elif "shot" in step:
            self.save_shot(step["shot"])
        else:
            raise Failed("recipe", f"unknown step {step}")

    def run(self, first=1, last=None):
        steps = self.recipe["steps"]
        last = len(steps) if last is None else min(last, len(steps))
        try:
            # only the steps that run need their vars (run --steps during discovery)
            unfilled = set().union(*self.unfilled_vars[max(first, 1) - 1:last])
            if unfilled:
                names = ", ".join("${" + n + "}" for n in sorted(unfilled))
                self.var_problems.append(f"the steps use {names}, which have no default and no --var")
            if self.var_problems:
                raise Failed("recipe", "; ".join(self.var_problems))
            if self.variables:
                self.log(f"vars: {json.dumps(self.variables, ensure_ascii=False)}")
            end = time.time() + self.recipe.get("window_timeout", 180)
            while not find_window(self.proc):
                if time.time() > end:
                    raise Failed("no_window", "the game window never appeared")
                time.sleep(1)
            for i, step in enumerate(steps, 1):
                if i < first or i > last:
                    continue
                desc = json.dumps(step, ensure_ascii=False)
                self.write_status(step=i, desc=desc)
                self.log(f"[{i}/{len(steps)}] {desc}")
                # the random gameplay keys of each step start from the same seed in every run (one seed for the
                # whole recipe would drift: play_until draws as many keys as the game's timing allows)
                self.rng = random.Random(f"{os.path.basename(self.recipe_path)}:{i}")
                try:
                    self.do(step)
                except GameExiting:
                    # the rest of the quit path needs no input any more: wait for the process like expect_exit
                    exits = [s for s in steps if "expect_exit" in s]
                    self.log("  the window is gone, waiting for the process to end")
                    self.do(exits[-1] if exits else {"expect_exit": 60})
            self.write_status(state="done")
            self.log("recipe finished")
        except GameQuit:
            self.write_status(state="done")
            self.log("the game quit")
        except Failed as e:
            self.write_status(state="failed", error=str(e), error_kind=e.kind)
            self.log(f"FAILED ({e.kind}): {e}")
        except (Exception, SystemExit) as e:  # noqa: BLE001 - report anything to the harness
            self.write_status(state="failed", error=str(e) or repr(e), error_kind="exception")
            self.log(f"FAILED (exception): {e!r}")
        return self.status["state"] == "done"

def write_dump(proc, out):
    """Thread stacks + module list of the process with this exe name. A launcher stub can share the game's name
    (Hogwarts: HogwartsLegacy.exe starts Phoenix\\Binaries\\Win64\\HogwartsLegacy.exe), so the one with the most
    threads is taken."""
    import msvcrt
    exe = proc.lower() + ".exe"
    threads = process_threads()
    pids = sorted((pid for pid, name in processes() if name == exe), key=lambda p: -threads.get(p, 0))
    if not pids:
        return {"error": f"no process {exe}"}
    h = kernel32.OpenProcess(0x0400 | 0x0010 | 0x0040, False, pids[0])  # QUERY_INFORMATION, VM_READ, DUP_HANDLE
    if not h:
        return {"error": f"OpenProcess failed ({ctypes.get_last_error()})"}
    try:
        dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
        with open(out, "wb") as f:
            # MiniDumpNormal | WithUnloadedModules | WithThreadInfo | WithIndirectlyReferencedMemory (objects the
            # stacks point at, e.g. the one a stuck worker waits on)
            ok = dbghelp.MiniDumpWriteDump(h, pids[0], msvcrt.get_osfhandle(f.fileno()), 0x20 | 0x1000 | 0x40, None, None, None)
            err = ctypes.get_last_error()
        if not ok:
            os.remove(out)
            return {"error": f"MiniDumpWriteDump failed ({err})"}
        return {"pid": pids[0], "dump": os.path.abspath(out)}
    finally:
        kernel32.CloseHandle(h)


def checkpoint_cmd(args):
    from PIL import Image
    path = os.path.abspath(args.recipe)
    recipe = load_recipe(path) if os.path.exists(path) else {"process": args.process,
                                                             "source": args.source or "window",
                                                             "max_width": 1280, "checkpoints": {}, "steps": []}
    recipe.setdefault("checkpoints", {})
    if args.action == "save":
        if len(args.region) != 4:
            raise SystemExit("save needs x y w h")
        src = args.src
        if not src:
            with open(os.path.join(STATE_DIR, "last_shot.json")) as f:
                last = json.load(f)
            src = last["path"]
            if not os.path.exists(path) and not args.source:
                recipe["source"] = last.get("source", "window")
        img = Image.open(src).convert("RGB")
        x, y, w, h = args.region
        images = checkpoint_dir(path, recipe)
        os.makedirs(images, exist_ok=True)
        image = f"{args.name}.png"
        img.crop((x, y, x + w, y + h)).save(os.path.join(images, image))
        recipe["checkpoints"][args.name] = {"image": image, "region": [x, y, w, h], "threshold": args.threshold}
        if args.mode != "shape":
            recipe["checkpoints"][args.name]["mode"] = args.mode
        save_recipe(path, recipe)
        score = checkpoint_score(path, recipe, args.name, img)
        print(json.dumps({"saved": args.name, "from": src, "self_score": round(score, 3)}))
    else:
        if args.src:
            img = Image.open(args.src).convert("RGB")
        else:
            if is_locked():
                print(json.dumps({"error": "locked"}))
                sys.exit(3)
            hwnd = find_window(args.process)
            if not hwnd:
                print(json.dumps({"error": f"no window for process {args.process}"}))
                sys.exit(2)
            img, _ = recipe_grab(hwnd, recipe, args.source)
        names = list(recipe["checkpoints"]) if args.name == "all" else [args.name]
        print(json.dumps({n: round(checkpoint_score(path, recipe, n, img), 3) for n in names}))


def matrix_cmd(args):
    """Score every checkpoint against every screenshot: each screen should match only its own checkpoint."""
    import glob
    from PIL import Image
    path = os.path.abspath(args.recipe)
    recipe = load_recipe(path)
    shots = sorted({f for pattern in args.shots for f in glob.glob(pattern)})
    bad = 0
    for shot_path in shots:
        img = Image.open(shot_path).convert("RGB")
        hits, near = [], []
        for name, cp in recipe["checkpoints"].items():
            score = checkpoint_score(path, recipe, name, img)
            if score >= cp.get("threshold", 0.8):
                hits.append(f"{name}={score:.2f}")
            elif score >= cp.get("threshold", 0.8) - 0.15:
                near.append(f"{name}={score:.2f}")
        bad += len(hits) > 1
        print(f"{os.path.basename(shot_path):40} {' '.join(hits):40} {('near: ' + ' '.join(near)) if near else ''}")
    print(f"{len(shots)} screenshots, {bad} matched more than one checkpoint (fine when one checks a state of another,"
          " e.g. main_menu + pvc_selected; not fine for two different screens)")

# ---------------------------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("process")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("focus")
    sub.add_parser("status")
    s = sub.add_parser("shot"); s.add_argument("--out", default=os.path.join(STATE_DIR, "shot.png")); s.add_argument("--max-width", type=int, default=1280)
    s.add_argument("--source", choices=["window", "mopic", "mopic-sbs"], default="window",
                   help="mopic: capture the Mopic display (what the viewer sees); mopic-sbs: its left eye while monado-service runs with MOPIC_MODE=sbs")
    s = sub.add_parser("key"); s.add_argument("name"); s.add_argument("--times", type=int, default=1); s.add_argument("--hold", type=int, default=80); s.add_argument("--gap", type=int, default=250)
    s = sub.add_parser("keys"); s.add_argument("names"); s.add_argument("--hold", type=int, default=80); s.add_argument("--gap", type=int, default=350)
    s = sub.add_parser("click"); s.add_argument("x", type=float); s.add_argument("y", type=float); s.add_argument("--button", default="left"); s.add_argument("--double", action="store_true")
    s = sub.add_parser("move"); s.add_argument("x", type=float); s.add_argument("y", type=float)
    s = sub.add_parser("type"); s.add_argument("text")
    s = sub.add_parser("dump", help="minidump (thread stacks) of the process, e.g. one stuck while exiting"); s.add_argument("out")
    s = sub.add_parser("run", help="replay a recipe"); s.add_argument("recipe"); s.add_argument("--out", default=os.path.join(STATE_DIR, "run"))
    s.add_argument("--status", default="", help="JSON status file for the harness")
    s.add_argument("--steps", default="", help="only these steps, e.g. 9-12 or 9- (1-based, as in pilot.log)")
    s.add_argument("--source", choices=["window", "mopic", "mopic-sbs"], default=None,
                   help="override the recipe's capture source (window: the game without UEVR; mopic-sbs: a mopic recipe while monado-service runs with MOPIC_MODE=sbs)")
    s.add_argument("--var", action="append", default=[], metavar="NAME=VALUE", help="value for ${NAME} in the recipe's steps (repeatable)")
    s.add_argument("--dry-run", action="store_true", help="print the steps after --var substitution and exit (no game needed)")
    s = sub.add_parser("matrix", help="score all checkpoints against screenshots (each screen should match one)")
    s.add_argument("recipe"); s.add_argument("shots", nargs="+", help="screenshot files or globs")
    s = sub.add_parser("checkpoint", help="save a checkpoint from a screenshot region / test one against the screen")
    s.add_argument("action", choices=["save", "test"]); s.add_argument("recipe"); s.add_argument("name")
    s.add_argument("region", nargs="*", type=int, help="save: x y w h in screenshot pixels")
    s.add_argument("--from", dest="src", default="", help="screenshot to crop/test (default: save uses the last shot, test grabs the screen)")
    s.add_argument("--threshold", type=float, default=0.8)
    s.add_argument("--mode", choices=["shape", "highlight"], default="shape", help="highlight: a menu cursor, decided by the bar's colour")
    s.add_argument("--source", choices=["window", "mopic", "mopic-sbs"], default=None,
                   help="test: capture source (window: the game without UEVR; mopic-sbs: monado-service in MOPIC_MODE=sbs); save: the source of a new recipe")
    args = ap.parse_args()

    if args.cmd == "matrix":
        matrix_cmd(args)
        return
    if args.cmd == "dump":
        result = write_dump(args.process, args.out)
        print(json.dumps(result))
        sys.exit(1 if "error" in result else 0)
    if args.cmd == "run":
        # step descriptions and vars can hold text the console code page lacks (Chinese save names on cp949)
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(errors="backslashreplace")
        variables = parse_vars(args.var)
        if args.dry_run:
            steps, resolved, problems, unfilled = recipe_steps_with_vars(load_recipe(args.recipe), variables)
            print(f"vars: {json.dumps(resolved, ensure_ascii=False)}")
            for i, step in enumerate(steps, 1):
                print(f"[{i}/{len(steps)}] {json.dumps(step, ensure_ascii=False)}")
            missing = sorted(set().union(*unfilled))
            if missing:
                problems.append(f"the steps use {', '.join('${' + n + '}' for n in missing)}, which have no default and no --var")
            if problems:
                print(json.dumps({"error": "vars", "problems": problems, "missing": missing}, ensure_ascii=False))
                sys.exit(1)
            return
        first, last = 1, None
        if args.steps:
            a, _, b = args.steps.partition("-")
            first = int(a) if a else 1
            last = int(b) if b else (None if _ else first)
        pilot = Pilot(args.process, args.recipe, args.out, args.status, args.source, variables)
        try:
            ok = pilot.run(first, last)
        except BaseException:  # noqa: BLE001 - the harness runs us hidden, keep the reason in pilot.log
            import traceback
            pilot.log("pilot crashed:\n" + traceback.format_exc())
            raise
        sys.exit(0 if ok else 1)
    if args.cmd == "checkpoint":
        checkpoint_cmd(args)
        return

    if is_locked():
        print(json.dumps({"error": "locked", "detail": "Windows is showing the lock screen"}))
        sys.exit(3)

    hwnd = find_window(args.process)
    if args.cmd == "status":
        print(json.dumps({"locked": False, "window": bool(hwnd), "rect": list(window_rect(hwnd)) if hwnd else None,
                          "foreground": bool(hwnd) and user32.GetForegroundWindow() == hwnd}))
        return
    if not hwnd:
        print(json.dumps({"error": f"no window for process {args.process}"}))
        sys.exit(2)

    if args.cmd == "shot":
        print(json.dumps(shot(hwnd, args.out, args.max_width, args.source)))
        return

    focused = focus(hwnd)
    if args.cmd == "focus":
        print(json.dumps({"focused": focused}))
    elif args.cmd == "key":
        for i in range(args.times):
            press(args.name, args.hold)
            if i + 1 < args.times:
                time.sleep(args.gap / 1000.0)
        print(json.dumps({"focused": focused, "key": args.name, "times": args.times}))
    elif args.cmd == "keys":
        names = args.names.split()
        for n in names:
            press(n, args.hold)
            time.sleep(args.gap / 1000.0)
        print(json.dumps({"focused": focused, "keys": names}))
    elif args.cmd in ("click", "move"):
        x, y = shot_to_screen(args.x, args.y)
        if args.cmd == "click":
            click_at(x, y, args.button, args.double)
        else:
            mouse_to(x, y)
        print(json.dumps({"focused": focused, "screen": [x, y]}))
    elif args.cmd == "type":
        for ch in args.text.lower():
            press("space" if ch == " " else ch, 40)
            time.sleep(0.05)
        print(json.dumps({"focused": focused, "typed": args.text}))


if __name__ == "__main__":
    main()
