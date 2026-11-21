# Right-eye cost runs: one experiment build of the exp/right-eye-cost branch (-Dll), run through the mopic-test
# harness (run-test.ps1, -Harness) with per-run UEVR settings. Arms differ in settings only:
#   plain  : the build with its defaults (Same Stereo Pass on = right eye PRIMARY, as shipped)
#   ab     : in-run A/B, VR_ExperimentSecondFamilyCvars="10,secondary,skip": 10 s QPC windows take turns
#            (phase = window % 3): 0 plain, 1 right eye SECONDARY (Same Pass off), 2 right eye's render skipped
#   abpm   : ab + VR_ExperimentPassMarkersSeconds=7 (60-present per-pass captures, GPU timestamps)
#   pm     : pass markers only (right eye PRIMARY all run)
#   hold:<spec> : VR_ExperimentSecondFamilyCvars=<spec> (a cvar-hold A/B, e.g. hold:10,r.Foo=0)
#   flat   : run-test -NoInject (the game without UEVR)
#   nort   : UEVR loaded without a VR session (experiment-no-runtime.txt in the game's UEVR folder for the run)
# Extra settings for every arm: -Set "Key=Value;Key=Value" (e.g. VR_ExperimentRightEyeGdfShare=2).
# Labels: "rc-<arm><suffix>". AutoRes is pinned off for measurement (VR_AutoRes=false, Mopic_ConfigVersion=4) unless
# -AutoRes. -Sampler: analysis\threadsample.py samples the game's threads from outside (cells\<label>-<time>\, abcpu.py
# input). -Runtime: game (default; the game's OpenXR client starts the service) / sbs / normal (a fresh service started
# by runtime.ps1 before every run, stopped after). Logs and side outputs go to -OutDir; run folders stay in the
# harness's runs\ folder. A recipe path containing {HARNESS} is resolved into -OutDir\recipes first.
# Each run: wait until no other harness/game runs and Steam's RunningAppID is 0, runtime, exitwatch.py, run-test.ps1
# (-Dll, -EngineDir), then checks (no service left, RunningAppID 0). Harness code unchanged.
#
#   powershell -ExecutionPolicy Bypass -File cost_run.ps1 -Harness <repo>\tools\mopic-test -Dll <build>\UEVRBackend.dll
#       -Game Wukong -Recipe Wukong-still -RecipeVars "still_s=300" -SteamOnline -Arms "abpm" -Sampler
param(
    [string]$Game = "Wukong",
    [string]$Recipe = "",
    [string]$Arms = "plain",
    [ValidateSet("game", "sbs", "normal")][string]$Runtime = "game",
    [switch]$SteamOnline,
    [switch]$EyeSampler,
    [int]$EyeFullMax = 60,            # eyesampler.py --full-max: whole-display (binocular) frames per run
    [switch]$DeferBinocular,          # hold the binocular frames out of the harness's post-run analysis (hold_frames.ps1); analyze after the block
    [double]$EyeHz = 0,               # eyesampler.py --hz (0 = its default, 120); 1 Hz is enough for the R/L luma check
    [double]$EyeFullEvery = 3,        # eyesampler.py --full-every: one whole-display frame every N s (the harness's binocular analysis grows faster than the frame count: 60 frames ~80 s, 219 frames ~24 min)
    [switch]$Sampler,
    [switch]$GpuSampler,              # gpu_proc_sampler.ps1 alongside: per-process GPU engine utilisation (cells\gpu-<label>-<time>.csv)
    [switch]$AutoRes,
    [string]$Set = "",
    [string]$LabelSuffix = "",
    [string]$SaveFile = "",
    [string]$RecipeVars = "",
    [string]$LatestStart = "",
    [string]$UserScript = "",         # run-test -UserScript: UEVR's user_script.txt for the run only (include the game's own lines)
    [Parameter(Mandatory)][string]$Dll,
    [string]$Harness = $env:MOPIC_TEST_DIR,   # the mopic-test harness folder (run-test.ps1, .venv, runs\)
    [string]$EngineDir = "$env:APPDATA\MOPIC\mopichub\engines\mopic-uevr\1.0.5.1",
    [string]$OutDir = "$env:LOCALAPPDATA\right-eye-cost"
)
$ErrorActionPreference = "Continue"
if (-not $Harness -or -not (Test-Path "$Harness\run-test.ps1")) { Write-Host "no harness at '$Harness' (-Harness or MOPIC_TEST_DIR)"; exit 4 }
$tools = $Harness
$py = "$tools\.venv\Scripts\python.exe"
$engine = $EngineDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir "campaign.log"
$watchDir = Join-Path $OutDir "exitwatch"
New-Item -ItemType Directory -Force $watchDir | Out-Null
$off = "VR_NativeStereoFixTextureArraySubmit=false;VR_NativeStereoFixAsyncOpenXRWait=false;VR_NativeOpenXRAsyncWaitOnly=false;VR_NativeStereoFixSamePass=true"
$autoResSet = $(if ($AutoRes) { "VR_AutoRes=true;VR_AutoResKeepSwapchains=true;Mopic_ConfigVersion=4" } else { "VR_AutoRes=false;VR_AutoResKeepSwapchains=true;Mopic_ConfigVersion=4" })
$procName = @{ Tekken8Demo = "Polaris-Win64-Shipping"; Stray = "Stray-Win64-Shipping"; Expedition33 = "SandFall-Win64-Shipping";
    Wukong = "b1-Win64-Shipping"; DeadAsDisco = "PagodaSteamDemo-Win64-Shipping"; Hozy = "CozyGame-Win64-Shipping"; Hogwarts = "HogwartsLegacy" }[$Game]
$gameExes = @("Polaris-Win64-Shipping", "SandFall-Win64-Shipping", "b1-Win64-Shipping", "PagodaSteamDemo-Win64-Shipping",
    "HogwartsLegacy", "Stray-Win64-Shipping", "CozyGame-Win64-Shipping", "AC2-Win64-Shipping", "acc",
    "SonicRacingCrossWorldsSteam", "SB-Win64-Shipping")
function Get-Cutoff([string]$hm) { $t = [datetime]::ParseExact($hm, "HH:mm", $null); if (((Get-Date) - $t).TotalHours -gt 12) { $t = $t.AddDays(1) }; return $t }
function Note($m) { $l = "$(Get-Date -Format s) $m"; Write-Host $l; Add-Content -Path $log -Value $l -Encoding utf8 }
function Get-ArmSet([string]$arm) {
    switch -Regex ($arm) {
        '^plain$' { return "" }
        '^ab$' { return "VR_ExperimentSecondFamilyCvars=10,secondary,skip" }
        '^abpm$' { return "VR_ExperimentSecondFamilyCvars=10,secondary,skip;VR_ExperimentPassMarkersSeconds=7" }
        '^pm$' { return "VR_ExperimentPassMarkersSeconds=7" }
        '^hold:(.+)$' { return "VR_ExperimentSecondFamilyCvars=$($Matches[1])" }
        '^flat$' { return "" }
        # UEVR loaded without a VR session (experiment-no-runtime.txt in the game's UEVR folder for the run): the game
        # renders flat in its window through the same frame source (perf log, pass markers); use a window-source recipe
        '^nort$' { return "VR_ExperimentPassMarkersSeconds=7" }
        default { return $null }
    }
}
function Get-ArmLabel([string]$arm) {
    if ($arm -match '^hold:') { return "rc-hold" + [Math]::Abs($arm.GetHashCode() % 1000) + $LabelSuffix }
    return "rc-$arm$LabelSuffix"
}

function Get-Busy {
    $me = $PID
    $why = @()
    foreach ($p in (Get-CimInstance Win32_Process)) {
        if ($p.ProcessId -eq $me -or -not $p.CommandLine) { continue }
        if ($p.Name -match '^(powershell|pwsh)\.exe$' -and $p.CommandLine -match 'run-test\.ps1|run-matrix\.ps1|run-ladder\.ps1|runexp\.ps1|svc3\.ps1') {
            $why += "harness pid $($p.ProcessId): $($p.CommandLine.Substring(0, [Math]::Min(140, $p.CommandLine.Length)))"
        }
        if ($p.Name -match '^python' -and $p.CommandLine -match 'gamepilot\.py') { $why += "gamepilot pid $($p.ProcessId)" }
    }
    foreach ($n in $gameExes) { foreach ($g in @(Get-Process $n -ErrorAction SilentlyContinue)) { $why += "game $n pid $($g.Id)" } }
    if (Test-Path "$env:TEMP\mopic-runexp.lock") { $why += "runexp lock $env:TEMP\mopic-runexp.lock" }
    return $why
}
function Wait-Idle([string]$what) {
    for ($i = 0; $i -lt 40; $i++) {
        $busy = @(Get-Busy)
        if ($busy.Count -eq 0) { return $true }
        if ($i -eq 0) { Note "busy before $what : $($busy -join ' | ')" }
        Start-Sleep 30
    }
    Note "still busy after 20 min before $what : $((Get-Busy) -join ' | '); stopping"
    return $false
}
function Get-RunningAppId { try { [int](Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).RunningAppID } catch { -1 } }
function Wait-SteamClear([string]$what) {
    for ($i = 0; $i -lt 24; $i++) {
        $id = Get-RunningAppId
        if ($id -eq 0) { return $true }
        if ($i -eq 0) { Note "RunningAppID $id before $what; waiting" }
        Start-Sleep 5
    }
    Note "RunningAppID still $(Get-RunningAppId) 2 min later, before $what; stopping"
    return $false
}
function Get-SvcText {
    $s = @(Get-CimInstance Win32_Process -Filter "Name='mopic-openxr-service.exe'")
    if ($s.Count -eq 0) { return "none" }
    return ($s | ForEach-Object { "pid $($_.ProcessId) parent $($_.ParentProcessId) created $($_.CreationDate.ToString('s'))" }) -join ", "
}
# Keys the harness recipes press: (name, virtual key, scan code, extended). Release-StuckKeys sends a key-up only for
# those that read down (GetAsyncKeyState) and returns their names.
$script:PilotKeys = @(
    @("LCTRL", 0xA2, 0x1D, $false), @("RCTRL", 0xA3, 0x1D, $true), @("LSHIFT", 0xA0, 0x2A, $false), @("RSHIFT", 0xA1, 0x36, $false),
    @("LALT", 0xA4, 0x38, $false), @("SPACE", 0x20, 0x39, $false), @("W", 0x57, 0x11, $false), @("A", 0x41, 0x1E, $false),
    @("S", 0x53, 0x1F, $false), @("D", 0x44, 0x20, $false), @("E", 0x45, 0x12, $false), @("V", 0x56, 0x2F, $false),
    @("1", 0x31, 0x02, $false), @("2", 0x32, 0x03, $false), @("ESC", 0x1B, 0x01, $false), @("ENTER", 0x0D, 0x1C, $false))
Add-Type -Namespace RcKeys2 -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
[DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, System.UIntPtr dwExtraInfo);
[DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, System.UIntPtr dwExtraInfo);
'@ -ErrorAction SilentlyContinue
function Release-StuckKeys {
    $names = @()
    foreach ($k in $script:PilotKeys) {
        if (([RcKeys2.U]::GetAsyncKeyState($k[1]) -band 0x8000) -ne 0) {
            $flags = 0x2 -bor 0x8 -bor $(if ($k[3]) { 0x1 } else { 0 })   # KEYUP | SCANCODE | EXTENDED
            [RcKeys2.U]::keybd_event([byte]$k[1], [byte]$k[2], [uint32]$flags, [System.UIntPtr]::Zero)
            $names += $k[0]
        }
    }
    foreach ($b in @(@("LMB", 0x01, 0x0004), @("RMB", 0x02, 0x0010))) {
        if (([RcKeys2.U]::GetAsyncKeyState($b[1]) -band 0x8000) -ne 0) { [RcKeys2.U]::mouse_event([uint32]$b[2], 0, 0, 0, [System.UIntPtr]::Zero); $names += $b[0] }
    }
    return ($names -join ", ")
}

# Start-Process + WaitForExit, never a captured "& powershell" (the service the child starts inherits the pipe)
function Invoke-Runtime([string]$mode) {
    $argv = @("-ExecutionPolicy", "Bypass", "-File", "`"$(Join-Path $PSScriptRoot "runtime.ps1")`"", "-Mode", $mode, "-OutDir", "`"$OutDir`"")
    $p = Start-Process -FilePath "powershell" -ArgumentList $argv -NoNewWindow -PassThru
    $null = $p.Handle
    $p.WaitForExit()
    return $p.ExitCode
}

$armList = @($Arms -split ";" | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
if ($armList.Count -eq 1 -and $Arms -notmatch '^hold:') { $armList = @($Arms -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }) }
foreach ($k in $armList) { if ($null -eq (Get-ArmSet $k)) { Note "unknown arm '$k'; stopping"; exit 4 } }
if (-not (Test-Path $Dll)) { Note "no build at $Dll; stopping"; exit 4 }
if (-not $procName) { Note "no process name for $Game; stopping"; exit 4 }
if (Get-Process LogonUI -ErrorAction SilentlyContinue) { Note "the PC is locked; not starting"; exit 4 }
# A modifier left logically down (a pilot stopped mid key-press, 2026-11-21 18:54) turns the pilot's Esc into Ctrl+Esc
# (Start/Search takes the foreground): read-only check, no input sent.
Add-Type -Namespace RcKeys -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);' -ErrorAction SilentlyContinue
$stuck = @(@{ n = "LCTRL"; v = 0xA2 }, @{ n = "RCTRL"; v = 0xA3 }, @{ n = "LSHIFT"; v = 0xA0 }, @{ n = "RSHIFT"; v = 0xA1 }, @{ n = "LALT"; v = 0xA4 }, @{ n = "LWIN"; v = 0x5B } |
    Where-Object { ([RcKeys.U]::GetAsyncKeyState($_.v) -band 0x8000) -ne 0 } | ForEach-Object { $_.n })
if ($stuck.Count -gt 0) { Note "modifier key(s) held down: $($stuck -join ', '); not starting (the pilot's keys would combine with them)"; exit 4 }
# The Start / Search panel in front refuses other processes' SetForegroundWindow (19:2x incident): not starting then
Add-Type -Namespace RcFg -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow(); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint pid);' -ErrorAction SilentlyContinue
function Get-ForegroundProcess { $fgPid = [uint32]0; [void][RcFg.U]::GetWindowThreadProcessId([RcFg.U]::GetForegroundWindow(), [ref]$fgPid); $p = Get-Process -Id $fgPid -ErrorAction SilentlyContinue; if ($p) { "$($p.ProcessName) pid $fgPid" } else { "pid $fgPid" } }
$fgNow = Get-ForegroundProcess
if ($fgNow -match '^(SearchHost|StartMenuExperienceHost|SearchApp|ShellExperienceHost) ') { Note "foreground is $fgNow (Start/Search panel); not starting"; exit 4 }
Set-Location $tools
. "$tools\steam-offline.ps1"
$switched = $false
if (-not (Wait-Idle "the Steam switch")) { exit 2 }
if (-not $SteamOnline) {
    $switched = Enter-SteamMode $true
    $env:MOPIC_STEAM_OFFLINE_HELD = "1"
} else {
    Remove-Item Env:MOPIC_STEAM_OFFLINE_HELD -ErrorAction SilentlyContinue
    [void](Enter-SteamMode $false)
}
$rec = $(if ($Recipe -ne "") { $Recipe } else { $Game })
if ((Test-Path $rec) -and (Select-String -Path $rec -SimpleMatch "{HARNESS}" -Quiet)) {
    $resolved = Join-Path $OutDir ("recipes\" + (Split-Path $rec -Leaf))
    New-Item -ItemType Directory -Force (Split-Path $resolved) | Out-Null
    $txt = [IO.File]::ReadAllText($rec, [Text.UTF8Encoding]::new($false)).Replace("{HARNESS}", ($tools -replace "\\", "\\"))
    [IO.File]::WriteAllText($resolved, $txt, [Text.UTF8Encoding]::new($false))
    $rec = $resolved
}
$rc = 0
try {
    if ($Runtime -eq "game") { Note "runtime: nothing started here; service before the batch: $(Get-SvcText)" }
    foreach ($k in $armList) {
        if ($LatestStart -ne "" -and (Get-Date) -gt (Get-Cutoff $LatestStart)) { Note "past $LatestStart; the rest of '$Arms' skipped"; break }
        $label = Get-ArmLabel $k
        $what = "$Game $rec $label ($k)"
        if (-not (Wait-Idle $what)) { $rc = 2; break }
        if (-not (Wait-SteamClear $what)) { $rc = 6; break }
        if ($Runtime -ne "game") {
            $e = Invoke-Runtime $Runtime
            if ($e -ne 0) { Note "runtime start failed (exit $e) before $what; stopping"; $rc = 3; break }
        }
        if (Get-Process LogonUI -ErrorAction SilentlyContinue) { Note "the PC is locked before $what; stopping"; $rc = 4; break }
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $watchOut = Join-Path $watchDir ("{0}-{1}-{2}.json" -f $stamp, $Game, $label)
        $watch = Start-Process -FilePath $py -ArgumentList @("-I", (Join-Path $PSScriptRoot "exitwatch.py"), $procName, "`"$watchOut`"") -WindowStyle Hidden -PassThru
        $cell = $null
        $samplerProc = $null
        if ($Sampler) {
            $cell = Join-Path $OutDir ("cells\{0}-{1}" -f $label, $stamp)
            New-Item -ItemType Directory -Force $cell | Out-Null
            $samplerProc = Start-Process -FilePath $py -ArgumentList @("-I", "`"$(Join-Path $PSScriptRoot 'analysis\threadsample.py')`"", "--exe", "$procName.exe",
                "--out", "`"$cell`"", "--wait", "900") -RedirectStandardOutput "$cell\sampler.out.txt" -RedirectStandardError "$cell\sampler.err.txt" -WindowStyle Hidden -PassThru
        }
        $gpuOut = $null
        if ($GpuSampler) {
            $gpuOut = Join-Path $OutDir ("cells\gpu-{0}-{1}" -f $label, $stamp)
            New-Item -ItemType Directory -Force (Split-Path $gpuOut) | Out-Null
            Start-Process -FilePath "powershell" -ArgumentList @("-ExecutionPolicy", "Bypass", "-File", "`"$(Join-Path $PSScriptRoot 'gpu_proc_sampler.ps1')`"", "-Out", "`"$gpuOut`"", "-Seconds", "1200") -WindowStyle Hidden | Out-Null
        }
        Note "run $what (runtime $Runtime, service before: $(Get-SvcText); exit watch $watchOut$(if ($cell) { "; sampler $cell" })$(if ($gpuOut) { "; gpu sampler $gpuOut.csv" }))"
        $parts = @($off, $autoResSet, (Get-ArmSet $k)) + $(if ($Set -ne "") { @($Set) } else { @() })
        $setAll = (@($parts | Where-Object { $_ -ne "" })) -join ";"
        $a = @("-ExecutionPolicy", "Bypass", "-File", "$tools\run-test.ps1", "-Game", $Game, "-Runs", "1",
            "-EngineDir", $engine, "-Label", $label, "-Recipe", $rec)
        if ($k -eq "flat") { $a += "-NoInject" } else { $a += @("-Dll", $Dll, "-Set", $setAll) }
        if ($EyeSampler) { $a += @("-EyeSampler", "-EyeSamplerArgs", ("--full-every $EyeFullEvery --full-max $EyeFullMax" + $(if ($EyeHz -gt 0) { " --hz $EyeHz" } else { "" }))) }
        if ($SteamOnline) { $a += "-SteamOnline" }
        if ($SaveFile -ne "") { $a += @("-SaveFile", $SaveFile) }
        if ($RecipeVars -ne "") { $a += @("-RecipeVars", $RecipeVars) }
        if ($UserScript -ne "" -and $k -ne "flat") { $a += @("-UserScript", $UserScript) }
        $runOut = Join-Path $OutDir ("harness-out\{0}-{1}-{2}.txt" -f $stamp, $Game, $label)
        New-Item -ItemType Directory -Force (Split-Path $runOut) | Out-Null
        $noRuntimeFile = "$env:APPDATA\UnrealVRMod\$procName\experiment-no-runtime.txt"
        if ($k -eq "nort") { Set-Content -Path $noRuntimeFile -Value "right-eye-cost flat reference $stamp" -Encoding ascii; Note "  $noRuntimeFile created for this run" }
        $line = "& powershell " + (($a | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" }) -join " ") + " *>&1 | Out-File -Encoding utf8 -FilePath '$runOut'"
        $holdStop = $null
        if ($DeferBinocular -and $EyeSampler) {
            $holdStop = Join-Path $OutDir ("hold-{0}-{1}.stop" -f $stamp, $label)
            Remove-Item $holdStop, "$holdStop.done" -ErrorAction SilentlyContinue
            Start-Process -FilePath "powershell" -ArgumentList @("-ExecutionPolicy", "Bypass", "-File", "`"$(Join-Path $PSScriptRoot 'hold_frames.ps1')`"",
                "-RunsDir", "`"$tools\runs`"", "-Match", "`"*-$Game-$label-r1`"", "-HoldDir", "`"$(Join-Path $OutDir 'binocular-held')`"",
                "-StopFile", "`"$holdStop`"", "-After", (Get-Date).AddSeconds(-5).ToString("s")) -WindowStyle Hidden | Out-Null
        }
        try { Invoke-Expression $line } finally {
            if (Test-Path $noRuntimeFile) { Remove-Item $noRuntimeFile -Force; Note "  $noRuntimeFile removed" }
            # A pilot stopped mid key-press leaves that key logically down (18:54 incident: left Ctrl). Release the keys
            # the recipes press if they read down after the run; nothing is sent when all are up.
            $released = Release-StuckKeys
            if ($released) { Note "  released keys left down after the run: $released" }
        }
        if (-not $watch.WaitForExit(20000)) { Note "exit watch still running after the run, stopped it"; try { $watch.Kill() } catch { } }
        if ($samplerProc -and -not $samplerProc.WaitForExit(30000)) { Note "sampler still running after the run, stopped it"; try { $samplerProc.Kill() } catch { } }
        if ($gpuOut) { Set-Content -Path "$gpuOut.stop" -Value "stop" -Encoding ascii }
        $last = Get-ChildItem "$tools\runs" -Directory | Where-Object { $_.Name -like "*-$Game-$label-r1" } | Sort-Object Name | Select-Object -Last 1
        if ($holdStop) {
            Set-Content -Path $holdStop -Value "stop" -Encoding ascii
            for ($w = 0; $w -lt 30 -and -not (Test-Path "$holdStop.done"); $w++) { Start-Sleep 2 }
            $held = $(if ($last) { Join-Path (Join-Path $OutDir "binocular-held") $last.Name } else { $null })
            if ($held -and (Test-Path $held)) {
                $back = @(Get-ChildItem $held -Filter *.png)
                New-Item -ItemType Directory -Force (Join-Path $last.FullName "binocular") | Out-Null
                $back | Move-Item -Destination (Join-Path $last.FullName "binocular") -Force
                Note "  binocular frames held out of the harness analysis and put back: $($back.Count) (analyze after the block: analysis\binocular.py analyze <run>)"
            } else { Note "  binocular hold: nothing held ($(if (Test-Path "$holdStop.done") { Get-Content "$holdStop.done" } else { 'mover did not finish' }))" }
        }
        if ($cell -and $last) { Set-Content -Path "$cell\rundir.txt" -Value $last.FullName -Encoding ascii }
        if ($last -and (Test-Path "$($last.FullName)\summary.txt")) {
            Note ("  -> " + $last.Name + "  " + (Get-Content "$($last.FullName)\summary.txt" -TotalCount 1))
            if (Select-String -Path "$($last.FullName)\summary.txt" -SimpleMatch "could not bring the game window to the foreground" -Quiet) {
                $downNow = @(1..254 | Where-Object { ([RcKeys2.U]::GetAsyncKeyState($_) -band 0x8000) -ne 0 } | ForEach-Object { "0x{0:X2}" -f $_ })
                Note ("  FOCUS FAILURE at $(Get-Date -Format HH:mm:ss): foreground $(Get-ForegroundProcess); keys down: " + $(if ($downNow.Count) { $downNow -join ' ' } else { 'none' }))
            }
        }
        else { Note "  -> no summary (harness output $runOut)" }
        # pass-marker captures: from the game's UEVR folder into the run folder
        $pmDir = "$env:APPDATA\UnrealVRMod\$procName\passmarkers"
        if (Test-Path $pmDir) {
            $files = @(Get-ChildItem $pmDir -File)
            if ($last -and $files.Count) {
                $dest = Join-Path $last.FullName "passmarkers"
                New-Item -ItemType Directory -Force $dest | Out-Null
                $files | Move-Item -Destination $dest -Force
                Note "  pass-marker files: $($files.Count) -> $dest"
            } elseif ($files.Count) {
                $dest = Join-Path $OutDir ("passmarkers-orphan\{0}-{1}" -f $stamp, $label)
                New-Item -ItemType Directory -Force $dest | Out-Null
                $files | Move-Item -Destination $dest -Force
                Note "  pass-marker files without a run folder -> $dest"
            }
        }
        # cvar dumps: from the game's UEVR folder into the run folder
        $cdDir = "$env:APPDATA\UnrealVRMod\$procName\cvardump"
        if ($last -and (Test-Path $cdDir)) {
            $dumps = @(Get-ChildItem $cdDir -File -Filter "*.tsv")
            if ($dumps.Count) {
                $dest = Join-Path $last.FullName "cvardump"
                New-Item -ItemType Directory -Force $dest | Out-Null
                $dumps | Move-Item -Destination $dest -Force
                Note "  cvar dumps: $($dumps.Count) -> $dest"
            }
        }
        Note "  service after the game: $(Get-SvcText); RunningAppID $(Get-RunningAppId)"
        if ($Runtime -ne "game") {
            $e = Invoke-Runtime "stop"
            if ($e -ne 0) { Note "a service I did not start is left after $what; stopping (report it, don't end it)"; $rc = 5; break }
        } else {
            for ($i = 0; $i -lt 12 -and (Get-RunningAppId) -ne 0; $i++) { Start-Sleep 5 }
            Note "  game runtime, 60 s after the run at most: service $(Get-SvcText); RunningAppID $(Get-RunningAppId)"
        }
        if (-not (Wait-SteamClear "the next step after $what")) { $rc = 6; break }
        Note "after $what : service $(Get-SvcText), RunningAppID 0"
    }
} finally {
    Remove-Item Env:MOPIC_STEAM_OFFLINE_HELD -ErrorAction SilentlyContinue
    if ($switched) { try { [void](Enter-SteamMode $false) } catch { Note "Steam: could not go back online: $($_.Exception.Message)" } }
    Note "batch done: $Game $Arms $LabelSuffix (exit $rc)"
}
exit $rc
