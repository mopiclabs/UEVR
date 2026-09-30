# Mopic UEVR — joeyhodge base

This branch (`mopic/joeyhodge-base`) is the Mopic UEVR product line. It is built on
[joeyhodge/UEVR](https://github.com/joeyhodge/UEVR) releases, which support up to UE 5.8.3, plus a small
queue of Mopic patches for the Mopic glasses-free 3D display (Monado "MOPIC 3D Display" OpenXR runtime,
eye tracking through Mopic Hub).

- Base: joeyhodge release `092826MONO+5_8_3_UEVR` (`49c423e`), local tag `joeyhodge/092826MONO+5_8_3_UEVR`.
- `git describe --tags` (and the "Tag / Commits past tag" lines at the top of UEVR's log.txt) shows which
  joeyhodge release a build is based on and how many Mopic commits sit on top.
- The older Mopic line (praydog-based `mopiclabs/master` + early-injection fixes, branch
  `fix/early-injection-crash`) is kept only until this branch has proven itself on the shipped titles.

## Mopic patch queue

Keep these as separate commits on top of each joeyhodge release. Re-check each one when updating.

| Commit | What it does | Why Mopic needs it | Check after an update |
| --- | --- | --- | --- |
| OpenXR: Recompute eye projections when the runtime FOV changes | Port of praydog/UEVR#372: `OpenXR::update_matrices` re-derives the eye projections whenever `views[].fov` changes (`last_fovs`). | Mopic's runtime changes FOV/convergence at runtime; without it the image keeps the first frustum. | FOV follows Mopic Hub changes on the display. |
| VR: Fix the Native Stereo Fix right eye on Mopic's TEKKEN 8 setup | (1) Compare D3D12 devices by identity/adapter LUID (`utility::is_same_d3d12_device`) when publishing/validating the scene capture. (2) When the engine version can't be read from the exe, pick the UE5.0–5.3 `FSceneViewInitOptions` layout from the discovered ViewFamily offset (0x140 vs 0x158). (3) Give the second `BeginRenderingViewFamilies` submission its own upscaler interfaces (port of Mopic `2e5b492`). | Without (1) and (2) the right eye never gets the capture and shows an unrendered area (the "one eye darker" report). Without (3) TEKKEN 8's FSR1 upscaler crashes once the right eye renders. | log.txt shows `[NativeStereoFix] state=active`, and no repeated `Refusing to publish a scene-capture resource` / `Candidate eye pair rejected`. On TEKKEN 8: `Gave the second view its own upscaler interface +0x130`. |
| VR: Enable Native Stereo Fix by default | `VR_NativeStereoFix` defaults to on. | Mopic's display needs it on native-stereo titles. | A game without a saved config starts with Native Stereo Fix enabled. |
| OpenXR: Rate-limit the projection derivation logs | FOV-driven re-derivations log at most every 5 s. | Otherwise ~7 log lines per frame (MBs per minute) on Mopic's runtime. | log.txt stays small (a few hundred KB for 2 minutes). |
| VR: Allocate the SceneViewExtensions array with the game's FMalloc by default | `VR_UseFMallocSceneViewExtensions` defaults to on; a one-time config migration (`Mopic_ConfigVersion` 1) turns it on in existing configs; only used when FMalloc and its Malloc slot were found. | Games reallocate UEVR's array with FMallocBinned2 on exit and hit a fatal error (Sonic Racing CrossWorlds: "Attempt to realloc an unrecognized block"). | Graceful exit (`-GracefulExit`) passes; log.txt shows `Migrated config to Mopic defaults version 1` once per old config. |
| VR: Turn the Native Stereo Fix off for the session when it can't activate | Watchdog: 20 s outside Active while 3D view families are rendered and the runtime asks for frames → the fix is turned off for the session (plain native stereo). | This line's fix fails closed (unrendered right eye); with the fix on by default that would black out titles where it can't activate. | Titles where the fix works never log `Not active after 20 s`. |
| D3D12: Reuse the last right-eye capture when a packet is refused | While the fix is Active, a frame whose packet is stale/missing reuses the capture from the last 500 ms instead of the unrendered backbuffer half. | Sonic Racing CrossWorlds refuses packets ("delta=2") around level changes → right eye flashed black. | `reusing the last right-eye capture` only around transitions. |
| OpenXR: Accept off-axis frusta and keep FOV-only updates cheap | FOV validity only requires a non-degenerate frustum; exact view_bounds mapping; single FOV read; no render-target resize on FOV-only updates. | Mopic's off-axis frustum can leave the view axis (eye past the panel edge), which froze the realtime FOV. | No `Refusing to recalculate eye projections` while moving in front of the display. |
| VR: Compare devices by identity in the scene-capture reallocation path | Same device comparison as the publish path. | Avoids rebuilding the capture on every reallocation on proxy-device setups. | — |
| Framework: Don't tear the framework down while the game exits | `DllMain(DLL_PROCESS_DETACH)` with a non-null `reserved` (process terminating) releases `g_framework` instead of letting the CRT destroy it. | The static destructors released D3D12 resources into the GPU driver after ExitProcess had killed its threads: TEKKEN 8 stayed in `~TextureContext` → dxgi → Intel driver forever after a menu quit (upstream has the same DllMain). | Menu-exit test (`-Recipe`) passes with no `EXIT_HANG`; the process is gone a few seconds after the quit. |

## Updating to a new joeyhodge release

```
git fetch --no-recurse-submodules joeyhodge "+refs/tags/<release>:refs/tags/joeyhodge/<release>"
git switch mopic/joeyhodge-base
git merge joeyhodge/<release>          # merge, don't rebase: keeps history and later pushes fast-forward
git submodule sync --recursive
git submodule update --init --recursive
```

- Use `--no-recurse-submodules` on fetches from `joeyhodge`: the UESDK submodule points at
  [joeyhodge/UESDK](https://github.com/joeyhodge/UESDK) and a recursive fetch against another remote fails.
- The `joeyhodge` remote has `tagOpt = --no-tags`, so joeyhodge's ~150 release tags don't flood the tag
  namespace. Fetch the release you need into `refs/tags/joeyhodge/` as above.
- If joeyhodge reworks one of the patched areas, re-apply the Mopic change by hand and keep the same check.

## Building

- Visual Studio 2022 (v143), CMake (the VS-bundled one works), Windows SDK 10.0.26100.
- `NoDefaultCurrentDirectoryInExePath` must not be set in the build shell, or DirectXTK's
  `CompileShaders.cmd` step fails with MSB8066 (Claude Code's shell sets it).

```
cmake -S . -B build-jh -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-jh --config Release --target uevr
```

Output: `build-jh\bin\uevr\UEVRBackend.dll` (+ `.pdb`). Mopic Hub loads it from
`%APPDATA%\MOPIC\mopichub\engines\mopic-uevr\<version>\UEVRBackend.dll`.

## Verification

The early-injection harness (`patches\early-injection\run-early-injection-test.ps1` in the working copy of
the main repo, not committed) starts `UEVRInjector.exe --attach=<game>`, launches the game through Steam and
classifies each run (PASS / CRASH / EXIT_CRASH / NO_VR). Keep Mopic Hub and monado-service running during
tests. Mopic Hub provides eye tracking.

```
powershell -ExecutionPolicy Bypass -File patches\early-injection\run-early-injection-test.ps1 -Game Tekken8Demo -Runs 3 -GracefulExit -Label <label> -Dll <path to UEVRBackend.dll>
```

To test real gameplay and the game's own quit path, `patches\early-injection\tools\gamepilot.py` drives the game
by looking at the screen (the Mopic display) and sending keys. A recorded recipe replays it unattended:

```
... -Game Tekken8Demo -Recipe Tekken8Demo -Runs 3 -Label menu      # title > PvC match > main menu > Options > Quit
... -Game <Game> -WaitForExit -Seconds 1800 -Label discover          # someone (Claude Code) drives and quits it
```

Verdicts add `MENU_FAIL` (the pilot couldn't follow the recipe, UEVR was fine) and `EXIT_HANG` (the game started
exiting but its process was still there 30 s later; `exit-hang.dmp` has the stacks). `tools\README.md` explains
recipes and how to record one for a new game.

Useful options: `-GracefulExit` (close the window like a normal quit and catch exit crashes),
`-Set "Key=Value;..."` (config overrides for one run), `-UserScript "cmd;..."` (console commands),
`-InjectDelay <s>` (late-injection control), `-Seconds <s>`, `-Game Custom -ProcessName <exe> -SteamInstallDir <dir>`.

Results on 2026-09-30 (early injection, graceful exit, this branch):

| Title | UE | Result |
| --- | --- | --- |
| TEKKEN 8 Demo | 5.2.1 | PASS; Native Stereo Fix active, FSR1 upscaler separated |
| Clair Obscur: Expedition 33 | 5.4.4 | PASS; fix active, temporal upscaler separated |
| Sonic Racing CrossWorlds Demo | 5.4.3 | PASS; fix active, cached right eye on refused packets; exit crash fixed |
| Hozy | 5.6.1 | PASS with the fix on; fix active |
| Stray | 4.27 | PASS; fix active |
| Hogwarts Legacy | 4.27 | PASS; fix active (the game ignores WM_CLOSE, the harness ends it) |
| Black Myth: Wukong | 5.0 | PASS; the fix never activates, watchdog falls back to plain native stereo after 20 s |
| Assetto Corsa Competizione | 4.26 | no stereo rendering within 100 s at the title screen; not conclusive |

Brightness of both eyes and realtime FOV were checked by eye on the Mopic display (2026-09-30). There is no
automatic per-eye luminance check yet.

Before the exit fix the harness couldn't see exit hangs: .NET's `HasExited` turns true as soon as the exit code is
set, and the harness killed the leftover process afterwards. The "graceful exit" PASS results above predate the
`EXIT_HANG` check; re-run them with `-GracefulExit` or a recipe.

## Known gaps

- TEKKEN 8: an intermittent hang after quitting through the menu, separate from the exit fix above. 2 of 9 full
  recipe runs with UEVR hung (the game never reaches ExitProcess), 0 of 10 without UEVR (`-NoInject`). In the dump
  the GameThread is in a "stop worker and join" (`SetEvent` + `WaitForSingleObject`, exe+0x5e09670) and the worker
  (an unnamed thread, loop at exe+0x5e09270) keeps asking its object whether the work is done (vtable+0x10) and
  sleeping 100 ms instead of exiting. UEVR code isn't on any stack. The harness reports `EXIT_HANG` and writes
  `exit-hang.dmp` with the memory the stacks point at; on the next occurrence
  `tools\analysis\workerobj.py exit-hang.dmp <exe> 5e09369` should name the worker's class (vtable/RTTI).

- Wukong (UE 5.0): the fix stays in "learning an exact primary/secondary eye pair" (no candidate family is
  found, nothing is logged); the praydog-based line did render the second view there. Needs a closer look.
- The cached right-eye fallback is D3D12/OpenXR double-wide only (not D3D11, texture-array or OpenVR).
- joeyhodge compares D3D devices by raw pointer in 20+ other places (UI composition, DIBR, alpha passes).
  Only the scene-capture paths use `is_same_d3d12_device`; the others may fail the same way on Mopic setups
  when those features are used.
- Other version gates (`is_ue_5_4_runtime`, `is_ue_5_5_runtime`, ...) use the same exe string / file-version
  detection. Only the UE5.0–5.3 layout choice has the layout-based fallback.
- `VR_NativeStereoFixPreserveSecondaryPass` (default on, UE5.5+ only) ties the right eye's exposure to the
  left eye's view state. Watch for an exposure difference between the eyes on UE5.5+ titles.
- A saved `config.txt` keeps its stored `VR_NativeStereoFix` value, so the new default only applies to titles
  without one (only `VR_UseFMallocSceneViewExtensions` is migrated).
