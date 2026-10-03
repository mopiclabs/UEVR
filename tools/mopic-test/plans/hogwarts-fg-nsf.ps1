# Hogwarts Legacy A/B at one spot: Intel XeSS frame generation (XeFG) on / off x Native Stereo Fix on / off, each run
# with UEVR's info log (FrameworkConfig_LogLevel=2) and the per-eye sampler (run-test.ps1 -EyeSampler), then one table
# (runs\ab-<time>-Hogwarts-fg-nsf.md + .json: verdict, frame rate, frame generation, one-eye events per arm).
#
#   powershell -ExecutionPolicy Bypass -File tools\mopic-test\plans\hogwarts-fg-nsf.ps1 [-Rounds 2] [-Dll <build>\UEVRBackend.dll]
#   ... -DryRun                                  # the harness commands, nothing runs
#   ... -Only fg-on-nsf-on,fg-off-nsf-on         # some arms
#
# Arms (one harness run each per round; round r runs them rotated by r - ABCD, BCDA, ... - so the drift of the spot
# and the GPU's warm-up spread over all arms):
#   fg-on-nsf-on    the game as the user set it (XeFG 2x by the NVIDIA app), Native Stereo Fix on (UEVR's default)
#   fg-off-nsf-on   GameUserSettings.ini FrameGeneration=(Mode=Off,NumFramesInterpolated=0,LocStr="Off") for the run
#   fg-on-nsf-off   -Set VR_NativeStereoFix=false
#   fg-off-nsf-off  both
#
# The Off value is the game's own: HogwartsLegacy.exe builds its frame generation list from EFrameGenerationMode {Off,
# Nvidia_DLSSG, Intel_XeFG, AMD_FFXFI}, the Off entry being Mode=Off, NumFramesInterpolated=0, LocStr "Off" (the
# static at RVA 0x334959c; XeFG 2x is Mode=Intel_XeFG, 1, "INTEL_XEFG_MODE_X2", RVA 0x334967c). Choosing Off in the
# menu leaves r.ChosenFrameGenProvider as it was (the game writes it only for the three on modes, RVA 0x2e2b9d8-
# 0x2e2bb7b), and the game picks its DXGI swapchain provider from that key at startup (RVA 0x25affa0): an fg-off arm is
# what a player who turns XeFG off gets, with XeFG's swapchain proxy still in the present path. -NoProxy also sets
# r.ChosenFrameGenProvider=None (no provider matches it) in the fg-off arms: no proxy at all (untested).
#
# The harness puts GameUserSettings.ini back byte for byte after each run (also after a crash; a killed harness is
# undone by the next start), so the fg-on arms always see the user's own setting; this script stops when that is not
# Intel_XeFG. Steam Cloud syncs that folder (steam_autocloud.vdf), so every run's table row also has the setting as
# the game left it at its quit ("FG setting at quit", from <run>\game-settings\GameUserSettings.after.ini) and whether
# it is the arm's ("MISMATCH": that run didn't test its arm), and UEVR's Present hook passes per VR frame: about 2 while
# XeFG generates frames (UEVR hooks the swapchain XeFG presents through), about 1 when it doesn't. The game saves where
# it quits, so each run starts where the previous one quit (Hogwarts-eyes walks a little); -SaveFile with the save
# ladder's route (-Recipe Hogwarts-save, after the ladder's prep) starts every run from the same save.
#
# Ask the user first: monado-service has to run with MOPIC_MODE=sbs (the sampler reads the side-by-side picture, the
# pilot the left eye), which changes what the Mopic display shows, and the fg-off arms change the game's graphics
# setting for their runs. Check the recipe's checkpoints on the side-by-side title screen once before the first run:
#   .venv\Scripts\python gamepilot.py HogwartsLegacy checkpoint test recipes\Hogwarts-eyes.json all --source mopic-sbs
# (the plan stops after a MENU_FAIL run that never got to a measured segment: the pilot couldn't read the screens).
# Keep the PC unlocked and unused meanwhile (about 6 minutes per run).

param(
    [int]$Rounds = 1,
    [string[]]$Only = @(),
    [string]$EngineDir = (Join-Path $env:APPDATA "MOPIC\mopichub\engines\mopic-uevr\1.0.5.1"),
    [string]$Dll = "",                 # deployed once, before the first run (run-test.ps1 -Dll)
    [string]$Recipe = "Hogwarts-eyes",
    [string]$RecipeVars = "",
    [string]$SaveFile = "",            # every run from this save (needs a recipe with a save_slot)
    [string]$Label = "ab",
    [switch]$NoEyeSampler,
    [switch]$NoProxy,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$Tools = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Harness = Join-Path $Tools "run-test.ps1"
$RunsDir = Join-Path $Tools "runs"
$Section = "/Script/Phoenix.PhoenixGameSettings"
$FgOff = 'FrameGeneration=(Mode=Off,NumFramesInterpolated=0,LocStr="Off")'
$UserSettings = Join-Path $env:LOCALAPPDATA "Hogwarts Legacy\Saved\Config\WindowsNoEditor\GameUserSettings.ini"

$fgOffIni = "GameUserSettings.ini|$Section|$FgOff" + $(if ($NoProxy) { ";r.ChosenFrameGenProvider=None" } else { "" })
$arms = [ordered]@{
    "fg-on-nsf-on"   = @{ ini = ""; set = "" }
    "fg-off-nsf-on"  = @{ ini = $fgOffIni; set = "" }
    "fg-on-nsf-off"  = @{ ini = ""; set = "VR_NativeStereoFix=false" }
    "fg-off-nsf-off" = @{ ini = $fgOffIni; set = "VR_NativeStereoFix=false" }
}
# "-Only A,B" through powershell -File arrives as one string
$Only = @($Only | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
foreach ($n in $Only) { if (-not $arms.Contains($n)) { throw "-Only: no arm '$n' (arms: $(@($arms.Keys) -join ', '))" } }
$names = @($arms.Keys | Where-Object { $Only.Count -eq 0 -or $Only -contains $_ })
if ($Rounds -lt 1) { throw "-Rounds must be 1 or more" }

function Format-Num($v, [string]$fmt = "0.0") {
    if ($null -eq $v) { return "" }
    return ([double]$v).ToString($fmt, [System.Globalization.CultureInfo]::InvariantCulture)
}

# One command-line argument as CommandLineToArgvW (and powershell.exe -File) reads it back (run-ladder.ps1 has the
# reasons): the fg-off value's quotes have to reach run-test.ps1 intact
function ConvertTo-ArgvString([string]$s) {
    if ($s -ne "" -and $s -notmatch '[\s";]') { return $s }
    $s = [regex]::Replace($s, '(\\*)"', { param($m) ($m.Groups[1].Value * 2) + '\"' })
    $s = [regex]::Replace($s, '(\\+)$', '$1$1')
    return '"' + $s + '"'
}

# the game's frame generation setting now (the fg-on arms run with it)
if (-not (Test-Path -LiteralPath $UserSettings)) { throw "Hogwarts Legacy's GameUserSettings.ini not found: $UserSettings" }
$fgNow = @(Select-String -LiteralPath $UserSettings -Pattern '^FrameGeneration=(.*)$' | ForEach-Object { $_.Matches[0].Groups[1].Value }) | Select-Object -First 1
Write-Host "Frame generation as set: $(if ($fgNow) { $fgNow } else { '(no FrameGeneration line)' })"
if (@($names | Where-Object { $_ -like "fg-on-*" }).Count -gt 0 -and "$fgNow" -notlike "(Mode=Intel_XeFG*") {
    throw "the fg-on arms expect the user's XeFG setting, but GameUserSettings.ini says FrameGeneration=$fgNow (run only the fg-off arms with -Only, or set XeFG in the game)"
}
if (-not (Test-Path -LiteralPath (Join-Path $EngineDir "UEVRInjector.exe"))) { throw "no UEVRInjector.exe in $EngineDir" }
if (-not $DryRun) {
    if (-not (Get-Process -Name "monado-service" -ErrorAction SilentlyContinue)) { throw "monado-service is not running (start it with MOPIC_MODE=sbs for the eye sampler)" }
    if (Get-Process -Name "HogwartsLegacy" -ErrorAction SilentlyContinue) { throw "Hogwarts Legacy is running: close it first (the harness would kill it)" }
    if (Test-Path -LiteralPath (Join-Path $RunsDir "game-ini-pending")) { Write-Warning "runs\game-ini-pending exists: an interrupted harness run changed game settings; the first harness start puts them back" }
}

$plan = @()
for ($r = 0; $r -lt $Rounds; $r++) {
    for ($i = 0; $i -lt $names.Count; $i++) { $plan += [pscustomobject]@{ round = $r + 1; arm = $names[($i + $r) % $names.Count] } }
}
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$mdPath = Join-Path $RunsDir "ab-$stamp-Hogwarts-fg-nsf.md"
$logPath = [System.IO.Path]::ChangeExtension($mdPath, ".log")
Write-Host "Plan:     $($plan.Count) runs ($($names -join ', ') x $Rounds round(s)), recipe $Recipe, engine $EngineDir$(if ($NoEyeSampler) { ', no eye sampler' })"

# Runs the harness, shows its verdict lines, appends all it printed to the .log -> its exit code
function Invoke-Harness([string[]]$argv) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = (@($argv | ForEach-Object { ConvertTo-ArgvString $_ }) -join " ")
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Console]::OutputEncoding
    $psi.StandardErrorEncoding = [Console]::OutputEncoding
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $logPath) | Out-Null
    $log = New-Object System.IO.StreamWriter($logPath, $true, (New-Object System.Text.UTF8Encoding($false)))
    try {
        $log.WriteLine("> powershell.exe " + $psi.Arguments)
        $log.Flush()
        $proc = [System.Diagnostics.Process]::Start($psi)
        $errTask = $proc.StandardError.ReadToEndAsync()
        while ($null -ne ($line = $proc.StandardOutput.ReadLine())) {
            $log.WriteLine($line)
            $log.Flush()
            if ($line -match "^(PASS|CRASH|FREEZE|EXIT_|MENU_|NO_|LAUNCH|HARNESS)|^pilot: |^game ini: |^perf|^eyes|^  - ") { Write-Host "  $line" }
        }
        $proc.WaitForExit()
        $err = $errTask.Result
        if ($err.Trim() -ne "") { $log.WriteLine("[stderr]"); $log.WriteLine($err.TrimEnd()); Write-Host "  $(($err -split "\r?\n" | Select-Object -First 1))" -ForegroundColor Red }
        $log.WriteLine("< exit code $($proc.ExitCode)")
        return $proc.ExitCode
    } finally {
        $log.Dispose()
    }
}

function Get-Median($values) {
    $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
    if ($v.Count -eq 0) { return $null }
    if ($v.Count % 2 -eq 1) { return $v[($v.Count - 1) / 2] }
    return ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2
}

function Write-Table($rows) {
    $md = @("# Hogwarts Legacy A/B ${stamp}: frame generation x Native Stereo Fix", "",
        "Recipe $Recipe, engine ``$EngineDir``$(if ($NoProxy) { ', fg-off arms without the swapchain proxy (-NoProxy)' }). Frame generation as set: ``$fgNow``.", "",
        "| Round | Arm | Verdict | VR fps | 1% low | Hitches | Display new fps | Presents/VR frame | R % | FG (perf) | FG setting at quit | NSF active | Eyes Hz | One-eye lag (max ms) | 1-refresh lags | Unmatched | One-eye only | One-eye black | L/R corr | Run folder |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    foreach ($r in $rows) {
        $md += "| $($r.round) | $($r.arm) | $($r.verdict) | $(Format-Num $r.vr_fps) | $(Format-Num $r.low1_fps) | $($r.hitches) | $(Format-Num $r.display_new_fps) | $(Format-Num $r.presents_per_frame '0.00') | $(Format-Num $r.r_pct) | $($r.fg) | $($r.fg_setting_after)$(if ($r.fg_check -eq 'MISMATCH') { ' **MISMATCH**' }) | $($r.nsf_active) | $(Format-Num $r.eyes_hz) | $($r.lag)$(if ($null -ne $r.lag_max_ms) { " ($(Format-Num $r.lag_max_ms '0'))" }) | $($r.lag_brief) | $($r.lag_unmatched) | $($r.only) | $($r.black) | $(Format-Num $r.lr_corr '0.00') | $($r.dir) |"
    }
    $md += ""
    $md += "Per arm (events per measured minute of eye samples):"
    foreach ($g in @($rows | Group-Object { $_.arm })) {
        $med = Get-Median @($g.Group | ForEach-Object { $_.vr_fps })
        $min = ($g.Group | ForEach-Object { if ($_.eyes_s) { [double]$_.eyes_s } else { 0 } } | Measure-Object -Sum).Sum / 60
        $per = { param($k) if ($min -gt 0) { Format-Num ((($g.Group | ForEach-Object { if ($_.$k) { [double]$_.$k } else { 0 } } | Measure-Object -Sum).Sum) / $min) "0.00" } else { "" } }
        $md += "- $($g.Name): $(@($g.Group | ForEach-Object { $_.verdict }) -join ', '); VR fps median $(Format-Num $med); presents/VR frame median $(Format-Num (Get-Median @($g.Group | ForEach-Object { $_.presents_per_frame })) '0.00'); one-eye lag $(& $per 'lag')/min, 1-refresh $(& $per 'lag_brief')/min, only $(& $per 'only')/min, black $(& $per 'black')/min over $(Format-Num $min '0.0') min"
    }
    # did the fg-off arms run without frame generation? The setting the game quit with, and the Present passes
    $bad = @($rows | Where-Object { $_.fg_check -eq "MISMATCH" })
    if ($bad.Count -gt 0) { $md += ""; $md += "WARN: $($bad.Count) run(s) quit with another frame generation setting than their arm's: $(@($bad | ForEach-Object { "$($_.arm) round $($_.round) ($($_.fg_setting_after))" }) -join '; ')" }
    $ppfOn = Get-Median @($rows | Where-Object { $_.arm -like "fg-on-*" } | ForEach-Object { $_.presents_per_frame })
    $ppfOff = Get-Median @($rows | Where-Object { $_.arm -like "fg-off-*" } | ForEach-Object { $_.presents_per_frame })
    if ($null -ne $ppfOn -and $null -ne $ppfOff) {
        $md += ""
        $md += "Present passes per VR frame: fg-on $(Format-Num $ppfOn '0.00'), fg-off $(Format-Num $ppfOff '0.00')$(if ([math]::Abs($ppfOn - $ppfOff) -lt 0.3) { ' (WARN: about the same: either XeFG generated no frames in the fg-on arms, or UEVR does not see its generated Presents; frame generation not confirmed off/on from the frame data)' })"
    }
    [System.IO.File]::WriteAllLines($mdPath, [string[]]$md, (New-Object System.Text.UTF8Encoding($false)))
    ConvertTo-Json -InputObject @($rows) -Depth 4 | Set-Content -Path ([System.IO.Path]::ChangeExtension($mdPath, ".json")) -Encoding UTF8
    return $md
}

$rows = @()
$dllArg = $Dll
$n = 0
foreach ($p in $plan) {
    $n++
    $arm = $arms[$p.arm]
    $runLabel = "$Label-$($p.arm)"
    $set = "FrameworkConfig_LogLevel=2" + $(if ($arm.set) { ";" + $arm.set } else { "" })
    $harnessArgs = @("-ExecutionPolicy", "Bypass", "-File", $Harness, "-Game", "Hogwarts", "-Recipe", $Recipe, "-EngineDir", $EngineDir,
        "-Label", $runLabel, "-Set", $set)
    if ($arm.ini) { $harnessArgs += @("-GameIni", $arm.ini) }
    if (-not $NoEyeSampler) { $harnessArgs += "-EyeSampler" }
    if ($RecipeVars -ne "") { $harnessArgs += @("-RecipeVars", $RecipeVars) }
    if ($SaveFile -ne "") { $harnessArgs += @("-SaveFile", $SaveFile) }
    if ($dllArg -ne "") { $harnessArgs += @("-Dll", $dllArg); $dllArg = "" }   # deploy once
    Write-Host "=== [$n/$($plan.Count)] round $($p.round): $($p.arm)" -ForegroundColor Cyan
    if ($DryRun) {
        Write-Host ("  powershell " + (@($harnessArgs | ForEach-Object { ConvertTo-ArgvString $_ }) -join " "))
        continue
    }
    $start = Get-Date
    $code = Invoke-Harness $harnessArgs
    $dirs = @(Get-ChildItem -LiteralPath $RunsDir -Directory | Where-Object { $_.Name -match ('-Hogwarts-' + [regex]::Escape($runLabel) + '-r\d+$') -and $_.CreationTime -ge $start.AddSeconds(-5) } | Sort-Object Name)
    $found = $false
    foreach ($d in $dirs) {
        $resultPath = Join-Path $d.FullName "result.json"
        if (-not (Test-Path -LiteralPath $resultPath)) { continue }
        $found = $true
        $r = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $hl = $(if ($r.perf -and $r.perf.headline -and $r.perf.headline.kind -eq "vr") { $r.perf.headline } else { $null })
        $logFile = Join-Path $d.FullName "log.txt"
        $e = $r.eyes
        # the frame generation setting the game quit with (it saves its settings when it quits): the file the
        # harness copied after the run, else (-GameIni) the value it read from the game's copy
        $afterIni = Join-Path $d.FullName "game-settings\GameUserSettings.after.ini"
        $fgAfter = $(if (Test-Path -LiteralPath $afterIni) { @(Select-String -LiteralPath $afterIni -Pattern '^FrameGeneration=(.*)$' | ForEach-Object { $_.Matches[0].Groups[1].Value }) | Select-Object -First 1 } else { $null })
        if ($null -eq $fgAfter -and $r.game_ini -and $r.game_ini.entries) { $fgAfter = @(@($r.game_ini.entries) | Where-Object { $_.key -eq "FrameGeneration" } | ForEach-Object { $_.after }) | Select-Object -First 1 }
        $fgWant = $(if ($p.arm -like "fg-on-*") { "(Mode=Intel_XeFG*" } else { "(Mode=Off*" })
        $rows += [pscustomobject][ordered]@{
            round = $p.round; arm = $p.arm; verdict = $r.verdict; harness_exit = $code
            vr_fps = $(if ($hl) { $hl.fps } else { $null }); low1_fps = $(if ($hl) { $hl.low1_fps } else { $null }); hitches = $(if ($hl) { $hl.hitches } else { $null })
            display_new_fps = $(if ($r.perf -and $r.perf.display) { $r.perf.display.new_fps } else { $null })
            presents_per_frame = $(if ($r.perf -and $r.perf.attrib) { $r.perf.attrib.presents_per_vr_frame } else { $null })
            r_pct = $(if ($r.perf -and $r.perf.vr) { $r.perf.vr.r_pct } else { $null })
            fg = $(if ($r.perf -and $r.perf.framegen) { $r.perf.framegen.on } else { $null })
            fg_setting_after = $fgAfter
            fg_check = $(if ($null -eq $fgAfter) { "?" } elseif ("$fgAfter" -like $fgWant) { "ok" } else { "MISMATCH" })
            nsf_active = $(if (Test-Path -LiteralPath $logFile) { @(Select-String -LiteralPath $logFile -Pattern '\[NativeStereoFix\] state=active').Count } else { $null })
            eyes_hz = $(if ($e) { $e.rate_hz } else { $null }); eyes_s = $(if ($e) { $e.measured_s } else { $null })
            pilot_segments = $(if ($r.pilot -and $r.pilot.segments) { @($r.pilot.segments).Count } else { 0 })
            lag = $(if ($e) { $e.one_eye_lag.count } else { $null }); lag_max_ms = $(if ($e) { $e.one_eye_lag.max_ms } else { $null })
            lag_brief = $(if ($e) { $e.one_eye_lag.brief } else { $null }); lag_unmatched = $(if ($e) { $e.one_eye_lag.unmatched } else { $null })
            only = $(if ($e) { $e.one_eye_only.count } else { $null })
            black = $(if ($e) { $e.one_eye_black.count } else { $null }); lr_corr = $(if ($e) { $e.lr.corr_p50 } else { $null })
            game_ini = $(if ($r.game_ini -and $r.game_ini.entries) { (@($r.game_ini.entries) | ForEach-Object { "$($_.key)=$($_.value) after: $($_.after)" }) -join "; " } else { "" })
            notes = (@($r.notes) | Where-Object { $_ -notmatch "^WARN: PostInitProperties" }) -join "; "
            dir = $d.Name
        }
    }
    if (-not $found) {
        $rows += [pscustomobject][ordered]@{ round = $p.round; arm = $p.arm; verdict = "HARNESS_ERROR"; harness_exit = $code; vr_fps = $null; low1_fps = $null
            hitches = $null; display_new_fps = $null; presents_per_frame = $null; r_pct = $null; fg = $null; fg_setting_after = $null; fg_check = "?"
            nsf_active = $null; eyes_hz = $null; eyes_s = $null; pilot_segments = 0; lag = $null; lag_max_ms = $null
            lag_brief = $null; lag_unmatched = $null; only = $null; black = $null; lr_corr = $null; game_ini = ""; notes = "run-test.ps1 wrote no result.json (see $logPath)"; dir = "" }
    }
    $null = Write-Table $rows
    if (@($rows | Where-Object { $_.verdict -in @("HARNESS_ERROR", "NO_VR") }).Count -gt 0) { Write-Warning "stopping: a run ended HARNESS_ERROR or NO_VR (the rest would not be valid either)"; break }
    # the pilot never got to the play steps: under MOPIC_MODE=sbs that is its screens not matching (the checkpoints
    # were recorded on the woven display), and every other run would end the same way
    if (@($rows | Where-Object { $_.verdict -eq "MENU_FAIL" -and $_.pilot_segments -eq 0 }).Count -gt 0) {
        Write-Warning "stopping: a run ended MENU_FAIL before any measured segment (check the checkpoints on the side-by-side screen: gamepilot.py HogwartsLegacy checkpoint test recipes\$Recipe.json all --source mopic-sbs)"
        break
    }
}

if ($DryRun) { exit 0 }
$md = Write-Table $rows
Write-Host ""
$md | ForEach-Object { Write-Host $_ }
Write-Host "`nWritten: $mdPath (harness output: $logPath)"
if (@($rows | Where-Object { $_.verdict -ne "PASS" }).Count -gt 0) { exit 1 }
exit 0
