# Right-eye cost experiments (exp/right-eye-cost)

Scripts to measure what the right eye's render costs with the native stereo fix (right eye rendered as a second view
family, PRIMARY, Same Pass on), and to A/B the experiment switches of this branch. Experiment code only: nothing here
changes a default. Run folders stay in the mopic-test harness's `runs\`. Logs and side outputs go to `-OutDir`
(default `%LOCALAPPDATA%\right-eye-cost`). Nothing in this folder needs a path edit. Pass the harness folder with
`-Harness` or set `MOPIC_TEST_DIR`.

## Files

| file | what |
|---|---|
| `cost_run.ps1` | one batch of runs through `run-test.ps1` with per-run UEVR settings (arms: plain, ab, abpm, pm, hold:, flat, nort; `-Set` for extra keys) |
| `walk_block.ps1` | whole runs of the walk route, one GDF mode per run, save files checked and put back around every run |
| `runtime.ps1` | starts / stops the Hub-installed OpenXR service for SBS per-eye captures (`-Runtime sbs`) |
| `free_check.ps1` | end-of-block check: engine DLL restored from a backup, service stopped, processes, RunningAppID, keys, saves |
| `firstframe_watch.ps1` | waits for a run's first in-play binocular frame (confirm the right save loaded) |
| `exitwatch.py`, `gpu_proc_sampler.ps1` | side samplers `cost_run.ps1` starts |
| `recipes\Wukong-walk-nomod.json` | the harness's Wukong-save route without modifier keys, play_s 150 (~11 min). `{HARNESS}` is resolved by `cost_run.ps1` |
| `analysis\*.py` | analysis (Python 3 with numpy and Pillow, e.g. the harness's `.venv`) |

## Experiment keys (UEVR config, per run through `-Set`)

- `VR_ExperimentPassMarkersSeconds=7`: per-pass D3D12 captures (CPU recording time per pass and GPU timestamps, 60 presents every 7 s) into the game's UEVR folder. `cost_run.ps1` moves them into the run folder (`passmarkers\`).
- `VR_ExperimentSecondFamilyCvars=10,secondary,skip`: 10 s QPC windows take turns: phase 0 right eye PRIMARY (as shipped), 1 SECONDARY (Same Pass off), 2 the right eye's render skipped.
- `VR_ExperimentRightEyeGdfShare`:
  - `0` off. The shipped right-eye GDF refresh runs (`VR_NativeStereoFixSecondGlobalDistanceFieldRefreshFrames=15`, on change 8).
  - `1` the right eye renders with the left eye's Global Distance Field all run. The two view states' GDF blocks are exchanged around the right family; the right refresh is skipped.
  - `2` A/B in 10 s windows. `N >= 10` A/B in N-second windows. Share is in the even windows of QPC time.
  - The log shows `[GdfShare] Global Distance Field block found ...` once in-world. Per window it shows `[GdfShare] window N (share|refresh): right families, exchanged, not shared (...)`.
- AutoRes is pinned off for measurement by `cost_run.ps1` (`VR_AutoRes=false;VR_AutoResKeepSwapchains=true;Mopic_ConfigVersion=4`) unless `-AutoRes`.

## Analysis

- Window A/B (`ab`, `abpm`, GdfShare `2`/`N`): `phase = floor(qpc seconds / window) % (groups + 1)`, the first `--guard` s of each window left out, play phase only.
  - `phase_stats.py <run> --window 10 --groups 2`: fps and GPU per phase (perf.csv, gpu.csv).
  - `phase_categories.py <run> --window 10 --groups 1 --names share,refresh`: GPU ms per frame by category and family (L, R, T) from pass markers.
  - `abcpu.py <cell> 10 --groups N`: per-thread CPU (game, render, RHI, task graph) per phase from `cost_run.ps1 -Sampler` output (`cells\...`). For GdfShare `2` use `--groups 0` (on = even windows = share).
  - `eyes_phase.py`, `eyes_ratio.py <run> --window 10 --groups 1 --names share,refresh`: eye equality per phase. `eyes_phase.py` gives right/left luma and L/R correlation from eyes.csv plus binocular detail mismatch. `eyes_ratio.py` gives the per-block right/left brightness ratio, median and p10.
- Whole runs, one mode each:
  - `route_compare.py label=<run> ...`: per route minute (time since the pilot's play phase): fps, hitches over 2x / 3x the median present interval, R/L luma, block ratio. Also every big hitch with its route time, and GdfShare totals.
  - `hitch_kinds.py label=<run> ... [--window 10 --groups 1 --names share,refresh]`: the frame-interval list summary (frames, median, p90, p99) and hitches (> 2x / 3x the median interval). Each hitch is split by the column that covers more than half of the hitch interval in perf-frames.csv: game tick (`tick_us`), render thread blocked (`rt_blocked_us`) or present (`present_us`). When none does, it counts as "other" (GPU or elsewhere). Works on whole runs and, with `--window`, per phase.
  - `run_categories.py label=<run> ...`: GPU ms per frame by category and family, play phase only.
  - `frame_gdf.py label=<run> ...`: per-frame GDF and unnamed-compute GPU per family, and on hitch frames.
- Shader names for the categories: the build writes each new shader's bytecode to `%APPDATA%\UnrealVRMod\<game exe>\passmarkers\shaders\`. That folder accumulates over runs, and `cost_run.ps1` moves only the capture files, not this folder. Run `dxilnames.py "%APPDATA%\UnrealVRMod\<game exe>\passmarkers"` once to write `names.tsv` there, then copy `names.tsv` into each run's `passmarkers\`. Without it every pass falls into the shape-only or "unnamed" categories, and the GDF rows read zero.
- Pass markers: the family split assigns L / R / T by the eye output binds. At `OpenXR_ResolutionScale` 1.417 the right eye lands in "T". `psotable.py` and `categories.py` list per PSO and per category. `sizecost.py` splits fixed per-view cost from per-pixel cost across two eye sizes.

## Gotchas

- Walk route saves: see the header of `walk_block.ps1`. Hash-check the save files before every run, and put them back after it.
- The harness's post-run binocular analysis is slow: about 6.6 s per frame on blade18 since 2026-11-22 (1.3 s the evening before, cause not found). 219 frames took 24 min; 60 frames took 6.7 min.
  - `cost_run.ps1 -DeferBinocular` (on in `walk_block.ps1`) holds the frames out of the run (`hold_frames.ps1`) and puts them back afterwards, so the block's time is game time.
  - Analyze them after the block with the harness's `analysis\binocular.py analyze <run>`, or read them directly with `route_compare.py` / `eyes_ratio.py`.
- A run stopped mid key-press can leave a modifier logically down. The pilot's Esc then becomes Ctrl+Esc, and every launch fails with "could not bring the game window to the foreground". `cost_run.ps1` refuses to start with a modifier down and releases the recipe keys after a run.
- The service's stdout can hold license text: never print or share `service-logs\` whole.
