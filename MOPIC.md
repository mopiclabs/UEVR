# Mopic UEVR — joeyhodge base

This branch (`mopic/joeyhodge-base`) is the Mopic UEVR product line. It is built on
[joeyhodge/UEVR](https://github.com/joeyhodge/UEVR) releases, which support up to UE 5.8.3, plus a small
queue of Mopic patches for the Mopic glasses-free 3D display (Monado "MOPIC 3D Display" OpenXR runtime,
eye tracking through Mopic Hub).

- Base: joeyhodge release `101226HELLRAISERp1` (`387ccb7`, on joeyhodge's `ue57performance` release line), local tag
  `joeyhodge/101226HELLRAISERp1`.
- `git describe --tags` (and the "Tag / Commits past tag" lines at the top of UEVR's log.txt) shows which
  joeyhodge release a build is based on and how many Mopic commits sit on top.
- Lineage: until 2026-10 this branch sat on `092826MONO+5_8_3_UEVR` (`49c423e`), which joeyhodge cut from his
  `eBaseballfocused` line (the DIBR / Mono / ProSpi experiments). Every later release tag is on `ue57performance`, and
  the two lines exchange the same changes as cherry-picks with different hashes (common ancestor praydog `4ee5c6b`),
  so merging a new tag the usual way conflicted in 39 files. On 2026-10-14 the Mopic patch queue
  (`49c423e..14f5198`, 92 commits) was replayed with `cherry-pick -x` onto `101226HELLRAISERp1`; the Mono-only parts
  (UI alpha swapchain generation guard, Mono perf flag, Mono copy paths) were left out because that line has no Mono
  mode. The old line is kept as the second parent of a history-only merge, so old builds' commits stay reachable.
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
| VR: Show in the menu when the watchdog turned the Native Stereo Fix off | The Native Fix status reads "off for this session: not active after 20 s (plain native stereo)" instead of joeyhodge's generic "skipped: title/runtime guard". | The generic text looked like a deliberate per-title block (reported on Wukong). | Only shown after `Not active after 20 s` in log.txt. |
| (UESDK worker lock: now upstream) | Dropped on the ue57 line: joeyhodge's UESDK (`0432e74`, in `e667df0`) already takes the queued jobs out under the enqueue lock and runs them without it, restores unfinished work in front, and adds a re-entrancy guard. Our `026791d` would have replaced that newer code. | The game-thread/render-thread enqueue deadlock (Dead as Disco froze, 1 in 3 runs with 21 s late injection like Mopic Hub). | Dead as Disco with `-InjectDelay 21`: no FREEZE; `freeze.dmp` stacks never show two threads in `ThreadWorker::enqueue`. |
| UESDK: Wait for a texture's render resource instead of rescanning its offset (submodule) | `UTexture::update_render_resource_offset_texture2d` returns "not ready" when the resource at the known/cached offset is still null. | A texture probed right after its creation reset the global offset and a rescan picked another member (0x180 for 0x130), so the next resource read called a null vtable slot (crash at `begin_render_viewfamily_real`). | `Found UTexture::PrivateResource offset` keeps one value per session. |
| VR: Keep the synced redraw and the dedicated UI publish ordered without the worker lock | The synced-AFR redraw no longer queues another redraw from its own nested draw; destroy/cancel and the game-thread publish of the dedicated UI texture share a mutex. | Both relied on the old worker lock (a nested job was dropped / `enqueue` blocked while the game thread ran its jobs). | — |
| VR: Recognize Dead as Disco's UE5.7 FSceneViewFamily copy | Title-gated (`PagodaSteamDemo-Win64-Shipping.exe`, file version 5.7.4.0, DX12) fallback in `resolve_ue57_fsceneviewfamily_functions` with this exe's copy-constructor bytes (`DeadAsDiscoNativeFix.hpp`); the vtable and destructor still go through the generic UE5.7 checks. Compatibility tests cover the gate, single-byte changes, the StateTree lookalike and a fixture of the real bytes. | Its layout is stock UE5.7, but the copy constructor holds the source in rbx (`48 8B DA`, not `48 8B FA`), so the generic signature only found an unrelated StateTree copy, the linked-family fix failed closed and UE5.7 left the right eye's 3D black. | log: `[DeadAsDisco][UE5.7][NativeStereoFix] Validated the stock-layout family copy`, `copy constructor RVA 0x3d5d8e0`, `state=active`. |
| D3D12: Size the Native Stereo Fix copies by the eye | The double-wide copy of the left eye and the capture (`pre_render`, `pre_render_cached`) takes its boxes and the right-eye offset from the HMD eye size, not `m_backbuffer_size`; a packet whose left source is smaller than two eyes is refused so the frame copies the whole target. | `m_backbuffer_size` is only set in `setup()`, which on Dead as Disco ran against the 1280x800 Slate texture before the VR-sized target arrived: the left eye's top-left 640x800 went to (0,0) and the right eye to x=640 (SBS showed a displaced picture, right/left brightness 0.2). | SBS (`MOPIC_MODE=sbs`) shows a full eye in each half; no `Left-eye source ... is smaller than two` warning; titles with a 3840-wide setup (TEKKEN 8, Wukong) copy the same boxes as before. |
| VR: Resolve the renderer entry from the view-extension callback's caller | When the singular-wrapper search finds nothing (UE4 before 4.25, UE5.0), the caller of UEVR's BeginRenderViewFamily callback is the `BeginRenderingViewFamily` entry if the callback's return address follows `CALL [reg+slot*8]` for that slot, whatever its unwind segment's size. Diagnostics: the values behind "unexpected FSceneViewFamily vtable", and why no family could hold the eye pair. | Black Myth: Wukong's entry has chained unwind info and its part up to the callback is 0x193 bytes, under the stack fallback's 0x200 minimum, so `FViewport::Draw` 7 frames up was hooked; every call failed validation and the fix never activated. praydog's line hooks the caller directly. | Wukong: `Resolved the callback's caller as the BeginRenderingViewFamily entry target=14d6bcd70` and `[NativeStereoFix] state=active`. Titles resolved through the wrapper or the UE4.25–4.27 path log the same lines as before. |
| D3D12: Reuse the last right-eye capture when a packet is refused | While the fix is Active, a frame whose packet is stale/missing reuses the capture from the last 500 ms instead of the unrendered backbuffer half. | Sonic Racing CrossWorlds refuses packets ("delta=2") around level changes → right eye flashed black. | `reusing the last right-eye capture` only around transitions. |
| OpenXR: Accept off-axis frusta and keep FOV-only updates cheap | FOV validity only requires a non-degenerate frustum; exact view_bounds mapping; single FOV read; no render-target resize on FOV-only updates. | Mopic's off-axis frustum can leave the view axis (eye past the panel edge), which froze the realtime FOV. | No `Refusing to recalculate eye projections` while moving in front of the display. |
| VR: Compare devices by identity in the scene-capture reallocation path | Same device comparison as the publish path. | Avoids rebuilding the capture on every reallocation on proxy-device setups. | — |
| Framework: Don't tear the framework down while the game exits | `DllMain(DLL_PROCESS_DETACH)` with a non-null `reserved` (process terminating) releases `g_framework` instead of letting the CRT destroy it. | The static destructors released D3D12 resources into the GPU driver after ExitProcess had killed its threads: TEKKEN 8 stayed in `~TextureContext` → dxgi → Intel driver forever after a menu quit (upstream has the same DllMain). | Menu-exit test (`-Recipe`) passes with no `EXIT_HANG` noted "stuck in shutdown"; the process is gone a few seconds after the quit (an `EXIT_HANG` noted "never called ExitProcess" is the known gap below). |
| VR: Log frame rate and frame pacing to perf CSVs | `VR_PerfLog` (default off since 2026-10-06; a one-time config migration, `Mopic_ConfigVersion` 2, turns it off in existing configs; `run-test.ps1` turns it on and raises the version for its runs) writes `perf.csv` (1 s rows: VR fps, new/repeated submits, xrWaitFrame/xrEndFrame times, Presents per engine tick, NSF fresh/reused/fallback) and `perf-frames.csv` (one row per `xrEndFrame`) next to log.txt; a submit counts as new only if its engine frame is newer than any submitted before. | UEVR's "Show FPS" sends `stat fps` once (screen only, stripped in Shipping); the harness needs a per-run frame rate, and frame generation doubled the naive count (Hogwarts "40 fps" was 20). | `perf.csv` exists after a run; `presents_per_tick` is 1.00 without frame generation. |
| VR: Show the resolution and frame rate in a label | `VR_ResolutionLabel` (default off) draws a window at the top of the framework UI layer with the VR resolution scale, the eye image size, the size the game renders at (`r.ScreenPercentage`, read once a second) and the VR frame rate (new engine frames submitted, the `VR_PerfLog` rule, counted with the log off too). `Framework::is_drawing_anything` keeps that layer up while it is on; controller mouse emulation only follows the menu or the always-on cursor (`is_drawing_interactive_ui`), so the label doesn't take the pointer. | Measurements and demos of resolution levels need to show what they run at, in the headset (tried live on Wukong, 2026-10-27, with the design's five levels). | With `VR_ResolutionLabel=true` the label shows in the headset with the menu closed and its FPS matches perf.csv's `vr_fps`; off, nothing changes. A recipe whose checkpoints sit near the top centre may need it off. |
| VR: Live resolution changes without a black eye or a new swapchain | A live resolution-scale change (the slider, through `OpenXR::apply_resolution_scale`) no longer copies a game render target of the old size into a swapchain of the new one (the overlap is copied); the left eye goes in both halves while the right one would be black: under the Native Stereo Fix until `VR_AutoResNsfHoldMs` (700) after its new capture is accepted (D3D11 also until the fix has been active again for 200 ms), without it for `VR_AutoResSourceHoldMs` (150) after the render target takes the new size. `VR_AutoResKeepSwapchains` (default off) keeps a double-wide swapchain that fits the new eye pair: the pair goes to its top left and the projection views cover just that. D3D11 and D3D12; not the texture-array submit or AFR. | Live-resolution research (2026-10-24/25, exp/live-res): a black right eye after nearly every change in every game, and compositor GPU faults during the swapchain rebuild (with the Mopic SDK queue-lock fix in monado, 570 forced changes on two PCs had no fault). | Forced live changes (exp/live-res's `VR_AutoResToggleSeconds`, or the slider): SBS eye check without a black right eye; with keep-swapchains, `Creating OpenXR swapchains` only at startup; no `nvlddmkm` event. |
| Framework: End the OpenXR session when the game's window is gone | Once the window the game last presented to got WM_DESTROY and no Present has run for 2.5 s, the D3D monitor thread destroys the session (try-locks only); a Present to a live window reinitializes OpenXR. `VR_EndSessionOnGameExit` turns it off. The XInput callbacks hold `m_openxr_session_exit_mtx` shared while they use the session (ue57 line: not `m_actions_mtx`, which upstream keeps out of XInput bookkeeping; `gamepad-recovery-tests`). | Hogwarts takes ~30 s from destroying its window to ExitProcess (D3D12 teardown in the NVIDIA driver, vanilla too) and the Mopic display showed the frozen last frame all that time. | Hogwarts: frozen tail ~2 s instead of ~36 s (monado `app_frame_stats.csv`); TEKKEN 8 / E33 / Dead as Disco menu exits still PASS. |
| VR: Turn frame generation off while the Native Stereo Fix is on | `VR_NativeStereoFixDisableFrameGeneration` (default on): once a second, while the fix is on, `r.XeFG.Enabled`, `r.Streamline.DLSSG.Enable`, `r.FidelityFX.FI.Enabled` are held at 0 and put back when it is off; a write the game refuses (Wukong's DLSSG, held above SetByCode) is retried at SetByConsole priority, and a cvar the game keeps even then is logged once and left alone. | UEVR copies the engine's eye targets, so generated frames never reach the display; their extra Presents land between the two eye renders and send the right eye one engine frame late (Hogwarts XeFG X2: 150-260 one-refresh splits per run). The engine frame rate is the same either way (~20 fps). | log `[FrameGen] Native Stereo Fix is on: r.XeFG.Enabled 1 -> 0`; Wukong with HAGS: `r.Streamline.DLSSG.Enable refused 0 at SetByCode; with SetByConsole: 1 -> 0`; `presents_per_tick` 1.00; games re-enable it at level loads / menus and it is off again within 1 s. |
| VR: Submit once per engine frame under frame generation | `VR_SubmitOncePerEngineFrame` (default on, OpenXR, not AFR): a Present pass with no new engine frame while the engine produced one in the last 250 ms skips the copy and the xrWaitFrame / xrBeginFrame / xrEndFrame trio; repeats while the engine stalls still submit. | Frame generation the cvars can't turn off would otherwise re-submit every frame and add an xrWaitFrame per generated frame. | Non-FG titles skip only a handful of passes per run (log count); no MENU_FAIL from frozen menus. |
| D3D12: Experimental per-engine-frame eye-pair snapshot (off by default) | `VR_NativeStereoFixPairSnapshot`: after the engine's first direct-queue submission following the pose callback, both eyes are copied into a 3-slot ring on the engine queue; Present passes off the engine thread submit the newest pair the GPU finished (no GPU wait). | For titles whose frame generation can't be turned off. The first version made XeFG's present queue wait on the snapshot fence and hung Hogwarts (wait cycle with XeFG's own fence). | Hogwarts with frame generation left on: no hang, frozen pairs 100%, 0 one-refresh splits; ~90% of passes take the pair one engine frame older. |
| VR: Give the Native Stereo Fix right eye the left eye's auto exposure | `VR_NativeStereoFixShareExposure` (default on): for the second submission the right view's exposure view-state field (found at run time: the third copy of `FSceneView::State` past 4 KB, Wukong UE5.0 `+0x24f0`) points at the left eye's state, then is put back. Dead as Disco (UE5.7) has no such copy and is left alone. | The same-pass relabel makes the right eye a primary view that meters its own exposure; stock stereo gives the secondary view the primary's. Black Myth: Wukong shrine, standing still: right eye 5.5% darker. | `[NativeStereoFix] Right eye uses the left eye's exposure view state` in log.txt; binocular luma right-left within 1% on `Wukong-shrine-still`. |
| D3D12: Clamp regions tagged for the XeSS-FG swapchain proxy to the swapchain | An inline hook on `libxess_fg.dll`'s `xefgSwapChainD3D12TagFrameResource` clamps each tagged region (base + size) to the swapchain size, read on every non-filtered Present (module looked up every 120 Presents). | With UEVR the engine's view is the eye, which can be larger than the swapchain XeFG sized its buffers for; Hogwarts Legacy at 125 % / 150 % tagged 2400x2700 regions on a 3840x2160 swapchain and the D3D12 runtime removed the device (INVALID_CALL). | Hogwarts at 125 % / 150 % with XeFG on: no device removal; log `[XeFG] Hooked xefgSwapChainD3D12TagFrameResource (ok)` and `[XeFG] Clamped tagged resource type ...`. |
| VR: Hold Lumen's view origin between refreshes under the Native Stereo Fix | `VR_NativeStereoFixLumenOriginRefreshFrames` (default 30, 0 = off): `r.LumenScene.UpdateViewOrigin` is held at 0 around both eye submissions and let through for both every Nth frame (render-thread safe, so it applies to those renders only). Games without the cvar (UE4) are untouched. | Each Lumen view-origin move dims that render's Lumen lighting for a while; the second submission of a frame moved it again, so the second-rendered eye was 8-16% darker (Black Myth: Wukong chapter-1 forest; rendering the right eye first moved it to the left eye). | `Holding r.LumenScene.UpdateViewOrigin at 0` in log.txt; Wukong chapter-1 binocular luma -10.2% -> -0.2%; TEKKEN fps unchanged. |

## Updating to a new joeyhodge release

Only release tags on joeyhodge's `ue57performance` line are candidates. Tags of the MONO / DIBR line and the
`eBaseballfocused` branch are not: that line shares no history with this one since 2026-10 and a merge conflicts
everywhere. Check before merging:

```
git fetch --no-recurse-submodules joeyhodge "+refs/heads/ue57performance:refs/remotes/joeyhodge/ue57performance"
git fetch --no-recurse-submodules joeyhodge "+refs/tags/<release>:refs/tags/joeyhodge/<release>"
git merge-base --is-ancestor joeyhodge/<release> joeyhodge/ue57performance && echo on the release line
git switch mopic/joeyhodge-base
git merge joeyhodge/<release>          # merge, don't rebase: keeps history and later pushes fast-forward
git submodule sync --recursive
git submodule update --init --recursive
```

- Use `--no-recurse-submodules` on fetches from `joeyhodge`: joeyhodge's UEVR points its UESDK submodule at
  [joeyhodge/UESDK](https://github.com/joeyhodge/UESDK), and a recursive fetch against another remote fails.
- The `joeyhodge` remote has `tagOpt = --no-tags`, so joeyhodge's ~150 release tags don't flood the tag
  namespace. Fetch the release you need into `refs/tags/joeyhodge/` as above.
- If joeyhodge reworks one of the patched areas, re-apply the Mopic change by hand and keep the same check.
- The Mopic UESDK patch (row marked "submodule") lives on branch `mopic/uesdk-ue57` of
  [mopiclabs/UESDK](https://github.com/mopiclabs/UESDK), a fork of joeyhodge/UESDK. This branch's `.gitmodules` points
  the submodule there; inside the submodule `origin` is the fork and `joeyhodge` the upstream. Push
  `mopic/uesdk-ue57` to the fork before pushing this branch, or clones can't check the submodule out.
  `mopic/uesdk` (the 092826MONO-era UESDK patches) stays as it is: old superproject commits point at it.
- When a joeyhodge release moves UESDK: in the submodule `git fetch joeyhodge`, then `git switch mopic/uesdk-ue57`
  and `git merge <the release's UESDK commit>` (no rebase, no force-push, so UESDK commits that older superproject
  commits point at never disappear); push it; in the superproject keep the fork URL in `.gitmodules` and resolve the
  submodule conflict to the merge commit (`git add dependencies/submodules/UESDK`).

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

`tools\mopic-test\` has the test harness (`run-test.ps1`), the game pilot (`gamepilot.py`), recorded recipes and
the hang-dump scripts; its README has the details and setup. The harness starts `UEVRInjector.exe
--attach=<game>`, launches the game through Steam (injection at launch, like Mopic Hub's auto-inject) and classifies
each run. Keep Mopic Hub (eye tracking) and monado-service running during tests.

```
powershell -ExecutionPolicy Bypass -File tools\mopic-test\run-test.ps1 -Game Tekken8Demo -Recipe Tekken8Demo -Runs 3 -Dll build-jh\bin\uevr\UEVRBackend.dll -Label <label>
... -Game Tekken8Demo -Recipe Tekken8Demo -NoInject -Runs 3 -Label vanilla     # the same without UEVR
... -Game <Game> -WaitForExit -Seconds 1800 -Label discover                     # someone (Claude Code) drives and quits it
... -Game <Game> -GracefulExit -Label close                                      # no recipe: close the window at the end
```

A recipe goes through real gameplay and quits through the game's own menu (TEKKEN 8: title > PvC match > main
menu > Options > Quit), checking every screen on the way. Verdicts: PASS, CRASH, FREEZE (the game window stopped
answering for 30 s while running; `freeze.dmp` has the stacks), EXIT_CRASH (crash while quitting),
EXIT_HANG (the game started exiting but its process was still there 30 s later, or it never got to ExitProcess:
still running a minute after the menu quit or 45 s after WM_CLOSE; `exit-hang.dmp` has the stacks),
MENU_FAIL (the pilot couldn't follow the recipe, UEVR was fine), NO_VR (runtime not ready, not a valid test).

Other options: `-Set "Key=Value;..."` (config overrides for one run), `-UserScript "cmd;..."` (console commands),
`-InjectDelay <s>` (late-injection control), `-Seconds <s>`, `-Game Custom -ProcessName <exe> -SteamInstallDir <dir>`.

Results on 2026-10-01 (`run-matrix.ps1`, a build of 439006ab = the code of e968b141, early injection, every run through
gameplay and the game's own quit path, 3 runs each):

| Title | UE | Route | Result |
| --- | --- | --- | --- |
| TEKKEN 8 Demo | 5.2.1 | PvC match, Options > Quit | 3/3 PASS; Native Stereo Fix active, FSR1 upscaler separated |
| Clair Obscur: Expedition 33 | 5.4.4 | Continue, walk, pause > quit | 3/3 PASS (after a recipe fix; 3 MENU_FAIL before it, pilot side) |
| Sonic Racing CrossWorlds Demo | 5.4.3 | Grand Prix race, pause > main menu > quit | 5/6 PASS (the MENU_FAIL was input timing in the recipe, fixed, then 3/3) |
| Hozy | 5.6.1 | a room, pause > quit | 3/3 PASS; the game's UI doesn't show on the Mopic display (the recipe reads the window) |
| Stray | 4.27 | slot 1, walk, pause > main menu > quit | 3/3 PASS |
| Hogwarts Legacy | 4.27 | a save, walk, field guide > quit | 3/3 PASS; exits ~30 s after the quit, same as without UEVR |
| Black Myth: Wukong | 5.0 | Continue, shrine, settings > quit | 3/3 PASS; Native Stereo Fix active since the renderer-entry fix (re-run below) |
| Assetto Corsa Competizione | 4.26 | practice session in the car, quit | 3/3 PASS for stability; nothing is shown on the Mopic display (see Known gaps) |
| Dead as Disco Demo | 5.7 (custom) | Free Play song, Esc > Stage Select > Exit Game | 3/3 PASS with the worker fixes (21 s late injection); Native Stereo Fix active since its family-copy recognizer (re-run below) |

After "VR: Resolve the renderer entry from the view-extension callback's caller": Wukong 3/3 PASS with
`[NativeStereoFix] state=active` from the title screen to the quit (before it, the watchdog fell back to plain native
stereo after 20 s), TEKKEN 8 1/1 PASS with the same wrapper-resolved entry and the fix active as before.

After the UESDK worker and texture-offset fixes (2026-10-01, one run per recipe, all nine): 9/9 PASS. Before them,
Dead as Disco froze at startup in 1 of 3 runs with 21 s late injection (FREEZE, the game-thread/render-thread worker
deadlock) and the shipped praydog-line build crashed it on UE 5.7 (`Ran out of memory allocating 2153813446656
bytes` from its UE4-style texture-create call).

After "VR: Recognize Dead as Disco's UE5.7 FSceneViewFamily copy" and "D3D12: Size the Native Stereo Fix copies by
the eye" (2026-10-01, a build of d2e8cf66): Dead as Disco 4/4 PASS with `[NativeStereoFix] state=active` and no
watchdog fallback (one early injection, three at 24 s). With monado `MOPIC_MODE=sbs`, the scene area of the two
halves measured 58.6/58.2, 65.3/64.5 and 91.0/89.9 (left/right, 0-255) at three points of a song, against 65-76
vs 0.2-0.4 before. Full matrix, one run each: 8/9 PASS, every exit code 0, the fix active on
TEKKEN 8, Stray, Hogwarts Legacy, Wukong and Dead as Disco. Sonic stopped at its main-menu checkpoint (score 0.75)
in the matrix, in a re-run, and the same way with a morning build from before these changes. That monado-service
was restarted at 21:51 with nobody at the monitor and never acquired a viewer (`eye_acquired=0`, read from the live
process): the hub only sent its "no face" sample (both eyes converged, IPD 0, anim 0), which the driver's first-
acquisition gate drops (IPD > 20 mm, `mopic_display.c:1375`), so it kept the default rig (IPD 63 mm, anim 1) and
every capture of the Mopic display showed two views. The other Mopic-display recipes still matched because their UI
sits at `UI_Distance` 2.0 = the zero-parallax plane; Sonic's UI is at 1.673 m, so its menu doubles by about 16 px
(at 1280 wide) and the references recorded in the merged state score 0.75-0.83. That UI_Distance (with UI_Size 0.723
and `VR_NativeStereoFix=false`) was a leftover config from 2025-09-25. It was deleted on 2026-10-02, so Sonic runs on
UEVR's defaults (UI 2.0 m / 2.0, Native Stereo Fix on): a 90 s smoke run passed with the fix active and no watchdog
fallback. The Sonic recipe's references show the old small UI and have to be re-recorded.

No run crashed or hung while quitting, and every exit code was 0. Brightness of both eyes and realtime FOV were
checked by eye on the Mopic display (2026-09-30). Per-eye brightness can be measured with monado-service restarted
under `MOPIC_MODE=sbs` (see the Dead as Disco re-run), but most recipes read the woven Mopic display and fail in that
mode, so there is no automatic per-eye check yet.

Before the exit fix the harness couldn't see exit hangs (.NET's `HasExited` turns true as soon as the exit code is
set, and the harness killed the leftover process afterwards), so the earlier 2026-09-30 graceful-exit results
could have hidden them. The table above replaces them.

Results on 2026-10-02/03 (builds of this branch with the rows above; `-EngineDir ...\engines\mopic-uevr\1.0.5.1`):

- Save ladders (`run-ladder.ps1`, community + own saves): Black Myth: Wukong 30/30 PASS (chapters 1-6, both
  endings, post-game; the fix active in every rung), Stray tier 1 9/10 PASS (the ending rung's walk to the exit is
  not reliable yet). Wukong's quit logs `crashhandler64.dll` 0xc0000409 in Application Error with and without UEVR
  (the game's own).
- Hogwarts one-eye "tick": measured per eye with monado `MOPIC_MODE=sbs` (`analysis\eyesampler.py`). Cause: Intel
  XeFG 2x (set by the NVIDIA app's Recommended settings) presenting off the engine's frame boundary while the Native
  Stereo Fix renders the eyes separately; 93-100% of the splits are the right eye exactly one engine frame late.
  XeFG off in the game's menu: 0 splits; with the frame-generation rows above: 0 one-refresh splits, same ~20 fps.
- Regression matrix after them (one run each): TEKKEN 8, Wukong, E33, Dead as Disco, Stray, Sonic 6/6 PASS, the fix
  active in all six.

Results on 2026-10-14/15 (the ue57performance rebuild: this branch on `101226HELLRAISERp1`, a build of 985eab55,
against the 092826MONO line's last build 14f51980; `-EngineDir ...\1.0.5.1`, early injection, Steam offline except
Hogwarts, monado `build-release` with the Mopic app pacer; one build after the other per title, 3 runs each):

- Unit tests: `ctest` 32/32 (the CI target list plus `perf-log-tests`).
- Every run logs `Tag: joeyhodge/101226HELLRAISERp1`. Native Stereo Fix active on every title where it was before
  (TEKKEN 8, E33, Sonic, Stray, Hogwarts, Wukong, Dead as Disco; not Hozy and ACC, the same on both builds), no
  `Not active after 20 s`, no `Refusing to publish a scene-capture resource`, no `Candidate eye pair rejected`, from
  the first frame on (upstream's scene-capture component creation change, `43d013d8`, showed nothing).
- Verdicts: ue57 48 PASS, 13 MENU_FAIL, no CRASH / FREEZE / EXIT_*; the old line 44 PASS, 16 MENU_FAIL, 1 EXIT_HANG
  (TEKKEN 8, the known gap below), 2 NO_VR. The MENU_FAILs are recipe-side on both builds (Sonic's pause-menu cursor,
  Stray's pause menu not opening after play, Hogwarts' new EULA screen while Steam is offline) and measured their
  gameplay first. Dead as Disco with `-InjectDelay 21`: 3/3 PASS, no FREEZE, on upstream's ThreadWorker alone.
- VR fps vs the old line: TEKKEN 8 +4.7 %, E33 -2.5 %, Sonic +0.1 %, Hozy -1.1 %, Stray +3.8 %, Hogwarts -0.6 %,
  Wukong +0.8 % (DLSS-G off on both: Presents per engine tick 1.00), Dead as Disco -6.1 % in the batch but -0.4 % in
  three alternating pairs (the batch put the old build's runs at a lower GPU load). Hozy's 1 % low (-11.7 %) drifted
  down run after run across both builds; in alternating pairs it matches.
- SBS (`MOPIC_MODE=sbs`, eye sampler, 2 runs per build): no one-eye lag / only / black on either build, one-eye
  shading median 0.0 % everywhere, luma difference within the old line's range except single frames that are a dark
  loading screen with a desktop FPS overlay in the left eye only (Wukong) or an extreme close-up cutscene (Sonic).
  TEKKEN 8 no longer identifies the exposure field (`exposure view state field not identified`: the two eye views
  sit back to back and hold the same view state, so the scan counted the right view's copies too); its left/right
  luma difference stays 3-5 %, the same as before.

## Known gaps

- TEKKEN 8: an intermittent hang after quitting through the menu, separate from the exit fix above. 2 of 12 full
  recipe runs with UEVR hung (the game never reaches ExitProcess), 0 of 10 without UEVR (`-NoInject`). In the dump
  the GameThread is in a "stop worker and join" (`SetEvent` + `WaitForSingleObject`, exe+0x5e09670) and the worker
  (an unnamed thread, loop at exe+0x5e09270) keeps asking its object whether the work is done (vtable+0x10) and
  sleeping 100 ms instead of exiting. UEVR code isn't on any stack. The harness reports `EXIT_HANG` and writes
  `exit-hang.dmp` with the memory the stacks point at; on the next occurrence
  `workerobj.py runs\<run>\exit-hang.dmp <game.exe> 5e09369` should name the worker's class (vtable/RTTI; see
  "Reading an EXIT_HANG dump" in `tools\mopic-test\README.md`).
- Assetto Corsa Competizione (D3D11): nothing reaches the Mopic display. UEVR's D3D11 component can't get the
  back buffer ("Failed to get back buffer (D3D11)" / "Failed to setup D3D11Component" every frame), while
  UEVR does render the stereo pair into the game's own window in a session. The game runs and quits cleanly.
- Hozy (UE 5.6): the game's UI (menus, HUD) doesn't show on the Mopic display, only the 3D scene; TEKKEN 8
  (5.2), Expedition 33 (5.4) and the others show theirs.
- The tests play briefly on the player's saves; games that autosave (Expedition 33) then start somewhere else
  next time. Saves were backed up before recording (`tools\mopic-test\runs\save-backups\`).
- Generic UE5.7 Native Stereo Fix depends on finding FSceneViewFamily's copy constructor by exact bytes. A UE5.7
  build whose compiler allocates registers differently fails closed; with the watchdog that means plain native
  stereo, where UE5.7 leaves the right eye's 3D scene black (Dead as Disco before its profile: with monado
  `MOPIC_MODE=sbs` the scene area measured 65-76 left vs 0.2-0.4 right, 0-255). Such a title needs its own
  recognizer like Halloween's and Dead as Disco's.
- D3D12 `m_backbuffer_size` stays at the first target `setup()` saw when the game only later hands UEVR its
  VR-sized target (Dead as Disco: the 1280x800 Slate texture). The Native Stereo Fix copies no longer use it, but its
  other users (AFR copy boxes, 2D-screen rects, mono) would be wrong on such a title; the root fix is to run `setup()`
  again when the borrowed target becomes 2x the eye size.
- With monado `MOPIC_MODE=sbs`, each eye appears about 3.7% magnified around its projection centre, before and after
  the Dead as Disco fixes. Likely the FOV submitted at `xrEndFrame` differs from the one the display composites
  with (eye-tracked FOV changes every frame); not yet checked.
- The Mopic display has three capture states, and recipes with `"source": "mopic"` screenshot the woven output:
  merged (monado has tracked a viewer since it started, and the viewer left: one view), tracked (two views following
  the viewer), and never acquired (monado started with nobody at the monitor: two views from a fixed 63 mm rig, until
  a face is seen once). The references were recorded merged. With UEVR's default UI_Distance (2.0 m, the
  zero-parallax plane) the UI itself is single in every state, but a checkpoint whose region lets the 3D scene show
  through still differs; compare a Mopic-display MENU_FAIL against a run in the same state before calling it a
  regression.
- The cached right-eye fallback is D3D12/OpenXR double-wide only (not D3D11, texture-array or OpenVR).
- joeyhodge compares D3D devices by raw pointer in 20+ other places (UI composition, alpha passes, and on the ue57
  line also title-gated Native Fix paths: Hellraiser's capture check `desc.device == device`, Stalker 2's exact pair
  `device == m_openxr.binding.device`, Galactic Racer's `device_identity`). Only the scene-capture paths use
  `is_same_d3d12_device`; the others may fail the same way on Mopic setups when those features or titles are used.
  Left as upstream wrote them (title-gated, not on Mopic's titles).
- Other version gates (`is_ue_5_4_runtime`, `is_ue_5_5_runtime`, ...) use the same exe string / file-version
  detection. Only the UE5.0–5.3 layout choice has the layout-based fallback.
- `VR_NativeStereoFixPreserveSecondaryPass` (default on, UE5.5+ only) ties the right eye's exposure to the
  left eye's view state. Watch for an exposure difference between the eyes on UE5.5+ titles.
- A saved `config.txt` keeps its stored `VR_NativeStereoFix` value, so the new default only applies to titles
  without one (only `VR_UseFMallocSceneViewExtensions` is migrated).
- Hogwarts, one walking spot (dark interior near the 2026-10-03 save): 10-15 one-eye changes per run where the left
  eye changes and the right never shows a matching change for 100-480 ms, with frame generation off by the menu as
  well as by the cvar rows. Not frame generation; possibly per-eye engine decisions under the Native Stereo Fix
  (LOD / occlusion / exposure per view). Not checked by eye yet.
- Mopic Hub picks the newest engine folder: the 1.0.5.2 package (commit 8cb40134) fails Dead as Disco's Native Stereo
  Fix ("found 0 from 1 source-shaped signature match(es)"), which this branch passes.
- Hogwarts Legacy keeps a second settings copy in `SaveGames\<account>\SavedUserOptions.sav` (Steam Cloud) that
  overrides `GameUserSettings.ini`, so `-GameIni` can't change its graphics; `recipes\Hogwarts-fgoff.json` sets frame
  generation through the game's menu instead.
- monado-service (upstream `vk_helpers.c` `vk_create_image_from_native`) never closes the Win32 NT handles of D3D12
  client swapchain images after importing them, so every session leaves its swapchain memory charged to the system
  (about 0.5 GB per session; after ~100 test sessions the commit charge hit its limit and Hogwarts crashed with "Ran
  out of memory"). A local, uncommitted patch closes the handle for non-DXGI imports (monado
  `patches-local\vk_helpers-close-imported-nt-handles.diff`): 0.07 GB per session after it. Until monado ships a fix,
  restart monado-service between long test batches.
- An overlay that draws into the game's own back buffer (here a desktop FPS overlay, Ctrl+Shift+O) reaches the Mopic
  display in the left eye only, since the left eye is the engine's back buffer and the right eye the scene capture.
- Hogwarts Legacy (build 1613387) shows WB Games' 2025-01 EULA on every start while Steam is offline (accepting it
  offline is not remembered); run it with `-SteamOnline`. The harness's `Wukong-shrine-still` recipe still has the
  old 120x180 main-menu region for the re-saved 120x34 image, and after the campaign's autosaves its save no longer
  starts at the shrine.
- monado-service crashed once (2026-10-14 23:19, access violation in `nvoglv64.dll`) at the start of a Dead as Disco
  run; the next runs got NO_VR until it was restarted.
- Intermittent, not reproduced: TEKKEN 8 froze once in ~38 runs right after UEVR's stereo-projection bootstrap
  (game thread dispatching an access violation in non-module executable memory; `freeze.dmp` kept), and Wukong
  crashed once while quitting with a null call on a game thread (Wukong also crashes at quit without UEVR).
