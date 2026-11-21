# Keeps the harness's post-run binocular analysis out of the run (cost_run.ps1 -DeferBinocular): while a run plays,
# moves each finished whole-display frame (runs\<run>\binocular\*.png older than 5 s; eyesampler.py writes each frame
# in one save and counts them itself) to -HoldDir\<run>\. The harness then finds (almost) no frames to analyze, so the
# block's time is game time. When -StopFile appears it moves the rest, and exits. cost_run.ps1 then moves the held
# frames back into the run folder; analyze them after the block: analysis\binocular.py analyze <run> (harness).
param([Parameter(Mandatory)][string]$RunsDir, [Parameter(Mandatory)][string]$Match, [Parameter(Mandatory)][string]$HoldDir,
    [Parameter(Mandatory)][string]$StopFile, [datetime]$After = (Get-Date), [int]$TimeoutS = 3600)
$end = (Get-Date).AddSeconds($TimeoutS)
$run = $null
$moved = 0
function Move-Ready([int]$minAgeS) {
    if (-not $run) { return }
    $src = Join-Path $run.FullName "binocular"
    if (-not (Test-Path $src)) { return }
    $dst = Join-Path $HoldDir $run.Name
    New-Item -ItemType Directory -Force $dst | Out-Null
    foreach ($f in @(Get-ChildItem $src -Filter *.png -ErrorAction SilentlyContinue)) {
        if (((Get-Date) - $f.LastWriteTime).TotalSeconds -lt $minAgeS) { continue }
        try { Move-Item -LiteralPath $f.FullName -Destination $dst -Force -ErrorAction Stop; $script:moved++ } catch { }
    }
}
while ((Get-Date) -lt $end) {
    if (-not $run) { $run = Get-ChildItem $RunsDir -Directory | Where-Object { $_.CreationTime -gt $After -and $_.Name -like $Match } | Sort-Object CreationTime | Select-Object -First 1 }
    if (Test-Path $StopFile) { Move-Ready 0; break }
    Move-Ready 5
    Start-Sleep 5
}
Set-Content -Path "$StopFile.done" -Value "$moved $(if ($run) { $run.Name } else { '' })" -Encoding ascii
