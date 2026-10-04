# Test harness for Mopic UEVR (see README.md next to this script).
#
# One run = start UEVRInjector.exe --attach=<game process> from the mopic-uevr engine folder, launch the game
# through Steam, let the injector inject as soon as the game window appears (same as auto-inject), watch the
# game for -Seconds (or drive it through gameplay and its menu quit with -Recipe), then collect log.txt /
# crash.dmp / config and write a verdict.
#
#   # a build, TEKKEN 8 Demo, 2 minutes, 3 runs
#   powershell -ExecutionPolicy Bypass -File tools\mopic-test\run-test.ps1 -Game Tekken8Demo -Runs 3 -Dll build-jh\bin\uevr\UEVRBackend.dll -Label mybuild
#
#   # into gameplay and back out through the game's menus (gamepilot.py + recipes\Tekken8Demo.json; a recipe's
#   # "config" block is applied to UEVR's config.txt for the run, like -Set)
#   ... -Game Tekken8Demo -Recipe Tekken8Demo -Runs 3 -Label menu
#
#   # the same without UEVR, as a baseline (the pilot reads the desktop window)
#   ... -Game Tekken8Demo -Recipe Tekken8Demo -NoInject -Runs 3 -Label vanilla
#
#   # someone (Claude Code with gamepilot, or a person) drives the game and quits it through its menu
#   ... -Game Hozy -WaitForExit -Seconds 1800 -Label discover
#
#   # no recipe: close the window like a normal quit at the end of the window, catch exit crashes and hangs
#   ... -Game Stray -GracefulExit -Label close
#
#   # A/B without rebuilding: config overrides / console commands for this run only
#   ... -Set "VR_NativeStereoFix=false" -UserScript "r.ScreenPercentage 100" -Label nsf-off
#
#   # the game's own settings for this run only (GameUserSettings.ini in its config folder; put back byte for byte
#   # afterwards, also after a crash or a killed harness: runs\game-ini-pending\ until then)
#   ... -Game Hogwarts -GameIni 'GameUserSettings.ini|/Script/Phoenix.PhoenixGameSettings|FrameGeneration=(Mode=Off,NumFramesInterpolated=0,LocStr="Off")'
#
#   # each eye on the Mopic display during the recipe's measured segments (monado-service started with MOPIC_MODE=sbs;
#   # the pilot then reads the left eye: -MopicSbs)
#   ... -Game Hogwarts -Recipe Hogwarts-eyes -EyeSampler
#
#   # start from a given save: copied over the recipe's "save_slot" before each run (the game's copy after the
#   # run lands in <run>\save-after\); -RecipeVars fills the recipe's "vars" (${name} in its steps, gamepilot run --var)
#   ... -Game Wukong -Recipe Wukong-save -SaveFile saves\wukong\x.sav -RecipeVars "play_s=180" -Label ladder
#   # the same without a recipe (discovery): -SaveSlot names the file to replace
#   ... -Game Wukong -WaitForExit -Seconds 2400 -SaveFile saves\wukong\x.sav -SaveSlot "{gamedir}\b1\Saved\SaveGames\{sid64}\ArchiveSaveFile.9.sav" -Label discover
#
# Results: runs\<time>-<game>-<label>-r<n>\ next to this script (summary.txt, result.json, log.txt, crash.dmp,
# config.txt, pilot\*.png, exit-hang.dmp) and a one-line verdict per run on the console:
#   PASS           game alive for the whole observation window (or quit cleanly through its menu), no dump
#   CRASH          game exited or UEVR wrote crash.dmp
#   EXIT_CRASH     crash while quitting (-GracefulExit, or the menu quit of -Recipe / -WaitForExit)
#   EXIT_HANG      the game started exiting but its process was still there 30 s later (stuck in shutdown,
#                  exit-hang.dmp has the stacks), or it never got to ExitProcess: still running a minute after
#                  the menu quit (-Recipe) or 45 s after WM_CLOSE (-GracefulExit; also a game that ignores it)
#   MENU_FAIL      -Recipe: UEVR survived, but the pilot couldn't follow the recipe (a screen didn't show up,
#                  PC locked, ...); pilot-status.json and pilot\*.png show where it stopped
#   NO_VR          game survived but OpenXR never reached READY (runtime unavailable), not a valid test
#   NO_INJECTION   UEVR never started inside the game (injector not attaching / wrong process name)
#   LAUNCH_FAILED  game process never appeared
# Warnings flag a LocalPlayer bootstrap that never completed (right eye would have no view state).
#
# Frame rate (unless -NoPerf; README "Frame rate"): nvidia-smi (1 Hz) and the PresentMon console app (when installed
# and allowed) sample while the game runs; UEVR's perf.csv / perf-frames.csv, monado-service's app frame stats for the
# run and the game's GameUserSettings.ini are copied into the run folder, and perfreport.py cuts them to the measured
# gameplay into perf.json: a "perf" block in result.json and a "perf[...]" line in summary.txt. Never a verdict.
# -EyeSampler adds eyes.csv / eyes-report.json: an "eyes" block and an "eyes[...]" line, never a verdict either.
#
# Exit code: 0 if every run passed, 1 otherwise.
#
# Prerequisites: Steam running and logged in, UEVRInjector's saved settings already set to OpenXR (and
# "Nullify VR plugins" as you normally use it), Mopic Hub running (it provides eye tracking; keep it open) with
# its auto-inject off so only this harness injects, monado-service running, the PC unlocked and nothing else using
# its focus during the run. Any running UEVRInjector.exe and the game itself are closed at the start and end of
# every run. -Dll leaves the given DLL deployed in the engine folder.
param(
    [ValidateSet("Tekken8Demo", "Expedition33", "Wukong", "Hogwarts", "Stray", "Hozy", "ACC", "SonicDemo", "DeadAsDisco", "Custom")]
    [string]$Game = "Tekken8Demo",
    [string]$ProcessName = "",        # Custom: process name without .exe
    [string]$SteamInstallDir = "",    # Custom: steamapps\common\<this>, used to find the app id
    [string]$LaunchTarget = "",       # overrides Steam: exe path or URL (e.g. steam://rungameid/123)
    [string]$EngineDir = "",          # default: newest folder under %APPDATA%\MOPIC\mopichub\engines\mopic-uevr
    [string]$Dll = "",                # deploy this UEVRBackend.dll before running (original backed up once as .orig)
    [string]$Set = "",                # extra config.txt keys for this run only, "Key=Value;Key2=Value2" (restored afterwards)
    [string]$UserScript = "",         # console commands for UEVR's user_script.txt for this run only, "r.Foo 0;r.Bar 1" (restored afterwards)
    [int]$Seconds = 120,              # observation window after injection
    [int]$HangSeconds = 30,           # FREEZE when the game window doesn't answer messages for this long
    [int]$LaunchTimeout = 240,        # max seconds from launch until UEVR is running in the game
    [int]$Runs = 1,
    [int]$InjectDelay = 0,            # >0: launch the game first and only start the injector this many seconds after the game process appears (late-injection control)
    [string]$Label = "test",
    [switch]$Screenshot,              # save a desktop screenshot at the end of the window
    [switch]$KeepGame,                # don't close the game after the last run
    [switch]$GracefulExit,            # at the end of the window close the game's main window (like a normal quit) and watch for crashes during shutdown
    [switch]$WaitForExit,             # the game is expected to quit by itself within -Seconds (someone quits it through its menu): an exit without a dump or UE crash report is a PASS
    [switch]$NoInject,                # baseline: the game without UEVR (with -Recipe the pilot reads the desktop window)
    [string]$Recipe = "",             # drive the game with gamepilot.py: recipe name (recipes\<name>.json) or path. Enters gameplay, plays, quits through the menu; implies -WaitForExit
    [string]$SaveFile = "",           # copy this save over the recipe's "save_slot" before each run (path, or relative to this script's folder); the file is overwritten, nothing is deleted
    [string]$SaveSlot = "",           # with -SaveFile: the file it replaces, with save_slot's placeholders ("{gamedir}\b1\Saved\SaveGames\{sid64}\ArchiveSaveFile.9.sav"); instead of the recipe's save_slot, also without a recipe (discovery)
    [string]$SaveAs = "",             # with -SaveFile: the target file name instead of the recipe's save_slot "file"
    [string]$RecipeVars = "",         # "k=v;k2=v2": values for the recipe's "vars" (${k} in its steps; passed to gamepilot run as --var k=v)
    [switch]$NoPerf,                  # no frame-rate measurement: no nvidia-smi / PresentMon sampling, no VR_PerfLog override, no perf.json
    [ValidateSet("auto", "off")]
    [string]$PresentMon = "auto",     # auto: also capture the game's Presents with the PresentMon console app when it is installed and allowed (admin or Performance Log Users)
    [string]$GameIni = "",            # the game's own settings for this run only, "file|Section|Key=Value;Key2=Value2" (file: GameUserSettings.ini, Engine.ini... in its config folder, or a full path); put back byte for byte afterwards
    [switch]$MopicSbs,                # monado-service runs with MOPIC_MODE=sbs: the pilot reads a "mopic" recipe's screens from the left eye (gamepilot --source mopic-sbs)
    [switch]$EyeSampler,              # sample each eye on the Mopic display (analysis\eyesampler.py) during the recipe's measured segments (the whole observation without a recipe); needs MOPIC_MODE=sbs, implies -MopicSbs
    [string]$EyeSamplerArgs = "",     # more eyesampler.py sample arguments, e.g. "--hz 90 --y 0.4"
    [string]$EyeLumaLog = "",         # optional log with lines like "eye_luma left=0.183 right=0.179"
    [string]$EyeLumaPattern = 'eye_luma\s+left=([0-9.]+)\s+right=([0-9.]+)'
)

$ErrorActionPreference = "Stop"

# SettingsDir: the game's folder under %LOCALAPPDATA% (<SettingsDir>\Saved\Config\Windows[NoEditor]\GameUserSettings.ini)
$Presets = @{
    Tekken8Demo  = @{ Process = "Polaris-Win64-Shipping";  InstallDir = "TEKKEN 8 Demo"; SettingsDir = "TEKKEN 8 Demo" }
    Expedition33 = @{ Process = "SandFall-Win64-Shipping"; InstallDir = "Expedition 33"; SettingsDir = "Sandfall" }
    Wukong       = @{ Process = "b1-Win64-Shipping";       InstallDir = "BlackMythWukong"; SettingsDir = "b1" }
    DeadAsDisco  = @{ Process = "PagodaSteamDemo-Win64-Shipping"; InstallDir = "Dead as Disco Demo"; SettingsDir = "Pagoda" }
    Hogwarts     = @{ Process = "HogwartsLegacy";          InstallDir = "Hogwarts Legacy"; SettingsDir = "Hogwarts Legacy" }
    Stray        = @{ Process = "Stray-Win64-Shipping";    InstallDir = "Stray"; SettingsDir = "Hk_project" }
    Hozy         = @{ Process = "CozyGame-Win64-Shipping"; InstallDir = "Hozy"; SettingsDir = "CozyGame" }
    ACC          = @{ Process = "AC2-Win64-Shipping";      InstallDir = "Assetto Corsa Competizione"; SettingsDir = "AC2" }
    SonicDemo    = @{ Process = "SonicRacingCrossWorldsSteam"; InstallDir = "SonicRacingCrossWorldsDemo"; SettingsDir = "UNION" }
}

$SettingsDir = ""
if ($Game -ne "Custom") {
    if ($ProcessName -eq "") { $ProcessName = $Presets[$Game].Process }
    if ($SteamInstallDir -eq "") { $SteamInstallDir = $Presets[$Game].InstallDir }
    $SettingsDir = $Presets[$Game].SettingsDir
}
if ($ProcessName -eq "") { throw "-ProcessName is required for -Game Custom" }

# Milestones searched in log.txt (first occurrence is timed, some are counted).
$Milestones = [ordered]@{
    injected          = "UnrealVR entry"
    process_age       = "Process age at injection: (\d+) ms"
    grace_wait        = "Injected at game launch, waiting"
    user_script       = "execute_console_script\] Loading"
    framework_init    = "Framework initialized"
    first_frame       = "Running first frame D3D initialization of mods"
    xr_instance_fail  = "Could not create openxr instance"
    xr_ready          = "XR_SESSION_STATE_READY"
    xr_focused        = "XR_SESSION_STATE_FOCUSED"
    stereo_projection = "calculate stereo projection matrix called"
    nsf_warmup        = "\[NativeStereoFix\] Warming up"
    nsf_second_view   = "\[NativeStereoFix\] Rendering the second view"
    nsf_brvf_real     = "Called BeginRenderViewFamilyReal for the first time"
    nsf_iface_found   = "\[NativeStereoFix\] FSceneViewFamily interfaces:"
    nsf_iface_missing = "\[NativeStereoFix\] (Could not locate|FSceneViewFamily has no usable)"
    nsf_separate      = "\[NativeStereoFix\] Gave the second view its own upscaler"
    nsf_not_recreated = "\[NativeStereoFix\] Upscaler interface .* was not recreated"
    nsf_shared        = "\[NativeStereoFix\] Upscaler interface .* both views share it"
    nsf_active        = "\[NativeStereoFix\] state=active"
    bootstrap_defer   = "Deferring LocalPlayer bootstrap"
    bootstrap_call    = "Calling PostInitProperties on local player!"
    bootstrap_done    = "PostInitProperties called!"
    bootstrap_noop    = "no-op thunk, skipping the bootstrap call"
    bootstrap_fail    = "Failed to find PostInitProperties"
    null_view_state   = "Scene state passed to FSceneView constructor is null"
    midhook_inserted  = "Inserting midhook after CalculateStereoProjectionMatrix"
    veh_exception     = "Encountered exception [0-9a-f]+ at"
    int3_skipped      = "Skipping int3 breakpoint"
    uevr_exception    = "Exception occurred: "
    d3d12_setup_fail  = "\[D3D12 VR\] Could not set up"
}

# ---------------------------------------------------------------------------------------------------------------

function Read-Shared([string]$path) {
    if (-not (Test-Path $path)) { return "" }
    try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]"ReadWrite, Delete")
        try {
            $reader = New-Object System.IO.StreamReader($fs)
            return $reader.ReadToEnd()
        } finally {
            $fs.Dispose()
        }
    } catch {
        return ""
    }
}

function Get-MTime([string]$path) {
    if (Test-Path $path) { return (Get-Item $path).LastWriteTime }
    return [DateTime]::MinValue
}

function Get-LogTime([string]$line) {
    if ($line -match '^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]') {
        return [DateTime]::ParseExact($Matches[1], "yyyy-MM-dd HH:mm:ss.fff", $null)
    }
    return $null
}

function Get-SteamLibraries {
    $steamPath = $null
    try { $steamPath = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath } catch { }
    if (-not $steamPath) { $steamPath = "${env:ProgramFiles(x86)}\Steam" }
    $steamPath = $steamPath -replace "/", "\"

    $libraries = @($steamPath)
    $vdf = Join-Path $steamPath "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $lib = $m.Groups[1].Value -replace "\\\\", "\"
            # the registry's SteamPath is all lower case; the same folder from the vdf keeps its real spelling
            $same = -1
            for ($i = 0; $i -lt $libraries.Count; $i++) { if ($libraries[$i].TrimEnd("\") -ieq $lib.TrimEnd("\")) { $same = $i; break } }
            if ($same -ge 0) { $libraries[$same] = $lib } else { $libraries += $lib }
        }
    }
    return @($libraries)
}

function Find-SteamAppId([string]$installDir) {
    foreach ($lib in Get-SteamLibraries) {
        $apps = Join-Path $lib "steamapps"
        if (-not (Test-Path $apps)) { continue }
        foreach ($acf in Get-ChildItem $apps -Filter "appmanifest_*.acf") {
            $text = Get-Content $acf.FullName -Raw
            if ($text -match '"installdir"\s+"([^"]+)"' -and $Matches[1] -ieq $installDir) {
                if ($text -match '"appid"\s+"(\d+)"') { return $Matches[1] }
            }
        }
    }
    return $null
}

# steamapps\common\<installDir> in whichever Steam library has it
function Find-SteamGameDir([string]$installDir) {
    if ($installDir -eq "") { return $null }
    foreach ($lib in Get-SteamLibraries) {
        $dir = Join-Path $lib "steamapps\common\$installDir"
        if (Test-Path -LiteralPath $dir -PathType Container) { return $dir }
    }
    return $null
}

# The logged-in Steam user's account id (the low 32 bits of the SteamID64), $null when nobody is logged in
function Get-SteamAccountId {
    $id = $null
    try { $id = (Get-ItemProperty "HKCU:\Software\Valve\Steam\ActiveProcess" -ErrorAction Stop).ActiveUser } catch { }
    if ($null -eq $id) { return $null }
    $n = [int64]$id
    if ($n -lt 0) { $n += 4294967296 }   # a REG_DWORD above 2^31 reads back as a negative Int32
    if ($n -eq 0) { return $null }
    return [uint64]$n
}

# Values for the placeholders in a recipe's save_slot "dir"; $null = not available on this PC / for this game
function Get-SavePlaceholderValues([string]$gameDir) {
    $account = Get-SteamAccountId
    return @{
        sid64        = $(if ($account) { [string]([uint64]76561197960265728 + $account) } else { $null })
        accountid    = $(if ($account) { [string]$account } else { $null })
        gamedir      = $(if ($gameDir) { $gameDir } else { $null })
        localappdata = $env:LOCALAPPDATA
        appdata      = $env:APPDATA
        documents    = [Environment]::GetFolderPath("MyDocuments")
    }
}

function Expand-SavePlaceholders([string]$text, [hashtable]$values, [string]$what = "save_slot") {
    $out = $text
    foreach ($m in [regex]::Matches($text, '\{([A-Za-z0-9_]+)\}')) {
        $name = $m.Groups[1].Value
        if (-not $values.ContainsKey($name)) { throw "${what}: unknown placeholder {$name} in '$text' (known: $((@($values.Keys) | Sort-Object | ForEach-Object { "{$_}" }) -join ' '))" }
        $value = $values[$name]
        if ($null -eq $value -or [string]$value -eq "") {
            $why = $(if ($name -in @("sid64", "accountid")) { "no Steam user is logged in" } else { "the game's install folder was not found" })
            throw "${what}: {$name} in '$text' has no value ($why)"
        }
        $out = $out.Replace($m.Value, [string]$value)
    }
    return $out
}

# Full path of the save file a run installs: the recipe's save_slot {"dir": ..., "file": ...} (or -SaveSlot split
# into those), -SaveAs overriding the file
function Resolve-SaveTarget($slot, [string]$saveAs, [hashtable]$values) {
    if (-not $slot -or -not $slot.dir) { throw "save_slot needs a `"dir`" (-SaveSlot: a full path)" }
    $file = $(if ($saveAs -ne "") { $saveAs } else { [string]$slot.file })
    if ($file -eq "") { throw "save_slot has no `"file`" (or pass -SaveAs <file name>)" }
    if ($file -ne [System.IO.Path]::GetFileName($file) -or $file -in @(".", "..")) { throw "save file name '$file' must be a plain file name, not a path" }
    $dir = Expand-SavePlaceholders ([string]$slot.dir) $values
    if (-not [System.IO.Path]::IsPathRooted($dir)) { throw "save_slot dir '$dir' is not an absolute path" }
    return [System.IO.Path]::GetFullPath((Join-Path $dir $file))
}

# Steam (cloud sync) or a game that is still going away can hold a save file for a moment
function Copy-FileRetry([string]$from, [string]$to, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        try { Copy-Item -LiteralPath $from -Destination $to -Force -ErrorAction Stop; return }
        catch { if ((Get-Date) -gt $deadline) { throw }; Start-Sleep -Seconds 2 }
    }
}

# Puts the run's save in place: the file already there is kept in <runDir>\save-before\, the save is copied over it
# and stamped with the current time (so Steam Cloud takes the local file as the newest). Nothing is ever deleted.
function Install-SaveFile([string]$source, [string]$sourceHash, [string]$target, [string]$runDir) {
    $dir = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { throw "save folder not found: $dir (wrong Steam user, or the game never saved on this PC?)" }
    $before = $null
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $beforeDir = Join-Path $runDir "save-before"
        New-Item -ItemType Directory -Force -Path $beforeDir | Out-Null
        $before = Join-Path $beforeDir (Split-Path -Leaf $target)
        Copy-FileRetry $target $before 30
    }
    if (-not [string]::Equals([System.IO.Path]::GetFullPath($source), $target, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-FileRetry $source $target 30
    }
    $now = Get-Date
    (Get-Item -LiteralPath $target).LastWriteTime = $now
    $installedHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($installedHash -ne $sourceHash) { throw "the installed save $target does not match $source (sha256 $installedHash vs $sourceHash)" }
    return [ordered]@{
        source        = $source
        source_sha256 = $sourceHash
        target        = $target
        size          = (Get-Item -LiteralPath $target).Length
        installed     = $now.ToString("s")
        target_before = $before
        after         = $null
    }
}

# Evidence after a run (also after crashes): the target as the game left it (autosaves, format upgrades) goes to
# <runDir>\save-after\, plus the names of other files in the save folder written during the run.
function Save-SaveEvidence([string]$target, [string]$installedHash, [string]$runDir, [DateTime]$since) {
    $dir = Split-Path -Parent $target
    $others = @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $since -and $_.FullName -ne $target } | ForEach-Object { $_.Name })
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        return [ordered]@{ path = $null; size = $null; sha256 = $null; changed = $true; modified = $null; missing = $true; others_written = $others }
    }
    $afterDir = Join-Path $runDir "save-after"
    New-Item -ItemType Directory -Force -Path $afterDir | Out-Null
    $dest = Join-Path $afterDir (Split-Path -Leaf $target)
    Copy-FileRetry $target $dest 10
    $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    return [ordered]@{
        path           = $dest
        size           = (Get-Item -LiteralPath $dest).Length
        sha256         = $hash
        changed        = ($hash -ne $installedHash)
        modified       = (Get-Item -LiteralPath $target).LastWriteTime.ToString("s")
        missing        = $false
        others_written = $others
    }
}

# One argument of a command line built for Start-Process (which joins -ArgumentList with spaces as it is): quoted,
# with embedded quotes and the backslashes before a quote (or before the closing one) escaped the way the MSVC
# runtime / CommandLineToArgvW read them back
function ConvertTo-ArgvString([string]$s) {
    $s = [regex]::Replace($s, '(\\*)"', { param($m) ($m.Groups[1].Value * 2) + '\"' })
    $s = [regex]::Replace($s, '(\\+)$', '$1$1')
    return '"' + $s + '"'
}

# -RecipeVars "k=v;k2=v2" -> ordered name -> value (names as in the recipe's "vars", case-sensitive)
function ConvertFrom-RecipeVars([string]$text) {
    $vars = New-Object System.Collections.Specialized.OrderedDictionary ([StringComparer]::Ordinal)
    foreach ($pair in ($text -split ";")) {
        if ($pair.Trim() -eq "") { continue }
        $kv = $pair.Split("=", 2)
        $name = $kv[0].Trim()
        if ($kv.Count -ne 2 -or $name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { throw "-RecipeVars expects name=value pairs separated by ';', got '$pair'" }
        $vars[$name] = $kv[1].Trim()
    }
    return $vars
}

# -RecipeVars -> gamepilot arguments (--var "k=v" ...); JSON values with quotes (["hud","loading"]) arrive intact
function ConvertTo-PilotVarArgs([string]$text) {
    $out = @()
    $vars = ConvertFrom-RecipeVars $text
    foreach ($name in $vars.PSBase.Keys) { $out += @("--var", (ConvertTo-ArgvString "$name=$($vars[$name])")) }
    return $out
}

function Set-ConfigValues([string]$path, [hashtable]$values) {
    $lines = @()
    if (Test-Path $path) { $lines = @(Get-Content $path -Encoding UTF8) }
    foreach ($key in $values.Keys) {
        $found = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match ("^" + [regex]::Escape($key) + "=")) {
                $lines[$i] = "$key=$($values[$key])"
                $found = $true
            }
        }
        if (-not $found) { $lines += "$key=$($values[$key])" }
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    # no BOM: UEVR's config parser would read it as part of the first key
    [System.IO.File]::WriteAllLines($path, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

# --- the game's own settings files for one run (-GameIni, a recipe's "game_ini"): changed with the game gone, put
# back byte for byte once it is gone again (also after crashes, hangs and HARNESS_ERROR). The files as they were
# also go to runs\game-ini-pending\ with a journal before anything changes, so a harness that dies before its
# restore (killed, closed, power loss) is undone by the next harness start.

# -GameIni "file|Section|Key=Value;Key2=Value2;file2|Section2|Key=Value" -> entries {file, section, key, value}. An
# entry without "file|" keeps the previous file (the first one defaults to GameUserSettings.ini), one with only
# Key=Value the previous section too. The value is everything after the first "=" (it may hold "|", "=", quotes,
# parentheses), but no ";".
function ConvertFrom-GameIniSpec([string]$text) {
    $out = @()
    $file = ""
    $section = ""
    foreach ($part in ($text -split ";")) {
        if ($part.Trim() -eq "") { continue }
        $eq = $part.IndexOf("=")
        $head = @()
        if ($eq -gt 0) { $head = @($part.Substring(0, $eq).Split("|") | ForEach-Object { $_.Trim() }) }
        if ($eq -le 0 -or $head.Count -gt 3 -or $head[-1] -eq "") { throw "-GameIni expects [file|][Section|]Key=Value entries separated by ';', got '$part'" }
        if ($head.Count -eq 3) { $file = $head[0]; $section = $head[1] }
        elseif ($head.Count -eq 2) { $section = $head[0]; if ($file -eq "") { $file = "GameUserSettings.ini" } }
        $section = $section.Trim().TrimStart("[").TrimEnd("]")
        if ($file -eq "" -or $section -eq "") { throw "-GameIni: '$part' needs a file and a section (file|Section|Key=Value)" }
        $out += [pscustomobject][ordered]@{ file = $file; section = $section; key = $head[-1]; value = $part.Substring($eq + 1).Trim() }
    }
    return $out
}

# A recipe's "game_ini": {"<file>": {"<Section>": {"<Key>": "<Value>"}}} -> the same entries
function ConvertFrom-GameIniBlock($block) {
    $out = @()
    if ($block -isnot [System.Management.Automation.PSCustomObject]) { throw "`"game_ini`" must be an object {file: {Section: {Key: Value}}}" }
    foreach ($f in $block.PSObject.Properties) {
        if ($f.Value -isnot [System.Management.Automation.PSCustomObject]) { throw "`"game_ini`".`"$($f.Name)`" must be an object {Section: {Key: Value}}" }
        foreach ($s in $f.Value.PSObject.Properties) {
            if ($s.Value -isnot [System.Management.Automation.PSCustomObject]) { throw "`"game_ini`".`"$($f.Name)`".`"$($s.Name)`" must be an object {Key: Value}" }
            foreach ($k in $s.Value.PSObject.Properties) {
                # UE writes booleans as True / False
                $v = $k.Value; if ($v -is [bool]) { $v = $(if ($v) { "True" } else { "False" }) }
                $out += [pscustomobject][ordered]@{ file = $f.Name; section = $s.Name.Trim().TrimStart("[").TrimEnd("]"); key = $k.Name; value = [string]$v }
            }
        }
    }
    return $out
}

# The file an entry names: a bare file name is in the game's config folder (next to its GameUserSettings.ini: Engine.ini,
# Scalability.ini, ...), anything else an absolute path, with save_slot's placeholders ({localappdata}, {gamedir}, ...)
function Resolve-GameIniPath([string]$file, [string]$configDir, [hashtable]$values) {
    $p = Expand-SavePlaceholders $file $values "-GameIni"
    if ($p -eq [System.IO.Path]::GetFileName($p)) {
        if (-not $configDir) { throw "-GameIni: '$file' is a file name, but this game has no config folder with a GameUserSettings.ini (give the full path)" }
        $p = Join-Path $configDir $p
    } elseif (-not [System.IO.Path]::IsPathRooted($p)) {
        throw "-GameIni: '$file' is neither a file name nor an absolute path"
    }
    $p = [System.IO.Path]::GetFullPath($p)
    if (-not (Test-Path -LiteralPath (Split-Path -Parent $p) -PathType Container)) { throw "-GameIni: folder not found: $(Split-Path -Parent $p)" }
    if (Test-Path -LiteralPath $p -PathType Container) { throw "-GameIni: $p is a folder" }
    return $p
}

# An ini file's text and how to write it back the same way: its encoding and byte order mark (UE writes UTF-16 LE with
# a BOM once a value needs it, else plain ASCII / UTF-8) and its line breaks
function Read-IniText([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    $skip = 0
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { $enc = New-Object System.Text.UTF8Encoding($true); $skip = 3 }
    elseif ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { $enc = New-Object System.Text.UnicodeEncoding($false, $true); $skip = 2 }
    elseif ($b.Length -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF) { $enc = New-Object System.Text.UnicodeEncoding($true, $true); $skip = 2 }
    else {
        $enc = New-Object System.Text.UTF8Encoding($false, $true)
        try { $null = $enc.GetString($b) } catch { $enc = [System.Text.Encoding]::Default }   # not UTF-8: the ANSI code page
    }
    $text = $enc.GetString($b, $skip, $b.Length - $skip)
    $nl = $(if ($text.Contains("`r`n")) { "`r`n" } elseif ($text.Contains("`n")) { "`n" } else { "`r`n" })
    # written back unchanged, the text has to give the same bytes, or "every other line stays" can't hold (UTF-16
    # without its BOM reads as text with NULs; a code page that doesn't map every byte back)
    $back = [byte[]](@($enc.GetPreamble()) + @($enc.GetBytes($text)))
    $why = $(if ($text.IndexOf([char]0) -ge 0) { "it has NUL characters (UTF-16 without a byte order mark?)" }
        elseif ([Convert]::ToBase64String($back) -cne [Convert]::ToBase64String($b)) { "its text doesn't encode back to the same bytes ($($enc.WebName))" }
        else { $null })
    return [pscustomobject]@{ text = $text; encoding = $enc; nl = $nl; roundtrip = ($null -eq $why); why = $why }
}

function Write-IniText([string]$path, $ini) {
    $bytes = [byte[]]@($ini.encoding.GetPreamble()) + $ini.encoding.GetBytes($ini.text)
    [System.IO.File]::WriteAllBytes($path, [byte[]]$bytes)
}

# The line index ranges of [Section]'s blocks, in file order: {start = the header's index, end = the next header or
# the end}, none when the file has no such section. UE reads a section that appears twice as one. Sections and keys
# compare without case, as UE's config does.
function Find-IniSection([System.Collections.Generic.List[string]]$lines, [string]$section) {
    $blocks = @()
    $start = -1
    for ($i = 0; $i -le $lines.Count; $i++) {
        $header = $i -lt $lines.Count -and $lines[$i].TrimStart().StartsWith("[")
        if (-not $header -and $i -lt $lines.Count) { continue }
        if ($start -ge 0) { $blocks += [pscustomobject]@{ start = $start; end = $i }; $start = -1 }
        if ($header -and $lines[$i].Trim() -ieq "[$section]") { $start = $i }
    }
    return $blocks
}

# The line indexes of Key in [Section] (every block), in file order
function Find-IniKeyLines([System.Collections.Generic.List[string]]$lines, $blocks, [string]$key) {
    $hits = @()
    foreach ($blk in $blocks) {
        for ($i = $blk.start + 1; $i -lt $blk.end; $i++) {
            $eq = $lines[$i].IndexOf("=")
            if ($eq -gt 0 -and $lines[$i].Substring(0, $eq).Trim() -ieq $key) { $hits += $i }
        }
    }
    return $hits
}

function Split-IniLines([string]$text, [string]$nl) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.AddRange([string[]]$text.Split([string[]]@($nl), [StringSplitOptions]::None))
    return , $lines
}

# Key's value in [Section] (its first line there), $null when the file has no such key
function Get-IniValue([string]$text, [string]$nl, [string]$section, [string]$key) {
    $lines = Split-IniLines $text $nl
    $hits = @(Find-IniKeyLines $lines @(Find-IniSection $lines $section) $key)
    if ($hits.Count -eq 0) { return $null }
    return $lines[$hits[0]].Substring($lines[$hits[0]].IndexOf("=") + 1)
}

# Key=Value in [Section]: the key's first line there gets the value and further lines of the same key are dropped,
# also in a second block of the section (it ends up with this one value); a missing key goes after the section's last
# line, a missing section to the end of the file. Every other line stays as it was. -> the new text and the value
# before ($null: there was none)
function Set-IniValue([string]$text, [string]$nl, [string]$section, [string]$key, [string]$value) {
    $lines = Split-IniLines $text $nl
    $blocks = @(Find-IniSection $lines $section)
    $old = $null
    if ($blocks.Count -eq 0) {
        $at = $lines.Count
        while ($at -gt 0 -and $lines[$at - 1].Trim() -eq "") { $at-- }
        $add = @("[$section]", "$key=$value")
        if ($at -gt 0) { $add = @("") + $add }
        $lines.InsertRange($at, [string[]]$add)
    } else {
        $hits = @(Find-IniKeyLines $lines $blocks $key)
        if ($hits.Count -gt 0) {
            $eq = $lines[$hits[0]].IndexOf("=")
            $old = $lines[$hits[0]].Substring($eq + 1)
            $lines[$hits[0]] = $lines[$hits[0]].Substring(0, $eq) + "=" + $value
            # the later lines from the back, so the indexes still to go stay valid
            for ($h = $hits.Count - 1; $h -ge 1; $h--) { $lines.RemoveAt($hits[$h]) }
        } else {
            $start = $blocks[0].start
            $at = $blocks[0].end
            while ($at -gt $start + 1 -and $lines[$at - 1].Trim() -eq "") { $at-- }
            $lines.Insert($at, "$key=$value")
        }
    }
    return [pscustomobject]@{ text = ($lines -join $nl); old = $old }
}

function Clear-ReadOnly([string]$path) {
    $item = Get-Item -LiteralPath $path -Force
    if ($item.Attributes -band [System.IO.FileAttributes]::ReadOnly) { $item.Attributes = $item.Attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly) }
}

# Puts one file back as it was before the run: its bytes, time and attributes (a file that wasn't there is removed
# again), then checks its sha256. -> $null, or why it failed
function Restore-GameIniFile($f) {
    try {
        if ($f.existed) {
            if (Test-Path -LiteralPath $f.path -PathType Leaf) { Clear-ReadOnly $f.path }
            Copy-FileRetry $f.backup $f.path 30
            Clear-ReadOnly $f.path
            (Get-Item -LiteralPath $f.path -Force).LastWriteTimeUtc = [DateTime]::new([int64]$f.modified, [DateTimeKind]::Utc)
            (Get-Item -LiteralPath $f.path -Force).Attributes = [System.IO.FileAttributes]([string]$f.attributes)
            $hash = (Get-FileHash -LiteralPath $f.path -Algorithm SHA256).Hash
            if ($hash -ne $f.sha256) { return "sha256 $hash after the restore, $($f.sha256) before the run" }
        } elseif (Test-Path -LiteralPath $f.path -PathType Leaf) {
            Clear-ReadOnly $f.path
            Remove-Item -LiteralPath $f.path -Force
        }
        return $null
    } catch {
        return $_.Exception.Message
    }
}

# Changes the files for a run. Each file as it was goes to <pendingDir> (with journal.json, written before the first
# byte changes) and to <runDir>\game-ini-before\; then its keys are set and read back. A failure puts back what was
# changed and throws. -> the run's state (files, entries with their values before) for Restore-GameIni and result.json
function Install-GameIni($entries, [string]$runDir, [string]$pendingDir, [string]$processName) {
    # the originals of an earlier run that couldn't be put back exist only in the pending folder: never copied over
    if (Test-Path -LiteralPath (Join-Path $pendingDir "journal.json")) { throw "$pendingDir still holds the originals of an earlier run that were not put back" }
    $paths = @($entries | ForEach-Object { $_.path } | Select-Object -Unique)
    $runEntries = @($entries | ForEach-Object { [pscustomobject][ordered]@{ path = $_.path; section = $_.section; key = $_.key; value = $_.value; before = $null; after = $null } })
    # every file has to be text that is written back the same way (checked before anything is copied or changed)
    foreach ($path in $paths) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $probe = Read-IniText $path
            if (-not $probe.roundtrip) { throw "$path can't be changed safely: $($probe.why)" }
        }
    }
    $beforeDir = Join-Path $runDir "game-ini-before"
    $files = @()
    $n = 0
    try {
        New-Item -ItemType Directory -Force -Path $pendingDir | Out-Null
        foreach ($path in $paths) {
            $n++
            $f = [pscustomobject][ordered]@{ path = $path; existed = (Test-Path -LiteralPath $path -PathType Leaf); sha256 = $null; size = $null
                modified = $null; attributes = $null; backup = $null; run_copy = $null; after = $null; after_sha256 = $null; restored = $null; error = $null }
            if ($f.existed) {
                $item = Get-Item -LiteralPath $path -Force
                $f.sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                $f.size = $item.Length
                $f.modified = $item.LastWriteTimeUtc.Ticks
                $f.attributes = [string]$item.Attributes
                $f.backup = Join-Path $pendingDir ("$n-" + $item.Name)
                Copy-FileRetry $path $f.backup 30
                Clear-ReadOnly $f.backup
                if ((Get-FileHash -LiteralPath $f.backup -Algorithm SHA256).Hash -ne $f.sha256) { throw "the copy of $path in $pendingDir differs from it" }
                New-Item -ItemType Directory -Force -Path $beforeDir | Out-Null
                $f.run_copy = Join-Path $beforeDir ("$n-" + $item.Name)
                Copy-Item -LiteralPath $f.backup -Destination $f.run_copy -Force
            }
            $files += $f
        }
        $journal = [ordered]@{ run = $runDir; process = $processName; written = (Get-Date).ToString("s")
            files = @($files | ForEach-Object { [ordered]@{ path = $_.path; existed = $_.existed; sha256 = $_.sha256; modified = $_.modified; attributes = $_.attributes; backup = $_.backup } }) }
        $tmp = Join-Path $pendingDir "journal.json.tmp"
        [System.IO.File]::WriteAllText($tmp, (ConvertTo-Json $journal -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
        Move-Item -LiteralPath $tmp -Destination (Join-Path $pendingDir "journal.json") -Force
    } catch {
        # nothing changed yet: the copies (and a journal, if it got written) are of no use
        Remove-Item -LiteralPath $pendingDir -Recurse -Force -ErrorAction SilentlyContinue
        throw
    }
    try {
        foreach ($f in $files) {
            $ini = $(if ($f.existed) { Read-IniText $f.path } else { [pscustomobject]@{ text = ""; encoding = (New-Object System.Text.UTF8Encoding($false)); nl = "`r`n"; roundtrip = $true; why = $null } })
            if (-not $ini.roundtrip) { throw "$($f.path) can't be changed safely: $($ini.why)" }
            foreach ($e in @($runEntries | Where-Object { $_.path -eq $f.path })) {
                $r = Set-IniValue $ini.text $ini.nl $e.section $e.key $e.value
                $e.before = $r.old
                $ini.text = $r.text
            }
            if ($f.existed) { Clear-ReadOnly $f.path }
            Write-IniText $f.path $ini
            # a read-only file stays read-only for the game
            if ($f.existed) { (Get-Item -LiteralPath $f.path -Force).Attributes = [System.IO.FileAttributes]([string]$f.attributes) }
            $check = Read-IniText $f.path
            foreach ($e in @($runEntries | Where-Object { $_.path -eq $f.path })) {
                $got = Get-IniValue $check.text $check.nl $e.section $e.key
                if ($got -cne $e.value) { throw "$($f.path) [$($e.section)] $($e.key) reads back as '$got', not '$($e.value)'" }
            }
        }
    } catch {
        $why = $_.Exception.Message
        $left = @($files | ForEach-Object { $err = Restore-GameIniFile $_; if ($err) { "$($_.path): $err" } })
        if ($left.Count -eq 0) { Remove-Item -LiteralPath $pendingDir -Recurse -Force -ErrorAction SilentlyContinue }
        else { $why += " (and putting back failed: $($left -join '; '); $pendingDir keeps the originals)" }
        throw $why
    }
    return [pscustomobject][ordered]@{ files = $files; entries = $runEntries; pending = $pendingDir }
}

# After the run, with the game gone: each file as the game left it goes to <runDir>\game-ini-after\ (did it keep the
# values?), then the original goes back. The pending folder is removed once every file is back. -> the failures
function Restore-GameIni($state, [string]$runDir) {
    $afterDir = Join-Path $runDir "game-ini-after"
    $failed = @()
    $n = 0
    foreach ($f in $state.files) {
        $n++
        if (Test-Path -LiteralPath $f.path -PathType Leaf) {
            try {
                New-Item -ItemType Directory -Force -Path $afterDir | Out-Null
                $dest = Join-Path $afterDir ("$n-" + (Split-Path -Leaf $f.path))
                Copy-FileRetry $f.path $dest 10
                Clear-ReadOnly $dest
                $f.after = $dest
                $f.after_sha256 = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
                $ini = Read-IniText $dest
                foreach ($e in @($state.entries | Where-Object { $_.path -eq $f.path })) { $e.after = Get-IniValue $ini.text $ini.nl $e.section $e.key }
            } catch { }
        }
        $err = Restore-GameIniFile $f
        $f.restored = ($null -eq $err)
        $f.error = $err
        if ($err) { $failed += "$($f.path): $err" }
    }
    if ($failed.Count -eq 0) { Remove-Item -LiteralPath $state.pending -Recurse -Force -ErrorAction SilentlyContinue }
    return $failed
}

# The journal of a harness that died before its restore: its game is stopped (it would write its settings when it
# quits) and its files go back before anything else runs. -> a note, or "" when there was nothing to do; throws
# when a file can't be put back (nothing should run on settings left changed)
function Restore-PendingGameIni([string]$pendingDir) {
    $journalPath = Join-Path $pendingDir "journal.json"
    if (-not (Test-Path -LiteralPath $journalPath -PathType Leaf)) { return "" }
    $doc = Get-Content -LiteralPath $journalPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($doc.process) { Stop-Leftovers ([string]$doc.process) }
    $failed = @()
    foreach ($f in @($doc.files)) {
        $err = Restore-GameIniFile $f
        if ($err) { $failed += "$($f.path): $err" }
    }
    if ($failed.Count -gt 0) { throw "could not put back the game settings an interrupted run changed ($($doc.run)): $($failed -join '; ') (originals and journal in $pendingDir)" }
    Remove-Item -LiteralPath $pendingDir -Recurse -Force
    return "put back the game settings an interrupted run left changed ($($doc.run)): $((@($doc.files) | ForEach-Object { $_.path }) -join ', ')"
}

function Get-GameProcess([string]$processName) {
    return Get-Process -Name $processName -ErrorAction SilentlyContinue | Select-Object -First 1
}

# .NET Framework's HasExited is true once the exit code is set; WaitForExit waits for the process handle (really gone)
function Wait-ProcessGone($proc, [int]$ms) {
    try { return $proc.WaitForExit($ms) } catch { return $true }
}

# Minidump of a game stuck while exiting (thread stacks and names, memory the stacks point at). $true when written.
function Save-HangDump([string]$path) {
    if (-not (Test-Path $PilotPython)) { return $false }
    # EAP Stop would turn any stderr line of the helper into an exception (and the run into HARNESS_ERROR)
    try { & { $ErrorActionPreference = "Continue"; & $PilotPython $PilotScript $ProcessName dump $path 2>&1 | Out-Null } } catch { }
    return (Test-Path $path) -and ((Get-Item $path).Length -gt 0)
}

function Stop-Leftovers([string]$processName) {
    foreach ($name in @($processName, "CrashReportClient", "UEVRInjector")) {
        $procs = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        foreach ($p in $procs) {
            try { $p.Kill(); $p.WaitForExit(10000) | Out-Null } catch { }
        }
    }
    # The game's launcher/bootstrap exe (e.g. Polaris.exe) can linger for a moment.
    Start-Sleep -Seconds 2
}

function Save-Screenshot([string]$path) {
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bmp.Size)
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
    } catch {
        Write-Warning "Screenshot failed: $_"
    }
}

function Get-EyeLuma([string]$path, [string]$pattern) {
    $text = Read-Shared $path
    if ($text -eq "") { return $null }
    $samples = @([regex]::Matches($text, $pattern) | Select-Object -Last 120)
    if ($samples.Count -eq 0) { return $null }
    $left = ($samples | ForEach-Object { [double]$_.Groups[1].Value } | Measure-Object -Average).Average
    $right = ($samples | ForEach-Object { [double]$_.Groups[2].Value } | Measure-Object -Average).Average
    $ratio = 0.0
    if ($left -gt 0) { $ratio = $right / $left }
    return [ordered]@{ samples = $samples.Count; left = [math]::Round($left, 4); right = [math]::Round($right, 4); right_over_left = [math]::Round($ratio, 3) }
}

# --- frame rate (perfreport.py) ---

# QueryPerformanceCounter in ns, the clock of UEVR's perf files, monado's stats and gamepilot's segments, with the
# Unix time read next to it, so perfreport.py can put wall-clock data (nvidia-smi, log lines) on the same time line
function Get-ClockPair {
    $ticks = [System.Diagnostics.Stopwatch]::GetTimestamp()
    $unixMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    # scaled like MSVC's steady_clock and Python's time.perf_counter_ns()
    $ns = [int64][math]::Floor([decimal]$ticks * 1000000000 / [System.Diagnostics.Stopwatch]::Frequency)
    return [ordered]@{ qpc_ns = $ns; unix_ms = $unixMs }
}

# Copies a file another process still writes to (UEVR's perf log with -KeepGame)
function Copy-Shared([string]$from, [string]$to) {
    $src = [System.IO.File]::Open($from, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]"ReadWrite, Delete")
    try {
        $dst = [System.IO.File]::Create($to)
        try { $src.CopyTo($dst) } finally { $dst.Dispose() }
    } finally {
        $src.Dispose()
    }
}

# The game's GameUserSettings.ini, $null when the preset names no folder or it isn't there
function Find-GameSettings([string]$settingsDir) {
    if ($settingsDir -eq "") { return $null }
    foreach ($platform in @("Windows", "WindowsNoEditor")) {
        $path = Join-Path $env:LOCALAPPDATA "$settingsDir\Saved\Config\$platform\GameUserSettings.ini"
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    return $null
}

# nvidia-smi writing GPU load, clocks, temperature, power and its limit once a second to <run>\gpu.csv (no admin
# rights needed; a laptop GPU's clocks move with its power limit and temperature). Its stdout, not -f: it flushes
# stdout after every sample, while -f buffers and loses everything when it is stopped.
function Start-GpuLog([string]$runDir) {
    $exe = Get-Command "nvidia-smi.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $exe) { return [ordered]@{ status = "not_found"; error = $null; proc = $null } }
    $query = "timestamp,utilization.gpu,clocks.gr,clocks.mem,temperature.gpu,power.draw,enforced.power.limit,pstate,clocks_event_reasons.active"
    try {
        $proc = Start-Process -FilePath $exe.Source -ArgumentList @("--query-gpu=$query", "--format=csv,nounits", "-lms", "1000") -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runDir "gpu.csv")
        return [ordered]@{ status = "ok"; error = $null; proc = $proc }
    } catch {
        return [ordered]@{ status = "failed"; error = $_.Exception.Message; proc = $null }
    }
}

function Stop-GpuLog($log) {
    if (-not $log -or -not $log.proc) { return }
    if ($log.proc.HasExited) { $log.status = "exited"; return }
    try { $log.proc.Kill(); $log.proc.WaitForExit(3000) | Out-Null } catch { }
}

$PresentMonSession = "MopicTest"

# The newest PresentMon console app of the Intel install, $null when there is none
function Find-PresentMon {
    $dir = Join-Path $env:ProgramFiles "Intel\PresentMon\PresentMonConsoleApplication"
    $exe = Get-ChildItem -LiteralPath $dir -Filter "PresentMon-*-x64.exe" -File -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if ($exe) { return $exe.FullName }
    return $null
}

# PresentMon capturing the game's Presents (and monado-service's) with raw QPC times into
# <run>\presentmon-<exe>-<pid>.csv. It needs admin rights or the Performance Log Users group; without either it ends
# at once with "access denied" (Stop-PresentMon tells), and the run goes on without it. --stop_existing_session ends
# a capture an aborted run left behind.
function Start-PresentMon([string]$runDir, [string]$processName) {
    $exe = Find-PresentMon
    if (-not $exe) { return [ordered]@{ status = "not_found"; exe = $null; error = $null; proc = $null } }
    $pmArgs = @("--process_name", "$processName.exe", "--process_name", "monado-service.exe", "--multi_csv",
        "--output_file", (ConvertTo-ArgvString (Join-Path $runDir "presentmon.csv")), "--qpc_time", "--track_pc_latency",
        "--track_frame_type", "--no_track_input", "--no_console_stats", "--session_name", $PresentMonSession, "--stop_existing_session")
    try {
        $proc = Start-Process -FilePath $exe -ArgumentList $pmArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runDir "presentmon.out.txt") -RedirectStandardError (Join-Path $runDir "presentmon.err.txt")
        return [ordered]@{ status = "ok"; exe = $exe; error = $null; proc = $proc }
    } catch {
        return [ordered]@{ status = "failed"; exe = $exe; error = $_.Exception.Message; proc = $null }
    }
}

# Ends the capture. One that already ended is classified from what it printed; a running one is told to stop its
# trace session, then it writes out its CSV and exits.
function Stop-PresentMon($pm, [string]$runDir) {
    if (-not $pm -or -not $pm.proc) { return }
    if ($pm.proc.HasExited) {
        $lines = @(((Read-Shared (Join-Path $runDir "presentmon.err.txt")) + "`n" + (Read-Shared (Join-Path $runDir "presentmon.out.txt"))) -split "\r?\n" | Where-Object { $_.Trim() -ne "" })
        $pm.status = $(if (($lines -join " ") -match "access denied") { "access_denied" } else { "failed" })
        $pm.error = (@($lines | Where-Object { $_ -match "error" }) + $lines | Select-Object -First 1)
        return
    }
    try {
        $stop = Start-Process -FilePath $pm.exe -ArgumentList @("--terminate_existing_session", "--session_name", $PresentMonSession) -WindowStyle Hidden -PassThru
        $stop.WaitForExit(15000) | Out-Null
    } catch { }
    if (-not $pm.proc.WaitForExit(15000)) {
        try { $pm.proc.Kill() } catch { }
        $pm.status = "killed"
    }
}

# Frame generation modules loaded in the game: nvngx_dlssg.dll (DLSS Frame Generation is set up) and others by name.
# $null when the module list can't be read (the game is gone).
function Get-FrameGenModules($proc) {
    try {
        $proc.Refresh()
        if ($proc.HasExited) { return $null }
        # (a Modules getter that fails reads as $null here, not as an exception)
        $names = @($proc.Modules | Where-Object { $_ } | ForEach-Object { $_.ModuleName })
    } catch { return $null }
    if ($names.Count -eq 0) { return $null }
    return [ordered]@{
        nvngx_dlssg = [bool]($names -contains "nvngx_dlssg.dll")
        fg_modules  = @($names | Where-Object { $_ -match 'dlssg|dlss_g|xess_fg|xefg|frameinterpolation|fsr3' } | Sort-Object -Unique)
    }
}

# AC or battery and the Windows power mode (a laptop GPU's power limit moves with both)
function Get-PowerContext {
    $out = [ordered]@{ ac = $null; battery_pct = $null; scheme = $null; overlay_ac = $null; overlay_dc = $null }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $ps = [System.Windows.Forms.SystemInformation]::PowerStatus
        if ([string]$ps.PowerLineStatus -ne "Unknown") { $out.ac = ([string]$ps.PowerLineStatus -eq "Online") }
        if ($ps.BatteryLifePercent -le 1) { $out.battery_pct = [int][math]::Round($ps.BatteryLifePercent * 100) }
    } catch { }
    try {
        $k = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes" -ErrorAction Stop
        $out.scheme = $k.ActivePowerScheme
        $out.overlay_ac = $k.ActiveOverlayAcPowerScheme
        $out.overlay_dc = $k.ActiveOverlayDcPowerScheme
    } catch { }
    return $out
}

# monado-service's per-second app frame stats (comp_window_mopic.c: <statsDir>\app_frame_stats.csv, the generation
# before it in app_frame_stats.1.csv) with qpc_ns from fromQpcNs to toQpcNs -> <run>\monado-frames.csv. -> rows copied.
# QPC restarts at every boot and the file keeps hours of sessions across boots, so with fromUnixMs/toUnixMs a row must
# also have its unix_ms within a minute of them: an earlier boot's row of the same game can carry the same qpc_ns.
function Save-MonadoSlice([string]$statsDir, [string]$runDir, [int64]$fromQpcNs, [int64]$toQpcNs, [int64]$fromUnixMs = 0, [int64]$toUnixMs = 0) {
    $header = $null
    $rows = New-Object System.Collections.Generic.List[string]
    foreach ($name in @("app_frame_stats.1.csv", "app_frame_stats.csv")) {
        foreach ($line in ((Read-Shared (Join-Path $statsDir $name)) -split "\r?\n")) {
            if ($line -like "qpc_ns,*") { if (-not $header) { $header = $line }; continue }
            $f = $line.Split(",", 3)
            if ($f.Count -lt 3) { continue }
            $q = [int64]0
            $u = [int64]0
            if (-not [int64]::TryParse($f[0], [ref]$q) -or $q -lt $fromQpcNs -or $q -gt $toQpcNs) { continue }
            if ($fromUnixMs -gt 0 -and $toUnixMs -gt 0 -and
                (-not [int64]::TryParse($f[1], [ref]$u) -or $u -lt $fromUnixMs - 60000 -or $u -gt $toUnixMs + 60000)) { continue }
            $rows.Add($line)
        }
    }
    if (-not $header -or $rows.Count -eq 0) { return 0 }
    [System.IO.File]::WriteAllLines((Join-Path $runDir "monado-frames.csv"), [string[]](@($header) + $rows.ToArray()), (New-Object System.Text.UTF8Encoding($false)))
    return $rows.Count
}

# perfreport.py over the run folder (perf.json, perf-summary.json) -> its summary (result.json's "perf") and its
# summary.txt line, or why there is none
function Invoke-PerfReport([string]$runDir) {
    if (-not (Test-Path $PilotPython) -or -not (Test-Path $PerfScript)) {
        return [pscustomobject]@{ summary = $null; line = "perf: not measured (no .venv or no perfreport.py)" }
    }
    $output = @()
    # EAP Stop would turn any stderr line of the script into an exception (and the run into HARNESS_ERROR)
    try { $output = @(& { $ErrorActionPreference = "Continue"; & $PilotPython $PerfScript "run" $runDir 2>&1 | ForEach-Object { "$_" } }) } catch { $output = @($_.Exception.Message) }
    $summaryPath = Join-Path $runDir "perf-summary.json"
    if (Test-Path $summaryPath) {
        try {
            $doc = Get-Content -LiteralPath $summaryPath -Raw -Encoding UTF8 | ConvertFrom-Json
            return [pscustomobject]@{ summary = $doc.summary; line = [string]$doc.line }
        } catch {
            $output += "perf-summary.json: $($_.Exception.Message)"
        }
    }
    $why = @($output | Where-Object { "$_".Trim() -ne "" }) | Select-Object -Last 1
    return [pscustomobject]@{ summary = $null; line = "perf: perfreport.py failed ($why)" }
}

# --- each eye on the Mopic display (-EyeSampler: analysis\eyesampler.py, monado-service in MOPIC_MODE=sbs)

# Asks the sampler to stop through its stop file (it then writes eyes-meta.json); killed after 10 s
function Stop-EyeSampler($proc, [string]$stopFile) {
    if (-not $proc -or $proc.HasExited) { return }
    try { [System.IO.File]::WriteAllText($stopFile, "stop") } catch { }
    if (-not $proc.WaitForExit(10000)) { try { $proc.Kill() } catch { } }
}

# eyesampler.py report over the run folder (eyes-report.json) -> its summary (result.json's "eyes") and summary.txt line
function Invoke-EyeReport([string]$runDir) {
    if (-not (Test-Path -LiteralPath (Join-Path $runDir "eyes.csv"))) {
        $err = @((Read-Shared (Join-Path $runDir "eyes.err.txt")) -split "\r?\n" | Where-Object { $_.Trim() -ne "" }) | Select-Object -Last 1
        return [pscustomobject]@{ summary = $null; line = "eyes: no samples (the sampler wrote no eyes.csv$(if ($err) { ": $err" }))" }
    }
    $output = @()
    try { $output = @(& { $ErrorActionPreference = "Continue"; & $PilotPython $EyeScript "report" $runDir 2>&1 | ForEach-Object { "$_" } }) } catch { $output = @($_.Exception.Message) }
    $reportPath = Join-Path $runDir "eyes-report.json"
    if (Test-Path -LiteralPath $reportPath) {
        try {
            $doc = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
            return [pscustomobject]@{ summary = $doc.summary; line = [string]$doc.line }
        } catch {
            $output += "eyes-report.json: $($_.Exception.Message)"
        }
    }
    $why = @($output | Where-Object { "$_".Trim() -ne "" }) | Select-Object -Last 1
    return [pscustomobject]@{ summary = $null; line = "eyes: eyesampler.py report failed ($why)" }
}

# binocular.py analyze over the whole-display frames eyesampler.py --full-every saved (binocular\*.png) -> its summary
# (result.json's "binocular") and summary.txt line; $null without frames
function Invoke-BinocularReport([string]$runDir) {
    if (-not (Get-ChildItem -LiteralPath (Join-Path $runDir "binocular") -Filter *.png -ErrorAction SilentlyContinue)) { return $null }
    $output = @()
    try { $output = @(& { $ErrorActionPreference = "Continue"; & $PilotPython $BinocularScript "analyze" $runDir 2>&1 | ForEach-Object { "$_" } }) } catch { $output = @($_.Exception.Message) }
    $reportPath = Join-Path $runDir "binocular-report.json"
    if (Test-Path -LiteralPath $reportPath) {
        try {
            $doc = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
            return [pscustomobject]@{ summary = $doc.summary; line = [string]$doc.summary.line }
        } catch {
            $output += "binocular-report.json: $($_.Exception.Message)"
        }
    }
    $why = @($output | Where-Object { "$_".Trim() -ne "" }) | Select-Object -Last 1
    return [pscustomobject]@{ summary = $null; line = "binocular: binocular.py analyze failed ($why)" }
}

# ---------------------------------------------------------------------------------------------------------------

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($EngineDir -eq "") {
    $engines = Join-Path $env:APPDATA "MOPIC\mopichub\engines\mopic-uevr"
    $latest = Get-ChildItem $engines -Directory -ErrorAction SilentlyContinue |
        Sort-Object { $v = $null; if ([version]::TryParse($_.Name, [ref]$v)) { $v } else { [version]"0.0" } } | Select-Object -Last 1
    if ($null -eq $latest) { throw "No engine folder under $engines, pass -EngineDir" }
    $EngineDir = $latest.FullName
}

$Injector = Join-Path $EngineDir "UEVRInjector.exe"
$InstalledDll = Join-Path $EngineDir "UEVRBackend.dll"
if (-not (Test-Path $Injector)) { throw "UEVRInjector.exe not found in $EngineDir" }

if ($Dll -ne "") {
    $Dll = (Resolve-Path $Dll).Path
    # a game left over from an earlier run (e.g. one stuck while exiting) keeps the DLL locked
    Stop-Leftovers $ProcessName
    $backup = Join-Path $EngineDir "UEVRBackend.dll.orig"
    if (-not (Test-Path $backup)) { Copy-Item $InstalledDll $backup; Write-Host "Backed up original DLL to $backup" }
    if ($Dll -ne $InstalledDll) {
        # The previous game can hold the DLL for a few seconds after it exits
        $copyDeadline = (Get-Date).AddSeconds(30)
        while ($true) {
            try { Copy-Item $Dll $InstalledDll -Force -ErrorAction Stop; break }
            catch { if ((Get-Date) -gt $copyDeadline) { throw }; Start-Sleep -Seconds 2 }
        }
        $pdb = [System.IO.Path]::ChangeExtension($Dll, ".pdb")
        $installedPdb = Join-Path $EngineDir "UEVRBackend.pdb"
        if (Test-Path $pdb) { Copy-Item $pdb $installedPdb -Force } elseif (Test-Path $installedPdb) { Remove-Item $installedPdb -Force }
    }
}

$DllHash = (Get-FileHash $InstalledDll -Algorithm SHA256).Hash
$DllText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($InstalledDll))
$DllCommit = ""
# UEVR_COMMIT_HASH is embedded as a plain 40-char hex string (skip digit-only lookup tables)
$hashes = [regex]::Matches($DllText, "(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])") | ForEach-Object { $_.Value } | Where-Object { $_ -match "[a-f]" } | Select-Object -Unique
if ($hashes) { $DllCommit = ($hashes -join ",") }
$DllText = $null

if ($LaunchTarget -eq "") {
    $appId = Find-SteamAppId $SteamInstallDir
    if (-not $appId) { throw "Could not find the Steam app id for '$SteamInstallDir', pass -LaunchTarget" }
    $LaunchTarget = "steam://rungameid/$appId"
}

$PersistentDir = Join-Path $env:APPDATA "UnrealVRMod\$ProcessName"
$LogPath = Join-Path $PersistentDir "log.txt"
$DumpPath = Join-Path $PersistentDir "crash.dmp"
$ConfigPath = Join-Path $PersistentDir "config.txt"
$RunsRoot = Join-Path $ScriptDir "runs"

$RecipeConfig = [ordered]@{}
$PilotPython = Join-Path $ScriptDir ".venv\Scripts\python.exe"
$PilotScript = Join-Path $ScriptDir "gamepilot.py"
$PerfScript = Join-Path $ScriptDir "perfreport.py"
$EyeScript = Join-Path $ScriptDir "analysis\eyesampler.py"
$BinocularScript = Join-Path $ScriptDir "analysis\binocular.py"
$MonadoStatsDir = Join-Path $env:LOCALAPPDATA "monado"
$GameIniPending = Join-Path $RunsRoot "game-ini-pending"
$RecipeGameIni = @()
if ($Recipe -ne "") {
    if (-not (Test-Path $Recipe)) { $Recipe = Join-Path $ScriptDir "recipes\$Recipe.json" }
    if (-not (Test-Path $Recipe)) { throw "Recipe not found: $Recipe" }
    $Recipe = (Resolve-Path $Recipe).Path
    if (-not (Test-Path $PilotPython)) { throw "gamepilot venv missing: $PilotPython (python -m venv `"$ScriptDir\.venv`", then `"$PilotPython`" -m pip install -r `"$ScriptDir\requirements.txt`")" }
    $recipeJson = Get-Content $Recipe -Raw -Encoding UTF8 | ConvertFrom-Json
    if (@($recipeJson.steps | Where-Object { $_.PSObject.Properties.Name -contains "expect_exit" }).Count -eq 0) {
        throw "Recipe $Recipe has no expect_exit step (unfinished?)"
    }
    # UEVR settings the route depends on (e.g. the UEVR menu closed at start), applied like -Set for each run
    if ($recipeJson.config) {
        foreach ($p in $recipeJson.config.PSObject.Properties) {
            # UEVR reads booleans only as lower-case true/false
            $val = $p.Value; if ($val -is [bool]) { $val = $val.ToString().ToLowerInvariant() }
            $RecipeConfig[$p.Name] = [string]$val
        }
    }
    # the game's own settings the route depends on, applied like -GameIni for each run
    if ($null -ne $recipeJson.game_ini) {
        try { $RecipeGameIni = @(ConvertFrom-GameIniBlock $recipeJson.game_ini) } catch { throw "Recipe $Recipe`: $($_.Exception.Message)" }
    }
    $WaitForExit = $true
    # the recipe decides how long the game runs; -Seconds is only the upper bound
    if (-not $PSBoundParameters.ContainsKey("Seconds")) { $Seconds = 900 }
}

# -Set is checked here, before any run touches config.txt or a save
$SetPairs = @()
foreach ($pair in ($Set -split ";")) {
    if ($pair.Trim() -eq "") { continue }
    $kv = $pair.Split("=", 2)
    if ($kv.Count -ne 2) { throw "-Set expects Key=Value pairs separated by ';', got '$pair'" }
    $SetPairs += ,@($kv[0].Trim(), $kv[1].Trim())
}

# -RecipeVars fills the recipe's "vars" ({name: default}, null = must be given); checked here so a typo doesn't
# cost a game launch (gamepilot checks the same)
$PilotVarArgs = @()
if ($RecipeVars -ne "" -and $Recipe -eq "") { throw "-RecipeVars needs -Recipe" }
if ($Recipe -ne "") {
    $given = ConvertFrom-RecipeVars $RecipeVars
    $declared = @()
    $required = @()
    if ($null -ne $recipeJson.vars) {
        if ($recipeJson.vars -isnot [System.Management.Automation.PSCustomObject]) { throw "Recipe $Recipe`: `"vars`" must be an object {name: default}" }
        foreach ($p in $recipeJson.vars.PSObject.Properties) { $declared += $p.Name; if ($null -eq $p.Value) { $required += $p.Name } }
    }
    $unknown = @($given.PSBase.Keys | Where-Object { $declared -cnotcontains $_ })
    if ($unknown.Count -gt 0) { throw "-RecipeVars: $Recipe declares no var $($unknown -join ', ') (its `"vars`": $(if ($declared.Count -gt 0) { $declared -join ', ' } else { 'none' }))" }
    $unset = @($required | Where-Object { -not $given.Contains($_) })
    if ($unset.Count -gt 0) { throw "$Recipe needs -RecipeVars for $($unset -join ', ') (no default in its `"vars`")" }
    $PilotVarArgs = @(ConvertTo-PilotVarArgs $RecipeVars)
}

# -SaveFile: the save every run starts from, copied over the file -SaveSlot or the recipe's save_slot names
$SaveSource = $null
$SaveSourceHash = $null
$SaveTarget = $null
if ($SaveFile -ne "") {
    $candidate = $SaveFile
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf) -and -not [System.IO.Path]::IsPathRooted($SaveFile)) { $candidate = Join-Path $ScriptDir $SaveFile }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "-SaveFile not found: $SaveFile" }
    $SaveSource = (Resolve-Path -LiteralPath $candidate).ProviderPath
    if ($SaveSlot -ne "") {
        $slotSpec = [pscustomobject]@{ dir = [System.IO.Path]::GetDirectoryName($SaveSlot); file = [System.IO.Path]::GetFileName($SaveSlot) }
    } elseif ($Recipe -ne "" -and $recipeJson.save_slot) {
        $slotSpec = $recipeJson.save_slot
    } else {
        throw "-SaveFile needs a recipe with a `"save_slot`" or -SaveSlot <file> (where the game keeps the save it loads)"
    }
    $SaveTarget = Resolve-SaveTarget $slotSpec $SaveAs (Get-SavePlaceholderValues (Find-SteamGameDir $SteamInstallDir))
    $SaveSourceHash = (Get-FileHash -LiteralPath $SaveSource -Algorithm SHA256).Hash
} elseif ($SaveAs -ne "" -or $SaveSlot -ne "") {
    throw "-SaveAs and -SaveSlot need -SaveFile"
}

# The game's settings files for each run: the recipe's "game_ini", then -GameIni (the later of two values for one key
# wins), each file resolved and checked here, before any run
$GameIniEntries = @()
$iniEntries = @($RecipeGameIni) + @(ConvertFrom-GameIniSpec $GameIni)
if ($iniEntries.Count -gt 0) {
    if ($KeepGame) { throw "-GameIni / a recipe's game_ini can't be combined with -KeepGame: the kept game writes its settings when it quits, after the harness put them back" }
    $settingsFile = Find-GameSettings $SettingsDir
    $configDir = $(if ($settingsFile) { Split-Path -Parent $settingsFile } else { $null })
    $placeholders = Get-SavePlaceholderValues (Find-SteamGameDir $SteamInstallDir)
    $byKey = [ordered]@{}
    foreach ($e in $iniEntries) {
        $path = Resolve-GameIniPath $e.file $configDir $placeholders
        $byKey["$path|$($e.section)|$($e.key)".ToLowerInvariant()] = [pscustomobject][ordered]@{ path = $path; section = $e.section; key = $e.key; value = $e.value }
    }
    $GameIniEntries = @($byKey.Values)
}
if ($EyeSampler) {
    $MopicSbs = $true
    if (-not (Test-Path $PilotPython) -or -not (Test-Path $EyeScript)) { throw "-EyeSampler needs $PilotPython and $EyeScript" }
}
# what makes these runs' frame rates incomparable with plain runs (perf-context.json "variant": perfreport.py compares a
# run only with runs of the same variant): the eye sampler's load, the game's own settings changed for the run
$RunVariant = [ordered]@{ eye_sampler = [bool]$EyeSampler
    game_ini = ((@($GameIniEntries | ForEach-Object { "$(Split-Path -Leaf $_.path)|$($_.section)|$($_.key)=$($_.value)".ToLowerInvariant() } | Sort-Object)) -join ";") }

Write-Host "Game:     $Game ($ProcessName) via $LaunchTarget"
Write-Host "Engine:   $EngineDir"
Write-Host "DLL:      $DllHash $DllCommit"
Write-Host "Window:   $Seconds s after injection, $Runs run(s)"
if ($Recipe -ne "") { Write-Host "Recipe:   $Recipe" }
if ($RecipeVars -ne "") { Write-Host "Vars:     $RecipeVars" }
if ($SaveSource) { Write-Host "Save:     $SaveSource -> $SaveTarget" }
foreach ($e in $GameIniEntries) { Write-Host "Game ini: $($e.path) [$($e.section)] $($e.key)=$($e.value)" }
if ($MopicSbs -and $Recipe -ne "" -and $recipeJson.source -eq "mopic" -and -not $NoInject) { Write-Host "Pilot:    left eye of the side-by-side Mopic display (monado-service must run with MOPIC_MODE=sbs)" }
if ($EyeSampler) {
    Write-Host "Eyes:     $(if ($Recipe -ne '') { "sampled during the recipe's measured segments" } else { 'sampled for the whole observation' }) (analysis\eyesampler.py)"
    if ($NoInject) { Write-Warning "-EyeSampler with -NoInject: the Mopic display shows no VR, the sampler reads whatever is on it" }
}
if ($NoInject) { Write-Host "Baseline: UEVR is not injected" }
if ($NoPerf) { Write-Host "Perf:     not measured (-NoPerf)" }
if ($KeepGame -and ($Set -ne "" -or $UserScript -ne "" -or $RecipeConfig.Count -gt 0)) {
    Write-Warning "-KeepGame with config overrides: the kept game still has them loaded and UEVR saves its config on later changes (menu toggles...), so they can end up in config.txt. Close the game and check config.txt afterwards."
}

# game settings an interrupted harness left changed go back before anything runs (whatever this run's options)
$pendingNote = Restore-PendingGameIni $GameIniPending
if ($pendingNote) { Write-Warning $pendingNote }

$results = @()

for ($run = 1; $run -le $Runs; $run++) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $runDir = Join-Path $RunsRoot "$stamp-$Game-$Label-r$run"
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    Write-Host ""
    Write-Host "=== Run $run/$Runs -> $runDir" -ForegroundColor Cyan

    Stop-Leftovers $ProcessName

    # Per-run config: info logging (the log is our main signal) + requested overrides. Restored afterwards.
    $configBackup = Join-Path $runDir "config.before.txt"
    $hadConfig = Test-Path $ConfigPath
    if ($hadConfig) { Copy-Item $ConfigPath $configBackup -Force }
    $overrides = @{ "FrameworkConfig_LogLevel" = "2" }
    # UEVR's frame-rate log (perf.csv, perf-frames.csv next to log.txt; on by default, unless a user turned it off)
    if (-not $NoPerf -and -not $NoInject) { $overrides["VR_PerfLog"] = "true" }
    foreach ($key in $RecipeConfig.Keys) { $overrides[$key] = $RecipeConfig[$key] }
    foreach ($kv in $SetPairs) { $overrides[$kv[0]] = $kv[1] }
    Set-ConfigValues $ConfigPath $overrides
    Copy-Item $ConfigPath (Join-Path $runDir "config.txt") -Force

    $userScriptPath = Join-Path $PersistentDir "user_script.txt"
    $userScriptBackup = Join-Path $runDir "user_script.before.txt"
    $hadUserScript = Test-Path $userScriptPath
    if ($UserScript -ne "") {
        if ($hadUserScript) { Copy-Item $userScriptPath $userScriptBackup -Force }
        $cmds = @($UserScript -split ";" | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
        [System.IO.File]::WriteAllLines($userScriptPath, [string[]]$cmds, (New-Object System.Text.UTF8Encoding($false)))
        Copy-Item $userScriptPath (Join-Path $runDir "user_script.txt") -Force
    }

    # The run's save (-SaveFile), put in place last before the launch, with the game gone; a failure is this run's
    # HARNESS_ERROR (still with result.json, save-after and the config restored)
    $saveInfo = $null
    $saveError = $null
    if ($SaveSource) {
        try {
            $saveInfo = Install-SaveFile $SaveSource $SaveSourceHash $SaveTarget $runDir
            Write-Host "Save installed: $SaveSource -> $SaveTarget"
        } catch {
            $saveError = "could not install the save: $($_.Exception.Message)"
        }
    }

    # the game's own settings files for this run (-GameIni, the recipe's "game_ini"), with the game gone; put back in
    # finally once it is gone again. A failure is this run's HARNESS_ERROR, with every file as it was.
    $runIni = $null
    $runIniError = $null
    $iniRetryNote = $null
    if ($GameIniEntries.Count -gt 0 -and -not $saveError) {
        try {
            # an earlier run's files that couldn't be put back (pending folder) are retried first, with the game gone;
            # while they fail this run changes nothing (its install would take the changed files for the originals)
            $iniRetryNote = Restore-PendingGameIni $GameIniPending
            $runIni = Install-GameIni $GameIniEntries $runDir $GameIniPending $ProcessName
            foreach ($e in $runIni.entries) { Write-Host "Game ini set: $(Split-Path -Leaf $e.path) [$($e.section)] $($e.key)=$($e.value) (was $(if ($null -ne $e.before) { $e.before } else { 'not set' }))" }
        } catch {
            $runIniError = "could not change the game's settings: $($_.Exception.Message)"
        }
    }

    # the game's graphics settings as the run starts (frame generation, caps, upscaler, monitor: perf.json's context)
    $settingsPath = $null
    if (-not $NoPerf) {
        $settingsPath = Find-GameSettings $SettingsDir
        if ($settingsPath) {
            New-Item -ItemType Directory -Force -Path (Join-Path $runDir "game-settings") | Out-Null
            try { Copy-Shared $settingsPath (Join-Path $runDir "game-settings\GameUserSettings.ini") } catch { $settingsPath = $null }
        }
    }

    $t0 = Get-Date
    $t0Pair = Get-ClockPair
    $dumpBefore = Get-MTime $DumpPath

    $verdict = ""
    $notes = @()
    $gameProc = $null
    $gameStart = $null
    $injectTime = $null
    $exitTime = $null
    $exitCode = $null
    $firstSeen = [ordered]@{}
    $counts = [ordered]@{}
    $processAgeMs = $null
    $exitCrash = $false
    $exitHang = $false
    $frozen = $false
    $closeSent = $false
    $exitPhase = $false
    $selfExit = $false
    $pilot = $null
    $pilotProc = $null
    $pilotOut = Join-Path $runDir "pilot"
    $pilotStatusPath = Join-Path $runDir "pilot-status.json"
    $gamePids = @()
    $gpuLog = $null
    $presentMonCapture = $null
    $frameGenModules = $null
    $observeEnd = $null
    $eyeProc = $null
    $eyeStop = Join-Path $runDir "eyes.stop"
    if ($run -eq 1 -and $pendingNote) { $notes += $pendingNote }
    if ($iniRetryNote) { $notes += $iniRetryNote }

    try {
        if ($saveError) { $verdict = "HARNESS_ERROR"; $notes += $saveError; throw "stop" }
        if ($runIniError) { $verdict = "HARNESS_ERROR"; $notes += $runIniError; throw "stop" }
        if ($InjectDelay -le 0 -and -not $NoInject) {
            $injectorProc = Start-Process -FilePath $Injector -ArgumentList "--attach=$ProcessName" -WorkingDirectory $EngineDir -PassThru
            Start-Sleep -Seconds 2
        }
        Start-Process $LaunchTarget | Out-Null

        # 1) wait for the game process
        $deadline = $t0.AddSeconds($LaunchTimeout)
        while ((Get-Date) -lt $deadline) {
            $gameProc = Get-GameProcess $ProcessName
            if ($gameProc) { break }
            Start-Sleep -Milliseconds 500
        }
        if (-not $gameProc) { $verdict = "LAUNCH_FAILED"; throw "stop" }
        # opening the handle now keeps the exit code readable after the process is gone
        try { $null = $gameProc.Handle } catch { }
        try { $gameStart = $gameProc.StartTime } catch { $gameStart = Get-Date }
        Write-Host "Game process $($gameProc.Id) started $($gameStart.ToString('HH:mm:ss.fff'))"
        $gamePids += $gameProc.Id
        # frame-rate sampling while the game runs (PresentMon follows the exe name, also through a restart)
        if (-not $NoPerf) {
            $gpuLog = Start-GpuLog $runDir
            if ($PresentMon -ne "off") { $presentMonCapture = Start-PresentMon $runDir $ProcessName }
        }

        if ($InjectDelay -gt 0 -and -not $NoInject) {
            Write-Host "Late injection: waiting $InjectDelay s before starting the injector"
            $injectAt = $gameStart.AddSeconds($InjectDelay)
            while ((Get-Date) -lt $injectAt -and -not $gameProc.HasExited) { Start-Sleep -Milliseconds 500 }
            $injectorProc = Start-Process -FilePath $Injector -ArgumentList "--attach=$ProcessName" -WorkingDirectory $EngineDir -PassThru
            $deadline = (Get-Date).AddSeconds($LaunchTimeout)
        }

        # 2) wait for UEVR to start inside it (fresh log.txt)
        if ($NoInject) { Write-Host "Baseline run: UEVR is not injected" } else {
        while ((Get-Date) -lt $deadline) {
            if ((Get-MTime $LogPath) -gt $t0) {
                $firstLine = ((Read-Shared $LogPath) -split "`n" | Select-Object -First 1)
                if ($firstLine -match "UnrealVR entry") { $injectTime = Get-LogTime $firstLine; break }
            }
            if ($gameProc.HasExited) {
                # some launchers/DRM restart the shipping exe once; follow the new process
                Start-Sleep -Seconds 3
                $next = Get-GameProcess $ProcessName
                if (-not $next) { break }
                $gameProc = $next
                try { $null = $gameProc.Handle } catch { }
                try { $gameStart = $gameProc.StartTime } catch { $gameStart = Get-Date }
                $gamePids += $gameProc.Id
                $notes += "game process restarted before injection (now pid $($gameProc.Id))"
            }
            Start-Sleep -Milliseconds 500
        }
        if (-not $injectTime) {
            if ($gameProc.HasExited) { $verdict = "CRASH"; $notes += "game exited before UEVR started" } else { $verdict = "NO_INJECTION" }
            throw "stop"
        }
        $ageAtInject = [math]::Round(($injectTime - $gameStart).TotalSeconds, 1)
        Write-Host "UEVR injected $ageAtInject s after the game process started"
        if ($ageAtInject -gt 30 -and $InjectDelay -le 0) { $notes += "WARN: injected $ageAtInject s after process start, this was not an early injection" }
        }

        # 3) observe (while the pilot drives the game, when there is a recipe)
        if ($Recipe -ne "") {
            $pilotArgs = @("`"$PilotScript`"", $ProcessName, "run", "`"$Recipe`"", "--out", "`"$pilotOut`"", "--status", "`"$pilotStatusPath`"")
            if ($NoInject) { $pilotArgs += @("--source", "window") }
            elseif ($MopicSbs -and $recipeJson.source -eq "mopic") { $pilotArgs += @("--source", "mopic-sbs") }
            $pilotArgs += $PilotVarArgs
            $pilotProc = Start-Process -FilePath $PilotPython -ArgumentList $pilotArgs -WindowStyle Hidden -PassThru
            Write-Host "Pilot started (pid $($pilotProc.Id)), status: $pilotStatusPath"
        }
        # each eye on the Mopic display: during the pilot's measured segments, or the whole observation
        if ($EyeSampler) {
            $eyeArgs = @("`"$EyeScript`"", "sample", "--out", "`"$runDir`"", "--pid", "$($gameProc.Id)", "--seconds", "$($Seconds + 300)", "--stop-file", "`"$eyeStop`"")
            if ($Recipe -ne "") { $eyeArgs += @("--status", "`"$pilotStatusPath`"") }
            if ($EyeSamplerArgs -ne "") { $eyeArgs += $EyeSamplerArgs }
            $eyeProc = Start-Process -FilePath $PilotPython -ArgumentList $eyeArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runDir "eyes.out.txt") -RedirectStandardError (Join-Path $runDir "eyes.err.txt")
            Write-Host "Eye sampler started (pid $($eyeProc.Id))"
        }
        $end = (Get-Date).AddSeconds($Seconds)
        $unresponsiveSince = $null
        $nextModuleProbe = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $end) {
            if ($gameProc.HasExited) { $exitTime = Get-Date; break }
            if ((Get-MTime $DumpPath) -gt $dumpBefore) { $notes += "crash.dmp written"; break }
            if ($pilotProc -and $pilotProc.HasExited) { break }
            # A deadlocked game keeps its process alive; its window stops answering messages (UE's game thread
            # pumps them). Loading screens can stall it briefly, hence the grace period.
            $responding = $true
            try { $gameProc.Refresh(); $responding = ($gameProc.MainWindowHandle -eq [IntPtr]::Zero) -or $gameProc.Responding } catch { }
            if ($responding) { $unresponsiveSince = $null }
            elseif (-not $unresponsiveSince) { $unresponsiveSince = Get-Date }
            elseif (((Get-Date) - $unresponsiveSince).TotalSeconds -ge $HangSeconds) { $frozen = $true; break }
            # frame generation is measured, never changed: which of its DLLs the game has loaded (every 30 s)
            if (-not $NoPerf -and (Get-Date) -ge $nextModuleProbe) {
                $nextModuleProbe = (Get-Date).AddSeconds(30)
                $probe = Get-FrameGenModules $gameProc
                if ($probe) {
                    if (-not $frameGenModules) { $frameGenModules = [ordered]@{ nvngx_dlssg = $false; fg_modules = @(); probes = 0 } }
                    $frameGenModules.nvngx_dlssg = $frameGenModules.nvngx_dlssg -or $probe.nvngx_dlssg
                    $frameGenModules.fg_modules = @(@($frameGenModules.fg_modules) + @($probe.fg_modules) | Sort-Object -Unique)
                    $frameGenModules.probes++
                }
            }
            Start-Sleep -Seconds 1
        }
        # the end of the observation for perfreport.py ("observe" runs without a recipe), before the screenshot
        $observeEnd = Get-ClockPair
        if ($frozen) {
            $dumpNote = $(if (Save-HangDump (Join-Path $runDir "freeze.dmp")) { "stacks in freeze.dmp" } else { "no dump" })
            $notes += "game window not responding for $HangSeconds s, $dumpNote"
        }
        if ($Screenshot -and -not $gameProc.HasExited) { Save-Screenshot (Join-Path $runDir "screenshot.png") }

        Start-Sleep -Seconds 2
        if ($pilotProc) {
            # the pilot notices the window going away by itself
            if (-not $pilotProc.HasExited) { $pilotProc.WaitForExit(15000) | Out-Null }
            if (-not $pilotProc.HasExited) {
                # the venv's python.exe is a launcher; the interpreter runs in its job and ends with it
                $notes += "pilot still running, stopped it"
                try { $pilotProc.Kill() } catch { }
            } else {
                try { if ($pilotProc.ExitCode -ne 0) { $notes += "pilot exit code $($pilotProc.ExitCode)" } } catch { }
            }
            if (Test-Path $pilotStatusPath) { try { $pilot = Get-Content $pilotStatusPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { } }
            if (-not $pilot) { $notes += "pilot wrote no status" }
            if ($pilot -and $pilot.error_kind -eq "exit_timeout" -and -not $gameProc.HasExited) {
                # quit through the menu, but the game never got to ExitProcess
                $exitHang = $true
                $dumpNote = $(if (Save-HangDump (Join-Path $runDir "exit-hang.dmp")) { "stacks in exit-hang.dmp" } else { "no dump" })
                $notes += "game still running after the menu quit (never called ExitProcess), $dumpNote"
            }
        }
        $dumped = (Get-MTime $DumpPath) -gt $dumpBefore
        # With a recipe the exit is expected only once the pilot has started quitting through the menu
        $exitPhase = $WaitForExit -and ($Recipe -eq "" -or ($pilot -and $pilot.exit_expected))
        if ($gameProc.HasExited -and $exitPhase) {
            $selfExit = $true
            if ($dumped) { $exitCrash = $true; $notes += "crash.dmp written while quitting" } else { $notes += "game quit by itself (menu exit)" }
            $crashed = $false
        } else {
            $crashed = $gameProc.HasExited -or $dumped
        }
        if ($GracefulExit -and -not $crashed -and -not $frozen -and -not $gameProc.HasExited) {
            Write-Host "Closing the game window (graceful exit)"
            $closeRequested = $false
            try { $closeRequested = $gameProc.CloseMainWindow() } catch { }
            $closeSent = $true
            if (-not $closeRequested) { $notes += "WM_CLOSE could not be sent (no main window)" }
            $closeDeadline = (Get-Date).AddSeconds(45)
            while ((Get-Date) -lt $closeDeadline -and -not $gameProc.HasExited) { Start-Sleep -Milliseconds 500 }
            if (-not $gameProc.HasExited) {
                # a hang before ExitProcess, or a game that ignores WM_CLOSE / asks to confirm: the dump's main thread tells
                $exitHang = $true
                $dumpNote = $(if (Save-HangDump (Join-Path $runDir "exit-hang.dmp")) { "stacks in exit-hang.dmp" } else { "no dump" })
                $notes += "game still running 45 s after WM_CLOSE (stuck before ExitProcess, or it ignores WM_CLOSE), $dumpNote"
            }
            Start-Sleep -Seconds 3
            if ((Get-MTime $DumpPath) -gt $dumpBefore) { $exitCrash = $true; $notes += "crash.dmp written during shutdown" }
        }
        # HasExited turns true as soon as the exit code is set (ExitProcess was called), but the process can still be
        # stuck in DLL shutdown after that (UEVR's static destructors released D3D objects into a driver that never
        # returned). It is only gone once its handle is signaled.
        if ($gameProc.HasExited -and -not (Wait-ProcessGone $gameProc 30000)) {
            $exitHang = $true
            $dumpNote = $(if (Save-HangDump (Join-Path $runDir "exit-hang.dmp")) { "stacks in exit-hang.dmp" } else { "no dump" })
            $notes += "process still alive 30 s after it started exiting (stuck in shutdown), $dumpNote"
        }
        if ($gameProc.HasExited) {
            if (-not $exitTime) { $exitTime = Get-Date }
            try {
                if ($null -ne $gameProc.ExitCode) {
                    $exitCode = "0x{0:X8}" -f $gameProc.ExitCode
                    # an NTSTATUS error while quitting (fast fail, heap corruption...) that no crash handler reported
                    $code = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$gameProc.ExitCode), 0)
                    if (($selfExit -or $closeSent) -and $code -ge [uint32]3221225472 -and -not $exitCrash) {
                        $exitCrash = $true
                        $notes += "exit code $exitCode while quitting"
                    }
                }
            } catch { }
        }
        if ($crashed) { $verdict = "CRASH" } elseif ($frozen) { $verdict = "FREEZE" } elseif ($exitCrash) { $verdict = "EXIT_CRASH" } elseif ($exitHang) { $verdict = "EXIT_HANG" } else { $verdict = "PASS" }
        # Quitting through the menu and the game exiting cleanly is the recipe's goal, even if the pilot didn't get
        # to record it before it was stopped
        $pilotDone = $pilot -and ($pilot.state -eq "done" -or ($selfExit -and $pilot.exit_expected -and -not $pilot.error))
        if ($pilotDone -and $pilot.state -ne "done") { $notes += "pilot didn't record the quit (status: $($pilot.state))" }
        if ($verdict -eq "PASS" -and $Recipe -ne "" -and -not $pilotDone) {
            # UEVR survived, but the run didn't go through gameplay and the menu exit as recorded
            if ($pilot -and $pilot.error_kind -eq "exit_timeout") { $verdict = "EXIT_HANG" } else { $verdict = "MENU_FAIL" }
        }
        if ($verdict -eq "PASS" -and $WaitForExit -and -not $selfExit) { $notes += "WARN: -WaitForExit but the game was still running at the end of the window" }
    } catch {
        if ($_.Exception.Message -ne "stop") { $verdict = "HARNESS_ERROR"; $notes += $_.Exception.Message }
    } finally {
        if ($pilotProc -and -not $pilotProc.HasExited) { try { $pilotProc.Kill() } catch { } }
        # (nothing before the settings files are put back below may throw)
        try { Stop-EyeSampler $eyeProc $eyeStop } catch { }
        # the frame-rate sampling ends with the observation, before the game is closed
        Stop-PresentMon $presentMonCapture $runDir
        Stop-GpuLog $gpuLog
        $perfEnd = Get-ClockPair
        $crashReporter = [bool](Get-Process -Name "CrashReportClient" -ErrorAction SilentlyContinue)
        if (-not ($KeepGame -and $run -eq $Runs)) { Stop-Leftovers $ProcessName }

        # the installed save as the game left it (autosaves, a format upgrade), also after crashes, first thing once
        # the game is gone; after a failed install, whatever the slot holds now
        $saveAfter = $null
        if ($SaveSource) {
            try {
                $saveAfter = Save-SaveEvidence $SaveTarget $SaveSourceHash $runDir $t0
                if ($saveInfo) {
                    $saveInfo.after = $saveAfter
                    if ($saveAfter.missing) { $notes += "the installed save is gone after the run: $SaveTarget" }
                    if ($KeepGame -and $run -eq $Runs) { $notes += "save-after was copied while the game was still running (-KeepGame)" }
                }
            } catch {
                $notes += "could not copy the save after the run: $($_.Exception.Message)"
            }
        }

        # the game's settings files back as they were, with the game gone (as it left them: game-ini-after\)
        $settingsAfter = $settingsPath
        if ($runIni) {
            $iniFailed = @(Restore-GameIni $runIni $runDir)
            foreach ($f in $iniFailed) { $notes += "ERROR: could not put back the game's settings file $f ($GameIniPending keeps the original; the next harness start retries)" }
            foreach ($e in $runIni.entries) {
                if (@($runIni.files | Where-Object { $_.path -eq $e.path -and $_.after }).Count -gt 0 -and $e.after -cne $e.value) {
                    $notes += "WARN: the game changed $($e.key) during the run: $(if ($null -ne $e.after) { $e.after } else { 'removed' }) instead of $($e.value)"
                }
            }
            # perf.json's settings "after" are the file as the game left it, not the one put back
            $hit = @($runIni.files | Where-Object { $settingsPath -and $_.path -ieq $settingsPath })
            if ($hit.Count -gt 0) { $settingsAfter = $hit[0].after }
        }

        # collect
        $logText = Read-Shared $LogPath
        if ((Get-MTime $LogPath) -gt $t0) { [System.IO.File]::WriteAllText((Join-Path $runDir "log.txt"), $logText) }
        if ((Get-MTime $DumpPath) -gt $dumpBefore) { Copy-Item $DumpPath (Join-Path $runDir "crash.dmp") -Force }
        if ($crashReporter) { $notes += "UE CrashReportClient appeared" }

        # The game's own UE crash report (%LOCALAPPDATA%\<project>\Saved\Crashes\UECC-*) has the error message and a minidump
        $ueCrashes = @(Get-ChildItem (Join-Path $env:LOCALAPPDATA "*\Saved\Crashes\*") -Directory -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 })
        foreach ($uc in $ueCrashes) {
            Copy-Item $uc.FullName (Join-Path $runDir "ue-crash-$($uc.Name)") -Recurse -Force
            $ctx = Join-Path $uc.FullName "CrashContext.runtime-xml"
            if (Test-Path $ctx) {
                try {
                    $msg = ([xml](Get-Content $ctx -Raw -Encoding UTF8)).FGenericCrashContext.RuntimeProperties.ErrorMessage
                    if ($msg) { $notes += "UE crash report: $msg" }
                } catch { }
            }
        }
        if ($ueCrashes.Count -gt 0 -and $verdict -in @("PASS", "MENU_FAIL", "EXIT_HANG")) { if ($selfExit -or $closeSent) { $verdict = "EXIT_CRASH" } else { $verdict = "CRASH" } }

        if ((Get-MTime $LogPath) -gt $t0) {
            $lines = $logText -split "`n"
            foreach ($key in $Milestones.Keys) {
                $hits = @($lines | Where-Object { $_ -match $Milestones[$key] })
                $counts[$key] = $hits.Count
                if ($hits.Count -gt 0) {
                    $t = Get-LogTime $hits[0]
                    if ($t -and $gameStart) { $firstSeen[$key] = [math]::Round(($t - $gameStart).TotalSeconds, 1) }
                    if ($key -eq "process_age" -and $hits[0] -match $Milestones[$key]) { $processAgeMs = [int]$Matches[1] }
                }
            }
            $lastLine = ($lines | Where-Object { $_.Trim() -ne "" } | Select-Object -Last 1)
            if ($verdict -eq "CRASH" -and $lastLine) { $notes += "last log line: $($lastLine.Trim())" }
        }

        # A game that survives without ever starting VR tells us nothing about the fix.
        if ($verdict -in @("PASS", "MENU_FAIL") -and -not $NoInject -and ($counts["xr_instance_fail"] -gt 0 -or $counts["xr_ready"] -eq 0)) {
            $verdict = "NO_VR"
            if ($counts["xr_instance_fail"] -gt 0) { $notes += "OpenXR instance creation failed (runtime not available? check Mopic Hub / monado-service)" } else { $notes += "OpenXR session never reached READY" }
        }

        if ($verdict -eq "PASS") {
            $bootstrapDone = ($counts["bootstrap_done"] -gt 0) -or ($counts["bootstrap_noop"] -gt 0)
            if ($counts["bootstrap_defer"] -gt 0 -and -not $bootstrapDone -and $counts["bootstrap_fail"] -eq 0) { $notes += "WARN: LocalPlayer bootstrap deferred but never ran (right eye has no view state)" }
            if ($counts["bootstrap_fail"] -gt 0) { $notes += "WARN: PostInitProperties slot not found (bootstrap had nothing to call; on UE5 the engine allocates the second view state itself)" }
            if ($counts["veh_exception"] -gt 0) { $notes += "bootstrap handler caught $($counts['veh_exception']) exception(s) on its thread" }
        }

        $eye = $null
        if ($EyeLumaLog -ne "") {
            $eye = Get-EyeLuma $EyeLumaLog $EyeLumaPattern
            if ($eye -and [math]::Abs(1.0 - $eye.right_over_left) -gt 0.10) { $notes += "WARN: eye luminance differs (right/left = $($eye.right_over_left))" }
        }

        if ($hadConfig) { Copy-Item $configBackup $ConfigPath -Force } elseif (Test-Path $ConfigPath) { Remove-Item $ConfigPath -Force }
        if ($UserScript -ne "") {
            if ($hadUserScript) { Copy-Item $userScriptBackup $userScriptPath -Force } elseif (Test-Path $userScriptPath) { Remove-Item $userScriptPath -Force }
        }

        # frame rate: UEVR's perf log, monado's stats during the run and the game's settings after it, then
        # perfreport.py (informational: a regression against the previous build is a WARN note, never a verdict)
        $perf = $null
        $perfLine = $null
        if (-not $NoPerf -and $gameProc) {
            try {
                # each input on its own: one that can't be copied leaves its part of perf.json null, not all of it
                $copyErrors = @()
                if (-not $NoInject) {
                    foreach ($name in @("perf.csv", "perf-frames.csv")) {
                        $src = Join-Path $PersistentDir $name
                        try { if ((Get-MTime $src) -gt $t0) { Copy-Shared $src (Join-Path $runDir $name) } } catch { $copyErrors += "${name}: $($_.Exception.Message)" }
                    }
                }
                $monadoRows = 0
                try { $monadoRows = Save-MonadoSlice $MonadoStatsDir $runDir $t0Pair.qpc_ns $perfEnd.qpc_ns $t0Pair.unix_ms $perfEnd.unix_ms } catch { $copyErrors += "monado-frames.csv: $($_.Exception.Message)" }
                if ($settingsAfter) {
                    try { Copy-Shared $settingsAfter (Join-Path $runDir "game-settings\GameUserSettings.after.ini") } catch { $copyErrors += "GameUserSettings.after.ini: $($_.Exception.Message)" }
                }
                $pmFiles = @(Get-ChildItem -LiteralPath $runDir -Filter "presentmon*.csv" -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
                $perfContext = [ordered]@{
                    game               = $Game
                    process            = $ProcessName
                    label              = $Label
                    recipe             = $Recipe
                    recipe_vars        = $RecipeVars
                    injected           = (-not $NoInject)
                    dll_sha256         = $DllHash
                    qpc_freq           = [System.Diagnostics.Stopwatch]::Frequency
                    t0                 = $t0Pair
                    observe_end        = $observeEnd
                    end                = $perfEnd
                    game_start_unix_ms = $(if ($gameStart) { ([DateTimeOffset]$gameStart).ToUnixTimeMilliseconds() } else { $null })
                    game_pids          = @($gamePids)
                    presentmon         = $(if ($presentMonCapture) { [ordered]@{ status = $presentMonCapture.status; exe = $presentMonCapture.exe; error = $presentMonCapture.error; files = $pmFiles } } else { [ordered]@{ status = "off" } })
                    nvidia_smi         = $(if ($gpuLog) { [ordered]@{ status = $gpuLog.status; error = $gpuLog.error } } else { $null })
                    monado_rows        = $monadoRows
                    copy_errors        = @($copyErrors)
                    modules            = $frameGenModules
                    power              = (Get-PowerContext)
                    variant            = $RunVariant
                }
                $perfContext | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $runDir "perf-context.json") -Encoding UTF8
                $report = Invoke-PerfReport $runDir
                $perf = $report.summary
                $perfLine = $report.line
            } catch {
                $perfLine = "perf: not measured ($($_.Exception.Message))"
            }
            $prevBuild = $(if ($perf -and $perf.compare) { $perf.compare.prev_build } else { $null })
            if ($prevBuild -and $prevBuild.regression) {
                $notes += "WARN: perf regression vs build $(([string]$prevBuild.dll_sha256) -replace '^(.{12}).*$', '$1'): $(@($prevBuild.regression) -join '; ')"
            }
        }

        # each eye on the Mopic display (informational, never a verdict)
        $eyes = $null
        $eyesLine = $null
        if ($EyeSampler -and $gameProc) {
            $eyeReport = Invoke-EyeReport $runDir
            $eyes = $eyeReport.summary
            $eyesLine = $eyeReport.line
            # alternate-eye rendering updates one eye per engine frame: one-eye changes are its design, not a fault
            if ($eyes -and $perf -and $perf.mode -and $perf.mode.afr) { $eyesLine += " | AFR: one eye per engine frame by design, lags expected" }
        }
        # both eyes' content compared beyond parallax (eyesampler.py --full-every frames; informational)
        $binocular = $null
        $binocularLine = $null
        if ($EyeSampler -and $gameProc) {
            $binReport = Invoke-BinocularReport $runDir
            if ($binReport) { $binocular = $binReport.summary; $binocularLine = $binReport.line }
        }

        $result = [ordered]@{
            verdict             = $verdict
            game                = $Game
            process             = $ProcessName
            label               = $Label
            run                 = $run
            started             = $t0.ToString("s")
            dll_sha256          = $DllHash
            dll_commit          = $DllCommit
            config_override     = $overrides
            user_script         = $UserScript
            inject_delay_s      = $InjectDelay
            inject_after_s      = $(if ($injectTime -and $gameStart) { [math]::Round(($injectTime - $gameStart).TotalSeconds, 1) } else { $null })
            process_age_ms      = $processAgeMs
            exit_after_s        = $(if ($exitTime -and $gameStart) { [math]::Round(($exitTime - $gameStart).TotalSeconds, 1) } else { $null })
            exit_code           = $exitCode
            milestones_s        = $firstSeen
            milestone_counts    = $counts
            eye_luma            = $eye
            recipe              = $Recipe
            recipe_vars         = $RecipeVars
            save                = $(if ($saveInfo) { $saveInfo } elseif ($SaveSource) {
                    $beforeCopy = Join-Path $runDir "save-before\$(Split-Path -Leaf $SaveTarget)"
                    [ordered]@{ source = $SaveSource; source_sha256 = $SaveSourceHash; target = $SaveTarget; error = $saveError
                        target_before = $(if (Test-Path -LiteralPath $beforeCopy) { $beforeCopy } else { $null }); after = $saveAfter }
                } else { $null })
            game_ini            = $(if ($runIni) { [ordered]@{ entries = $runIni.entries; files = $runIni.files } } elseif ($runIniError) { [ordered]@{ error = $runIniError } } else { $null })
            pilot               = $pilot
            perf                = $perf
            eyes                = $eyes
            binocular           = $binocular
            notes               = $notes
        }
        $result | ConvertTo-Json -Depth 10 | Set-Content -Path (Join-Path $runDir "result.json") -Encoding UTF8

        $summary = @(
            "$verdict  $Game/$Label run $run  inject@$($result.inject_after_s)s  exit@$($result.exit_after_s)s  $exitCode"
            ("milestones (s after process start): " + (($firstSeen.Keys | ForEach-Object { "$_=$($firstSeen[$_])" }) -join "  "))
        )
        if ($pilot) {
            $pilotLine = "pilot: $($pilot.state), reached: $(@($pilot.reached) -join ',')"
            if ($pilot.error) { $pilotLine += " | step $($pilot.step) ($($pilot.desc)): $($pilot.error)" }
            $summary += $pilotLine
        }
        if ($SaveSource) {
            $summary += "save: $SaveSource (sha256 $SaveSourceHash) -> $SaveTarget"
            if ($saveAfter) {
                $after = $saveAfter
                $what = $(if ($saveInfo) { "save after" } else { "slot after the failed install" })
                if ($after.missing) { $summary += "${what}: missing" } else {
                    $saveLine = "${what}: $($after.size) B, $(if ($after.changed) { 'changed' } else { 'unchanged' }) -> $($after.path)"
                    if (@($after.others_written).Count -gt 0) { $saveLine += " | also written: $(@($after.others_written) -join ', ')" }
                    $summary += $saveLine
                }
            }
        }
        if ($RecipeVars -ne "") { $summary += "vars: $RecipeVars" }
        if ($runIni) {
            foreach ($e in $runIni.entries) {
                $f = @($runIni.files | Where-Object { $_.path -eq $e.path })[0]
                $summary += "game ini: $(Split-Path -Leaf $e.path) [$($e.section)] $($e.key)=$($e.value) (was $(if ($null -ne $e.before) { $e.before } else { 'not set' }); after the run $(if (-not $f.after) { 'no file' } elseif ($e.after -ceq $e.value) { 'kept' } else { $e.after })) | $(if (-not $f.restored) { "NOT put back: $($f.error)" } elseif ($f.existed) { 'put back, sha256 ok' } else { 'removed again (it was not there)' })"
            }
        }
        if ($perfLine) { $summary += $perfLine }
        if ($eyesLine) { $summary += $eyesLine }
        if ($binocularLine) { $summary += $binocularLine }
        $summary += ($notes | ForEach-Object { "  - $_" })
        $summary | Set-Content -Path (Join-Path $runDir "summary.txt") -Encoding UTF8

        $color = "Red"
        if ($verdict -eq "PASS") { $color = "Green" }
        $summary | ForEach-Object { Write-Host $_ -ForegroundColor $color }

        $results += $result
        if ($gameProc) { try { $gameProc.Dispose() } catch { } }
    }
}

Write-Host ""
Write-Host "=== Summary" -ForegroundColor Cyan
foreach ($r in $results) { Write-Host ("{0,-14} run {1}  inject@{2}s  exit@{3}s" -f $r.verdict, $r.run, $r.inject_after_s, $r.exit_after_s) }

if (@($results | Where-Object { $_.verdict -ne "PASS" }).Count -gt 0) { exit 1 }
exit 0
