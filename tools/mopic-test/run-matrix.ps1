# Runs every recipe through run-test.ps1 and writes one table (runs\matrix-<time>.md + .json).
#
#   powershell -ExecutionPolicy Bypass -File tools\mopic-test\run-matrix.ps1 -Runs 3 -Dll build-jh\bin\uevr\UEVRBackend.dll
#   ... -Games Tekken8Demo,Stray -NoInject -Runs 1           # baselines without UEVR
#
# Each game's runs use the recipe of the same name (recipes\<Game>.json). Keep the PC unlocked and unused meanwhile.

param(
    [string[]]$Games = @("Tekken8Demo", "Expedition33", "SonicDemo", "Hozy", "Stray", "Hogwarts", "Wukong", "ACC", "DeadAsDisco"),
    [int]$Runs = 3,
    [string]$Dll = "",
    [string]$EngineDir = "",
    [switch]$NoInject,
    [string]$Label = "matrix"
)

$ErrorActionPreference = "Stop"
# "-Games A,B" through powershell -File arrives as one string
$Games = @($Games | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Harness = Join-Path $ScriptDir "run-test.ps1"
$RunsRoot = Join-Path $ScriptDir "runs"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$kind = $(if ($NoInject) { "vanilla" } else { "uevr" })
$runLabel = "$Label-$kind"

$rows = @()
$dllArg = $Dll
foreach ($game in $Games) {
    $harnessArgs = @("-ExecutionPolicy", "Bypass", "-File", $Harness, "-Game", $game, "-Recipe", $game, "-Runs", "$Runs", "-Label", $runLabel)
    if ($dllArg -ne "") { $harnessArgs += @("-Dll", $dllArg); $dllArg = "" }   # deploy once
    if ($NoInject) { $harnessArgs += "-NoInject" }
    if ($EngineDir -ne "") { $harnessArgs += @("-EngineDir", $EngineDir) }
    Write-Host "=== $game ($kind, $Runs run(s))" -ForegroundColor Cyan
    $start = Get-Date
    & powershell @harnessArgs | ForEach-Object { "$_" } | Where-Object { $_ -match "^(PASS|CRASH|FREEZE|EXIT_|MENU_|NO_|LAUNCH|HARNESS)|^pilot: |^  - " } | ForEach-Object { Write-Host "  $_" }
    $dirs = @(Get-ChildItem $RunsRoot -Directory | Where-Object { $_.Name -like "*-$game-$runLabel-r*" -and $_.CreationTime -ge $start.AddSeconds(-5) } | Sort-Object Name)
    foreach ($d in $dirs) {
        $resultPath = Join-Path $d.FullName "result.json"
        if (-not (Test-Path $resultPath)) { continue }
        $r = Get-Content $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $rows += [ordered]@{
            game = $game; run = $r.run; kind = $kind; verdict = $r.verdict; exit_code = $r.exit_code
            exit_after_s = $r.exit_after_s; inject_after_s = $r.inject_after_s
            reached = $(if ($r.pilot) { @($r.pilot.reached).Count } else { 0 })
            notes = (@($r.notes) | Where-Object { $_ -notmatch "^WARN: PostInitProperties" }) -join "; "
            dir = $d.Name
        }
    }
}

$md = @("# Recipe matrix $stamp ($kind)", "", "| Game | Run | Verdict | Exit code | Exit after (s) | Screens | Notes |", "| --- | --- | --- | --- | --- | --- | --- |")
foreach ($r in $rows) { $md += "| $($r.game) | $($r.run) | $($r.verdict) | $($r.exit_code) | $($r.exit_after_s) | $($r.reached) | $($r.notes) |" }
$md += ""
$md += "Totals: " + (($rows | Group-Object { $_.verdict } | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ", ")
$mdPath = Join-Path $RunsRoot "matrix-$stamp-$kind.md"
[System.IO.File]::WriteAllLines($mdPath, [string[]]$md, (New-Object System.Text.UTF8Encoding($false)))
$rows | ConvertTo-Json -Depth 4 | Set-Content -Path ([System.IO.Path]::ChangeExtension($mdPath, ".json")) -Encoding UTF8
Write-Host ""
$md | ForEach-Object { Write-Host $_ }
Write-Host "`nWritten: $mdPath"
if ($rows.Count -eq 0) { Write-Warning "no runs were recorded"; exit 1 }
if (@($rows | Where-Object { $_.verdict -ne "PASS" }).Count -gt 0) { exit 1 }
exit 0
