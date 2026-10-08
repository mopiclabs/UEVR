# Save ladder: one harness run per rung of ladders\<Game>.json, each starting from its own save (run-test.ps1
# -SaveFile), and one table of all rungs in runs\ladder-<time>-<game>.md (+ .json, rewritten after every rung;
# everything the harness printed goes to the .log next to them).
#
#   powershell -ExecutionPolicy Bypass -File tools\mopic-test\run-ladder.ps1 -Game Wukong -Tier 1 -EngineDir <dir>
#   ... -Game Wukong -Only ch1-boss3,ch2-boss1     # just these rungs
#   ... -Game Wukong -From ch3-boss1               # this rung and every one after it
#   ... -Game Wukong -DryRun                       # print the harness command of every selected rung
#
# ladders\<Game>.json:
#   {"game": "Wukong", "recipe": "Wukong-save",
#    "rungs": [{"name": "...", "save": "<path relative to tools\mopic-test>", "vars": {"k": "v"}, "tier": 1}]}
# "game" is the run-test.ps1 preset (default: -Game). A rung may name its own "recipe". "tier" defaults to 1;
# -Tier N runs the rungs with tier <= N. A rung's "vars" go to run-test.ps1 -RecipeVars (null: the recipe's
# default). A rung without "save" runs without -SaveFile: it plays the game's own progress as it is (demos, games
# whose progress is several files). Before the first launch every selected rung is checked: its save exists (and
# matches its "sha256", when it has one), its recipe exists, has a "save_slot" (rungs with a save) and declares the
# rung's vars, and every var the recipe needs is given. Other fields (notes...) are ignored. Stops after the
# first rung whose verdict is in -StopOn (HARNESS_ERROR, NO_VR: nothing after it would be a valid test). The VR fps /
# 1% low / Hitches columns are each run's perf headline (result.json "perf", README "Frame rate"; " FG" = frame
# generation on).
# Keep the PC unlocked and unused meanwhile.

param(
    [Parameter(Mandatory = $true)][string]$Game,   # ladders\<Game>.json
    [string]$Ladder = "",                          # another ladder file (path) instead of ladders\<Game>.json
    [int]$Tier = 0,                                # >0: only rungs with "tier" <= this
    [string[]]$Only = @(),                         # only these rungs (names, comma separated)
    [string]$From = "",                            # start at this rung (file order)
    [int]$Runs = 1,
    [string]$EngineDir = "",
    [string]$Label = "ladder",
    [switch]$Binocular,                            # compare both eyes every 3 s (run-test.ps1 -EyeSampler, binocular.py); monado-service in MOPIC_MODE=sbs
    [string[]]$StopOn = @("HARNESS_ERROR", "NO_VR"),   # stop the ladder after a rung with one of these verdicts
    [switch]$DryRun,                               # print what would run, run nothing
    [switch]$SteamOnline,                          # keep Steam online (default: offline for the whole ladder, back online after)
    [string]$Harness = "",                         # test hook: a stand-in for run-test.ps1
    [string]$RunsDir = ""                          # test hook: where the harness writes its run folders (default runs\)
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Harness -eq "") { $Harness = Join-Path $ScriptDir "run-test.ps1" }
if ($RunsDir -eq "") { $RunsDir = Join-Path $ScriptDir "runs" }
# "-Only A,B" through powershell -File arrives as one string
$Only = @($Only | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
$StopOn = @($StopOn | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
if ($Runs -lt 1) { throw "-Runs must be 1 or more" }

if ($Ladder -eq "") { $Ladder = Join-Path $ScriptDir "ladders\$Game.json" }
if (-not (Test-Path -LiteralPath $Ladder -PathType Leaf)) { throw "Ladder not found: $Ladder" }
$Ladder = (Resolve-Path -LiteralPath $Ladder).ProviderPath
$ladderJson = Get-Content -LiteralPath $Ladder -Raw -Encoding UTF8 | ConvertFrom-Json
$preset = $(if ($ladderJson.game) { [string]$ladderJson.game } else { $Game })
$allRungs = @($ladderJson.rungs)
if ($allRungs.Count -eq 0) { throw "$Ladder has no rungs" }

# rung "vars" -> name -> text for run-test.ps1 -RecipeVars "k=v;k2=v2" (lists / objects as JSON, as gamepilot reads
# them back); a null var is left out (the recipe's default applies)
function Get-RungVars($vars) {
    $out = [ordered]@{}
    if (-not $vars) { return $out }
    foreach ($p in $vars.PSObject.Properties) {
        $v = $p.Value
        if ($null -eq $v) { continue }
        if ($v -is [bool]) { $v = $v.ToString().ToLowerInvariant() }
        elseif ($v -is [array] -or $v -is [System.Management.Automation.PSCustomObject]) { $v = ConvertTo-Json $v -Compress -Depth 4 }
        else { $v = [System.Convert]::ToString($v, [System.Globalization.CultureInfo]::InvariantCulture) }
        if ($v.Contains(";")) { throw "rung var $($p.Name): a value can't contain ';'" }
        $out[$p.Name] = $v
    }
    return $out
}

# a number for the table ("" when the run measured nothing), with a dot whatever the PC's locale
function Format-Num($v, [string]$fmt = "0.0") {
    if ($null -eq $v) { return "" }
    return ([double]$v).ToString($fmt, [System.Globalization.CultureInfo]::InvariantCulture)
}

# rung name -> part of the run folder name (run-test.ps1 puts the label in it)
function ConvertTo-SafeName([string]$name) {
    return ($name -replace '[^A-Za-z0-9._-]', '_')
}

# One command-line argument as CommandLineToArgvW (and powershell.exe -File) reads it back. Windows PowerShell 5.1's
# own quoting for native commands (& powershell ...) drops embedded quotes (a JSON list in a var) and lets a quoted
# trailing backslash swallow the next arguments, so the harness is started through ProcessStartInfo with these.
function ConvertTo-ArgvString([string]$s) {
    if ($s -ne "" -and $s -notmatch '[\s";]') { return $s }
    $s = [regex]::Replace($s, '(\\*)"', { param($m) ($m.Groups[1].Value * 2) + '\"' })
    $s = [regex]::Replace($s, '(\\+)$', '$1$1')
    return '"' + $s + '"'
}

# the recipe's "vars" ({name: default}, null = must be given) and whether it has a "save_slot"; cached per file
$recipeInfo = @{}
function Get-RecipeInfo([string]$path) {
    if (-not $recipeInfo.ContainsKey($path)) {
        $json = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        $declared = @(); $required = @()
        if ($null -ne $json.vars) {
            if ($json.vars -isnot [System.Management.Automation.PSCustomObject]) { throw "recipe $path`: `"vars`" must be an object {name: default}" }
            foreach ($p in $json.vars.PSObject.Properties) { $declared += $p.Name; if ($null -eq $p.Value) { $required += $p.Name } }
        }
        $recipeInfo[$path] = [pscustomobject]@{ declared = $declared; required = $required; has_slot = [bool]$json.save_slot }
    }
    return $recipeInfo[$path]
}

# check every rung before the first launch (a typo shouldn't surface hours into the ladder)
$names = @{}
$index = 0
foreach ($rung in $allRungs) {
    $index++
    if (-not $rung.name) { throw "rung $index has no name" }
    $safe = ConvertTo-SafeName $rung.name
    if ($names.ContainsKey($safe)) { throw "rung names '$($names[$safe])' and '$($rung.name)' give the same run folder name ($safe)" }
    $names[$safe] = $rung.name
}
$rungNames = @($allRungs | ForEach-Object { [string]$_.name })
foreach ($n in $Only) { if ($rungNames -notcontains $n) { throw "-Only: no rung named '$n' in $Ladder" } }
$startAt = 0
if ($From -ne "") {
    $startAt = -1
    for ($i = 0; $i -lt $allRungs.Count; $i++) { if ([string]$allRungs[$i].name -eq $From) { $startAt = $i; break } }
    if ($startAt -lt 0) { throw "-From: no rung named '$From' in $Ladder" }
}

$selected = @()
for ($i = $startAt; $i -lt $allRungs.Count; $i++) {
    $rung = $allRungs[$i]
    $rungTier = $(if ($null -ne $rung.tier) { [int]$rung.tier } else { 1 })
    if ($Tier -gt 0 -and $rungTier -gt $Tier) { continue }
    if ($Only.Count -gt 0 -and $Only -notcontains $rung.name) { continue }
    $recipe = $(if ($rung.recipe) { [string]$rung.recipe } elseif ($ladderJson.recipe) { [string]$ladderJson.recipe } else { "" })
    if ($recipe -eq "") { throw "rung $($rung.name): no recipe (set the ladder's or the rung's `"recipe`")" }
    # no "save": the rung plays the game's own progress as it is (no -SaveFile, the recipe needs no save_slot)
    $save = ""
    if ($rung.save) {
        $save = [string]$rung.save
        if (-not [System.IO.Path]::IsPathRooted($save)) { $save = Join-Path $ScriptDir $save }
        if (-not (Test-Path -LiteralPath $save -PathType Leaf)) { throw "rung $($rung.name): save not found: $save" }
        if ($rung.sha256) {
            $hash = (Get-FileHash -LiteralPath $save -Algorithm SHA256).Hash
            if ($hash -ne [string]$rung.sha256) { throw "rung $($rung.name): $save has sha256 $hash, the ladder says $($rung.sha256)" }
        }
        $save = (Resolve-Path -LiteralPath $save).ProviderPath
    } elseif ($rung.sha256) { throw "rung $($rung.name) has a sha256 but no save" }
    $vars = Get-RungVars $rung.vars
    $recipePath = $(if (Test-Path -LiteralPath $recipe -PathType Leaf) { $recipe } else { Join-Path $ScriptDir "recipes\$recipe.json" })
    if (-not (Test-Path -LiteralPath $recipePath -PathType Leaf)) {
        if ($DryRun) { Write-Warning "rung $($rung.name): recipe not found: $recipePath" } else { throw "rung $($rung.name): recipe not found: $recipePath" }
    } else {
        # what run-test.ps1 checks before its first run, here before the ladder's first launch
        $ri = Get-RecipeInfo ((Resolve-Path -LiteralPath $recipePath).ProviderPath)
        if ($save -ne "" -and -not $ri.has_slot) { throw "rung $($rung.name): recipe $recipePath has no `"save_slot`" (where -SaveFile installs the save)" }
        $unknown = @($vars.PSBase.Keys | Where-Object { $ri.declared -cnotcontains $_ })
        if ($unknown.Count -gt 0) { throw "rung $($rung.name): recipe $recipePath declares no var $($unknown -join ', ') (its `"vars`": $(if ($ri.declared.Count -gt 0) { $ri.declared -join ', ' } else { 'none' }))" }
        $unset = @($ri.required | Where-Object { -not $vars.Contains($_) })
        if ($unset.Count -gt 0) { throw "rung $($rung.name): recipe $recipePath needs vars $($unset -join ', ') (no default)" }
    }
    $selected += [pscustomobject]@{
        name   = [string]$rung.name
        tier   = $rungTier
        recipe = $recipe
        save   = $save
        vars   = (@($vars.PSBase.Keys | ForEach-Object { "$_=$($vars[$_])" }) -join ";")
        label  = "$Label-$(ConvertTo-SafeName $rung.name)"
    }
}
if ($selected.Count -eq 0) { throw "no rung selected (-Tier / -Only / -From)" }

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$mdPath = Join-Path $RunsDir "ladder-$stamp-$Game.md"
$logPath = [System.IO.Path]::ChangeExtension($mdPath, ".log")
Write-Host "Ladder:   $Ladder ($preset, $($selected.Count) of $($allRungs.Count) rungs, $Runs run(s) each, stops on $($StopOn -join ', '))"
if ($EngineDir -eq "" -and -not $DryRun) { Write-Warning "no -EngineDir: run-test.ps1 picks the newest engine folder" }

# the harness lines shown while it runs (all of them go to the .log)
$EchoPattern = "^(PASS|CRASH|FREEZE|EXIT_|MENU_|NO_|LAUNCH|HARNESS)|^pilot: |^save after: |^perf|^  - "

# Runs the harness, shows its verdict lines and appends everything it printed to the ladder's .log. -> its exit code
# and, when it stopped with an error, the error's message (stderr up to PowerShell's "At <script>:<line>" part)
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
            if ($line -match $EchoPattern) { Write-Host "  $line" }
        }
        $proc.WaitForExit()
        $err = $errTask.Result
        $message = ""
        if ($err.Trim() -ne "") {
            $log.WriteLine("[stderr]")
            $log.WriteLine($err.TrimEnd())
            # a message wrapped at the console width comes back in pieces
            $parts = @()
            foreach ($l in ($err -split "\r?\n")) { if ($l -match '^At .+:\d+ char:\d+' -or $l -match '^\+ ') { break }; $parts += $l }
            $message = ($parts -join "").Trim()
        }
        $log.WriteLine("< exit code $($proc.ExitCode)")
        return [pscustomobject]@{ code = $proc.ExitCode; error = $message }
    } finally {
        $log.Dispose()
    }
}

function Get-NsfActiveCount([string]$runDir) {
    $log = Join-Path $runDir "log.txt"
    if (-not (Test-Path -LiteralPath $log)) { return $null }
    return @(Select-String -LiteralPath $log -Pattern '\[NativeStereoFix\] state=active').Count
}

function Write-LadderTable($rows, [string]$path, [bool]$stopped) {
    $md = @("# Save ladder $stamp ($preset)", "", "Ladder: ``$Ladder``", "",
        "| Rung | Tier | Run | Verdict | Harness exit | Game exit code | NSF active | Save after | Screens | VR fps | 1% low | Hitches | Notes | Run folder |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    foreach ($r in $rows) {
        $notes = ($r.notes -replace '\|', '/')
        $fps = $(if ($null -ne $r.vr_fps) { (Format-Num $r.vr_fps) + $(if ($r.fg) { " FG" } else { "" }) } elseif ($null -ne $r.present_fps) { "(Present $(Format-Num $r.present_fps))" } else { "" })
        $md += "| $($r.rung -replace '\|', '/') | $($r.tier) | $($r.run) | $($r.verdict) | $($r.harness_exit) | $($r.exit_code) | $($r.nsf_active) | $($r.save_after) | $($r.reached) | $fps | $(Format-Num $r.low1_fps) | $($r.hitches) | $notes | $($r.dir) |"
    }
    $md += ""
    $md += "Totals: " + ((@($rows) | Group-Object { $_.verdict } | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ", ")
    if ($stopped) { $md += ""; $md += "Stopped early: $($rows[-1].rung) ended with $($rows[-1].verdict)." }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [System.IO.File]::WriteAllLines($path, [string[]]$md, (New-Object System.Text.UTF8Encoding($false)))
    ConvertTo-Json -InputObject @($rows) -Depth 4 | Set-Content -Path ([System.IO.Path]::ChangeExtension($path, ".json")) -Encoding UTF8
    return $md
}

# Steam offline once for the whole ladder (steam-offline.ps1); the rungs see MOPIC_STEAM_OFFLINE_HELD and leave it alone.
# Not for a dry run or a stand-in harness (self-test).
. (Join-Path $ScriptDir "steam-offline.ps1")
$steamSwitched = $false
$holdSteam = -not $SteamOnline -and -not $DryRun -and $Harness -eq (Join-Path $ScriptDir "run-test.ps1")
if ($holdSteam) { $steamSwitched = Enter-SteamMode $true; $env:MOPIC_STEAM_OFFLINE_HELD = "1" }

$rows = @()
$stopped = $false
$rungNo = 0
try {
foreach ($rung in $selected) {
    $rungNo++
    $harnessArgs = @("-ExecutionPolicy", "Bypass", "-File", $Harness, "-Game", $preset, "-Recipe", $rung.recipe)
    if ($rung.save -ne "") { $harnessArgs += @("-SaveFile", $rung.save) }
    $harnessArgs += @("-Runs", "$Runs", "-Label", $rung.label)
    if ($rung.vars -ne "") { $harnessArgs += @("-RecipeVars", $rung.vars) }
    if ($EngineDir -ne "") { $harnessArgs += @("-EngineDir", $EngineDir) }
    if ($Binocular) { $harnessArgs += @("-EyeSampler", "-EyeSamplerArgs", "--full-every 3") }
    if ($SteamOnline) { $harnessArgs += "-SteamOnline" }
    Write-Host "=== [$rungNo/$($selected.Count)] $($rung.name) (tier $($rung.tier)) <- $(if ($rung.save -ne '') { $rung.save } else { "(no save: the game's own progress)" })" -ForegroundColor Cyan
    if ($DryRun) {
        Write-Host ("  powershell " + (@($harnessArgs | ForEach-Object { ConvertTo-ArgvString $_ }) -join " "))
        continue
    }
    $start = Get-Date
    $h = Invoke-Harness $harnessArgs
    $harnessExit = $h.code
    $dirPattern = '-' + [regex]::Escape("$preset-$($rung.label)") + '-r\d+$'
    $dirs = @(Get-ChildItem -LiteralPath $RunsDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $dirPattern -and $_.CreationTime -ge $start.AddSeconds(-5) } | Sort-Object Name)
    $found = 0
    foreach ($d in $dirs) {
        $resultPath = Join-Path $d.FullName "result.json"
        if (-not (Test-Path -LiteralPath $resultPath)) { continue }
        $r = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $found++
        $saveAfter = ""
        if ($r.save -and $r.save.after) {
            if ($r.save.after.missing) { $saveAfter = "missing" }
            else { $saveAfter = "$($r.save.after.size) B" + $(if ($r.save.after.changed) { " (changed)" } else { "" }) }
        }
        if ($r.save -and $r.save.error) { $saveAfter = "not installed" }
        # VR numbers only: without UEVR's frame log (an engine without VR_PerfLog) the headline is the game's Present rate
        $perfHl = $(if ($r.perf) { $r.perf.headline } else { $null })
        $hl = $(if ($perfHl -and $perfHl.kind -eq "vr") { $perfHl } else { $null })
        $rows += [pscustomobject][ordered]@{
            rung = $rung.name; tier = $rung.tier; run = $r.run; verdict = $r.verdict; harness_exit = $harnessExit
            exit_code = $r.exit_code; nsf_active = (Get-NsfActiveCount $d.FullName); save_after = $saveAfter
            reached = $(if ($r.pilot) { @($r.pilot.reached).Count } else { 0 })
            vr_fps = $(if ($hl) { $hl.fps } else { $null }); low1_fps = $(if ($hl) { $hl.low1_fps } else { $null })
            hitches = $(if ($hl) { $hl.hitches } else { $null }); fg = $(if ($hl) { [bool]$hl.fg } else { $null })
            present_fps = $(if ($perfHl -and -not $hl) { $perfHl.fps } else { $null })
            notes = (@($r.notes) | Where-Object { $_ -notmatch "^WARN: PostInitProperties" }) -join "; "
            save = $rung.save; vars = $rung.vars; dir = $d.Name
        }
    }
    if ($found -eq 0) {
        # run-test.ps1 stopped before writing a result (a bad argument, a missing recipe, ...)
        $why = "run-test.ps1 wrote no result.json (exit code $harnessExit)"
        if ($h.error -ne "") { $why += ": $($h.error)" }
        $rows += [pscustomobject][ordered]@{
            rung = $rung.name; tier = $rung.tier; run = ""; verdict = "HARNESS_ERROR"; harness_exit = $harnessExit
            exit_code = ""; nsf_active = ""; save_after = ""; reached = 0; vr_fps = $null; low1_fps = $null; hitches = $null; fg = $null; present_fps = $null
            notes = $why; save = $rung.save; vars = $rung.vars; dir = ""
        }
        Write-Host "  $why" -ForegroundColor Red
    }
    $rungVerdicts = @($rows | Where-Object { $_.rung -eq $rung.name } | ForEach-Object { $_.verdict })
    $stopped = @($rungVerdicts | Where-Object { $StopOn -contains $_ }).Count -gt 0
    $null = Write-LadderTable $rows $mdPath $stopped
    if ($stopped) { Write-Warning "stopping the ladder: $($rung.name) ended with $($rungVerdicts -join ', ')"; break }
}
} finally {
    if ($holdSteam) { $env:MOPIC_STEAM_OFFLINE_HELD = $null }
    if ($steamSwitched) { try { [void](Enter-SteamMode $false) } catch { Write-Warning "Steam: could not go back online: $($_.Exception.Message)" } }
}

if ($DryRun) { exit 0 }
$md = Write-LadderTable $rows $mdPath $stopped
Write-Host ""
$md | ForEach-Object { Write-Host $_ }
Write-Host "`nWritten: $mdPath (harness output: $logPath)"
if (@($rows | Where-Object { $_.verdict -ne "PASS" }).Count -gt 0 -or $stopped) { exit 1 }
exit 0
