# Self-test of the save-install / recipe vars / ladder / frame-rate harness changes. Launches no game: where a run
# needs a game process, a stand-in (fakegame.cs, compiled into the work folder) plays it; the frame-rate samplers
# (nvidia-smi, the PresentMon console app when installed) run around it as in a real run. Never touches the real save folder:
# every save file lives under runs\selftest\work-<time>\ (gitignored). Stops nothing but its own stand-ins (and, through the
# harness, UEVRInjector / CrashReportClient, so it refuses to run while one of those is running).
$ErrorActionPreference = "Stop"
$Repo = "C:\Users\zzong\source\repos\UEVR-jh"
$Tools = Join-Path $Repo "tools\mopic-test"
$Here = $PSScriptRoot   # the self-test sources (tools\mopic-test\selftest)
$EngineDirReal = "C:\Users\zzong\AppData\Roaming\MOPIC\mopichub\engines\mopic-uevr\1.0.5.1"
$Py = Join-Path $Tools ".venv\Scripts\python.exe"
$Work = Join-Path $Tools ("runs\selftest\work-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$RealSaveDir = "C:\Program Files (x86)\Steam\steamapps\common\BlackMythWukong\b1\Saved\SaveGames\76561199750818073"
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Cn = -join [char[]](0x9ED1, 0x98CE, 0x5C71)   # this script is ASCII: Chinese text from code points

$script:fails = 0
$script:passes = 0
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { $script:passes++; Write-Host "ok    $name" } else { $script:fails++; Write-Host "FAIL  $name  $detail" }
}
function Throws([string]$name, [scriptblock]$block, [string]$like = "*") {
    try { & $block | Out-Null; Check $name $false "(no error)" }
    catch { $m = $_.Exception.Message; Check $name ($m -like $like) "(got: $m)" }
}
# argv quoting as CommandLineToArgvW reads it (the same rules as run-ladder.ps1 uses)
function ConvertTo-Argv([string]$s) {
    if ($s -ne "" -and $s -notmatch '[\s";]') { return $s }
    $s = [regex]::Replace($s, '(\\*)"', { param($m) ($m.Groups[1].Value * 2) + '\"' })
    $s = [regex]::Replace($s, '(\\+)$', '$1$1')
    return '"' + $s + '"'
}
# another powershell, arguments passed exactly (& powershell would drop embedded quotes); stdout then stderr lines.
# flat: the lines joined without separators, so a message the console wrapped still matches.
function Invoke-PS([string[]]$argv) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = (@((@("-NoProfile", "-ExecutionPolicy", "Bypass") + $argv) | ForEach-Object { ConvertTo-Argv $_ }) -join " ")
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Console]::OutputEncoding
    $psi.StandardErrorEncoding = [Console]::OutputEncoding
    $proc = [System.Diagnostics.Process]::Start($psi)
    $errTask = $proc.StandardError.ReadToEndAsync()
    $outText = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit()
    $lines = @(@($outText -split "\r?\n") + @($errTask.Result -split "\r?\n") | Where-Object { $_ -ne "" })
    return [pscustomobject]@{ code = $proc.ExitCode; out = $lines; text = ($lines -join "`n"); flat = ($lines -join "") }
}
function Write-Json([string]$path, $obj) { [System.IO.File]::WriteAllText($path, (ConvertTo-Json $obj -Depth 8), $Utf8) }
function Read-Json([string]$path) { return (Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json) }
function Get-Listing([string]$dir) {
    @(Get-ChildItem -LiteralPath $dir -File | Sort-Object Name | ForEach-Object { "$($_.Name)|$($_.Length)|$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)|$($_.LastWriteTimeUtc.Ticks)" })
}
function Get-Hash([string]$path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
$realBefore = Get-Listing $RealSaveDir

Write-Host "== 1. parse"
foreach ($f in @("run-test.ps1", "run-ladder.ps1", "run-matrix.ps1", "runs\selftest\fake-harness.ps1", "runs\selftest\selftest.ps1")) {
    $tokens = $null; $errs = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Tools $f), [ref]$tokens, [ref]$errs)
    Check "parse $f ($(@($errs).Count) errors)" (@($errs).Count -eq 0) ((@($errs) | ForEach-Object { "line $($_.Extent.StartLineNumber): $($_.Message)" }) -join "; ")
}
foreach ($f in @("run-test.ps1", "run-ladder.ps1", "runs\selftest\selftest.ps1", "runs\selftest\fake-harness.ps1")) {
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Tools $f))
    Check "$f is ASCII (PowerShell 5.1 reads BOM-less scripts as ANSI)" (@($bytes | Where-Object { $_ -gt 127 }).Count -eq 0)
}

Write-Host "== 2. run-test.ps1 functions (loaded from the script's AST)"
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Tools "run-test.ps1"), [ref]$null, [ref]$null)
$wanted = @("Get-SteamLibraries", "Find-SteamAppId", "Find-SteamGameDir", "Get-SteamAccountId", "Get-SavePlaceholderValues",
    "Expand-SavePlaceholders", "Resolve-SaveTarget", "Copy-FileRetry", "Install-SaveFile", "Save-SaveEvidence",
    "ConvertTo-ArgvString", "ConvertFrom-RecipeVars", "ConvertTo-PilotVarArgs")
foreach ($fn in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
    if ($wanted -contains $fn.Name) { . ([scriptblock]::Create($fn.Extent.Text)) }
}
foreach ($w in $wanted) { Check "function $w defined" ([bool](Get-Command $w -ErrorAction SilentlyContinue)) }

$vals = @{ sid64 = "76561199750818073"; accountid = "1790552345"; gamedir = "D:\Games\BMW"; localappdata = "L"; appdata = "A"; documents = "Docs" }
$e = Expand-SavePlaceholders "{gamedir}\b1\{sid64}\{accountid}|{localappdata}|{appdata}|{documents}" $vals
Check "expand every placeholder" ($e -eq "D:\Games\BMW\b1\76561199750818073\1790552345|L|A|Docs") $e
Check "placeholder names ignore case" ((Expand-SavePlaceholders "{SID64}" $vals) -eq "76561199750818073")
Check "repeated placeholder" ((Expand-SavePlaceholders "{appdata}{appdata}" $vals) -eq "AA")
Check "text without placeholders unchanged" ((Expand-SavePlaceholders "C:\x\y.sav" $vals) -eq "C:\x\y.sav")
Throws "unknown placeholder" { Expand-SavePlaceholders "C:\{steamid}" $vals } "*unknown placeholder {steamid}*"
$noUser = $vals.Clone(); $noUser.sid64 = $null; $noUser.gamedir = $null
Throws "{sid64} without a Steam user" { Expand-SavePlaceholders "{sid64}" $noUser } "*no Steam user is logged in*"
Throws "{gamedir} without an install" { Expand-SavePlaceholders "{gamedir}" $noUser } "*install folder was not found*"

# SteamID64 = 76561197960265728 + account id, in 64-bit integers (an [int] or [double] would lose digits)
$real = Get-SavePlaceholderValues (Find-SteamGameDir "BlackMythWukong")
Write-Host ("      real values: " + (($real.Keys | Sort-Object | ForEach-Object { "$_=$($real[$_])" }) -join "  "))
Check "real {sid64} = 76561199750818073" ($real.sid64 -ceq "76561199750818073") $real.sid64
Check "real {accountid} = 1790552345" ($real.accountid -ceq "1790552345") $real.accountid
Check "real {gamedir} = the Wukong install, real spelling" ($real.gamedir -ceq "C:\Program Files (x86)\Steam\steamapps\common\BlackMythWukong") $real.gamedir
Check "real {localappdata} {appdata} {documents}" ($real.localappdata -eq $env:LOCALAPPDATA -and $real.appdata -eq $env:APPDATA -and (Test-Path -LiteralPath $real.documents))
Check "SteamID64 of the highest account id (no overflow, no rounding)" ([string]([uint64]76561197960265728 + [uint64]4294967295) -ceq "76561202255233023")
Check "Find-SteamGameDir of a missing game" ($null -eq (Find-SteamGameDir "NoSuchGame-selftest"))
Check "Find-SteamAppId still finds Wukong" ((Find-SteamAppId "BlackMythWukong") -eq "2358720")

$slot = '{"dir": "{gamedir}\\b1\\Saved\\SaveGames\\{sid64}", "file": "ArchiveSaveFile.9.sav"}' | ConvertFrom-Json
$t = Resolve-SaveTarget $slot "" $real
Check "Wukong save_slot target (computed only)" ($t -eq "$RealSaveDir\ArchiveSaveFile.9.sav") $t
Check "-SaveAs overrides the file" ((Resolve-SaveTarget $slot "ArchiveSaveFile.8.sav" $real) -eq "$RealSaveDir\ArchiveSaveFile.8.sav")
Throws "-SaveAs with a path" { Resolve-SaveTarget $slot "..\evil.sav" $real } "*plain file name*"
Throws "-SaveAs .." { Resolve-SaveTarget $slot ".." $real } "*plain file name*"
Throws "save_slot without file" { Resolve-SaveTarget ('{"dir": "C:\\x"}' | ConvertFrom-Json) "" $real } '*no "file"*'
Throws "save_slot without dir" { Resolve-SaveTarget ('{"file": "a.sav"}' | ConvertFrom-Json) "" $real } '*needs a "dir"*'
Throws "relative save_slot dir" { Resolve-SaveTarget ('{"dir": "rel\\{sid64}", "file": "a.sav"}' | ConvertFrom-Json) "" $real } "*not an absolute path*"

$pv = ConvertTo-PilotVarArgs "play_s=180; keys = w a s d ;dir=C:\x\;pos=[110,305];eq=a=b;q=x`"y"
$expect = @("--var", '"play_s=180"', "--var", '"keys=w a s d"', "--var", '"dir=C:\x\\"', "--var", '"pos=[110,305]"', "--var", '"eq=a=b"', "--var", '"q=x\"y"')
Check "-RecipeVars -> gamepilot args (quoted for Start-Process)" (($pv -join " ") -eq ($expect -join " ")) ($pv -join " ")
Check "empty -RecipeVars" (@(ConvertTo-PilotVarArgs "").Count -eq 0)
Throws "-RecipeVars without =" { ConvertTo-PilotVarArgs "novalue" } "*name=value*"
Throws "-RecipeVars bad name" { ConvertTo-PilotVarArgs "1a=2" } "*name=value*"
$rv = ConvertFrom-RecipeVars "a=1;A=2"
Check "-RecipeVars names are case-sensitive" ($rv.PSBase.Count -eq 2 -and $rv["a"] -eq "1" -and $rv["A"] -eq "2")
# a dictionary's .Keys / .Count / .Values would give the entry of that name instead
$pv = ConvertTo-PilotVarArgs "Keys=w a s d;Count=2;Values=3"
Check "vars named Keys / Count / Values" (($pv -join " ") -eq '--var "Keys=w a s d" --var "Count=2" --var "Values=3"') ($pv -join " ")

Write-Host "== 2b. run-test.ps1's frame-rate helpers"
# QPC in ns as Python's time.perf_counter_ns() counts it (gamepilot's segments; UEVR's steady_clock alike)
$c1 = Get-ClockPair
$pyNs = [int64](& $Py -c "import time; print(time.perf_counter_ns())")
$c2 = Get-ClockPair
Check "Get-ClockPair: QPC ns on Python's perf_counter_ns clock, Unix ms now" ($c1.qpc_ns -le $pyNs -and $pyNs -le $c2.qpc_ns -and [math]::Abs($c2.unix_ms - [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) -lt 5000) "$($c1.qpc_ns) <= $pyNs <= $($c2.qpc_ns)"

# monado's app frame stats: the run's rows of both generations under one header, nothing outside the window
$statsDir = Join-Path $Work "monado"
New-Item -ItemType Directory -Force -Path $statsDir | Out-Null
$mh = "qpc_ns,unix_ms,window_ms,pid,exe,mode,presents,new,repeated,no_layer,present_fps,new_fps,gap_avg_ms,gap_max_ms,dts_avg_ms,dts_max_ms,dropped,rebased"
[System.IO.File]::WriteAllLines((Join-Path $statsDir "app_frame_stats.1.csv"), [string[]]@($mh, "100,1,1000,7,g.exe,sbs,60,60,0,0,60,60,16,17,16,17,0,0", "200,1,1000,7,g.exe,sbs,60,60,0,0,60,60,16,17,16,17,0,0"))
[System.IO.File]::WriteAllText((Join-Path $statsDir "app_frame_stats.csv"), "$mh`r`n300,1,1000,7,g.exe,sbs,60,30,30,0,60,30,33,34,33,34,1,0`r`n400,1,1000,7,g.exe,sbs,60,30,30,0,60,30,33,34,33,34,0,0`r`n500,1,10")
$sliceDir = Join-Path $Work "slice"
New-Item -ItemType Directory -Force -Path $sliceDir | Out-Null
$n = Save-MonadoSlice $statsDir $sliceDir 150 450
$slice = @(Get-Content -LiteralPath (Join-Path $sliceDir "monado-frames.csv"))
Check "Save-MonadoSlice: rows 200-400 of both files under one header" ($n -eq 3 -and $slice.Count -eq 4 -and $slice[0] -eq $mh -and $slice[1] -like "200,*" -and $slice[3] -like "400,*") ($slice -join " / ")
Check "Save-MonadoSlice: nothing in the window -> no file, 0" ((Save-MonadoSlice $statsDir (Join-Path $Work "slice-none") 1000 2000) -eq 0 -and -not (Test-Path (Join-Path $Work "slice-none")))

# UEVR keeps perf.csv open for writing while the game runs (-KeepGame)
$held = Join-Path $Work "held.csv"
$writer = [System.IO.File]::Open($held, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]"ReadWrite, Delete")
try {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes("qpc_ns,kind`n1,P`n")
    $writer.Write($bytes, 0, $bytes.Length)
    $writer.Flush()
    try { Copy-Shared $held (Join-Path $Work "held-copy.csv"); $copied = [System.IO.File]::ReadAllText((Join-Path $Work "held-copy.csv")) } catch { $copied = "error: $_" }
} finally { $writer.Dispose() }
Check "Copy-Shared copies a file another handle still writes" ($copied -eq "qpc_ns,kind`n1,P`n") $copied

$realLocal = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $Work "localappdata"
    $iniDir = Join-Path $env:LOCALAPPDATA "Hk_project\Saved\Config\WindowsNoEditor"
    New-Item -ItemType Directory -Force -Path $iniDir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $iniDir "GameUserSettings.ini"), "[/Script/Engine.GameUserSettings]`r`nbUseVSync=False")
    $found = Find-GameSettings "Hk_project"
    Check "Find-GameSettings: a UE4 game's WindowsNoEditor; nothing for an unknown folder or no preset" ($found -eq (Join-Path $iniDir "GameUserSettings.ini") -and $null -eq (Find-GameSettings "NoSuchGame-selftest") -and $null -eq (Find-GameSettings "")) $found
} finally {
    $env:LOCALAPPDATA = $realLocal
}
$presetAst = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$Presets' }, $true)
$presets = Invoke-Expression $presetAst.Right.Extent.Text
$noSettings = @($presets.Keys | Where-Object { -not (Find-GameSettings $presets[$_].SettingsDir) } | Sort-Object)
Check "every preset's GameUserSettings.ini is on this PC ($($presets.Count) presets)" ($presets.Count -ge 9 -and $noSettings.Count -eq 0) ($noSettings -join ", ")

$fgm = Get-FrameGenModules (Get-Process -Id $PID)
Check "Get-FrameGenModules: this PowerShell has no frame generation module" ($fgm -and $fgm.nvngx_dlssg -eq $false -and @($fgm.fg_modules).Count -eq 0) ($fgm | ConvertTo-Json -Compress)
$gone = Start-Process -FilePath "cmd.exe" -ArgumentList "/c exit" -WindowStyle Hidden -PassThru
$gone.WaitForExit()
Check "Get-FrameGenModules: a process that is gone -> null" ($null -eq (Get-FrameGenModules $gone))
$power = Get-PowerContext
Check "Get-PowerContext: AC known, battery percent, power scheme" ($power.ac -is [bool] -and $null -ne $power.battery_pct -and "$($power.scheme)" -match '^[0-9a-f-]{36}$') ($power | ConvertTo-Json -Compress)
Write-Host "      PresentMon console app: $(Find-PresentMon)"

Write-Host "== 3. install / evidence against a temp save folder"
$sg = Join-Path $Work "SaveGames\$($real.sid64)"
New-Item -ItemType Directory -Force -Path $sg | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $sg "ArchiveSaveFile.1.sav"), [byte[]](11, 12, 13))
[System.IO.File]::WriteAllBytes((Join-Path $sg "ArchiveSaveFile.9.sav"), [byte[]](1, 2, 3))
[System.IO.File]::WriteAllBytes((Join-Path $sg "ShareArchiveSaveFile.sav"), [byte[]](90, 90))
(Get-Item (Join-Path $sg "ArchiveSaveFile.9.sav")).LastWriteTime = (Get-Date).AddDays(-30)
$srcDir = Join-Path $Work "saves"
New-Item -ItemType Directory -Force -Path $srcDir | Out-Null
$src = Join-Path $srcDir "1.2.3_rung one.sav"
$rnd = New-Object byte[] 65536; (New-Object System.Random 7).NextBytes($rnd); [System.IO.File]::WriteAllBytes($src, $rnd)
$srcHash = Get-Hash $src
$slotTmp = ('{"dir": "' + ($Work -replace '\\', '\\') + '\\SaveGames\\{sid64}", "file": "ArchiveSaveFile.9.sav"}') | ConvertFrom-Json
$target = Resolve-SaveTarget $slotTmp "" $real
Check "temp target via {sid64}" ($target -eq (Join-Path $sg "ArchiveSaveFile.9.sav")) $target
$others = @("ArchiveSaveFile.1.sav", "ShareArchiveSaveFile.sav")
$othersBefore = @($others | ForEach-Object { Get-Hash (Join-Path $sg $_) })

$runDir = Join-Path $Work "run1"; New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$info = Install-SaveFile $src $srcHash $target $runDir
Check "installed bytes = source" ((Get-Hash $target) -eq $srcHash)
Check "installed LastWriteTime is now" ([math]::Abs(((Get-Item $target).LastWriteTime - (Get-Date)).TotalSeconds) -lt 10) (Get-Item $target).LastWriteTime
Check "old slot file kept in save-before" (((Get-Content -LiteralPath (Join-Path $runDir "save-before\ArchiveSaveFile.9.sav") -Encoding Byte) -join ",") -eq "1,2,3")
Check "info: source, sha256, target, size" ($info.source -eq $src -and $info.source_sha256 -eq $srcHash -and $info.target -eq $target -and $info.size -eq 65536)
Check "other saves untouched by the install" ((@($others | ForEach-Object { Get-Hash (Join-Path $sg $_) }) -join ",") -eq ($othersBefore -join ","))
Check "source untouched" ((Get-Hash $src) -eq $srcHash)

$since = Get-Date
Start-Sleep -Milliseconds 200
# the "game" autosaves into the slot and writes the global file
$fs = [System.IO.File]::Open($target, "Append", "Write"); $fs.Write([byte[]](7, 7, 7, 7), 0, 4); $fs.Dispose()
[System.IO.File]::WriteAllBytes((Join-Path $sg "ShareArchiveSaveFile.sav"), [byte[]](91, 91))
$after = Save-SaveEvidence $target $info.source_sha256 $runDir $since
$info.after = $after
Check "save-after copy = the slot after the run" ((Get-Hash (Join-Path $runDir "save-after\ArchiveSaveFile.9.sav")) -eq (Get-Hash $target))
Check "save-after: size 65540, changed" ($after.size -eq 65540 -and $after.changed -eq $true -and $after.missing -eq $false) "$($after.size) $($after.changed)"
Check "save-after: other file written during the run listed" ((@($after.others_written) -join ",") -eq "ShareArchiveSaveFile.sav") (@($after.others_written) -join ",")
Check "nested ordered dict assignment (result.json shape)" ($info.after.size -eq 65540)
$json = $info | ConvertTo-Json -Depth 5 | ConvertFrom-Json
Check "save info survives ConvertTo-Json" ($json.after.size -eq 65540 -and $json.source_sha256 -eq $srcHash -and $json.target_before -like "*save-before*")
Check "nothing deleted from the save folder" ((@(Get-ChildItem -LiteralPath $sg -File | ForEach-Object Name | Sort-Object) -join ",") -eq "ArchiveSaveFile.1.sav,ArchiveSaveFile.9.sav,ShareArchiveSaveFile.sav")

$runDir2 = Join-Path $Work "run2"; New-Item -ItemType Directory -Force -Path $runDir2 | Out-Null
$info2 = Install-SaveFile $src $srcHash $target $runDir2
$after2 = Save-SaveEvidence $target $info2.source_sha256 $runDir2 (Get-Date)
Check "reinstall over an autosaved slot, unchanged after" ($after2.changed -eq $false -and $after2.size -eq 65536 -and @($after2.others_written).Count -eq 0)
Check "reinstall kept the autosaved file in save-before" ((Get-Item (Join-Path $runDir2 "save-before\ArchiveSaveFile.9.sav")).Length -eq 65540)

$runDir3 = Join-Path $Work "run3 [x]"; New-Item -ItemType Directory -Force -Path $runDir3 | Out-Null
$info3 = Install-SaveFile $src $srcHash (Join-Path $sg "ArchiveSaveFile.7.sav") $runDir3
Check "new slot: no save-before" ($null -eq $info3.target_before -and -not (Test-Path -LiteralPath (Join-Path $runDir3 "save-before")))
$info4 = Install-SaveFile $target $srcHash $target $runDir3
Check "source = target: no copy, still stamped and verified" ($info4.source_sha256 -eq $srcHash)
$ev3 = Save-SaveEvidence (Join-Path $sg "ArchiveSaveFile.7.sav") $srcHash $runDir3 (Get-Date)
Check "evidence into a run folder with brackets in its name" (-not $ev3.missing -and (Test-Path -LiteralPath (Join-Path $runDir3 "save-after\ArchiveSaveFile.7.sav")))
Throws "missing save folder" { Install-SaveFile $src $srcHash (Join-Path $Work "NoSuchDir\ArchiveSaveFile.9.sav") $runDir3 } "*save folder not found*"
Throws "hash mismatch is caught" { Install-SaveFile $src "0000" (Join-Path $sg "ArchiveSaveFile.6.sav") $runDir3 } "*does not match*"
$gone = Save-SaveEvidence (Join-Path $sg "ArchiveSaveFile.5.sav") $srcHash $runDir3 (Get-Date)
Check "evidence of a vanished target: missing, no throw" ($gone.missing -eq $true -and $null -eq $gone.path)

$runsRoot = Join-Path $Tools "runs"
$fakeNames = @("mopicselftest_noproc", "mopicselftest_game")
# the harness's frame-rate samplers (nvidia-smi, the PresentMon console app); every run has to end its own
function Get-SamplerCount { return @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq "nvidia-smi" -or $_.ProcessName -like "PresentMon-*" }).Count }
$samplersBefore = Get-SamplerCount
$busy = @(Get-Process -Name (@("UEVRInjector", "CrashReportClient") + $fakeNames) -ErrorAction SilentlyContinue)
$persistentDirs = @($fakeNames | ForEach-Object { Join-Path $env:APPDATA "UnrealVRMod\$_" } | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($busy.Count -gt 0) {
    Write-Host "SKIP  sections 4-5: $(@($busy | ForEach-Object Name) -join ', ') running (the harness would stop it)"
} else {
    # the stand-in game
    $gameDir = Join-Path $Work "game"
    New-Item -ItemType Directory -Force -Path $gameDir | Out-Null
    $GameExe = Join-Path $gameDir "mopicselftest_game.exe"
    Add-Type -TypeDefinition (Get-Content -LiteralPath (Join-Path $Here "fakegame.cs") -Raw) -ReferencedAssemblies System.Windows.Forms, System.Drawing -OutputAssembly $GameExe -OutputType WindowsApplication
    Check "stand-in game compiled" (Test-Path -LiteralPath $GameExe)

    Write-Host "== 4. runs without -SaveFile: this run-test.ps1 against the one in HEAD (same arguments)"
    $cmp = [ordered]@{ head = (Join-Path $Work "cmp-head"); new = (Join-Path $Work "cmp-new") }
    foreach ($k in $cmp.Keys) { New-Item -ItemType Directory -Force -Path (Join-Path $cmp[$k] ".venv\Scripts") | Out-Null; [System.IO.File]::WriteAllBytes((Join-Path $cmp[$k] ".venv\Scripts\python.exe"), [byte[]]@()) }
    $headText = (& git -C $Repo show "HEAD:tools/mopic-test/run-test.ps1") -join "`r`n"
    [System.IO.File]::WriteAllText((Join-Path $cmp.head "run-test.ps1"), $headText + "`r`n", [System.Text.Encoding]::ASCII)
    Copy-Item -LiteralPath (Join-Path $Tools "run-test.ps1") -Destination (Join-Path $cmp.new "run-test.ps1")
    Check "HEAD run-test.ps1 extracted" ((Get-Content -LiteralPath (Join-Path $cmp.head "run-test.ps1") -TotalCount 1) -like "# Test harness for Mopic UEVR*")
    $recipeCmp = Join-Path $Work "cmp-recipe.json"
    Write-Json $recipeCmp ([ordered]@{ game = "selftest"; process = "mopicselftest_noproc"; source = "window"; checkpoints = @{}; steps = @(@{ wait = 1 }, @{ expect_exit = 5 }) })

    # result.json without what differs between two runs of one case (times), summary.txt with the times blanked,
    # the run folder's file names
    function Get-RunDigest([string]$runsDir, [string]$label) {
        $digests = @()
        foreach ($d in @(Get-ChildItem -LiteralPath $runsDir -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*-Custom-$label-r*" } | Sort-Object Name)) {
            $res = Read-Json (Join-Path $d.FullName "result.json")
            $values = [ordered]@{}
            foreach ($p in $res.PSObject.Properties) {
                if ($p.Name -eq "started") { continue }
                if ($p.Name -eq "exit_after_s") { $values[$p.Name] = [string]($null -ne $p.Value); continue }
                # (ConvertTo-Json of $null gives $null in PowerShell 5.1)
                $values[$p.Name] = $(if ($null -eq $p.Value) { "null" } else { ConvertTo-Json -InputObject $p.Value -Compress -Depth 8 })
            }
            $sum = @(Get-Content -LiteralPath (Join-Path $d.FullName "summary.txt") -Encoding UTF8 | ForEach-Object { $_ -replace '@-?[\d.]*s', '@s' })
            $files = @(Get-ChildItem -LiteralPath $d.FullName -Recurse | ForEach-Object { $_.FullName.Substring($d.FullName.Length) } | Sort-Object)
            $digests += [pscustomobject]@{ keys = @($values.Keys); values = $values; summary = $sum; files = $files }
        }
        return , $digests
    }
    $cmpCases = @(
        @{ name = "LAUNCH_FAILED, no recipe"; game = $null; argv = @("-ProcessName", "mopicselftest_noproc", "-LaunchTarget", "C:\Windows\System32\whoami.exe", "-LaunchTimeout", "5") },
        @{ name = "LAUNCH_FAILED, recipe without save_slot"; game = $null; argv = @("-ProcessName", "mopicselftest_noproc", "-LaunchTarget", "C:\Windows\System32\whoami.exe", "-LaunchTimeout", "5", "-Recipe", $recipeCmp, "-Runs", "2") },
        @{ name = "CRASH"; game = "exit:2:C0000005"; argv = @("-ProcessName", "mopicselftest_game", "-LaunchTarget", $GameExe, "-Seconds", "20") },
        @{ name = "EXIT_CRASH (-WaitForExit)"; game = "exit:2:C0000409"; argv = @("-ProcessName", "mopicselftest_game", "-LaunchTarget", $GameExe, "-Seconds", "20", "-WaitForExit") },
        @{ name = "PASS, -Set -UserScript -KeepGame -Runs 2"; game = "window:60"; argv = @("-ProcessName", "mopicselftest_game", "-LaunchTarget", $GameExe, "-Seconds", "2", "-Runs", "2", "-KeepGame", "-Set", "FrameworkConfig_SelfTest=1", "-UserScript", "r.SelfTest 1") })
    $caseNo = 0
    foreach ($case in $cmpCases) {
        $caseNo++
        $label = "cmp$caseNo"
        $outputs = @{}
        foreach ($k in $cmp.Keys) {
            if ($case.game) { $env:MOPIC_FAKE_GAME = $case.game } else { Remove-Item Env:\MOPIC_FAKE_GAME -ErrorAction SilentlyContinue }
            $outputs[$k] = Invoke-PS (@("-File", (Join-Path $cmp[$k] "run-test.ps1"), "-Game", "Custom", "-NoInject", "-EngineDir", $EngineDirReal, "-Label", $label) + $case.argv)
            Get-Process -Name "mopicselftest_game" -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
        }
        $dh = Get-RunDigest (Join-Path $cmp.head "runs") $label
        $dn = Get-RunDigest (Join-Path $cmp.new "runs") $label
        $verdicts = @($dn | ForEach-Object { ($_.values["verdict"] | ConvertFrom-Json) }) -join ","
        Check "cmp $($case.name): runs and exit code as before ($verdicts, exit $($outputs.new.code))" ($dh.Count -gt 0 -and $dh.Count -eq $dn.Count -and $outputs.head.code -eq $outputs.new.code) "head $($dh.Count) runs exit $($outputs.head.code), new $($dn.Count) exit $($outputs.new.code)"
        for ($i = 0; $i -lt [math]::Min($dh.Count, $dn.Count); $i++) {
            $added = @($dn[$i].keys | Where-Object { $dh[$i].keys -notcontains $_ })
            $lost = @($dh[$i].keys | Where-Object { $dn[$i].keys -notcontains $_ })
            Check "cmp $($case.name) r$($i + 1): result.json keys = before + recipe_vars, save" (($added -join ",") -eq "recipe_vars,save" -and $lost.Count -eq 0) "added $($added -join ','); lost $($lost -join ',')"
            $changed = @($dh[$i].keys | Where-Object { $dh[$i].values[$_] -cne $dn[$i].values[$_] })
            Check "cmp $($case.name) r$($i + 1): result.json values as before" ($changed.Count -eq 0) (($changed | ForEach-Object { "$_ head=$($dh[$i].values[$_]) new=$($dn[$i].values[$_])" }) -join "; ")
            Check "cmp $($case.name) r$($i + 1): recipe_vars empty, save null" ($dn[$i].values["recipe_vars"] -eq '""' -and $dn[$i].values["save"] -eq "null")
            # this copy has no perfreport.py next to it (and an empty python.exe): no numbers, the reason in summary.txt
            $perfLines = @($dn[$i].summary | Where-Object { $_ -like "perf*" })
            $launched = $null -ne $case.game
            Check "cmp $($case.name) r$($i + 1): perf null, $(if ($launched) { 'one perf line (not measured)' } else { 'no perf line (no game)' })" ($dn[$i].values["perf"] -eq "null" -and $(if ($launched) { $perfLines.Count -eq 1 -and $perfLines[0] -eq "perf: not measured (no .venv or no perfreport.py)" } else { $perfLines.Count -eq 0 })) ($perfLines -join " / ")
            $sumNew = @($dn[$i].summary | Where-Object { $_ -notlike "perf*" })
            Check "cmp $($case.name) r$($i + 1): summary.txt as before (but the perf line)" (($dh[$i].summary -join "`n") -ceq ($sumNew -join "`n")) ("head: " + ($dh[$i].summary -join " / ") + " | new: " + ($dn[$i].summary -join " / "))
            # the samplers' files: nvidia-smi's gpu.csv, PresentMon's output (or its "access denied"), perf-context.json,
            # monado's stats when a VR session happened to run
            $filesNew = @($dn[$i].files | Where-Object { $_ -notmatch '^\\(gpu\.csv|presentmon[^\\]*|perf-context\.json|monado-frames\.csv)$' })
            Check "cmp $($case.name) r$($i + 1): same files in the run folder (but the perf samplers')" (($dh[$i].files -join ",") -eq ($filesNew -join ",")) ("head: " + ($dh[$i].files -join ",") + " | new: " + ($dn[$i].files -join ","))
            if ($launched) { Check "cmp $($case.name) r$($i + 1): perf-context.json written" ($dn[$i].files -contains "\perf-context.json") ($dn[$i].files -join ",") }
        }
        $norm = @{}
        # times and pids differ between two runs (exit@3s / exit@3.1s); the new harness adds its perf line
        foreach ($k in $cmp.Keys) { $norm[$k] = @($outputs[$k].out | Where-Object { $_ -notlike "perf*" } | ForEach-Object { ($_.Replace($cmp[$k], "<dir>")) -replace '@-?[\d.]+s', '@s' -replace '\d+', '#' }) -join "`n" }
        Check "cmp $($case.name): console output as before (but the perf line)" ($norm.head -ceq $norm.new) ("head:`n" + $norm.head + "`nnew:`n" + $norm.new)
    }
    Remove-Item Env:\MOPIC_FAKE_GAME -ErrorAction SilentlyContinue

    Write-Host "== 5. run-test.ps1 with -SaveFile, end to end (no game: a process that never starts, or the stand-in)"
    $e2eSgDir = Join-Path $Work "e2e\SaveGames\{sid64}"
    $e2eSg = Join-Path $Work "e2e\SaveGames\$($real.sid64)"
    New-Item -ItemType Directory -Force -Path $e2eSg | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $e2eSg "ArchiveSaveFile.9.sav"), [byte[]](5, 5))
    [System.IO.File]::WriteAllBytes((Join-Path $e2eSg "ArchiveSaveFile.8.sav"), [byte[]](8, 8))
    [System.IO.File]::WriteAllBytes((Join-Path $e2eSg "ArchiveSaveFile.2.sav"), [byte[]](2, 2))
    $slot9 = Join-Path $e2eSg "ArchiveSaveFile.9.sav"
    $slot8 = Join-Path $e2eSg "ArchiveSaveFile.8.sav"
    function New-Recipe([string]$name, $vars, [bool]$withSlot = $true, [string]$slotDir = $e2eSgDir, $steps = $null, [string]$proc = "mopicselftest_noproc") {
        $path = Join-Path $Work "$name.json"
        $rec = [ordered]@{ game = "selftest"; process = $proc; source = "window"; window_timeout = 30 }
        if ($null -ne $vars) { $rec.vars = $vars }
        if ($withSlot) { $rec.save_slot = [ordered]@{ dir = $slotDir; file = "ArchiveSaveFile.9.sav" } }
        $rec.checkpoints = @{}
        $rec.steps = $(if ($steps) { $steps } else { @(@{ wait = 1 }, @{ expect_exit = 5 }) })
        Write-Json $path $rec
        return $path
    }
    $recipeOk = New-Recipe "e2e-recipe" ([ordered]@{ play_s = 60; keys = "w" })
    $recipeReq = New-Recipe "e2e-recipe-req" ([ordered]@{ need = $null; play_s = 60 })
    $recipeBad = New-Recipe "e2e-recipe-noslotdir" $null $true (Join-Path $Work "e2e\NoSuchFolder\{sid64}")
    $recipeNoSlot = New-Recipe "e2e-recipe-noslot" $null $false
    $recipeGame = New-Recipe "e2e-recipe-game" ([ordered]@{ rung = $null; cps = @("x"); dirv = ""; q = ""; cn = ""; empty = "default"; def1 = 5 }) $true $e2eSgDir @(
        [ordered]@{ note = "rung `${rung} `${cn}" }, [ordered]@{ phase = "exit" }, [ordered]@{ expect_exit = 30 }) "mopicselftest_game"
    $baseNo = @("-File", (Join-Path $Tools "run-test.ps1"), "-Game", "Custom", "-ProcessName", "mopicselftest_noproc",
        "-LaunchTarget", "C:\Windows\System32\whoami.exe", "-NoInject", "-LaunchTimeout", "5", "-EngineDir", $EngineDirReal)
    $baseGame = @("-File", (Join-Path $Tools "run-test.ps1"), "-Game", "Custom", "-ProcessName", "mopicselftest_game",
        "-LaunchTarget", $GameExe, "-NoInject", "-LaunchTimeout", "30", "-EngineDir", $EngineDirReal)

    # one harness call; its run folders are moved into the work folder
    function Invoke-Harness([string[]]$base, [string[]]$extra, [string]$label, [string]$gameMode = "", [string]$touch = "") {
        if ($gameMode) { $env:MOPIC_FAKE_GAME = $gameMode } else { Remove-Item Env:\MOPIC_FAKE_GAME -ErrorAction SilentlyContinue }
        if ($touch) { $env:MOPIC_FAKE_GAME_TOUCH = $touch } else { Remove-Item Env:\MOPIC_FAKE_GAME_TOUCH -ErrorAction SilentlyContinue }
        $start = Get-Date
        $res = Invoke-PS ($base + $extra + @("-Label", $label))
        Remove-Item Env:\MOPIC_FAKE_GAME, Env:\MOPIC_FAKE_GAME_TOUCH -ErrorAction SilentlyContinue
        $dirs = @(Get-ChildItem -LiteralPath $runsRoot -Directory | Where-Object { $_.Name -like "*-Custom-$label-r*" -and $_.CreationTime -ge $start.AddSeconds(-2) } | Sort-Object Name)
        $moved = @()
        foreach ($d in $dirs) { $dest = Join-Path $Work "harness-runs\$($d.Name)"; New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null; Move-Item -LiteralPath $d.FullName -Destination $dest; $moved += $dest }
        $results = @($moved | ForEach-Object { $rp = Join-Path $_ "result.json"; if (Test-Path -LiteralPath $rp) { Read-Json $rp } else { $null } })
        return [pscustomobject]@{ code = $res.code; out = $res.out; flat = $res.flat; text = $res.text; dirs = $moved; results = $results }
    }
    function Show-Out($h) { return (($h.out | Where-Object { $_ -match "^(Save|Vars|PASS|CRASH|FREEZE|EXIT_|MENU_|NO_|LAUNCH|HARNESS|save|vars|pilot|  - )" }) -join " / ") }

    $h = Invoke-Harness $baseNo @("-Recipe", $recipeOk, "-SaveFile", $src, "-RecipeVars", "play_s=1;keys=w a s d", "-Runs", "2") "selftest-e2e"
    Write-Host ("      harness: exit $($h.code); " + (Show-Out $h))
    Check "e2e: two run folders" ($h.dirs.Count -eq 2) $h.text
    Check "e2e: exit code 1 (LAUNCH_FAILED is not a pass)" ($h.code -eq 1)
    foreach ($d in $h.dirs) {
        $r = Read-Json (Join-Path $d "result.json")
        $n = Split-Path -Leaf $d
        Check "e2e $n verdict LAUNCH_FAILED" ($r.verdict -eq "LAUNCH_FAILED") $r.verdict
        Check "e2e $n result.json save source/sha256/target" ($r.save.source -eq $src -and $r.save.source_sha256 -eq $srcHash -and $r.save.target -eq $slot9) ($r.save | ConvertTo-Json -Compress)
        Check "e2e $n result.json save.after (copied in finally)" ($r.save.after.size -eq 65536 -and $r.save.after.changed -eq $false -and (Test-Path -LiteralPath (Join-Path $d "save-after\ArchiveSaveFile.9.sav")))
        Check "e2e $n recipe_vars recorded" ($r.recipe_vars -eq "play_s=1;keys=w a s d")
        $sum = Get-Content -LiteralPath (Join-Path $d "summary.txt") -Encoding UTF8
        Check "e2e $n summary.txt save lines" ((@($sum | Where-Object { $_ -like "save: $src (sha256 $srcHash) -> *ArchiveSaveFile.9.sav" }).Count -eq 1) -and (@($sum | Where-Object { $_ -like "save after: 65536 B, unchanged*" }).Count -eq 1)) ($sum -join " / ")
    }
    if ($h.dirs.Count -ge 1) { Check "e2e run 1 kept the old slot file in save-before" (((Get-Content -LiteralPath (Join-Path $h.dirs[0] "save-before\ArchiveSaveFile.9.sav") -Encoding Byte) -join ",") -eq "5,5") }
    Check "e2e: other save in the folder untouched" (((Get-Content -LiteralPath (Join-Path $e2eSg "ArchiveSaveFile.2.sav") -Encoding Byte) -join ",") -eq "2,2")
    Check "e2e: config.txt of the fake process cleaned up" (-not (Test-Path (Join-Path $env:APPDATA "UnrealVRMod\mopicselftest_noproc\config.txt")))

    $h = Invoke-Harness $baseNo @("-Recipe", $recipeBad, "-SaveFile", $src) "selftest-e2e-nodir"
    $r = $h.results | Select-Object -First 1
    Check "e2e: missing save folder -> HARNESS_ERROR result" ($r -and $r.verdict -eq "HARNESS_ERROR" -and (@($r.notes) -join ";") -like "*could not install the save: save folder not found*" -and $r.save.error) $h.text
    Check "e2e: missing save folder -> folder not created" (-not (Test-Path (Join-Path $Work "e2e\NoSuchFolder")))

    foreach ($case in @(
            @{ name = "-SaveFile without -Recipe or -SaveSlot"; argv = @("-SaveFile", $src); msg = "*needs a recipe with a*save_slot*-SaveSlot*" },
            @{ name = "-SaveFile with a recipe without save_slot"; argv = @("-Recipe", $recipeNoSlot, "-SaveFile", $src); msg = "*needs a recipe with a*save_slot*" },
            @{ name = "-SaveFile that doesn't exist"; argv = @("-Recipe", $recipeOk, "-SaveFile", "C:\nope\x.sav"); msg = "*-SaveFile not found*" },
            @{ name = "-SaveAs without -SaveFile"; argv = @("-Recipe", $recipeOk, "-SaveAs", "a.sav"); msg = "*-SaveAs and -SaveSlot need -SaveFile*" },
            @{ name = "-SaveSlot without -SaveFile"; argv = @("-SaveSlot", "C:\x\a.sav"); msg = "*-SaveAs and -SaveSlot need -SaveFile*" },
            @{ name = "-SaveAs with a path"; argv = @("-Recipe", $recipeOk, "-SaveFile", $src, "-SaveAs", "..\a.sav"); msg = "*plain file name*" },
            @{ name = "-SaveSlot without a folder"; argv = @("-SaveFile", $src, "-SaveSlot", "a.sav"); msg = "*needs a*dir*" },
            @{ name = "-SaveSlot with an unknown placeholder"; argv = @("-SaveFile", $src, "-SaveSlot", "C:\{nope}\a.sav"); msg = "*unknown placeholder {nope}*" },
            @{ name = "-RecipeVars without -Recipe"; argv = @("-RecipeVars", "a=1"); msg = "*-RecipeVars needs -Recipe*" },
            @{ name = "-RecipeVars malformed"; argv = @("-Recipe", $recipeOk, "-RecipeVars", "oops"); msg = "*name=value*" },
            @{ name = "-RecipeVars undeclared var"; argv = @("-Recipe", $recipeOk, "-RecipeVars", "play_s=1;typo=2"); msg = "*declares no var typo*" },
            @{ name = "-RecipeVars name case"; argv = @("-Recipe", $recipeOk, "-RecipeVars", "Play_s=1"); msg = "*declares no var Play_s*" },
            @{ name = "-RecipeVars on a recipe without vars"; argv = @("-Recipe", $recipeNoSlot, "-RecipeVars", "a=1"); msg = "*declares no var a*none*" },
            @{ name = "a required var not given"; argv = @("-Recipe", $recipeReq, "-RecipeVars", "play_s=1"); msg = "*needs -RecipeVars for need*" },
            @{ name = "-Set malformed"; argv = @("-Recipe", $recipeOk, "-Set", "oops"); msg = "*-Set expects Key=Value*" })) {
        $h = Invoke-Harness $baseNo $case.argv "selftest-e2e-args"
        Check "e2e refuses $($case.name) before any run" ($h.code -ne 0 -and $h.dirs.Count -eq 0 -and $h.flat -like $case.msg) (($h.out | Select-Object -First 3) -join " / ")
    }
    # relative -SaveFile resolved from tools\mopic-test; Chinese and brackets in the save's name
    $rel = $src.Substring($Tools.Length + 1)
    $h = Invoke-Harness $baseNo @("-Recipe", $recipeOk, "-SaveFile", $rel) "selftest-e2e-rel"
    $r = $h.results | Select-Object -First 1
    Check "e2e: -SaveFile relative to tools\mopic-test ($rel)" ($r -and $r.save.source -eq $src) $h.text
    $cnSrc = Join-Path $srcDir ("1.0_" + $Cn + " [v2].sav")
    Copy-Item -LiteralPath $src -Destination $cnSrc
    $h = Invoke-Harness $baseNo @("-Recipe", $recipeOk, "-SaveFile", $cnSrc) "selftest-e2e-cn"
    $r = $h.results | Select-Object -First 1
    Check "e2e: -SaveFile with Chinese characters and brackets" ($r -and $r.save.source -eq $cnSrc -and $r.save.after.size -eq 65536 -and $r.save.source_sha256 -eq $srcHash) $h.text
    $recipeKeys = New-Recipe "e2e-recipe-keys" ([ordered]@{ Keys = $null; Count = 1 })
    $h = Invoke-Harness $baseNo @("-Recipe", $recipeKeys, "-SaveFile", $src, "-RecipeVars", "Keys=w a s d;Count=2") "selftest-e2e-keys"
    $r = $h.results | Select-Object -First 1
    Check "e2e: recipe vars named Keys / Count accepted" ($r -and $r.verdict -eq "LAUNCH_FAILED" -and $r.recipe_vars -eq "Keys=w a s d;Count=2") $h.text
    $h = Invoke-Harness $baseNo @("-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.9.sav", "-SaveAs", "ArchiveSaveFile.8.sav") "selftest-e2e-slot"
    $r = $h.results | Select-Object -First 1
    Check "e2e: -SaveSlot without a recipe, -SaveAs overrides its file" ($r -and $r.save.target -eq $slot8 -and $r.save.after.size -eq 65536 -and $r.recipe -eq "") $h.text
    $h = Invoke-Harness $baseNo @("-Recipe", $recipeOk, "-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.8.sav") "selftest-e2e-slot2"
    $r = $h.results | Select-Object -First 1
    Check "e2e: -SaveSlot instead of the recipe's save_slot" ($r -and $r.save.target -eq $slot8) $h.text

    Write-Host "   -- with the stand-in game"
    # vars with quotes, spaces, a trailing backslash, Chinese and an empty value reach the pilot as given
    $varText = "rung=ch1 Guangzhi;cps=[`"hud`",`"loading`"];dirv=C:\x y\;q=a`"b c;cn=$Cn x;empty="
    $h = Invoke-Harness $baseGame @("-Recipe", $recipeGame, "-SaveFile", $src, "-RecipeVars", $varText) "selftest-e2e-pilot" "window:15" $slot9
    $r = $h.results | Select-Object -First 1
    Write-Host ("      harness: exit $($h.code); " + (Show-Out $h))
    Check "e2e pilot: menu exit PASS" ($r -and $r.verdict -eq "PASS" -and $r.pilot.state -eq "done") $h.text
    if ($r -and $r.pilot) {
        $pv = $r.pilot.vars
        $want = [ordered]@{ rung = "ch1 Guangzhi"; cps = '["hud","loading"]'; dirv = "C:\x y\"; q = 'a"b c'; cn = "$Cn x"; empty = ""; def1 = "5" }
        $bad = @($want.Keys | Where-Object { $pv.$_ -cne $want[$_] })
        Check "e2e pilot: every var arrived exactly as given (+ the recipe's default)" ($bad.Count -eq 0 -and @($pv.PSObject.Properties).Count -eq $want.Count) (($bad | ForEach-Object { "$_=<$($pv.$_)>" }) -join " ")
        $plog = Get-Content -LiteralPath (Join-Path $h.dirs[0] "pilot\pilot.log") -Raw -Encoding UTF8
        Check "e2e pilot: the note step got the substituted text" ($plog.Contains("rung ch1 Guangzhi $Cn x")) $plog
    }
    Check "e2e pilot: save installed before the launch (the game's autosave is in save-after)" ($r -and $r.save.after.changed -and $r.save.after.size -eq 65544) ($r.save | ConvertTo-Json -Compress -Depth 4)

    # frame rate around the same run: nvidia-smi and PresentMon sampled while the stand-in ran, perfreport.py found no
    # measured segment (the recipe has no play step) and said so
    if ($h.dirs.Count -ge 1) {
        $d = $h.dirs[0]
        $pc = $(if (Test-Path -LiteralPath (Join-Path $d "perf-context.json")) { Read-Json (Join-Path $d "perf-context.json") } else { $null })
        Check "e2e perf: perf-context.json (game pid, clock pairs, not injected)" ($pc -and @($pc.game_pids).Count -eq 1 -and $pc.t0.qpc_ns -gt 0 -and $pc.end.qpc_ns -gt $pc.t0.qpc_ns -and $pc.injected -eq $false -and $pc.process -eq "mopicselftest_game") ($pc | ConvertTo-Json -Compress -Depth 4)
        $smi = [bool](Get-Command "nvidia-smi.exe" -ErrorAction SilentlyContinue)
        $gpuRows = @(Get-Content -LiteralPath (Join-Path $d "gpu.csv") -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\d{4}/' })
        Check "e2e perf: nvidia-smi sampled once a second into gpu.csv" ((-not $smi -and $pc.nvidia_smi.status -eq "not_found") -or ($pc.nvidia_smi.status -eq "ok" -and $gpuRows.Count -ge 5)) "status $($pc.nvidia_smi.status), $($gpuRows.Count) rows"
        $pmStatus = $pc.presentmon.status
        Write-Host "      PresentMon: $pmStatus $($pc.presentmon.error)"
        Check "e2e perf: PresentMon ran, or its refusal was recognized" ($pmStatus -in @("ok", "not_found") -or ($pmStatus -eq "access_denied" -and (Get-Content -LiteralPath (Join-Path $d "presentmon.err.txt") -Raw) -match "access denied")) "status $pmStatus, error $($pc.presentmon.error)"
        $sum = @(Get-Content -LiteralPath (Join-Path $d "summary.txt") -Encoding UTF8)
        $perfLine = @($sum | Where-Object { $_ -like "perf*" })
        Check "e2e perf: result.json perf = no measured segment, the sources, and its summary.txt line" ($r.perf -and $r.perf.note -eq "no measured segment" -and $null -eq $r.perf.headline -and $r.perf.sources.uevr -eq "not injected" -and $r.perf.sources.presentmon -eq $(if ($pmStatus -eq "ok") { "no_rows" } else { $pmStatus }) -and $perfLine.Count -eq 1 -and $perfLine[0] -like "perf: no measured segment (uevr not injected, presentmon *") (($perfLine -join " / ") + " | " + ($r.perf | ConvertTo-Json -Compress -Depth 4))
        Check "e2e perf: perf.json and perf-summary.json written" ((Test-Path -LiteralPath (Join-Path $d "perf.json")) -and (Test-Path -LiteralPath (Join-Path $d "perf-summary.json")))
    }
    $h = Invoke-Harness $baseGame @("-Seconds", "2", "-NoPerf") "selftest-e2e-noperf" "window:8"
    $r = $h.results | Select-Object -First 1
    $files = $(if ($h.dirs.Count -ge 1) { @(Get-ChildItem -LiteralPath $h.dirs[0] -Recurse | ForEach-Object { $_.Name }) } else { @() })
    Check "e2e -NoPerf: PASS, perf null, no perf line, no sampler files" ($r -and $r.verdict -eq "PASS" -and $null -eq $r.perf -and $h.flat -like "*Perf:     not measured (-NoPerf)*" -and @($files | Where-Object { $_ -match '^(gpu\.csv|presentmon|perf)' }).Count -eq 0 -and @(Get-Content -LiteralPath (Join-Path $h.dirs[0] "summary.txt") | Where-Object { $_ -like "perf*" }).Count -eq 0) (($files -join ",") + " | " + (Show-Out $h))
    $h = Invoke-Harness $baseGame @("-Seconds", "6", "-PresentMon", "off") "selftest-e2e-pmoff" "window:15"
    $r = $h.results | Select-Object -First 1
    Check "e2e -PresentMon off: no capture, an observation too short to measure" ($r -and $r.verdict -eq "PASS" -and $r.perf.sources.presentmon -eq "off" -and $r.perf.note -eq "observation too short or not timed" -and -not (Test-Path -LiteralPath (Join-Path $h.dirs[0] "presentmon.err.txt"))) ($r.perf | ConvertTo-Json -Compress -Depth 4)

    $h = Invoke-Harness $baseGame @("-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.8.sav", "-Runs", "2", "-Seconds", "20") "selftest-e2e-crash" "exit:3:C0000005" $slot8
    Check "e2e CRASH: two runs, both CRASH 0xC0000005" ($h.results.Count -eq 2 -and @($h.results | Where-Object { $_.verdict -eq "CRASH" -and $_.exit_code -eq "0xC0000005" }).Count -eq 2) (Show-Out $h)
    foreach ($i in 0..1) {
        $rr = $h.results[$i]
        Check "e2e CRASH r$($i + 1): save-after = the installed save + the game's autosave" ($rr.save.after.changed -and $rr.save.after.size -eq 65544 -and (Test-Path -LiteralPath (Join-Path $h.dirs[$i] "save-after\ArchiveSaveFile.8.sav"))) ($rr.save.after | ConvertTo-Json -Compress)
    }
    if ($h.dirs.Count -eq 2) {
        Check "e2e CRASH: run 2 reinstalled the save over run 1's autosave (save-before r2 = save-after r1)" ((Get-Hash (Join-Path $h.dirs[1] "save-before\ArchiveSaveFile.8.sav")) -eq (Get-Hash (Join-Path $h.dirs[0] "save-after\ArchiveSaveFile.8.sav")))
    }

    $h = Invoke-Harness $baseGame @("-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.8.sav", "-Seconds", "60", "-HangSeconds", "2") "selftest-e2e-freeze" "freeze:120" $slot8
    $r = $h.results | Select-Object -First 1
    Check "e2e FREEZE: verdict, save-after copied" ($r -and $r.verdict -eq "FREEZE" -and $r.save.after.size -eq 65544) (Show-Out $h)
    $h = Invoke-Harness $baseGame @("-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.8.sav", "-Seconds", "20", "-WaitForExit") "selftest-e2e-exitcrash" "exit:3:C0000409" $slot8
    $r = $h.results | Select-Object -First 1
    Check "e2e EXIT_CRASH: verdict, save-after copied" ($r -and $r.verdict -eq "EXIT_CRASH" -and $r.save.after.size -eq 65544) (Show-Out $h)
    $baseErr = @("-File", (Join-Path $Tools "run-test.ps1"), "-Game", "Custom", "-ProcessName", "mopicselftest_game",
        "-LaunchTarget", (Join-Path $Work "nope\missing.exe"), "-NoInject", "-LaunchTimeout", "5", "-EngineDir", $EngineDirReal)
    $h = Invoke-Harness $baseErr @("-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.8.sav") "selftest-e2e-harnesserr"
    $r = $h.results | Select-Object -First 1
    Check "e2e HARNESS_ERROR (launch threw): verdict, save-after copied" ($r -and $r.verdict -eq "HARNESS_ERROR" -and $r.save.after.size -eq 65536 -and -not $r.save.after.changed) (Show-Out $h)
    $h = Invoke-Harness $baseGame @("-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.8.sav", "-Seconds", "2", "-KeepGame") "selftest-e2e-keep" "window:90" $slot8
    $r = $h.results | Select-Object -First 1
    $kept = @(Get-Process -Name "mopicselftest_game" -ErrorAction SilentlyContinue)
    Check "e2e -KeepGame: PASS, game kept, save-after copied while it runs (noted)" ($r -and $r.verdict -eq "PASS" -and $kept.Count -eq 1 -and $r.save.after.size -eq 65544 -and (@($r.notes) -join ";") -like "*copied while the game was still running*") (Show-Out $h)
    $kept | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
    $h = Invoke-Harness $baseGame @("-SaveFile", $src, "-SaveSlot", "$e2eSgDir\ArchiveSaveFile.8.sav", "-Seconds", "2", "-GracefulExit") "selftest-e2e-exithang" "noclose:150" $slot8
    $r = $h.results | Select-Object -First 1
    Check "e2e EXIT_HANG (-GracefulExit, WM_CLOSE refused): verdict, save-after copied" ($r -and $r.verdict -eq "EXIT_HANG" -and $r.save.after.size -eq 65544) (Show-Out $h)
    Check "e2e: no stand-in game left running" (@(Get-Process -Name $fakeNames -ErrorAction SilentlyContinue).Count -eq 0)
    Check "e2e: no nvidia-smi / PresentMon left running by the harness" ((Get-SamplerCount) -eq $samplersBefore) "before $samplersBefore, now $(Get-SamplerCount)"
    Check "e2e: the other save in the folder untouched by all of it" (((Get-Content -LiteralPath (Join-Path $e2eSg "ArchiveSaveFile.2.sav") -Encoding Byte) -join ",") -eq "2,2")
    Check "e2e: config.txt / user_script.txt of the stand-ins cleaned up" (@($fakeNames | Where-Object { (Test-Path (Join-Path $env:APPDATA "UnrealVRMod\$_\config.txt")) -or (Test-Path (Join-Path $env:APPDATA "UnrealVRMod\$_\user_script.txt")) }).Count -eq 0)
}
foreach ($p in $persistentDirs) { if ((Test-Path -LiteralPath $p) -and @(Get-ChildItem -LiteralPath $p -Force).Count -eq 0) { Remove-Item -LiteralPath $p } }

Write-Host "== 6. run-ladder.ps1"
$ladderSaves = Join-Path $Work "ladder saves"
New-Item -ItemType Directory -Force -Path $ladderSaves | Out-Null
foreach ($n in @("one", "two", "three", "four")) { [System.IO.File]::WriteAllBytes((Join-Path $ladderSaves "$n.sav"), [byte[]](1..(10 * ($n.Length)))) }
$cnSave = Join-Path $ladderSaves ("4.0_" + $Cn + ".sav")
[System.IO.File]::WriteAllBytes($cnSave, [byte[]](1..40))
$cnHash = (Get-Hash $cnSave).ToLowerInvariant()
$oneSave = Join-Path $ladderSaves "one.sav"
$oneHash = (Get-Hash $oneSave).ToLowerInvariant()
$slotAny = [ordered]@{ dir = "{gamedir}\b1\Saved\SaveGames\{sid64}"; file = "ArchiveSaveFile.9.sav" }
$ladderVarsDecl = [ordered]@{ play_s = 60; keys = "w"; pos = @(0, 0); fast = $false; nsf = 2; verdict = "PASS"; nores = 0; cps = @("x"); dirv = ""; skipme = 1 }
$recipeSave = Join-Path $Work "Wukong-save.json"
$recipeOther = Join-Path $Work "Wukong-other.json"
$recipeNeed = Join-Path $Work "Wukong-need.json"
$recipeLadderNoSlot = Join-Path $Work "Wukong-noslot.json"
Write-Json $recipeSave ([ordered]@{ game = "selftest"; vars = $ladderVarsDecl; save_slot = $slotAny; checkpoints = @{}; steps = @(@{ expect_exit = 5 }) })
Write-Json $recipeOther ([ordered]@{ game = "selftest"; save_slot = $slotAny; checkpoints = @{}; steps = @(@{ expect_exit = 5 }) })
Write-Json $recipeNeed ([ordered]@{ game = "selftest"; vars = [ordered]@{ need = $null }; save_slot = $slotAny; checkpoints = @{}; steps = @(@{ expect_exit = 5 }) })
Write-Json $recipeLadderNoSlot ([ordered]@{ game = "selftest"; checkpoints = @{}; steps = @(@{ expect_exit = 5 }) })
$relTwo = (Join-Path $ladderSaves "two.sav").Substring($Tools.Length + 1)
$ladderFile = Join-Path $Work "ladder.json"
Write-Json $ladderFile ([ordered]@{ game = "Wukong"; recipe = $recipeSave; rungs = @(
    [ordered]@{ name = "1.1.1_one"; save = $oneSave; sha256 = $oneHash; vars = [ordered]@{ play_s = 180; keys = "w a s d"; pos = @(110, 305); fast = $true; nsf = 3; cps = @("hud", "loading"); dirv = "C:\x y\"; skipme = $null }; tier = 1; note = "ignored" },
    [ordered]@{ name = "ch2 two"; save = $relTwo; tier = 2; recipe = $recipeOther },
    [ordered]@{ name = "ch3-three"; save = (Join-Path $ladderSaves "three.sav"); vars = [ordered]@{ verdict = "HARNESS_ERROR" } },
    [ordered]@{ name = "ch4-four"; save = $cnSave; sha256 = $cnHash; tier = 1 }) })
$oneVars = 'play_s=180;keys=w a s d;pos=[110,305];fast=true;nsf=3;cps=["hud","loading"];dirv=C:\x y\'
$ladderPs = Join-Path $Tools "run-ladder.ps1"
$fakeHarness = Join-Path $Here "fake-harness.ps1"
function Invoke-Ladder([string]$file, [string[]]$extra) { return Invoke-PS (@("-File", $ladderPs, "-Game", "Wukong", "-Ladder", $file) + $extra) }
function Get-RungLines($l) { return @($l.out | Where-Object { $_ -like "=== *" }) }
$l = Invoke-Ladder $ladderFile @("-DryRun", "-EngineDir", $EngineDirReal)
Write-Host ("      dry run:`n        " + ($l.out -join "`n        "))
Check "ladder dry run: exit 0, 4 rungs" ($l.code -eq 0 -and (Get-RungLines $l).Count -eq 4) $l.text
Check "ladder dry run: vars as -RecipeVars (JSON list, trailing backslash, null var left out)" ($l.text.Contains('-RecipeVars "play_s=180;keys=w a s d;pos=[110,305];fast=true;nsf=3;cps=[\"hud\",\"loading\"];dirv=C:\x y\\"')) $l.text
Check "ladder dry run: relative save resolved, rung recipe used" ($l.text -like "*-Recipe $recipeOther -SaveFile `"$(Join-Path $ladderSaves 'two.sav')`"*") $l.text
Check "ladder dry run: labels from rung names" ($l.text -like "*-Label ladder-1.1.1_one*" -and $l.text -like "*-Label ladder-ch2_two*")
Check "ladder dry run: -EngineDir passed" ($l.text -like "*-EngineDir $EngineDirReal*")
$l = Invoke-Ladder $ladderFile @("-DryRun", "-Tier", "1")
Check "ladder -Tier 1 skips the tier-2 rung" (((Get-RungLines $l) -join "|") -notlike "*ch2 two*" -and (Get-RungLines $l).Count -eq 3) $l.text
$l = Invoke-Ladder $ladderFile @("-DryRun", "-From", "ch3-three")
Check "ladder -From" (((Get-RungLines $l) | ForEach-Object { ($_ -split " ")[2] }) -join "," -eq "ch3-three,ch4-four") $l.text
$l = Invoke-Ladder $ladderFile @("-DryRun", "-Only", "ch4-four,1.1.1_one")
Check "ladder -Only (file order)" (((Get-RungLines $l) | ForEach-Object { ($_ -split " ")[2] }) -join "," -eq "1.1.1_one,ch4-four") $l.text
$l = Invoke-Ladder $ladderFile @("-DryRun", "-Only", "nope")
Check "ladder -Only unknown rung refused" ($l.code -ne 0 -and $l.flat -like "*no rung named 'nope'*") $l.text
$l = Invoke-Ladder $ladderFile @("-DryRun", "-From", "nope")
Check "ladder -From unknown rung refused" ($l.code -ne 0 -and $l.flat -like "*-From: no rung named 'nope'*") $l.text

# refused before the first launch (the stand-in harness would create the runs folder)
$refusals = @(
    @{ name = "a missing save"; rungs = @([ordered]@{ name = "a"; save = $oneSave }, [ordered]@{ name = "b"; save = "saves\nope.sav" }); msg = "*rung b: save not found*" },
    @{ name = "colliding rung names"; rungs = @([ordered]@{ name = "a b"; save = $oneSave }, [ordered]@{ name = "a_b"; save = $oneSave }); msg = "*same run folder name*" },
    @{ name = "a wrong sha256"; rungs = @([ordered]@{ name = "a"; save = $oneSave; sha256 = ("0" * 64) }); msg = "*rung a:*has sha256*the ladder says 0000*" },
    @{ name = "a missing recipe"; recipe = "NoSuchRecipe-selftest"; rungs = @([ordered]@{ name = "a"; save = $oneSave }); msg = "*recipe not found*NoSuchRecipe-selftest.json*" },
    @{ name = "a recipe without save_slot"; recipe = $recipeLadderNoSlot; rungs = @([ordered]@{ name = "a"; save = $oneSave }); msg = '*rung a: recipe*has no "save_slot"*' },
    @{ name = "a var the recipe doesn't declare"; rungs = @([ordered]@{ name = "a"; save = $oneSave }, [ordered]@{ name = "b"; save = $oneSave; vars = @{ typo = 1 } }); msg = "*rung b: recipe*declares no var typo*" },
    @{ name = "a required var not given"; recipe = $recipeNeed; rungs = @([ordered]@{ name = "a"; save = $oneSave; vars = [ordered]@{ need = $null } }); msg = "*rung a: recipe*needs vars need*" },
    @{ name = "a var with ';'"; rungs = @([ordered]@{ name = "a"; save = $oneSave; vars = @{ keys = "a;b" } }); msg = "*can't contain ';'*" })
$refNo = 0
foreach ($case in $refusals) {
    $refNo++
    $file = Join-Path $Work "ladder-refuse$refNo.json"
    $recipeForCase = $(if ($case.recipe) { $case.recipe } else { $recipeSave })
    Write-Json $file ([ordered]@{ game = "Wukong"; recipe = $recipeForCase; rungs = $case.rungs })
    $rd = Join-Path $Work "ladder-runs-refuse$refNo"
    $l = Invoke-Ladder $file @("-Harness", $fakeHarness, "-RunsDir", $rd)
    Check "ladder with $($case.name) refused before any run" ($l.code -ne 0 -and $l.flat -like $case.msg -and -not (Test-Path -LiteralPath $rd)) $l.text
}
$l = Invoke-Ladder (Join-Path $Work "ladder-refuse4.json") @("-DryRun")
Check "ladder -DryRun only warns about a missing recipe" ($l.code -eq 0 -and $l.flat -like "*WARNING*recipe not found*") $l.text

# full ladder with the stand-in harness: stops after the HARNESS_ERROR rung
$env:MOPIC_SELFTEST_RUNS = Join-Path $Work "ladder-runs"
New-Item -ItemType Directory -Force -Path $env:MOPIC_SELFTEST_RUNS | Out-Null
function Get-NewestLadder([string]$ext) { return (Get-ChildItem -LiteralPath $env:MOPIC_SELFTEST_RUNS -Filter "ladder-*-Wukong$ext" | Sort-Object LastWriteTime | Select-Object -Last 1) }
function Get-RunResult([string]$like) { $d = Get-ChildItem -LiteralPath $env:MOPIC_SELFTEST_RUNS -Directory | Where-Object { $_.Name -like $like } | Select-Object -First 1; if ($d) { return Read-Json (Join-Path $d.FullName "result.json") }; return $null }
$l = Invoke-Ladder $ladderFile @("-Harness", $fakeHarness, "-RunsDir", $env:MOPIC_SELFTEST_RUNS, "-EngineDir", $EngineDirReal, "-Label", "lt")
Write-Host ("      ladder output:`n        " + ($l.out -join "`n        "))
$md = @(Get-ChildItem -LiteralPath $env:MOPIC_SELFTEST_RUNS -Filter "ladder-*-Wukong.md")
Check "ladder table written" ($md.Count -eq 1)
if ($md.Count -eq 1) {
    $mdText = Get-Content -LiteralPath $md[0].FullName -Encoding UTF8
    $rowsMd = @($mdText | Where-Object { $_ -like "| 1.1.1_one*" -or $_ -like "| ch*" })
    Check "ladder: 3 rows (stopped after ch3-three, ch4-four not run)" ($rowsMd.Count -eq 3 -and ($mdText -join "`n") -notlike "*ch4-four*") ($rowsMd -join " / ")
    Check "ladder: NSF active count from log.txt" ($rowsMd[0] -like "| 1.1.1_one | 1 | 1 | PASS | 0 | 0x00000000 | 3 | 40 B (changed) | 3 |*") $rowsMd[0]
    Check "ladder: tier-2 rung row" ($rowsMd[1] -like "| ch2 two | 2 | 1 | PASS | 0 | 0x00000000 | 2 | 40 B (changed) | 3 |*") $rowsMd[1]
    Check "ladder: HARNESS_ERROR row, harness exit 1" ($rowsMd[2] -like "| ch3-three | 1 | 1 | HARNESS_ERROR | 1 |*") $rowsMd[2]
    Check "ladder: pipes in notes escaped, PostInitProperties warning filtered" ($rowsMd[0] -like "*fake note / with a pipe |*" -and $rowsMd[0] -notlike "*PostInitProperties*") $rowsMd[0]
    Check "ladder: VR fps / 1% low / hitches from result.json's perf headline, empty without one" ($rowsMd[0] -like "*| 40 B (changed) | 3 | 47.8 | 30.0 | 2 | fake note*" -and $rowsMd[2] -like "*| HARNESS_ERROR | 1 | *(changed) | 3 |  |  |  | fake note*") ($rowsMd[0] + " / " + $rowsMd[2])
    Check "ladder: stop noted, exit 1" ((($mdText -join "`n") -like "*Stopped early: ch3-three ended with HARNESS_ERROR.*") -and $l.code -eq 1) ($mdText -join " / ")
    $js = Read-Json ([System.IO.Path]::ChangeExtension($md[0].FullName, ".json"))
    Check "ladder json: 3 rows" (@($js).Count -eq 3)
    $logFile = [System.IO.Path]::ChangeExtension($md[0].FullName, ".log")
    Check "ladder .log has each harness command and its output" ((Test-Path -LiteralPath $logFile) -and @(Select-String -LiteralPath $logFile -Pattern '^> powershell.exe ').Count -eq 3 -and @(Select-String -LiteralPath $logFile -Pattern '^PASS  Wukong/lt-1.1.1_one run 1').Count -eq 1)
    $recv1 = Get-RunResult "*-Wukong-lt-1.1.1_one-r1"
    Check "ladder passed the vars exactly (JSON list quotes, trailing backslash)" ($recv1 -and $recv1.received.RecipeVars -ceq $oneVars) "<$($recv1.received.RecipeVars)>"
    $recv2 = Get-RunResult "*-Wukong-lt-ch2_two-r1"
    Check "ladder passed save with spaces, rung recipe, engine dir, no -RecipeVars" ($recv2.received.save_exists -and $recv2.received.Recipe -eq $recipeOther -and $recv2.received.EngineDir -eq $EngineDirReal -and $recv2.received.RecipeVars -eq "") ($recv2.received | ConvertTo-Json -Compress)
}
$l = Invoke-Ladder $ladderFile @("-Harness", $fakeHarness, "-RunsDir", $env:MOPIC_SELFTEST_RUNS, "-Only", "ch4-four", "-Runs", "2", "-Label", "lt2")
$rows2 = @(Get-Content -LiteralPath (Get-NewestLadder ".md").FullName -Encoding UTF8 | Where-Object { $_ -like "| ch4-four*" })
Check "ladder -Runs 2: one row per run, exit 0 when all pass" ($rows2.Count -eq 2 -and $l.code -eq 0) (($rows2 -join " / ") + " exit $($l.code)")
$recvCn = Get-RunResult "*-Wukong-lt2-ch4-four-r1"
Check "ladder passed the Chinese save path intact, -Runs 2" ($recvCn.received.save_exists -and $recvCn.received.SaveFile -ceq $cnSave -and $recvCn.received.Runs -eq 2) ($recvCn.received | ConvertTo-Json -Compress)

# a rung whose harness writes no result (stops with an error): HARNESS_ERROR row with the message, stop
$ladderNores = Join-Path $Work "ladder-nores.json"
Write-Json $ladderNores ([ordered]@{ game = "Wukong"; recipe = $recipeSave; rungs = @([ordered]@{ name = "x"; save = $oneSave; vars = @{ nores = 1 } }, [ordered]@{ name = "y"; save = $oneSave }) })
$l = Invoke-Ladder $ladderNores @("-Harness", $fakeHarness, "-RunsDir", $env:MOPIC_SELFTEST_RUNS, "-Label", "lt3")
Check "ladder: no result.json -> HARNESS_ERROR row with the harness's error, stop" ($l.flat -like "*| x | 1 |  | HARNESS_ERROR | 1 |*wrote no result.json (exit code 1): fake harness refused lt3-x (nores)*" -and $l.text -notlike "*| y |*" -and $l.code -eq 1) $l.text
# NO_VR stops the ladder by default, -StopOn replaces the list
$ladderNoVr = Join-Path $Work "ladder-novr.json"
Write-Json $ladderNoVr ([ordered]@{ game = "Wukong"; recipe = $recipeSave; rungs = @([ordered]@{ name = "v"; save = $oneSave; vars = @{ verdict = "NO_VR" } }, [ordered]@{ name = "w"; save = $oneSave }) })
$l = Invoke-Ladder $ladderNoVr @("-Harness", $fakeHarness, "-RunsDir", $env:MOPIC_SELFTEST_RUNS, "-Label", "lt4")
Check "ladder: NO_VR stops it by default" ($l.flat -like "*| v | 1 | 1 | NO_VR |*Stopped early: v ended with NO_VR.*" -and $l.text -notlike "*| w |*" -and $l.code -eq 1) $l.text
$l = Invoke-Ladder $ladderNoVr @("-Harness", $fakeHarness, "-RunsDir", $env:MOPIC_SELFTEST_RUNS, "-Label", "lt5", "-StopOn", "HARNESS_ERROR")
Check "ladder: -StopOn HARNESS_ERROR goes on after NO_VR" ($l.flat -like "*| v | 1 | 1 | NO_VR |*| w | 1 | 1 | PASS |*" -and $l.flat -notlike "*Stopped early*" -and $l.code -eq 1) $l.text
Remove-Item Env:\MOPIC_SELFTEST_RUNS

# the real ladder file (dry run only: checks its saves' sha256)
$l = Invoke-PS @("-File", $ladderPs, "-Game", "Wukong", "-DryRun", "-Tier", "1", "-EngineDir", $EngineDirReal)
Check "ladders\Wukong.json -Tier 1 dry run: 7 rungs, saves verified" ($l.code -eq 0 -and (Get-RungLines $l).Count -eq 7 -and $l.flat -like "*-EngineDir $EngineDirReal*") $l.text

Write-Host "== 7. gamepilot.py"
& { $ErrorActionPreference = "Continue"; & $Py -W error -m py_compile (Join-Path $Tools "gamepilot.py") 2>&1 | ForEach-Object { Write-Host "      $_" } }
Check "py_compile gamepilot.py (warnings as errors)" ($LASTEXITCODE -eq 0)
& { $ErrorActionPreference = "Continue"; & $Py -W error -m py_compile (Join-Path $Here "selftest_vars.py") 2>&1 | ForEach-Object { Write-Host "      $_" } }
$pyOut = @(& { $ErrorActionPreference = "Continue"; & $Py (Join-Path $Here "selftest_vars.py") (Join-Path $Work "pilot") 2>&1 | ForEach-Object { "$_" } })
$pyOut | Where-Object { $_ -notlike "ok *" } | ForEach-Object { Write-Host "  $_" }
$pyOk = @($pyOut | Where-Object { $_ -like "ok *" }).Count
Check "gamepilot --var self-test ($pyOk checks passed)" ($LASTEXITCODE -eq 0 -and $pyOk -gt 0)

Write-Host "== 7b. frame rate: gamepilot's measured segments, perfreport.py on synthetic runs"
foreach ($f in @((Join-Path $Tools "perfreport.py"), (Join-Path $Here "selftest_perf.py"))) {
    & { $ErrorActionPreference = "Continue"; & $Py -W error -m py_compile $f 2>&1 | ForEach-Object { Write-Host "      $_" } }
    Check "py_compile $(Split-Path -Leaf $f) (warnings as errors)" ($LASTEXITCODE -eq 0)
}
$pyOut = @(& { $ErrorActionPreference = "Continue"; & $Py (Join-Path $Here "selftest_perf.py") (Join-Path $Work "perf") 2>&1 | ForEach-Object { "$_" } })
# (the stubbed pilot logs its steps to stdout too: only the failures and the two example lines)
$pyOut | Where-Object { $_ -like "FAIL*" -or $_ -like "  line: *" -or $_ -like "Traceback*" -or $_ -match "Error" } | ForEach-Object { Write-Host "  $_" }
$pyOk = @($pyOut | Where-Object { $_ -like "ok *" }).Count
Check "frame-rate self-test ($pyOk checks passed)" ($LASTEXITCODE -eq 0 -and $pyOk -gt 0)

Write-Host "== 8. the real save folder"
Check "real Wukong save folder unchanged by the self-test" ((Get-Listing $RealSaveDir) -join "`n" -eq ($realBefore -join "`n"))

Write-Host ""
Write-Host "self-test: $($script:passes) passed, $($script:fails) failed (work folder $Work)"
if ($script:fails -gt 0) { exit 1 }
exit 0
