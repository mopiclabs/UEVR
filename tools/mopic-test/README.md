# Mopic UEVR test tools

- `run-test.ps1`: the test harness. Starts UEVRInjector, launches a game through Steam, injects at launch (like
  Mopic Hub's auto-inject), watches it, and writes a verdict (PASS / CRASH / FREEZE / EXIT_CRASH / EXIT_HANG / MENU_FAIL /
  NO_VR / ...) plus log.txt, crash dumps and screenshots to `runs\`.
- `gamepilot.py`: looks at the screen and sends keyboard/mouse input, so a run can go from the title screen into
  real gameplay and quit through the game's own menu (where exit crashes and hangs show up).
- `recipes\<Game>.json` (+ checkpoint images in `recipes\<Game>\`): a recorded route through a game's menus.
- `perfreport.py`: the frame rate of a run (VR fps, lows, hitches, pacing, frame generation), cut to the gameplay the
  pilot measured (see "Frame rate").
- `analysis\eyesampler.py`: whether both eyes on the Mopic display change together, and one-eye black frames
  (monado-service in `MOPIC_MODE=sbs`; see "Each eye").
- `analysis\`: also scripts for the hang dumps the harness writes.
- `plans\hogwarts-fg-nsf.ps1`: Hogwarts Legacy's frame generation x Native Stereo Fix A/B with the eye sampler.

Setup (Python 3.10+), in `tools\mopic-test`:

```
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt
```

The harness runs gamepilot with `.venv\Scripts\python.exe` by itself. The `gamepilot.py` and `analysis\` commands
below are run from `tools\mopic-test` with the same interpreter (`.venv\Scripts\python`).

Before a run: Steam logged in, Mopic Hub running (it provides eye tracking; keep it open) with its auto-inject
off, monado-service running, UEVRInjector's saved settings on OpenXR, the PC unlocked and not used meanwhile.

## Harness

From the repository root:

```
powershell -ExecutionPolicy Bypass -File tools\mopic-test\run-test.ps1 -Game <preset> [options]
```

| Option | |
| --- | --- |
| `-Game` | `Tekken8Demo`, `Expedition33`, `Wukong`, `Hogwarts`, `Stray`, `Hozy`, `ACC`, `SonicDemo`, or `Custom -ProcessName <exe> -SteamInstallDir <dir>` |
| `-Dll <path>` | deploy this UEVRBackend.dll (+pdb) to the Mopic Hub engine folder first (it stays deployed) |
| `-Recipe <name>` | drive the game with `recipes\<name>.json`: gameplay, then quit through the menu (implies `-WaitForExit`, default `-Seconds 900`) |
| `-NoInject` | the game without UEVR (baseline); with `-Recipe` the pilot reads the desktop window |
| `-WaitForExit -Seconds 1800` | someone else drives the game (discovery); a quit by itself counts as a clean menu exit |
| `-GracefulExit` | no recipe: close the window at the end of `-Seconds` and watch the shutdown |
| `-Set "K=V;..."`, `-UserScript "cmd;..."` | config.txt overrides / console commands for this run only (restored) |
| `-InjectDelay <s>` | late injection (inject this many seconds after the game started) |
| `-SaveFile <path> [-SaveAs <name>]` | before each run, copy this save over the file the recipe's `save_slot` names (`-SaveAs`: another file name in that folder), stamped with the current time so Steam Cloud keeps it. The file there before goes to `<run>\save-before\`, the file after the run (also after a crash or a hang) to `<run>\save-after\`; source, sha256 and target are in result.json / summary.txt. Nothing in the save folder is deleted. A relative path is tried from the current folder, then from `tools\mopic-test`. A failed install makes that run `HARNESS_ERROR` without launching the game. |
| `-SaveSlot <file>` | with `-SaveFile`: the file it replaces, with `save_slot`'s placeholders (`"{gamedir}\b1\Saved\SaveGames\{sid64}\ArchiveSaveFile.9.sav"`), instead of the recipe's `save_slot`; also works without a recipe (discovery with `-WaitForExit`) |
| `-RecipeVars "k=v;k2=v2"` | values for the recipe's `vars` (`${k}` in its steps; gamepilot `run --var k=v`). A name the recipe doesn't declare, or a declared var without a default that isn't given, stops the harness before the first run. Values can't contain `;`. |
| `-GameIni "file\|Section\|Key=Value;Key2=Value2"` | the game's own settings for this run only (frame generation, a cvar in Engine.ini): `file` is a file name in the game's config folder (where its GameUserSettings.ini is), or a full path with `save_slot`'s placeholders; an entry without `file\|` keeps the previous file (the first defaults to GameUserSettings.ini), one with only `Key=Value` the previous section too; the value is everything after the first `=` (quotes, `\|`, parentheses), no `;`. A recipe's `game_ini` does the same. Each file is copied to `runs\game-ini-pending\` (with a journal) and `<run>\game-ini-before\` before it changes, keeps its encoding, line breaks and every other line, and goes back byte for byte once the game is gone (also after a crash or a hang; its time and attributes too, sha256 checked); the file as the game left it is in `<run>\game-ini-after\`. A harness that dies in between is undone by the next harness start, which stops that run's game first; a file that couldn't be put back is retried before the next run's change, and while that fails the run is `HARNESS_ERROR` without touching anything (the pending copies are the only originals). A file that wouldn't be written back byte for byte unchanged (UTF-16 without its BOM, bytes its encoding doesn't map back) is refused before anything is copied. A section that appears twice counts as one, as in UE. result.json `game_ini` has each key's value before and after the run; a value the game changed is a WARN note. Not with `-KeepGame`. |
| `-MopicSbs` | monado-service runs with `MOPIC_MODE=sbs`: the pilot reads a `"source": "mopic"` recipe's screens from the left eye (gamepilot `--source mopic-sbs`) |
| `-EyeSampler [-EyeSamplerArgs "--hz 90"]` | sample each eye on the Mopic display during the recipe's measured segments (the whole observation without a recipe): `eyes.csv`, `eyes-report.json`, result.json `eyes`, an `eyes[...]` line in summary.txt (see "Each eye"). Needs `MOPIC_MODE=sbs`; implies `-MopicSbs`. Never a verdict |
| `-NoPerf` | no frame-rate measurement (see "Frame rate"): no nvidia-smi / PresentMon, no `VR_PerfLog` override, no perf.json |
| `-PresentMon auto` / `off` | `auto` (default): also capture the game's Presents with the PresentMon console app when it is installed and allowed |
| `-Runs <n>`, `-Label <text>`, `-KeepGame`, `-Screenshot` | |

`run-matrix.ps1 -Runs 3 [-Dll <path>] [-Games A,B] [-NoInject]` runs every recipe (or the given games) through the
harness and writes one table to `runs\matrix-<time>-uevr.md` / `-vanilla.md` (+ `.json`), with each run's frame
rate (VR fps, or flat fps for `-NoInject`), 1% low and hitches, and per game the median of its runs. An injected
run without UEVR's frame log (an engine without `VR_PerfLog`) shows `(Present <fps>)`, the game's own Present rate,
which is not a VR frame rate (2 Presents per VR frame in AFR) and is left out of the median.

`run-ladder.ps1 -Game Wukong [-Tier 1] [-Only a,b] [-From name] [-Runs 1] [-EngineDir <dir>] [-Label ladder] [-StopOn HARNESS_ERROR,NO_VR] [-DryRun]`
runs a save ladder: one harness run per rung of `ladders\<Game>.json`, each from its own save, and writes
`runs\ladder-<time>-<Game>.md` (+ `.json`, rewritten after every rung: verdict, harness and game exit codes,
`[NativeStereoFix] state=active` count in log.txt, save-after size, screens reached, VR fps / 1% low / hitches;
`.log`: everything the harness printed). Before the first launch it checks every selected rung: its save (and its `sha256`, when the rung has
one), its recipe (exists, has a `save_slot`, declares the rung's vars, gets every var it has no default for). It
stops after a rung that ended `HARNESS_ERROR` or `NO_VR` (the runs after it wouldn't be valid tests; `-StopOn`
sets the list). A rung whose harness wrote no result.json is a `HARNESS_ERROR` row with the harness's error
message. Other rung fields (notes, descriptions) are ignored.

```
{"game": "Wukong", "recipe": "Wukong-save",
 "rungs": [{"name": "ch1-guangzhi", "save": "saves\\wukong\\x.sav", "vars": {"play_s": 180}, "tier": 1}]}
```

`game` is the harness preset, `save` is relative to `tools\mopic-test`, a rung may set its own `recipe`, `tier`
defaults to 1 and `-Tier N` runs the rungs with tier <= N. A rung's `vars` become `-RecipeVars` (lists and
objects as JSON; a `null` var is left out, so the recipe's default applies). Run folders are
`<time>-<game>-<Label>-<rung>-r<n>`.

After changing run-test.ps1, run-ladder.ps1, gamepilot.py, perfreport.py or eyesampler.py, run `selftest\selftest.ps1` (about 9
minutes, no game: a stand-in process compiled from `selftest\fakegame.cs` plays the game; it compares runs without
`-SaveFile` against the committed run-test.ps1, never touches a real save folder or game settings file, round-trips
`-GameIni` on temp files (and on a copy of Hogwarts' GameUserSettings.ini when there is one), checks perfreport.py on
synthetic runs with `selftest\selftest_perf.py` and the eye sampler on synthetic side-by-side frames with
`selftest\selftest_eyes.py`). `-NoWindows` skips the sections whose stand-in has a window (invisible, but a window) or
that start whoami.exe in a console, for a PC someone is using; the `-GameIni` end-to-end runs (a crash, a killed
harness) use the stand-in without a window and still run. Work folders go to `runs\selftest\work-<time>\`.

Verdicts:

- `PASS`
- `CRASH`: the game exited, or crash.dmp / a UE crash report appeared, while it should have been running.
- `FREEZE`: the game's window stopped answering messages for `-HangSeconds` (default 30) while it should have been
  running (a deadlocked game thread). `freeze.dmp` has the stacks (see "Reading an EXIT_HANG dump").
- `EXIT_CRASH`: a crash while quitting (crash.dmp, a UE crash report, or an NTSTATUS error exit code such as
  0xC0000409 after the menu quit or WM_CLOSE).
- `EXIT_HANG`: the game didn't finish quitting. Either its process was still there 30 s after it started exiting
  (stuck in DLL shutdown: note "stuck in shutdown"), or it never got to ExitProcess: still running a minute after
  the menu quit (note "never called ExitProcess") or 45 s after WM_CLOSE (`-GracefulExit`; a game that ignores
  WM_CLOSE or asks to confirm ends up here too). `exit-hang.dmp` has the stacks; the main thread tells which.
- `MENU_FAIL`: the pilot couldn't follow the recipe (a screen didn't show up, focus lost, PC locked) and UEVR was
  fine. `pilot-status.json`, `pilot\pilot.log` and `pilot\*.png` show where it stopped.
- `NO_VR`: OpenXR never got ready (runtime not running), not a valid test. `NO_INJECTION`, `LAUNCH_FAILED`.

.NET's `Process.HasExited` is true as soon as the exit code is set, which is before DLL shutdown; the harness waits
for the process handle so exits stuck in shutdown are caught (that's how UEVR's exit hang was found).

## Frame rate

Every run that got the game started (unless `-NoPerf`) is measured. It never changes a verdict. The numbers come
from these sources, all timed in QueryPerformanceCounter ns ("QPC ns": Python's `time.perf_counter_ns()`, MSVC's
`steady_clock`, monado's `os_monotonic_get_ns()`), so they can be cut to the same seconds of gameplay:

| Source | In the run folder | What it gives |
| --- | --- | --- |
| UEVR, `VR_PerfLog` (on by default; the harness sets it for injected runs) | `perf-frames.csv`, `perf.csv` (copied from `%APPDATA%\UnrealVRMod\<exe>\`) | one row per `xrEndFrame` (`P` a new engine frame, `R` the previous one again, `E` empty, `F` failed) with xrWaitFrame / Present / Tick times and mode flags; one row per second, also while nothing is submitted, with QPC/XrTime pairs |
| monado-service | `monado-frames.csv` (the run's rows of `%LOCALAPPDATA%\monado\app_frame_stats.csv`) | per second: compositor presents to the Mopic display and how many showed a new frame of the game, frames dropped before display |
| PresentMon console app | `presentmon-<exe>-<pid>.csv` | the game's own Presents (the only source without UEVR), monado-service's |
| nvidia-smi, 1 Hz | `gpu.csv` | GPU load, clocks, temperature, power and its limit, throttle reasons |
| gamepilot | `pilot-status.json`, `pilot\captures.csv` | the measured segments (below), the game window's display and adapter, AC/battery |
| run-test.ps1 | `perf-context.json`, `game-settings\` | clock pairs, pids, PresentMon / nvidia-smi status, frame generation DLLs loaded in the game, power mode, the game's GameUserSettings.ini before and after (with `-GameIni`: as the game saw it and as it left it) |

`perfreport.py run <run folder>` (the harness runs it) writes `perf.json` (every segment) and `perf-summary.json`;
its summary is result.json's `perf`, its line goes into summary.txt:

```
perf[gameplay 50.0s, settle 4.0s, excluded menu 10s]: VR 47.8 fps (1% low 30.0, p99 33.3 ms, 2 hitches, 1 stalls) | paced 60:60% 30:40% <=20:0% (max period 33 ms) | work 8.7 ms | display: new frame on 80% of refreshes (48.0/s), 1 dropped | game Present 120.0 fps (sim 60.0) | FG on | GPU 95% 2100 MHz 79C 120/160 W | hint paced
```

What is measured:

- Recipe runs: the `play` and `play_until` steps (label `gameplay`), and any step with `"measure": true` or
  `"measure": "<label>"` (a `wait` during a movie, a `bench`); `"measure": false` leaves a play step out. Each step's
  random keys come from its own seed (`<recipe file>:<step>`), so runs press the same keys. A recipe without such a
  step (Tekken8Demo-quit) has no numbers. Runs without a recipe: `observe`, from XR focus (without UEVR: the game's
  start) + 30 s to the end of `-Seconds`; never used as a baseline.
- Each segment starts once monado paces the game at 2 refreshes or less for 2 s (after a stall its period estimate
  stays at 6-25 refreshes for seconds): `settle_s`, at most 10 s (`settle_capped`); without UEVR data 5 s (at most
  a quarter of the segment).
- Seconds left out (`excluded_s`): UEVR menu open, session not focused, and 3 s or more in a row of loading (no
  engine tick, no new frame, more empty than new submits) or no submit at all. Shorter ones stay in, as stalls. No
  interval spans a left-out second.

Fields (summary; `perf.json` has the same per segment):

| Field | |
| --- | --- |
| `vr.fps` | new engine frames submitted (`P` rows: an engine frame newer than every one submitted before) per measured second: the VR frame rate. monado paces in whole refreshes, so it moves in steps of 60 / 30 / 20. With 2x frame generation every engine frame is submitted twice (the second Present resubmits the previous one): those are `R`. perf-frames.csv files from before UEVR counted them as `R` are reclassified from their `frame` column (with a warning) |
| `vr.frame_ms` {p50, p95, p99, p999, max}, `low1_fps`, `low01_fps` | wall-clock intervals between new-frame submits; 1% low = 1000 / p99; p99.9 only with 10000+ intervals |
| `vr.hitches`, `hitch_per_min`, `stalls`, `hitches_in_capture` | interval >= max(50 ms, 2x median); stall >= 250 ms; hitches overlapping a pilot screen capture |
| `vr.paced_pct` {1, 2, 3+}, `s_at_3plus`, `period_max_ms` | time at the predicted display period of 1 / 2 / 3+ refreshes |
| `vr.predicted_slot_ms` | steps of the predicted display time between new frames. monado's display timing is a synthetic 60 Hz grid: not what the viewer saw (that is `display`) |
| `vr.work_ms` | median of (submit interval - time in xrWaitFrame): the game's cost, continuous where fps moves in steps. Null with a note unless the very-late sync runs on one thread (no async wait, xrWaitFrame at `vr_very_late_post_present`) and no frame cap applies |
| `vr.presents_per_tick`, `older_pct`, `older_submits`, `nsf_snapshot_pct` | UEVR Present passes per engine tick (2.00 with 2x frame generation, 1.00 without); submits of an engine frame older than the newest one submitted; Native Stereo Fix submits whose eye pair came from the pair frozen at an engine frame boundary (`VR_NativeStereoFixPairSnapshot`, perf-frames.csv `nsf_pair` 2) rather than the live targets (1) |
| `vr.engine_fps`, `fps_fresh`, `r_pct`, `submits_per_frame`, `wait_ok_per_frame`, `late_pct`, `nsf_reused_pct`, `no_submit_s`, `engine_time_ratio`, `ring_dropped`, `frames_1hz` | engine ticks per second; new frames without a reused / fallback Native Stereo Fix eye; repeats; submits and waits per new frame; frames submitted after their display time - 4 ms; seconds without any submit (whole segment); engine delta time / wall time; frame records UEVR lost (buffer full, or perf-frames.csv at its size cap); `perf.csv`'s count of the new frames (a warning when perf-frames.csv has clearly fewer) |
| `attrib` | per VR frame medians: `tick_ms`, `present_ms`, `wait_ms`, `uevr_gt_ms`, `uevr_rt_ms` (UEVR's own time, blocked time apart in `rt_blocked_ms`), presents and ticks per VR frame; `bound`: capped / paced (headroom) / gpu / game_thread |
| `display` | monado: refreshes per second, `new_fps` / `new_pct` (refreshes that showed a new game frame), `dropped`, `uevr_vs_display_pct` |
| `flat` | PresentMon, the game's main swap chain: `fps`, `sim_fps` (Reflex markers: the real engine rate with frame generation), lows, hitches, `gpu_busy_ms`, `frame_types`. With UEVR it is the game's Present rate (2 per VR frame in AFR) |
| `framegen` | `on`, `measured`, `presents_per_tick`, `evidence`. Measured first: UEVR Present passes per engine tick (1.7 or more, not in AFR) and PresentMon frame types / presents per simulated frame; when one of them could be measured they decide (`measured` true). UEVR's Streamline frame-generation swapchain flag, nvngx_dlssg.dll loaded (UEVR, or the harness's module probe) and the log line only show it is available, and decide only without a measurement. `setting` from GameUserSettings.ini. Measured and flagged, never changed |
| `capped` | the game's FrameRateLimit (and VSync without UEVR); with UEVR it binds when the limit, halved in AFR, is at most the refresh rate |
| `mode`, `gpu`, `window`, `power`, `settings`, `render_config` | UEVR's mode (AFR, NSF, D3D12, sync stage, rendering method, the xrWaitFrame `callsite` of most frames, foreground share); GPU load, clock, temperature, power limit and throttle reasons; the game window's monitor and adapter; AC/battery and power mode; the settings fingerprint and what changed; a fingerprint of the run's `VR_*` config |
| `compare` | `flat`: the newest PASS `-NoInject` runs with the same game, recipe, vars, label, settings, monitor and variant (perf-context.json `variant`: `-EyeSampler` on, the `-GameIni` / `game_ini` values; runs with them only compare among themselves) (`vr_vs_flat60` = VR fps / (min(flat, refresh) / 2 in AFR)); `prev_build`: the newest PASS runs of another UEVRBackend.dll with the same keys. With 3+ such runs and a spread under 10 %, a 5 % lower fps below their range, a 15 % higher p99, doubled hitches or 10 % more work is a `regression`: a `WARN: perf regression` note, never a verdict |
| `headline` | what the matrix and ladder tables show: fps, 1% low, hitches, FG |

PresentMon needs admin rights or the "Performance Log Users" group (add the account in `compmgmt.msc`, then sign
out and in). Without them it stops at once with "access denied": the harness notes `PresentMon access denied` in
the perf line and goes on. The game's flat numbers (and the `-NoInject` baselines) need it. Nothing here needs
admin rights otherwise; `-PresentMon off` skips it.

Cost: UEVR does a few atomic adds per frame and writes once a second from its own thread; nvidia-smi samples once a
second; PresentMon reads ETW events. Not measured on this PC yet: compare a game's numbers with `VR_PerfLog`
on and off (`-Set "VR_PerfLog=false"`) before trusting small differences.

## Each eye

With monado-service started with `MOPIC_MODE=sbs` (ask first: it changes what the Mopic display shows), the display
shows the left eye in its left half and the right eye in its right half, and a screen capture sees both. Both eyes
come from one submitted frame, so they normally change in the same refresh. `analysis\eyesampler.py` checks that:

```
.venv\Scripts\python analysis\eyesampler.py shot --out strip.png          # the strip it samples (check its place first)
.venv\Scripts\python analysis\eyesampler.py sample --out <dir> --seconds 30
.venv\Scripts\python analysis\eyesampler.py report <dir> [--threshold 1.0] [--print]
```

- `sample` grabs one strip across the whole display (96 px around the middle: `--y`, `--height`) about 120 times a
  second (`--hz`; a grab takes about 3 ms here, the sampler about a third of one CPU core, mostly the grab: every
  arm of an A/B should run it, and frame rates with and without it are not comparable). Both halves come from the
  same grab, so from the same composed desktop frame. Per sample (`eyes.csv`, QPC ns like the frame-rate files): each eye's mean luminance, how much it
  changed since the previous sample (mean absolute luma difference, 0-255), and the left/right match (normalized
  correlation of the halves' column profiles at the best shift: the strip's parallax in px). With `--status
  <pilot-status.json>` (the harness's `-EyeSampler`) it samples only while one of the recipe's measured segments is
  open (it looks for a new one every 50 ms); it stops with the pilot, the game (`--pid`), `--seconds` or a stop file.
  `eyes-shots\` keeps up to 40 candidate frames (previous sample over current, left | right) of one eye changing
  alone or going black. Its picture is from somewhere inside the grab, so each sample's `grab_us` is kept.
- `report` writes `eyes-report.json` (summary, events with QPC and wall-clock times and their segment) and a line:
  `eyes[120.0 Hz, 7 segment(s) 141.3 s, moving 91%]: one-eye lag 3 (max 50 ms = 3.0 refreshes) + 2 of 1 refresh,
  one-eye only 0, one-eye black 1 (max 50 ms) | L/R corr 0.95 shift 24 px`.
  - one-eye lag: one eye changed (its diff over `--threshold` and 4 times the other's) and the other followed more
    than 1.5 refreshes later with a change about as big (1/3 to 3 times: a lagging eye makes the same step later),
    while the scene was moving (3+ samples within +-0.5 s where both eyes changed). A one-refresh lag measures 8-25
    ms (the samples come every ~8 ms) and is counted apart, "of 1 refresh". The eye named is the one that changed
    first. A follower of another size is "unmatched" (`one_eye_change`, not counted as a lag): a small change only
    one eye's strip shows (an animation near the strip's edge, parallax), then the camera moving.
  - one-eye only: the other eye didn't change at all within `--max-lag-ms` (500) while the first kept changing (or
    changed by 5 x the threshold or more); one event per freeze (`frozen_ms`: until the other eye changed again). A
    single small change of one eye followed by a still scene is dropped.
  - one-eye black: one eye's mean under 8 while the other's is 20 or more, grouped per stretch.
  - `uniform` on an event: most of the change was the whole strip's brightness (one view's auto exposure stepping
    alone), not a different frame; counted as "brightness step(s)" in the line.
  - `uncertain` on an event: a sampler gap inside it (the pilot's own 4K captures, `pilot_capture`, can stall it) or
    a grab longer than half a refresh; the summary has the grab times (`grab_ms`), the line "slow grabs" when their
    p95 is over half a refresh.
  - `L/R corr` under 0.6: not side by side (monado-service without `MOPIC_MODE=sbs`?), a WARN in the line.
  - With alternate-eye rendering (AFR in perf.json `mode`) one eye changes per engine frame by design: the harness
    adds that to the line.
- What it can't see: one eye showing an old picture for a refresh while the camera moves (both eyes still change;
  only a left/right comparison of every sample would, and `lr_corr` is computed on every 12th). Not checked yet: what
  the halves show in the Mopic merged state (a tracked viewer left; one view on the woven display). If both halves
  came from one eye there, a fault of the other eye would not show; an L/R `shift` near 0 px would be the hint.
- The checkpoints of `"source": "mopic"` recipes were recorded on the woven display; under `MOPIC_MODE=sbs` the
  pilot reads the left eye stretched back to the full width (`--source mopic-sbs`, the harness's `-MopicSbs`). UI at
  the zero-parallax plane sits where it was, but each eye appears about 3.7% magnified in SBS (MOPIC.md), so test
  a recipe's checkpoints on a live side-by-side screen first (`checkpoint test ... all --source mopic-sbs`).
- `selftest\selftest_eyes.py` runs the sampler on a fake clock against synthetic side-by-side frames with known
  faults (a two-refresh and a one-refresh lag, three black refreshes, a one-eye change in a still scene, a sampler
  stall, a closed segment, jittery grabs).

## gamepilot

```
.venv\Scripts\python gamepilot.py <process> status                        # locked? window? foreground?
.venv\Scripts\python gamepilot.py <process> shot [--source mopic|mopic-sbs|window] [--out PATH] [--max-width 1280]
.venv\Scripts\python gamepilot.py <process> key <name> [--times N] [--hold MS] [--gap MS]    # enter, esc, down, j, alt+f4, shift+w, lmb, look:600,0, wheel:-5, ...
.venv\Scripts\python gamepilot.py <process> keys "down down enter"
.venv\Scripts\python gamepilot.py <process> click <x> <y>                  # in the last screenshot's pixels
.venv\Scripts\python gamepilot.py <process> move <x> <y> / type "text" / focus
.venv\Scripts\python gamepilot.py -         checkpoint save <recipe.json> <name> <x> <y> <w> <h> [--from PNG] [--mode highlight] [--threshold 0.8] [--source mopic]
.venv\Scripts\python gamepilot.py <process> checkpoint test <recipe.json> <name|all> [--from PNG] [--source window]
.venv\Scripts\python gamepilot.py -         matrix <recipe.json> <screenshots/globs...>
.venv\Scripts\python gamepilot.py <process> run <recipe.json> [--steps 9-12] [--source window] [--out DIR] [--status FILE] [--var k=v ...] [--dry-run]
.venv\Scripts\python gamepilot.py <process> dump <out.dmp>
```

- `--source mopic` captures the Mopic display (what the viewer sees). Use it with UEVR: the joeyhodge-based build
  leaves the desktop game window black for most games while it renders. Clicks are mapped back to the game window.
  `--source mopic-sbs`: the same while monado-service runs with `MOPIC_MODE=sbs` (its left eye, stretched back to
  the full width; see "Each eye").
- monado-service (7dcf4a0ad, 66cfe1166, 2026-10-07) makes a UEVR game's borderless window fullscreen on the Mopic
  display (3840x2160, 16:9), right under the click-through "Mopic XR" window. Clicks still map through the game
  window. `--source window` reads a game window on the Mopic display with PrintWindow (a screen grab there sees the
  3D picture). Checkpoints recorded with the game at 2560x1600 on the laptop panel (16:10) don't match the 16:9
  layout (Tekken's UI is larger, Stray's pause menu hint moves, Dead as Disco's menus wrap text differently):
  record them again on the new default, or run old recipes with monado-service started with
  `MOPIC_MOVE_GAME_WINDOW=false`.
- While Windows shows the lock screen, input and capture don't reach the desktop: the screen and input commands
  exit with code 3 and `{"error":"locked"}`, and `run` stops with `error_kind` "locked". Ask the user to unlock;
  auto-lock stays on.
- Key names: letters, digits, `enter`, `esc`, `space`, `tab`, arrows, `f1`-`f12`, `shift`/`ctrl`/`alt`, chords
  (`alt+f4`, `shift+w`: the modifier is held while the key is held), `lmb` / `rmb` (a click where the mouse is),
  `look:dx,dy` (relative mouse motion spread over the hold time: a camera turn in games that read raw mouse
  input; `look:900,0` with hold 700 is about a quarter turn in Wukong) and `wheel:N` (N wheel notches where the
  mouse is, negative = down: scrolls a list). They work in `key`, `keys` and `play` alike.
- Keys are scan codes through SendInput, so DirectInput / Raw Input games see them. The game window is brought to
  the foreground first. If that fails, a recipe's key / keys / click / move steps fail (`error_kind` "focus");
  waiting and seeking steps skip that input (pilot.log warns) and then fail on their checkpoint. Nothing is typed
  into whatever window is in front. The CLI commands report `"focused": false`.

## Recipes

```
{"game": "...", "process": "...", "source": "mopic", "max_width": 1280, "notes": "keys, quirks, quit path",
 "window_fit": [64, 0, 1152, 720], "height": 720, "config": {"FrameworkConfig_MenuOpen": "false"},
 "checkpoints": {"main_menu": {"image": "main_menu.png", "region": [x, y, w, h], "threshold": 0.8}},
 "steps": [...]}
```

- `source`: where the screens are read (`mopic` or `window`); `max_width`: screenshots are scaled to this width,
  checkpoint regions are in those pixels.
- `config`: UEVR config.txt values (as in config.txt: `"true"`, `"1.000000"`) the route depends on, applied for each harness run like `-Set` (Sonic: the UEVR
  menu must stay closed at start).
- `game_ini`: `{"GameUserSettings.ini": {"/Script/Game.Section": {"Key": "Value"}}}`, the game's own settings the
  route depends on, applied for each harness run like `-GameIni` (and put back the same way; booleans become
  `True` / `False`). `-GameIni` overrides a key it also names.
- `window_fit` / `height`: lets a recipe recorded on the Mopic display also drive the game without UEVR
  (`-NoInject`, or `run --source window`): the desktop window's image is scaled into that rectangle of a canvas of
  `max_width` x `height`, where the game image sits in the Mopic capture, so the same checkpoints apply.
- `images`: take the checkpoint images from another recipe's folder (`Tekken8Demo-quit.json` uses `Tekken8Demo`,
  a quick boot-and-quit check without a match).
- `window_timeout`: seconds to wait for the game window before the first step (default 180).
- `save_slot`: `{"dir": "{gamedir}\\b1\\Saved\\SaveGames\\{sid64}", "file": "ArchiveSaveFile.9.sav"}`, where
  `-SaveFile` installs its save. Placeholders: `{sid64}` (the logged-in Steam user's SteamID64, from
  `HKCU\Software\Valve\Steam\ActiveProcess` ActiveUser), `{accountid}` (ActiveUser itself), `{gamedir}` (the
  preset's install folder), `{localappdata}`, `{appdata}`, `{documents}`. The folder has to exist already.
  `-SaveFile` without a `save_slot` (or `-SaveSlot`) is an error.
- `vars`: `{"play_s": 120, "boss": null}`, the names the steps may use as `${name}`, with their defaults (`null`:
  no default, it must be given). `--var name=value` (`-RecipeVars`) sets one; the value is read as JSON when it
  parses (`180`, `[110, 305]`, `true`, `["hud", "loading"]`), else as text. A step string that is only `${name}`
  takes the value's type (`{"play": "${play_s}"}` is a number, `{"click": "${pos}"}` a list), except in key and
  text fields (`key`, `keys`, `press`, `idle_press`, `note`, `shot`, `phase`), which get the text; inside a
  longer string it is text. Only declared names are replaced: a recipe without `vars` is used exactly as
  written (a literal `${...}` in it stays), and `--var` of an undeclared name, or a step to run that uses a var
  without a value, fails the run before the first step (`error_kind` "recipe"). `run --dry-run` prints the
  steps after substitution.
- The harness refuses a recipe without an `expect_exit` step.

Steps run in order (one per line in the file). `"if": "<checkpoint>"` on any step runs it only if that screen is
showing right now.

| Step | Does |
| --- | --- |
| `{"wait_for": "x", "timeout": 30}` | wait until checkpoint x shows. A list waits for any of them. `"press": "enter", "every": 5` presses a key meanwhile (skipping movies), `"click": [x, y]` clicks instead; only after a failed check. |
| `{"wait_gone": "x", "timeout": 180, "for": 3}` | wait until checkpoint x (or any of a list) has not shown for `for` seconds in a row: a loading screen going away, so gameplay starts right when the player can move |
| `{"seek": "x", "press": "down", "max": 8}` | press until x shows (menu cursors don't always start on the same item; menus usually wrap around, so `max` should cover one lap) |
| `{"key": "enter", "times": 1}` / `{"keys": "up enter"}` / `{"click": [x, y]}` / `{"move": [x, y]}` / `{"wait": 2}` | input (`move` hovers: menus that highlight under the mouse) / pause |
| `{"play_until": "x", "while": "hud", "keys": "u i j k a d", "timeout": 600}` | random gameplay keys until x shows; with `while` only while the HUD checkpoint is visible, so they don't act as menu input on result screens |
| `{"play": 60, "keys": "...", "every": 0.6, "hold": 500}` | random gameplay keys for a fixed time (`lmb` / `rmb` click where the mouse is, `look:dx,dy` turns the camera); one `hold` for all its keys, so movement, camera and attacks go in separate `play` steps |
| `{"phase": "exit"}` | from here the game quitting is expected (the harness counts the exit as a menu quit) |
| `{"expect_exit": 60}` | wait for the process to end; still running afterwards = `EXIT_HANG` |
| `{"shot": "name"}` / `{"note": "..."}` | save a screenshot / comment |
| `"measure": true` / `"<label>"` / `false` on a step | its frame rate is measured (see "Frame rate"): `play` and `play_until` are, as `gameplay`, unless `false`; `true` = `gameplay` |

Checkpoints compare a region of the live screenshot with the saved crop (grayscale, shifts up to `margin` px,
default 8):

- default (`shape`): correlation, and the brightness has to agree. Use a region that only this screen has and that
  sits on an opaque part of the UI (a menu title, a key hint, a HUD name plate). Never animated backgrounds, 3D
  scenes, anything a character model can cover, or semi-transparent text over the game scene.
- `"mode": "highlight"` for a menu cursor: the highlight bar's colour decides, the text only has to roughly line
  up. Highlights pulse and shimmer, so shape alone can't tell a selected item from the same item unselected. The
  colour test can match other screens, so only use these right after a `wait_for` of their screen.

Before trusting a recipe, check that each screenshot matches only its own checkpoint(s) (a nested state, like a
menu and its selected item, is fine): `.venv\Scripts\python gamepilot.py - matrix recipes\<Game>.json
runs\discover\<game>\*.png runs\*<Game>*\pilot\*.png`. Don't edit a recipe or its images while a run is using it
(the running pilot has the old regions loaded; a size mismatch fails with a clear error).

## Recording a recipe (discovery, done by Claude Code)

1. Back up the game's saves (`%LOCALAPPDATA%\<project>\Saved\SaveGames`, `Documents\<game>`,
   `Steam\userdata\<id>\<appid>`) into `runs\save-backups\`: loading a save and playing can autosave.
2. Start the game: `run-test.ps1 -Game <Game> -WaitForExit -Seconds 2400 -Label discover` (in the background).
3. Loop: `gamepilot.py <process> shot --source mopic --out runs\discover\<game>\NN.png`, read it, decide,
   `key` / `click`. On each new screen save a checkpoint (`checkpoint save`). Note the key hints the game shows
   (confirm/back keys differ), and which choices would change the player's data (delete / restart / new game over
   a slot / store links): the recipe must never go near them.
4. Find the way into gameplay and the quit path (Options > Quit, pause > Quit to title > Quit, ...). When there is
   no stable in-game HUD, the in-game pause menu is the proof that gameplay was reached.
5. Write the steps, replay parts against the running game with `run --steps A-B`, check the matrix, then run the
   harness with `-Recipe <Game>` a few times, and `-NoInject` once to see the baseline.

## Reading an EXIT_HANG dump

`runs\<run>\exit-hang.dmp` has thread stacks, thread names and the memory the stacks point at:

```
.venv\Scripts\python analysis\hangreport.py runs\<run>\exit-hang.dmp <build>\UEVRBackend.dll 20 > runs\<run>\hang.txt   # per-thread stacks, UEVR frames symbolized (PDB next to the DLL)
.venv\Scripts\python analysis\threadgroups.py runs\<run>\hang.txt runs\<run>\exit-hang.dmp                         # threads grouped by stack, with UE thread names
.venv\Scripts\python analysis\sysexports.py C:\Windows\System32\ntdll.dll <rva>                                    # which system call a thread sits in (sleep vs wait)
.venv\Scripts\python analysis\whatfunc.py <game.exe> <rva> ...                                                     # unsymbolized game code: function bounds, strings, imports
.venv\Scripts\python analysis\disfunc.py <game.exe> <rva>                                                          # disassembly of that function
.venv\Scripts\python analysis\workerobj.py runs\<run>\exit-hang.dmp <game.exe> <return-rva>                        # the object a stuck worker loop runs on (vtable, RTTI name)
```

Stack scanning is heuristic (every stack slot that points into a module): the top frames are reliable, deeper ones
can be stale. Check the top system function before concluding a thread is blocked: `ZwDelayExecution` is a sleep,
`ZwWaitFor*Object*` a wait.

## Game notes

- TEKKEN 8 Demo: J confirm, K back, W/A/S/D move, U/I/J/K attacks. Arrow keys move menu cursors only until the
  first match; afterwards the post-match menu takes W/A/S/D. Quit: Main menu > OPTIONS > last item > yes. The
  opening movie loops for minutes unless a key is pressed. Random stages: HUD checkpoints must not include the stage
  background (the name plate works, the portrait doesn't). Menu cursors wrap around and their highlight pulses.
- Clair Obscur: Expedition 33: Enter confirm, Esc pause/back, F loads the selected save. In the load list R deletes
  a save. Route: Continue > newest save > the camp > pause > 종료 (quit to desktop) > 확인.
- Hozy: mouse driven, read from the desktop window (with UEVR the game's UI doesn't show on the Mopic display).
  Resume > click the house on the rotating island > Continue (never Restart) > the room (no clicks there: they
  move the player's items) > Esc > Quit > Yes.
- Stray: slot 1 > 계속 (never 지우기, it deletes the save) > walk > pause > 나가기 > 예 (to the main menu) > 나가기 >
  예. The pause menu's gray text is see-through: its checkpoint is the ENTER key hint.
- Sonic Racing CrossWorlds Demo: Space confirm, C back. The UEVR menu opens at every start in this profile (the
  recipe's `config` keeps it closed). Only Grand Prix cup 1 and some characters are unlocked; a locked one opens a
  Steam store dialog (the recipe picks Sonic; close such a dialog with 닫기). Time Trial doesn't start in the demo.
- Hogwarts Legacy: mouse-driven menus. Title > click the ARAM HAN character (loads its newest save; never the in-game
  store) > walk > Esc (field guide) > 설정 > 게임 종료 > Space. The process ends about 30 s after the quit (the same
  without UEVR; right after an update it can take over a minute).
  It ignores WM_CLOSE, so `-GracefulExit` reports EXIT_HANG for it: use the recipe.
  Frame generation is `FrameGeneration=(...)` in `[/Script/Phoenix.PhoenixGameSettings]` of
  `%LOCALAPPDATA%\Hogwarts Legacy\Saved\Config\WindowsNoEditor\GameUserSettings.ini` (the NVIDIA app set
  `(Mode=Intel_XeFG,NumFramesInterpolated=1,LocStr="INTEL_XEFG_MODE_X2")`). The game's Off entry is
  `(Mode=Off,NumFramesInterpolated=0,LocStr="Off")` (EFrameGenerationMode Off / Nvidia_DLSSG / Intel_XeFG /
  AMD_FFXFI, read from HogwartsLegacy.exe); choosing Off leaves `r.ChosenFrameGenProvider` (the DXGI swapchain
  provider the game registers at startup) as it was. For one run:
  `-GameIni 'GameUserSettings.ini|/Script/Phoenix.PhoenixGameSettings|FrameGeneration=(Mode=Off,NumFramesInterpolated=0,LocStr="Off")'`.
  Steam Cloud syncs that folder (`steam_autocloud.vdf` in it): the game's copy with the run's value can reach the
  cloud when it quits, before the harness puts the original back; whether Steam ever brings it back down is
  untested, so check the setting a run quit with (`<run>\game-settings\GameUserSettings.after.ini`).
  `recipes\Hogwarts-eyes.json` (not run yet) is the Hogwarts route with a steady camera turn and two field guide
  toggles for the eye sampler (each toggle measured from the Esc press for 3 s, without the pilot's captures);
  `plans\hogwarts-fg-nsf.ps1` runs it XeFG on / off x Native Stereo Fix on / off (`-DryRun` prints the harness
  commands; read its header before running it). Its table shows per run the frame generation setting the game quit
  with (MISMATCH when it isn't the arm's) and UEVR's Present passes per VR frame (about 2 while XeFG generates
  frames, if UEVR's hook sees them), so an fg-off arm that wasn't off shows.
- Black Myth: Wukong: mouse-driven menus that highlight under the mouse (the recipe hovers first). The first start
  after an update compiles shaders for about a minute. Title > 게임 계속하기 (never 새 게임) > shrine > walk (never E
  there) > Esc > 설정 > 게임 종료 > 바탕 화면으로 > 확인. A config saved by this line can have
  `FrameworkConfig_RememberMenuState=false`, which opens the UEVR menu over the title screen and swallows the keys;
  the recipe's `config` keeps it closed.
  Save ladder (`recipes\Wukong-save.json`, `ladders\Wukong.json`): the rung's save goes to slot 9
  (`ArchiveSaveFile.9.sav`). 게임 계속하기 loads the journey whose save date (inside the file, not the file time)
  is newest, so it never picks an old community save; the route is 게임 로딩 > the list (newest first; the
  community saves are older than every journey of the user, so slot 9 is the last entry: wheel to the end, hover,
  E) > 확인. A cleared save then asks 새 라운드 (NG+): 취소 loads it as it is, 확인 would start NG+. R in the list
  deletes a save. The pause menu's tab count grows with progress (5 early, 6 with 근기), so the route presses D
  until the 설정 tab instead of clicking it. The quit question gets a second line ("unsaved progress is lost")
  when the game hasn't saved lately. The loading screen (20-60 s) animates its bottom ornament (icon and swirls
  fade out, sometimes all of it for 5 s and more), so the band behind the tip title (`loading_band`, the same on
  every tip; it also shows on the respawn loading after a death) is the loading checkpoint: the recipe waits until
  neither has shown for 5 s (`wait_gone`) and plays from there; a fixed wait either idles in a fight (Ch1 spawns next to wolves) or starts on a black
  screen. Keys: WASD, Shift sprint, Space dodge, Ctrl jump, LMB/RMB attacks, V staff
  spin, 1-4 spells, mouse camera; avoid E (shrine), Q (items), R (gourd), P (photo mode), M (map). After a run the
  game has re-saved slot 9 with today's date, which makes it the journey 게임 계속하기 loads (the plain `Wukong`
  recipe then plays the community save instead of the user's slot 2) until the next ladder run puts an old save
  back. Shrine travel (`recipes\Wukong-shrine.json`, a sketch: its default path, Ch1 앞산, passed once through the harness; other rows untested): from the
  post-game save, E at the Zodiac Village shrine > 축지 > region > area > shrine > E (a click only selects);
  `region_y` / `area_y` / `shrine_y` are the rows to click, listed in its notes. In the shrine menu only 축지 and
  나가기 are safe (rest, skills, crafting, shops and rematches change the save).
- Dead as Disco Demo (UE 5.7, custom engine branch): mouse-driven menus, read from the desktop window (the Mopic
  display doubles the UI text). The very first start asks about Streamer Safe Mode (click OFF); a fresh install shows
  NEW GAME instead of CONTINUE. Route: CONTINUE > stage hub > Enter > FREE PLAY > first song > click again to play >
  attack with the mouse > Esc ends the song (RESULT) > Stage Select > Esc > EXIT GAME > EXIT GAME on the wishlist
  screen (no confirmation). Never GET THE GAME!, GIVE FEEDBACK or SIGN UP (store/browser). A played song posts its
  score to the Steam account's leaderboard. `play` steps can click with `lmb` / `rmb`. With Native Stereo Fix
  active the desktop window shows the 3D scene behind the menus (without it, black), so the checkpoints sit on opaque
  UI; the gameplay HUD pulses with the beat and loses hearts, so the RESULT screen after Esc is the gameplay proof.
  To look at each eye, restart monado-service with `MOPIC_MODE=sbs` (the display then shows left | right).