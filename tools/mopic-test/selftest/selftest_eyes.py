"""Self-test of analysis\\eyesampler.py (no game, no screen): the sampler runs on a fake clock against synthetic
side-by-side frames (a textured strip panning at 20 game frames per second on a 60 Hz display, the right eye 12 px
apart), and the report has to find exactly the faults put in: the right eye a game frame late for two refreshes, the
left eye black for three refreshes, and nothing for a one-eye change while the scene stands still, a sampler stall,
a closed pilot segment or jittery grabs. Also gamepilot's mopic-sbs crop."""
import json
import math
import os
import random
import shutil
import sys

import numpy as np

TOOLS = r"C:\Users\zzong\source\repos\UEVR-jh\tools\mopic-test"
sys.path.insert(0, os.path.join(TOOLS, "analysis"))
sys.path.insert(0, TOOLS)
import eyesampler  # noqa: E402
import gamepilot  # noqa: E402

work = sys.argv[1]
os.makedirs(work, exist_ok=True)
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("ok    " if ok else "FAIL  ") + name + ("" if ok else f"  {detail}"))
    fails += 0 if ok else 1


# --- one sample's numbers
px = np.zeros((4, 16, 4), np.uint8)
px[:, :8, :3] = 100            # left half gray 100
px[:, 8:, 2] = 255             # right half pure red: luma 54/256 * 255
left, right = eyesampler.eye_lumas(px.tobytes(), 4, 16)
check("eye_lumas: halves of every 4th pixel", left.shape == (1, 2) and right.shape == (1, 2), f"{left.shape} {right.shape}")
check("eye_lumas: Rec. 709 luma", int(left[0, 0]) == 100 and int(right[0, 0]) == (255 * 54) >> 8, f"{left} {right}")
row = eyesampler.measure(left, right, (left - 10, right))
check("measure: means and diffs", abs(row["l_diff"] - 10) < 1e-9 and row["r_diff"] == 0 and row["l_mean"] == 100, row)
rng = np.random.default_rng(1)
prof = np.convolve(rng.uniform(0, 255, 700), np.ones(5) / 5, mode="same")
grid = np.tile(prof, (8, 1)).astype(np.int16)
corr, s = eyesampler.lr_match(grid[:, 100:500], grid[:, 107:507])
check("lr_match: right[x] = left[x + s]", s == 7 and corr > 0.99, f"{corr} {s}")
check("lr_match: a flat eye has no match", eyesampler.lr_match(grid[:, :400], np.zeros((8, 400), np.int16)) == (None, None))


# --- synthetic display: 20 fps game, 60 Hz display, sampled ~120 Hz on a fake clock
W, H, HALF, DISP, SPEED = 960, 32, 480, 12, 6
REFRESH_NS = 1e9 / 60
GAME_NS = 50e6
T0 = 10 ** 12   # QPC-like start
world = np.convolve(rng.uniform(0, 255, 6000), np.ones(5) / 5, mode="same")
world = np.clip(np.tile(world, (H, 1)) + rng.normal(0, 6, (H, 6000)), 0, 255)


class Scene:
    def __init__(self, faults=True, jitter=False, seed=0):
        self.faults, self.jitter = faults, jitter
        self.now = T0
        self.rng = random.Random(seed)
        self.stalled = False

    def clock(self):
        return int(self.now)

    def sleep(self, s):
        self.now += s * 1e9

    def frame_at(self, n):
        """The game frame the display shows at refresh n (frame k is presented at k * 50 ms + 5 ms)."""
        return max(0, int(math.floor((n * REFRESH_NS - 5e6) / GAME_NS)))

    def pos(self, k):
        # pans until 5.0 s, stands still until 7.0 s, then pans again
        if not self.faults:
            return SPEED * k
        return SPEED * min(k, 100) + SPEED * max(0, k - 140)

    def grab(self, rect):
        t = self.now - T0
        if self.faults and not self.stalled and t >= 8.0e9:
            self.stalled = True
            self.now += 60e6   # one 60 ms stall of the sampler (its grab blocked)
        self.now += (self.rng.uniform(1.5, 4.5) if self.jitter else 3.0) * 1e6
        n = int(t // REFRESH_NS)
        kl = kr = self.frame_at(n)
        if self.faults:
            n1 = next(m for m in range(10 ** 6) if self.frame_at(m) >= 40)
            if n1 <= n < n1 + 2:
                kr = 39   # the right eye still shows the previous game frame for two refreshes
            n2 = next(m for m in range(10 ** 6) if self.frame_at(m) >= 80)
            if n2 == n:
                kl = 79   # the left eye a frame late for one refresh: "brief", not a lag
        lx, rx = self.pos(kl), self.pos(kr) + DISP
        L = world[:, lx:lx + HALF].copy()
        R = world[:, rx:rx + HALF].copy()
        if self.faults:
            nb = int(3.0e9 // REFRESH_NS)
            if nb <= n < nb + 3:
                L[:] = 0   # the left eye black for three refreshes
            if 6.0e9 <= n * REFRESH_NS < 6.2e9:
                R[:, 200:240] = np.clip(R[:, 200:240] + 80, 0, 255)   # a one-eye change while the scene stands still
        v = np.hstack([L, R]).astype(np.uint8)
        out = np.empty((H, W, 4), np.uint8)
        out[..., 0] = out[..., 1] = out[..., 2] = v
        out[..., 3] = 255
        return out.tobytes()


def run_scene(name, scene, seconds, segments):
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    status = os.path.join(d, "pilot-status.json")
    with open(status, "w", encoding="utf-8") as f:
        json.dump({"state": "running", "clock": {"qpc_ns": T0, "unix_ns": 1759400000 * 10 ** 9},
                   "segments": [{"label": "gameplay", "start_qpc_ns": T0 + int(a * 1e9),
                                 "end_qpc_ns": T0 + int(b * 1e9)} for a, b in segments]}, f)
    rect = {"left": 0, "top": 0, "width": W, "height": H}
    sampler = eyesampler.Sampler(d, rect, hz=120, status_path=status, seconds=seconds, grab=scene.grab,
                                 clock=scene.clock, sleep=scene.sleep)
    sampler.meta["refresh_hz"] = 60
    meta = sampler.run()
    return d, meta, eyesampler.report(d)


d, meta, doc = run_scene("faults", Scene(), 11.2, [(0.2, 8.5), (9.0, 11.0)])
s = doc["summary"]
rows = eyesampler.load_csv(os.path.join(d, "eyes.csv"))
print("  line: " + doc["line"])
check("sampler: stopped by --seconds", meta["stopped_by"] == "seconds", meta["stopped_by"])
check("sampler: ~120 samples a second", s["rate_hz"] and 110 <= s["rate_hz"] <= 125, s["rate_hz"])
check("sampler: no samples outside the pilot's segments",
      not any(8.5e9 <= r["t"] - T0 < 9.0e9 or r["t"] - T0 < 0.2e9 for r in rows) and {r["seg"] for r in rows} == {0, 1},
      sorted({r["seg"] for r in rows}))
first1 = next(r for r in rows if r["seg"] == 1)
check("sampler: no diff on the first sample after a pause", first1["l_diff"] is None and first1["r_diff"] is None, first1)
lag = [e for e in doc["events"] if e["kind"] == "one_eye_lag"]
check("report: exactly one one-eye lag, the left eye first (the right was late)",
      len(lag) == 1 and lag[0]["eye"] == "left", json.dumps(doc["events"]))
if lag:
    e = lag[0]
    check("report: the lag is about two refreshes (25-45 ms), at 2.0 s", 25 <= e["lag_ms"] <= 45 and 1.95e9 <= e["t_ns"] - T0 <= 2.1e9, e)
    check("report: the event has its wall-clock time and segment offset", e.get("time") and e.get("seg_s") is not None and e.get("label") == "gameplay", e)
brief = [e for e in doc["events"] if e["kind"] == "one_eye_lag_brief"]
check("report: the one-refresh lag at 4.0 s is 'brief' (right eye first), counted apart",
      len(brief) == 1 and brief[0]["eye"] == "right" and 3.95e9 <= brief[0]["t_ns"] - T0 <= 4.1e9 and brief[0]["lag_ms"] <= 25
      and s["one_eye_lag"]["brief"] == 1 and "+ 1 of 1 refresh" in doc["line"], brief)
black = [e for e in doc["events"] if e["kind"] == "one_eye_black"]
check("report: exactly one one-eye black, left, about three refreshes",
      len(black) == 1 and black[0]["eye"] == "left" and 35 <= black[0]["ms"] <= 70, black)
check("report: no one-eye-only event (the one-eye change at 6 s was in a still scene)",
      s["one_eye_only"]["count"] == 0, [e for e in doc["events"] if e["kind"] == "one_eye_only"])
check("report: the still scene is not 'moving'", s["moving_pct"] is not None and s["moving_pct"] < 95, s["moving_pct"])
check("report: side by side recognized, the 12 px parallax measured", s["lr"]["sbs"] is True and s["lr"]["shift_p50_px"] == DISP, s["lr"])
check("report: the sampler stall shows as a gap", s["gaps"]["max_ms"] >= 60 and s["gaps"]["over_2_refreshes"] >= 1, s["gaps"])
check("report: summary counts", s["one_eye_lag"]["count"] == 1 and s["one_eye_black"]["count"] == 1 and s["segments"] == 2, s)
check("report: eyes-report.json written", os.path.exists(os.path.join(d, "eyes-report.json")))
check("report: line", doc["line"].startswith("eyes[") and "one-eye lag 1" in doc["line"] and "one-eye black 1" in doc["line"], doc["line"])
shots = meta["shots"]
check("sampler: candidate shots of the lag and the black eye",
      any(x["kind"] == "left-only" for x in shots) and any(x["kind"] == "left-black" for x in shots)
      and all(os.path.exists(os.path.join(d, x["file"])) for x in shots), shots)

# a killed sampler leaves a cut-off last line: skipped
with open(os.path.join(d, "eyes.csv"), "a", encoding="utf-8") as f:
    f.write(f"{T0 + int(10.99e9)},3000,1,12")
check("report: a cut-off last line is skipped", len(eyesampler.load_csv(os.path.join(d, "eyes.csv"))) == len(rows))

# no faults, jittery grab times (1.5-4.5 ms), 20 s: nothing found
d, meta, doc = run_scene("clean", Scene(faults=False, jitter=True, seed=3), 20.0, [(0.1, 19.9)])
s = doc["summary"]
print("  line: " + doc["line"])
check("clean: no events with jittery grabs", not doc["events"], json.dumps(doc["events"][:3]))
check("clean: moving nearly all the time", s["moving_pct"] > 97, s["moving_pct"])

# a stricter threshold leaves the lag alone (it changes the whole eye)
doc = eyesampler.report(os.path.join(work, "faults"), threshold=5.0)
check("report --threshold 5: the lag still found", doc["summary"]["one_eye_lag"]["count"] == 1, doc["line"])
check("report: the lag's follower made the same step (matched), not a brightness step, grab times summarized",
      lag and lag[0]["follow_diff"] is not None and not lag[0]["uniform"] and s["one_eye_lag"]["unmatched"] == 0
      and doc["summary"]["grab_ms"] and doc["summary"]["grab_ms"]["p50"] == 3.0, (lag, doc["summary"]["grab_ms"]))


# --- detect() on hand-made rows: what is not a lag. 120 samples a second; a 20 fps game pans (both eyes change by
# 20 on the first sample of each game frame), stands still 2-3 s and 5-7 s
def synth_rows(seconds=16.0):
    rows, prev_g = [], None
    for i in range(int(seconds * 120)):
        ts = i / 120.0
        k = int(ts / 0.05)
        g = min(k, 40) + max(0, min(k, 100) - 60) + max(0, k - 140)   # frames 40-60 and 100-140 show no motion
        moved = prev_g is not None and g != prev_g
        prev_g = g
        rows.append({"t": T0 + int(ts * 1e9), "seg": 0, "grab_us": 3000.0, "l_mean": 100.0, "r_mean": 100.0,
                     "l_diff": 20.0 if moved else 0.0, "r_diff": 20.0 if moved else 0.0, "lr_corr": 0.9, "lr_shift": 12.0})
    rows[0]["l_diff"] = rows[0]["r_diff"] = None
    return rows


def at(rows, ts):
    return min(range(len(rows)), key=lambda i: abs(rows[i]["t"] - T0 - ts * 1e9))


R = synth_rows()
# (a) a small change only the left strip shows, 0.1 s before the camera moves again: not a lag
R[at(R, 2.9)].update(l_diff=1.6, r_diff=0.0)
# (b) a small change only the right strip shows, 0.2 s after the camera stopped, then stillness: nothing
R[at(R, 5.2)].update(l_diff=0.0, r_diff=1.6)
# (c) the left eye's brightness steps by 3 between two game frames while panning (auto exposure of one view)
c = at(R, 8.02)
R[c].update(l_diff=3.0, r_diff=0.0)
for row in R[c:]:
    row["l_mean"] = 103.0
# (d) a real lag: at the game frame of 10.0 s the right eye stays on the old frame for two refreshes (one grab slow)
d = next(i for i in range(at(R, 10.0) - 3, len(R)) if R[i]["l_diff"] == 20.0)
R[d]["r_diff"] = 0.0
R[d + 4]["r_diff"] = 20.0
R[d + 2]["grab_us"] = 12000.0
# (e) a frozen right eye: from 12.0 s it doesn't change for 0.6 s while the left keeps changing
e0 = at(R, 12.0)
for row in R[e0:at(R, 12.6)]:
    row["r_diff"] = 0.0
ev, _ = eyesampler.detect(R, refresh_hz=60)
kinds = [(x["kind"], round((x["t_ns"] - T0) / 1e9, 2)) for x in ev]
check("detect: a small one-eye change, then the camera moving: 'one_eye_change' (unmatched), not a lag",
      any(k == "one_eye_change" and 2.85 <= ts <= 2.95 for k, ts in kinds) and not any(k == "one_eye_lag" and ts < 3.5 for k, ts in kinds), kinds)
check("detect: a single small one-eye change, then stillness: no event", not any(5.1 <= ts <= 5.3 for _, ts in kinds), kinds)
cev = [x for x in ev if 7.95 <= (x["t_ns"] - T0) / 1e9 <= 8.05]
check("detect: a one-eye brightness step: flagged uniform", len(cev) == 1 and cev[0]["uniform"] and cev[0]["kind"] == "one_eye_change", cev)
dev = [x for x in ev if 9.9 <= (x["t_ns"] - T0) / 1e9 <= 10.1]
check("detect: a real two-refresh lag still found (matched), uncertain for its slow grab",
      len(dev) == 1 and dev[0]["kind"] == "one_eye_lag" and dev[0]["eye"] == "left" and dev[0]["uncertain"] and dev[0]["grab_ms"] == 12.0, dev)
eev = [x for x in ev if 11.95 <= (x["t_ns"] - T0) / 1e9 <= 12.1]
check("detect: a frozen eye while the other keeps changing: one_eye_only", len(eev) == 1 and eev[0]["kind"] == "one_eye_only" and eev[0]["eye"] == "left" and eev[0]["first_changes"] >= 2, eev)
check("detect: nothing else", len(ev) == 4, kinds)

# --- gamepilot: the left eye of a side-by-side capture, stretched back to the display's shape
from PIL import Image  # noqa: E402
img = Image.new("RGB", (16, 4), (0, 0, 255))
img.paste((255, 0, 0), (0, 0, 8, 4))
out = gamepilot.sbs_left_eye(img, 8, 2)
check("gamepilot sbs_left_eye: the left half, scaled to the asked size", out.size == (8, 2) and out.getpixel((7, 1)) == (255, 0, 0), (out.size, out.getpixel((7, 1))))
meta = {"rect": [-7680, 0, -3840, 2160], "scale": 1 / 3, "size": [1280, 720], "source": "mopic-sbs", "window": [-7680, 0, -3840, 2160]}
check("gamepilot shot_to_screen: mopic-sbs maps like mopic", gamepilot.shot_to_screen(640, 360, meta) == (-5760, 1080),
      gamepilot.shot_to_screen(640, 360, meta))

print(f"eyesampler self-test: {'all passed' if fails == 0 else f'{fails} failed'}")
sys.exit(1 if fails else 0)
