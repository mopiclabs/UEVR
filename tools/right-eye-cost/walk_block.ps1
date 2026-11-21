# Whole runs of the Wukong-walk-nomod route (recipes\, about 11 min of walking and fighting from the forest-shrine
# save, no modifier keys), one GDF mode per run in -Order (r = refresh = VR_ExperimentRightEyeGdfShare 0, the shipped
# behaviour; s = share = 1; i = share with the update index kept = 3), SBS + EyeSampler (binocular frames over the whole run, one every 11 s, at most 60: the
# harness's post-run binocular analysis grows faster than the frame count) + pass markers, through cost_run.ps1.
#
# Saves: the recipe loads the LAST entry of the game's save list, which is ordered by the date inside each save file.
# The forest-shrine save is a community save installed as slot 9 (ArchiveSaveFile.9.sav); it is older than any of
# the player's own journeys, so it is last only while it is unchanged. Wukong writes the loaded slot when the recipe
# quits to the title, and may autosave during play: after one run slot 9 is the newest entry, and the next run would
# load the player's oldest journey instead (off-route, and it autosaves into that journey). So the files named in
# -Expect are checked by hash BEFORE every run (the run does not start if one differs after putting it back) and put
# back AFTER every run (whatever its verdict), with the current time (Steam Cloud undoes old-dated restores). Every
# save file that a run changed is listed. Key check after every run; stops at the first focus failure; no run starts
# later than -MaxStartMinutes after the block's start (cost_run -LatestStart).
#
#   powershell -ExecutionPolicy Bypass -File walk_block.ps1 -Harness <repo>\tools\mopic-test -Dll <build>\UEVRBackend.dll
#       -SaveDir "<game>\b1\Saved\SaveGames\<id>" -Expect "ArchiveSaveFile.9.sav=<sha256 prefix 8>;ArchiveSaveFile.3.sav=...;ShareArchiveSaveFile.sav=..."
#       [-Order rsr] [-MaxStartMinutes 38] [-Set "Key=Value;..."]
param([Parameter(Mandatory)][string]$Dll,
    [Parameter(Mandatory)][string]$SaveDir,
    [Parameter(Mandatory)][string]$Expect,
    [string]$Harness = $env:MOPIC_TEST_DIR,
    [string]$EngineDir = "$env:APPDATA\MOPIC\mopichub\engines\mopic-uevr\1.0.5.1",
    [string]$OutDir = "$env:LOCALAPPDATA\right-eye-cost",
    [string]$Recipe = "",
    [string]$Order = "rsr", [string]$Tag = "walk", [int]$MaxStartMinutes = 38, [string]$Set = "", [double]$EyeHz = 0,
    [int]$FirstIndex = 1)   # label number of the first run (gdfwalk<N><mode>), to continue a block's numbering
$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
if ($Recipe -eq "") { $Recipe = Join-Path $here "recipes\Wukong-walk-nomod.json" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir "campaign.log"
$runs = Join-Path $Harness "runs"
$aside = Join-Path $OutDir "save-restore\before-$Tag-$(Get-Date -Format yyyyMMdd-HHmmss)"
$expectMap = @{}
foreach ($kv in ($Expect -split ";" | Where-Object { $_ -match "=" })) { $k, $v = $kv -split "=", 2; $expectMap[$k.Trim()] = $v.Trim().ToUpper() }
. (Join-Path $PSScriptRoot "steam_state.ps1")
function Note($m) { $l = "$(Get-Date -Format s) [$Tag] $m"; Write-Host $l; Add-Content -Path $log -Value $l -Encoding utf8 }
function H8($p) { (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0, 8) }
Add-Type -Namespace WalkBlock -Name K -MemberDefinition '[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int v); [DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow(); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint p);'
function KeyCheck($when) {
    $down = @(0x10,0x11,0x12,0x5B,0x5C,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5) | Where-Object { ([WalkBlock.K]::GetAsyncKeyState($_) -band 0x8000) -ne 0 } | ForEach-Object { '0x{0:X2}' -f $_ }
    $fpid = [uint32]0; [void][WalkBlock.K]::GetWindowThreadProcessId([WalkBlock.K]::GetForegroundWindow(), [ref]$fpid)
    $fg = (Get-Process -Id $fpid -ErrorAction SilentlyContinue).ProcessName
    $games = @(Get-Process b1-Win64-Shipping -ErrorAction SilentlyContinue).Count
    Note "key check $($when): keys down $(if ($down) { $down -join ',' } else { 'none' }); foreground $fg; RunningAppID $(Get-SteamRunningAppId) (both registry views); game processes $games"
    return (-not $down) -and ($fg -notin @('SearchHost', 'StartMenuExperienceHost')) -and $games -eq 0
}
function FocusStop {
    $last = Get-ChildItem $runs -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $sum = Join-Path $last.FullName "summary.txt"
    if ((Test-Path $sum) -and (Select-String -Path $sum -Pattern 'could not bring the game window to the foreground' -Quiet)) { Note "FOCUS FAILURE in $($last.Name); stopping"; return $true }
    return $false
}
# Puts the -Expect files back to the aside copies when they differ (keeps what the run left); $false if a file still
# differs from its expected hash afterwards.
function PutBack($when, $tag) {
    $ok = $true
    foreach ($f in $expectMap.Keys) {
        $a = Join-Path $aside $f; $t = Join-Path $SaveDir $f
        if ((H8 $t) -ne (H8 $a)) {
            Copy-Item $t (Join-Path $aside "$f.$tag") -Force
            Copy-Item $a $t -Force
            (Get-Item $t).LastWriteTime = Get-Date
            Note "$f put back to $(H8 $t) $when (run left $(H8 (Join-Path $aside "$f.$tag")))"
        } else { Note "$f unchanged $when ($(H8 $t))" }
        if ((H8 $t) -ne $expectMap[$f]) { Note "$f is $(H8 $t), expected $($expectMap[$f])"; $ok = $false }
    }
    return $ok
}
# Every save file in the folder: name -> "hash@time", to list what a run changed.
function Snap { $s = @{}; Get-ChildItem $SaveDir -Filter *.sav | ForEach-Object { $s[$_.Name] = "$(H8 $_.FullName)@$($_.LastWriteTime.ToString('MM-dd HH:mm:ss'))" }; return $s }
function SnapDiff($a, $b, $when) {
    $ch = @($a.Keys + $b.Keys | Sort-Object -Unique | Where-Object { $a[$_] -ne $b[$_] } | ForEach-Object { "$_ $(if ($a[$_]) { $a[$_] } else { 'none' }) -> $(if ($b[$_]) { $b[$_] } else { 'none' })" })
    Note "save files changed $($when): $(if ($ch) { $ch -join '; ' } else { 'none' })"
}

if ($expectMap.Count -eq 0) { Write-Host "-Expect names no files"; exit 5 }
# The harness's perf report reads every earlier run's result.json (its build comparison). On blade18 that step took
# 145-168 s on the first run of two blocks and 3-4 s otherwise (cold file cache suspected, not proven): read them
# once here, before the block's clock starts.
$pre = Measure-Command { Get-ChildItem $runs -Directory | ForEach-Object { $p = Join-Path $_.FullName "result.json"; if (Test-Path $p) { [void][IO.File]::ReadAllBytes($p) } } }
Note "pre-read the harness's earlier result.json files in $([math]::Round($pre.TotalSeconds, 1)) s"
$blockStart = Get-Date
$latest = $blockStart.AddMinutes($MaxStartMinutes).ToString("HH:mm")
Note "block start $($blockStart.ToString('HH:mm:ss')); order $Order; no run starts after $latest (block start + $MaxStartMinutes min, rounded down)"
if (-not (KeyCheck "before the block")) { Note "pre-run check failed; not starting"; exit 3 }
foreach ($f in $expectMap.Keys) { $h = H8 (Join-Path $SaveDir $f); if ($h -ne $expectMap[$f]) { Note "$f is $h before the block, expected $($expectMap[$f]); not starting"; exit 5 } }
New-Item -ItemType Directory -Force $aside | Out-Null
foreach ($f in $expectMap.Keys) { Copy-Item (Join-Path $SaveDir $f) $aside -Force }
Note "copied aside to $($aside): $(($expectMap.Keys | ForEach-Object { "$_ $(H8 (Join-Path $aside $_))" }) -join ', ')"

$modes = @{ r = @{ mode = 0; name = "refresh" }; s = @{ mode = 1; name = "share" }; i = @{ mode = 3; name = "shareidx" } }
$plan = @(); $n = $FirstIndex
foreach ($c in $Order.ToCharArray()) { $m = $modes["$c"]; if (-not $m) { Note "unknown mode '$c' in -Order"; exit 5 }; $plan += @{ mode = $m.mode; label = "gdfwalk$n$($m.name)" }; $n++ }
for ($i = 0; $i -lt $plan.Count; $i++) {
    $p = $plan[$i]
    if (-not (PutBack "before $($p.label)" "before-$($p.label)")) { Note "save files not as expected before $($p.label); stopping"; exit 6 }
    $before = Snap
    $setAll = (@("VR_ExperimentRightEyeGdfShare=$($p.mode)") + $(if ($Set) { @($Set) } else { @() })) -join ";"
    try {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $here "cost_run.ps1") -Harness $Harness -Dll $Dll -EngineDir $EngineDir -OutDir $OutDir `
            -Game Wukong -Recipe $Recipe -SteamOnline -Runtime sbs -EyeSampler -EyeFullEvery 11 -EyeFullMax 60 -EyeHz $EyeHz -DeferBinocular -Arms pm `
            -LabelSuffix $p.label -Set $setAll -LatestStart $latest | Out-Host
    } finally {
        for ($w = 0; $w -lt 60 -and (Get-Process b1-Win64-Shipping -ErrorAction SilentlyContinue); $w++) { Start-Sleep 2 }
        SnapDiff $before (Snap) "by $($p.label)"
        [void](PutBack "after $($p.label)" "after-$($p.label)")
        $ok = KeyCheck "after $($p.label)"
    }
    if (FocusStop) { exit 0 }
    if (-not $ok) { Note "key/foreground check failed after $($p.label); stopping"; exit 4 }
}
Note "block done"
