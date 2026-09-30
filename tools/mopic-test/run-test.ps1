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
# Exit code: 0 if every run passed, 1 otherwise.
#
# Prerequisites: Steam running and logged in, UEVRInjector's saved settings already set to OpenXR (and
# "Nullify VR plugins" as you normally use it), Mopic Hub running (it provides eye tracking; keep it open) with
# its auto-inject off so only this harness injects, monado-service running, the PC unlocked and nothing else using
# its focus during the run. Any running UEVRInjector.exe and the game itself are closed at the start and end of
# every run. -Dll leaves the given DLL deployed in the engine folder.
param(
    [ValidateSet("Tekken8Demo", "Expedition33", "Wukong", "Hogwarts", "Stray", "Hozy", "ACC", "SonicDemo", "Custom")]
    [string]$Game = "Tekken8Demo",
    [string]$ProcessName = "",        # Custom: process name without .exe
    [string]$SteamInstallDir = "",    # Custom: steamapps\common\<this>, used to find the app id
    [string]$LaunchTarget = "",       # overrides Steam: exe path or URL (e.g. steam://rungameid/123)
    [string]$EngineDir = "",          # default: newest folder under %APPDATA%\MOPIC\mopichub\engines\mopic-uevr
    [string]$Dll = "",                # deploy this UEVRBackend.dll before running (original backed up once as .orig)
    [string]$Set = "",                # extra config.txt keys for this run only, "Key=Value;Key2=Value2" (restored afterwards)
    [string]$UserScript = "",         # console commands for UEVR's user_script.txt for this run only, "r.Foo 0;r.Bar 1" (restored afterwards)
    [int]$Seconds = 120,              # observation window after injection
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
    [string]$EyeLumaLog = "",         # optional log with lines like "eye_luma left=0.183 right=0.179"
    [string]$EyeLumaPattern = 'eye_luma\s+left=([0-9.]+)\s+right=([0-9.]+)'
)

$ErrorActionPreference = "Stop"

$Presets = @{
    Tekken8Demo  = @{ Process = "Polaris-Win64-Shipping";  InstallDir = "TEKKEN 8 Demo" }
    Expedition33 = @{ Process = "SandFall-Win64-Shipping"; InstallDir = "Expedition 33" }
    Wukong       = @{ Process = "b1-Win64-Shipping";       InstallDir = "BlackMythWukong" }
    Hogwarts     = @{ Process = "HogwartsLegacy";          InstallDir = "Hogwarts Legacy" }
    Stray        = @{ Process = "Stray-Win64-Shipping";    InstallDir = "Stray" }
    Hozy         = @{ Process = "CozyGame-Win64-Shipping"; InstallDir = "Hozy" }
    ACC          = @{ Process = "AC2-Win64-Shipping";      InstallDir = "Assetto Corsa Competizione" }
    SonicDemo    = @{ Process = "SonicRacingCrossWorldsSteam"; InstallDir = "SonicRacingCrossWorldsDemo" }
}

if ($Game -ne "Custom") {
    if ($ProcessName -eq "") { $ProcessName = $Presets[$Game].Process }
    if ($SteamInstallDir -eq "") { $SteamInstallDir = $Presets[$Game].InstallDir }
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

function Find-SteamAppId([string]$installDir) {
    $steamPath = $null
    try { $steamPath = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath } catch { }
    if (-not $steamPath) { $steamPath = "${env:ProgramFiles(x86)}\Steam" }
    $steamPath = $steamPath -replace "/", "\"

    $libraries = @($steamPath)
    $vdf = Join-Path $steamPath "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libraries += ($m.Groups[1].Value -replace "\\\\", "\")
        }
    }

    foreach ($lib in ($libraries | Select-Object -Unique)) {
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
    $WaitForExit = $true
    # the recipe decides how long the game runs; -Seconds is only the upper bound
    if (-not $PSBoundParameters.ContainsKey("Seconds")) { $Seconds = 900 }
}

Write-Host "Game:     $Game ($ProcessName) via $LaunchTarget"
Write-Host "Engine:   $EngineDir"
Write-Host "DLL:      $DllHash $DllCommit"
Write-Host "Window:   $Seconds s after injection, $Runs run(s)"
if ($Recipe -ne "") { Write-Host "Recipe:   $Recipe" }
if ($NoInject) { Write-Host "Baseline: UEVR is not injected" }
if ($KeepGame -and ($Set -ne "" -or $UserScript -ne "" -or $RecipeConfig.Count -gt 0)) {
    Write-Warning "-KeepGame with config overrides: the kept game still has them loaded and UEVR saves its config on later changes (menu toggles...), so they can end up in config.txt. Close the game and check config.txt afterwards."
}

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
    foreach ($key in $RecipeConfig.Keys) { $overrides[$key] = $RecipeConfig[$key] }
    foreach ($pair in ($Set -split ";")) {
        if ($pair.Trim() -eq "") { continue }
        $kv = $pair.Split("=", 2)
        if ($kv.Count -ne 2) { throw "-Set expects Key=Value pairs separated by ';', got '$pair'" }
        $overrides[$kv[0].Trim()] = $kv[1].Trim()
    }
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

    $t0 = Get-Date
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
    $closeSent = $false
    $exitPhase = $false
    $selfExit = $false
    $pilot = $null
    $pilotProc = $null
    $pilotOut = Join-Path $runDir "pilot"
    $pilotStatusPath = Join-Path $runDir "pilot-status.json"

    try {
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
            $pilotProc = Start-Process -FilePath $PilotPython -ArgumentList $pilotArgs -WindowStyle Hidden -PassThru
            Write-Host "Pilot started (pid $($pilotProc.Id)), status: $pilotStatusPath"
        }
        $end = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $end) {
            if ($gameProc.HasExited) { $exitTime = Get-Date; break }
            if ((Get-MTime $DumpPath) -gt $dumpBefore) { $notes += "crash.dmp written"; break }
            if ($pilotProc -and $pilotProc.HasExited) { break }
            Start-Sleep -Seconds 1
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
        if ($GracefulExit -and -not $crashed -and -not $gameProc.HasExited) {
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
        if ($crashed) { $verdict = "CRASH" } elseif ($exitCrash) { $verdict = "EXIT_CRASH" } elseif ($exitHang) { $verdict = "EXIT_HANG" } else { $verdict = "PASS" }
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
        $crashReporter = [bool](Get-Process -Name "CrashReportClient" -ErrorAction SilentlyContinue)
        if (-not ($KeepGame -and $run -eq $Runs)) { Stop-Leftovers $ProcessName }

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
            pilot               = $pilot
            notes               = $notes
        }
        $result | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $runDir "result.json") -Encoding UTF8

        $summary = @(
            "$verdict  $Game/$Label run $run  inject@$($result.inject_after_s)s  exit@$($result.exit_after_s)s  $exitCode"
            ("milestones (s after process start): " + (($firstSeen.Keys | ForEach-Object { "$_=$($firstSeen[$_])" }) -join "  "))
        )
        if ($pilot) {
            $pilotLine = "pilot: $($pilot.state), reached: $(@($pilot.reached) -join ',')"
            if ($pilot.error) { $pilotLine += " | step $($pilot.step) ($($pilot.desc)): $($pilot.error)" }
            $summary += $pilotLine
        }
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
