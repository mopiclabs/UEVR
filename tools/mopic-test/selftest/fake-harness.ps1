# Stand-in for run-test.ps1 in the run-ladder.ps1 self-test: writes a run folder like the harness does, no game.
# -RecipeVars may hold verdict=<VERDICT>, nsf=<count>, nores=1 (write no result.json and stop with an error, like
# run-test.ps1 refusing an argument: the message on stderr, exit 1).
param(
    [string]$Game = "",
    [string]$Recipe = "",
    [string]$SaveFile = "",
    [string]$RecipeVars = "",
    [int]$Runs = 1,
    [string]$EngineDir = "",
    [string]$Label = "test"
)
$ErrorActionPreference = "Stop"
$runsRoot = $env:MOPIC_SELFTEST_RUNS
$vars = @{}
foreach ($pair in ($RecipeVars -split ";")) { if ($pair -match '^([^=]+)=(.*)$') { $vars[$Matches[1]] = $Matches[2] } }
$verdict = $(if ($vars.ContainsKey("verdict")) { $vars["verdict"] } else { "PASS" })
$nsf = $(if ($vars.ContainsKey("nsf")) { [int]$vars["nsf"] } else { 2 })
# record exactly what the ladder passed, for the self-test to check
$received = [ordered]@{ Game = $Game; Recipe = $Recipe; SaveFile = $SaveFile; RecipeVars = $RecipeVars; Runs = $Runs; EngineDir = $EngineDir; Label = $Label; save_exists = (Test-Path -LiteralPath $SaveFile) }
if ($vars.ContainsKey("nores")) {
    Write-Host "fake harness: no result"
    throw "fake harness refused $Label (nores)"
}
for ($run = 1; $run -le $Runs; $run++) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $runDir = Join-Path $runsRoot "$stamp-$Game-$Label-r$run"
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $lines = @("[2026-10-02 00:00:00.000] [UnrealVR] [info] UnrealVR entry")
    for ($i = 0; $i -lt $nsf; $i++) { $lines += "[2026-10-02 00:00:01.000] [UnrealVR] [info] [FFakeStereoRenderingHook.cpp:26313] [NativeStereoFix] state=active (validated right-eye resource submitted)" }
    $lines += "[2026-10-02 00:00:02.000] [UnrealVR] [info] [NativeStereoFix] state=learning"
    [System.IO.File]::WriteAllLines((Join-Path $runDir "log.txt"), [string[]]$lines)
    $saveSize = (Get-Item -LiteralPath $SaveFile).Length
    $result = [ordered]@{
        verdict = $verdict; game = $Game; label = $Label; run = $run; exit_code = "0x00000000"
        recipe = $Recipe; recipe_vars = $RecipeVars
        save = [ordered]@{ source = $SaveFile; after = [ordered]@{ size = $saveSize + 10; changed = $true; missing = $false } }
        pilot = [ordered]@{ state = "done"; reached = @("title", "main_menu", "hud") }
        notes = @("fake note | with a pipe", "WARN: PostInitProperties slot not found (filtered)")
        received = $received
    }
    $result | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $runDir "result.json") -Encoding UTF8
    Write-Host "$verdict  $Game/$Label run $run"
    Write-Host "save after: $($saveSize + 10) B, changed"
}
if ($verdict -ne "PASS") { exit 1 }
exit 0
