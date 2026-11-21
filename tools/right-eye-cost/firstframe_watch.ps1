# Waits for the next run folder (created after -After, name matching -Match) in the harness's runs\, then for its first
# binocular frame at least -Delay s after the pilot's play phase, prints its path and its left-eye mean luma next to the
# reference frame's (analysis\frame_similar.py), and exits. Read-only. Use it to confirm a walk run loaded the
# forest-shrine save: look at the frame; the luma tells the dark forest (~40-50) from a bright wrong save (~100).
# The thumbnail correlation it also prints does not separate the two scenes.
param([Parameter(Mandatory)][string]$Reference, [string]$Harness = $env:MOPIC_TEST_DIR, [datetime]$After = (Get-Date),
    [string]$Match = "gdfwalk", [double]$Delay = 3, [int]$TimeoutS = 1500)
$runs = Join-Path $Harness "runs"
$py = Join-Path $Harness ".venv\Scripts\python.exe"
$end = (Get-Date).AddSeconds($TimeoutS)
$dir = $null
while ((Get-Date) -lt $end) {
    if (-not $dir) { $dir = Get-ChildItem $runs -Directory | Where-Object { $_.CreationTime -gt $After -and $_.Name -match $Match } | Sort-Object CreationTime | Select-Object -First 1 }
    if ($dir) {
        $st = Join-Path $dir.FullName "pilot-status.json"
        $play = $null
        if (Test-Path $st) { try { $play = ((Get-Content $st -Raw | ConvertFrom-Json).phases | Where-Object name -eq 'play').qpc_ns } catch {} }
        if ($play) {
            $f = Get-ChildItem (Join-Path $dir.FullName "binocular") -Filter *.png -ErrorAction SilentlyContinue |
                Where-Object { [int64]($_.BaseName -replace '^(\d+).*', '$1') -gt [int64]$play + [int64]($Delay * 1e9) } | Sort-Object Name | Select-Object -First 1
            if ($f) {
                Start-Sleep 1
                "$(Get-Date -Format s) run $($dir.Name): first frame $([math]::Round(([int64]($f.BaseName -replace '^(\d+).*', '$1') - [int64]$play) / 1e9, 1)) s into play: $($f.FullName)"
                & $py (Join-Path $PSScriptRoot "analysis\frame_similar.py") $Reference $f.FullName
                exit 0
            }
        }
    }
    Start-Sleep 5
}
"$(Get-Date -Format s) timeout: run folder $(if ($dir) { $dir.Name } else { 'not found' }), no in-play frame"
exit 1
