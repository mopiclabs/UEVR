# Mopic UEVR test tools

- `run-test.ps1`: the test harness. Starts UEVRInjector, launches a game through Steam, injects at launch (like
  Mopic Hub's auto-inject), watches it, and writes a verdict (PASS / CRASH / FREEZE / EXIT_CRASH / EXIT_HANG / MENU_FAIL /
  NO_VR / ...) plus log.txt, crash dumps and screenshots to `runs\`.
- `gamepilot.py`: looks at the screen and sends keyboard/mouse input, so a run can go from the title screen into
  real gameplay and quit through the game's own menu (where exit crashes and hangs show up).
- `recipes\<Game>.json` (+ checkpoint images in `recipes\<Game>\`): a recorded route through a game's menus.
- `analysis\`: scripts for the hang dumps the harness writes.

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
| `-Runs <n>`, `-Label <text>`, `-KeepGame`, `-Screenshot` | |

`run-matrix.ps1 -Runs 3 [-Dll <path>] [-Games A,B] [-NoInject]` runs every recipe (or the given games) through the
harness and writes one table to `runs\matrix-<time>-uevr.md` / `-vanilla.md` (+ `.json`).

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

## gamepilot

```
.venv\Scripts\python gamepilot.py <process> status                        # locked? window? foreground?
.venv\Scripts\python gamepilot.py <process> shot [--source mopic|window] [--out PATH] [--max-width 1280]
.venv\Scripts\python gamepilot.py <process> key <name> [--times N] [--hold MS] [--gap MS]    # enter, esc, down, j, alt+f4, ...
.venv\Scripts\python gamepilot.py <process> keys "down down enter"
.venv\Scripts\python gamepilot.py <process> click <x> <y>                  # in the last screenshot's pixels
.venv\Scripts\python gamepilot.py <process> move <x> <y> / type "text" / focus
.venv\Scripts\python gamepilot.py -         checkpoint save <recipe.json> <name> <x> <y> <w> <h> [--from PNG] [--mode highlight] [--threshold 0.8] [--source mopic]
.venv\Scripts\python gamepilot.py <process> checkpoint test <recipe.json> <name|all> [--from PNG] [--source window]
.venv\Scripts\python gamepilot.py -         matrix <recipe.json> <screenshots/globs...>
.venv\Scripts\python gamepilot.py <process> run <recipe.json> [--steps 9-12] [--source window] [--out DIR] [--status FILE]
.venv\Scripts\python gamepilot.py <process> dump <out.dmp>
```

- `--source mopic` captures the Mopic display (what the viewer sees). Use it with UEVR: the joeyhodge-based build
  leaves the desktop game window black for most games while it renders. Clicks are mapped back to the game window.
- While Windows shows the lock screen, input and capture don't reach the desktop: the screen and input commands
  exit with code 3 and `{"error":"locked"}`, and `run` stops with `error_kind` "locked". Ask the user to unlock;
  auto-lock stays on.
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
- `window_fit` / `height`: lets a recipe recorded on the Mopic display also drive the game without UEVR
  (`-NoInject`, or `run --source window`): the desktop window's image is scaled into that rectangle of a canvas of
  `max_width` x `height`, where the game image sits in the Mopic capture, so the same checkpoints apply.
- `images`: take the checkpoint images from another recipe's folder (`Tekken8Demo-quit.json` uses `Tekken8Demo`,
  a quick boot-and-quit check without a match).
- `window_timeout`: seconds to wait for the game window before the first step (default 180).
- The harness refuses a recipe without an `expect_exit` step.

Steps run in order (one per line in the file). `"if": "<checkpoint>"` on any step runs it only if that screen is
showing right now.

| Step | Does |
| --- | --- |
| `{"wait_for": "x", "timeout": 30}` | wait until checkpoint x shows. A list waits for any of them. `"press": "enter", "every": 5` presses a key meanwhile (skipping movies), `"click": [x, y]` clicks instead; only after a failed check. |
| `{"seek": "x", "press": "down", "max": 8}` | press until x shows (menu cursors don't always start on the same item; menus usually wrap around, so `max` should cover one lap) |
| `{"key": "enter", "times": 1}` / `{"keys": "up enter"}` / `{"click": [x, y]}` / `{"move": [x, y]}` / `{"wait": 2}` | input (`move` hovers: menus that highlight under the mouse) / pause |
| `{"play_until": "x", "while": "hud", "keys": "u i j k a d", "timeout": 600}` | random gameplay keys until x shows; with `while` only while the HUD checkpoint is visible, so they don't act as menu input on result screens |
| `{"play": 60, "keys": "...", "every": 0.6, "hold": 500}` | random gameplay keys for a fixed time (`lmb` / `rmb` click where the mouse is) |
| `{"phase": "exit"}` | from here the game quitting is expected (the harness counts the exit as a menu quit) |
| `{"expect_exit": 60}` | wait for the process to end; still running afterwards = `EXIT_HANG` |
| `{"shot": "name"}` / `{"note": "..."}` | save a screenshot / comment |

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
- Black Myth: Wukong: mouse-driven menus that highlight under the mouse (the recipe hovers first). The first start
  after an update compiles shaders for about a minute. Title > 게임 계속하기 (never 새 게임) > shrine > walk (never E
  there) > Esc > 설정 > 게임 종료 > 바탕 화면으로 > 확인. A config saved by this line can have
  `FrameworkConfig_RememberMenuState=false`, which opens the UEVR menu over the title screen and swallows the keys;
  the recipe's `config` keeps it closed.
- Dead as Disco Demo (UE 5.7, custom engine branch): mouse-driven menus, read from the desktop window (the Mopic
  display doubles the UI text). The very first start asks about Streamer Safe Mode (click OFF); a fresh install shows
  NEW GAME instead of CONTINUE. Route: CONTINUE > stage hub > Enter > FREE PLAY > first song > click again to play >
  attack with the mouse > Esc ends the song (RESULT) > Stage Select > Esc > EXIT GAME > EXIT GAME on the wishlist
  screen (no confirmation). Never GET THE GAME!, GIVE FEEDBACK or SIGN UP (store/browser). A played song posts its
  score to the Steam account's leaderboard. `play` steps can click with `lmb` / `rmb`. With Native Stereo Fix
  active the desktop window shows the 3D scene behind the menus (without it, black), so the checkpoints sit on opaque
  UI; the gameplay HUD pulses with the beat and loses hearts, so the RESULT screen after Esc is the gameplay proof.
  To look at each eye, restart monado-service with `MOPIC_MODE=sbs` (the display then shows left | right).