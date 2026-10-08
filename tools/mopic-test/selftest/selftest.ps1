# Self-test of the save-install / recipe vars / ladder / frame-rate harness changes. Launches no game: where a run
# needs a game process, a stand-in (fakegame.cs, compiled into the work folder) plays it; the frame-rate samplers
# (nvidia-smi, the PresentMon console app when installed) run around it as in a real run. Never touches the real save folder:
# every save file lives under runs\selftest\work-<time>\ (gitignored). Stops nothing but its own stand-ins (and, through the
# harness, UEVRInjector / CrashReportClient, so it refuses to run while one of those is running).
# -NoWindows skips sections 4-5 (the stand-in with its invisible window, whoami.exe consoles): for a PC someone is using.
# The -GameIni end-to-end section (5b) still runs: its stand-in never opens a window.
param([switch]$NoWindows)
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
foreach ($f in @("run-test.ps1", "run-ladder.ps1", "run-matrix.ps1", "selftest\fake-harness.ps1", "selftest\selftest.ps1", "plans\hogwarts-fg-nsf.ps1")) {
    $tokens = $null; $errs = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Tools $f), [ref]$tokens, [ref]$errs)
    Check "parse $f ($(@($errs).Count) errors)" (@($errs).Count -eq 0) ((@($errs) | ForEach-Object { "line $($_.Extent.StartLineNumber): $($_.Message)" }) -join "; ")
}
foreach ($f in @("run-test.ps1", "run-ladder.ps1", "run-matrix.ps1", "selftest\selftest.ps1", "selftest\fake-harness.ps1", "plans\hogwarts-fg-nsf.ps1")) {
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Tools $f))
    Check "$f is ASCII (PowerShell 5.1 reads BOM-less scripts as ANSI)" (@($bytes | Where-Object { $_ -gt 127 }).Count -eq 0)
}

Write-Host "== 2. run-test.ps1 functions (loaded from the script's AST)"
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Tools "run-test.ps1"), [ref]$null, [ref]$null)
$wanted = @("Get-SteamLibraries", "Find-SteamAppId", "Find-SteamGameDir", "Get-SteamAccountId", "Get-SavePlaceholderValues",
    "Expand-SavePlaceholders", "Resolve-SaveTarget", "Copy-FileRetry", "Install-SaveFile", "Save-SaveEvidence",
    "ConvertTo-ArgvString", "ConvertFrom-RecipeVars", "ConvertTo-PilotVarArgs",
    "Read-Shared", "Get-ClockPair", "Copy-Shared", "Find-GameSettings", "Find-PresentMon", "Get-FrameGenModules", "Get-PowerContext",
    "Save-MonadoSlice",
    "ConvertFrom-GameIniSpec", "ConvertFrom-GameIniBlock", "Resolve-GameIniPath", "Read-IniText", "Write-IniText", "Find-IniSection",
    "Find-IniKeyLines", "Split-IniLines", "Get-IniValue", "Set-IniValue", "Clear-ReadOnly", "Restore-GameIniFile", "Install-GameIni", "Restore-GameIni",
    "Restore-PendingGameIni")
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

Write-Host "== 2c. -GameIni: the game's settings files changed for a run and put back byte for byte (temp files only)"
$iniDir = Join-Path $Work "gameini"
New-Item -ItemType Directory -Force -Path $iniDir | Out-Null
$pending = Join-Path $iniDir "pending"
$q = [char]34
# Restore-PendingGameIni stops the interrupted run's game first; here it only records what it would stop (the real
# Stop-Leftovers also stops UEVRInjector and CrashReportClient)
$script:stopped = @()
function Stop-Leftovers([string]$processName) { $script:stopped += $processName }

# the -GameIni spec
$spec = @(ConvertFrom-GameIniSpec ("GameUserSettings.ini|/Script/Phoenix.PhoenixGameSettings|FrameGeneration=(Mode=Off,NumFramesInterpolated=0,LocStr=" + $q + "Off" + $q + ");bUseVSync=True;Engine.ini|[SystemSettings]|r.A=1|2=3; ;C:\x\y.ini|S|k= v "))
Check "spec: four entries" ($spec.Count -eq 4) ($spec | ConvertTo-Json -Compress)
if ($spec.Count -eq 4) {
    Check "spec: file|Section|Key=Value, the value with quotes and parentheses kept" ($spec[0].file -eq "GameUserSettings.ini" -and $spec[0].section -eq "/Script/Phoenix.PhoenixGameSettings" -and $spec[0].key -eq "FrameGeneration" -and $spec[0].value -ceq ("(Mode=Off,NumFramesInterpolated=0,LocStr=" + $q + "Off" + $q + ")"))
    Check "spec: Key=Value keeps the file and section" ($spec[1].file -eq "GameUserSettings.ini" -and $spec[1].section -eq "/Script/Phoenix.PhoenixGameSettings" -and $spec[1].key -eq "bUseVSync" -and $spec[1].value -eq "True")
    Check "spec: [Section] brackets dropped, the value may hold | and =" ($spec[2].file -eq "Engine.ini" -and $spec[2].section -eq "SystemSettings" -and $spec[2].key -eq "r.A" -and $spec[2].value -eq "1|2=3")
    Check "spec: a full path, the value trimmed" ($spec[3].file -eq "C:\x\y.ini" -and $spec[3].value -eq "v")
}
Check "spec: Section|Key=Value defaults to GameUserSettings.ini" ((@(ConvertFrom-GameIniSpec "S|k=1")[0]).file -eq "GameUserSettings.ini")
Check "spec: empty" (@(ConvertFrom-GameIniSpec "").Count -eq 0)
Throws "spec: Key=Value without a section" { ConvertFrom-GameIniSpec "k=1" } "*needs a file and a section*"
Throws "spec: no =" { ConvertFrom-GameIniSpec "f|S|k" } "*expects*"
Throws "spec: too many parts" { ConvertFrom-GameIniSpec "a|b|c|k=1" } "*expects*"
$iniBlock = '{"GameUserSettings.ini": {"[/Script/X]": {"A": "1", "B": true}}, "Engine.ini": {"S": {"C": 2}}}' | ConvertFrom-Json
$fromBlock = @(ConvertFrom-GameIniBlock $iniBlock)
Check "recipe game_ini block -> entries (booleans as True/False)" ($fromBlock.Count -eq 3 -and $fromBlock[0].section -eq "/Script/X" -and $fromBlock[1].value -ceq "True" -and $fromBlock[2].file -eq "Engine.ini" -and $fromBlock[2].value -eq "2") ($fromBlock | ConvertTo-Json -Compress)
Throws "recipe game_ini: not an object" { ConvertFrom-GameIniBlock ('{"a": "b"}' | ConvertFrom-Json) } "*must be an object*"

# paths
$iniVals = Get-SavePlaceholderValues $null
Check "path: a file name is in the config folder" ((Resolve-GameIniPath "Engine.ini" $iniDir $iniVals) -eq (Join-Path $iniDir "Engine.ini"))
Check "path: placeholders" ((Resolve-GameIniPath "{localappdata}\x.ini" $null $iniVals) -eq (Join-Path $env:LOCALAPPDATA "x.ini"))
Throws "path: a file name without a config folder" { Resolve-GameIniPath "Engine.ini" $null $iniVals } "*no config folder*"
Throws "path: a relative path" { Resolve-GameIniPath "sub\x.ini" $iniDir $iniVals } "*neither a file name nor an absolute path*"
Throws "path: a missing folder" { Resolve-GameIniPath (Join-Path $iniDir "nope\x.ini") $null $iniVals } "*folder not found*"
Throws "path: unknown placeholder, named as -GameIni" { Resolve-GameIniPath "{nope}\x.ini" $null $iniVals } "-GameIni: unknown placeholder {nope}*"

# Set-IniValue / Get-IniValue on text
$nl = "`r`n"
$text = "[A]${nl}x=1${nl}Dup=1${nl}y=2${nl}Dup=2${nl}${nl}[B]${nl}x=9${nl}${nl}"
$r = Set-IniValue $text $nl "a" "DUP" "new"
Check "set: the first line of the key gets the value (spelling kept), later duplicates go, the rest stays" ($r.text -ceq "[A]${nl}x=1${nl}Dup=new${nl}y=2${nl}${nl}[B]${nl}x=9${nl}${nl}" -and $r.old -eq "1") $r.text
$r = Set-IniValue $text $nl "B" "z" "3"
Check "set: a missing key after the section's last line" ($r.text -ceq "[A]${nl}x=1${nl}Dup=1${nl}y=2${nl}Dup=2${nl}${nl}[B]${nl}x=9${nl}z=3${nl}${nl}" -and $null -eq $r.old) $r.text
$r = Set-IniValue $text $nl "C" "k" "v"
Check "set: a missing section at the end" ($r.text -ceq "[A]${nl}x=1${nl}Dup=1${nl}y=2${nl}Dup=2${nl}${nl}[B]${nl}x=9${nl}${nl}[C]${nl}k=v${nl}${nl}") $r.text
$r = Set-IniValue "" $nl "C" "k" "v"
Check "set: an empty file" ($r.text -ceq "[C]${nl}k=v${nl}") $r.text
Check "get: a key, case-insensitive; a missing one" ((Get-IniValue $text $nl "b" "X") -eq "9" -and $null -eq (Get-IniValue $text $nl "B" "nope") -and $null -eq (Get-IniValue $text $nl "Z" "x"))
# UE reads a section that appears twice as one
$dup = "[A]${nl}k=1${nl}${nl}[B]${nl}x=1${nl}${nl}[a]${nl}k=2${nl}j=3${nl}"
$r = Set-IniValue $dup $nl "A" "k" "9"
Check "set: a section twice: the first line of the key gets the value, the one in the second block goes" ($r.text -ceq "[A]${nl}k=9${nl}${nl}[B]${nl}x=1${nl}${nl}[a]${nl}j=3${nl}" -and $r.old -eq "1") $r.text
$r = Set-IniValue $dup $nl "A" "new" "5"
Check "set: a section twice: a missing key goes into its first block" ($r.text -ceq "[A]${nl}k=1${nl}new=5${nl}${nl}[B]${nl}x=1${nl}${nl}[a]${nl}k=2${nl}j=3${nl}") $r.text
Check "get: a key in the section's second block" ((Get-IniValue $dup $nl "A" "j") -eq "3" -and (Get-IniValue $dup $nl "A" "k") -eq "1")

# round trip on files: Install-GameIni / Restore-GameIni
function New-IniFile([string]$name, [byte[]]$bytes, [switch]$ReadOnly) {
    $p = Join-Path $iniDir $name
    if (Test-Path -LiteralPath $p) { Clear-ReadOnly $p }
    [System.IO.File]::WriteAllBytes($p, $bytes)
    (Get-Item -LiteralPath $p).LastWriteTimeUtc = [DateTime]::new(2026, 9, 1, 12, 0, 0, [DateTimeKind]::Utc)
    if ($ReadOnly) { (Get-Item -LiteralPath $p).Attributes = "ReadOnly, Archive" }
    return $p
}
function Get-FileState([string]$p) { $i = Get-Item -LiteralPath $p -Force; return "$((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash)|$($i.Length)|$($i.LastWriteTimeUtc.Ticks)|$($i.Attributes)" }
$ascii = [System.Text.Encoding]::ASCII
# a copy of the real Hogwarts GameUserSettings.ini when this PC has one (read only), else a stand-in with its lines
$realHl = Join-Path $env:LOCALAPPDATA "Hogwarts Legacy\Saved\Config\WindowsNoEditor\GameUserSettings.ini"
$realHlBefore = $(if (Test-Path -LiteralPath $realHl) { Get-FileState $realHl } else { $null })
$hlBytes = $(if ($realHlBefore) { [System.IO.File]::ReadAllBytes($realHl) } else { $ascii.GetBytes("[/Script/Phoenix.PhoenixGameSettings]${nl}LatencyMode=Intel_XeLL_LowLatency${nl}FrameGeneration=(Mode=Intel_XeFG,NumFramesInterpolated=1,LocStr=" + $q + "INTEL_XEFG_MODE_X2" + $q + ")${nl}r.ChosenFrameGenProvider=FXeFGDXGISwapChainProvider${nl}${nl}[SystemSettings]${nl}r.GPULUIDLow=1${nl}${nl}") })
$hl = New-IniFile "GameUserSettings.ini" $hlBytes
$u16 = New-IniFile "utf16.ini" ([byte[]](@(0xFF, 0xFE) + [System.Text.Encoding]::Unicode.GetBytes("[S]`r`nk=1`r`nname=$Cn`r`n")))
$u8 = New-IniFile "utf8bom.ini" ([byte[]](@(0xEF, 0xBB, 0xBF) + [System.Text.Encoding]::UTF8.GetBytes("[S]`nk=1`n")))
$ro = New-IniFile "readonly.ini" ($ascii.GetBytes("[S]`r`nk=1")) -ReadOnly
$absent = Join-Path $iniDir "absent.ini"
if (Test-Path -LiteralPath $absent) { Remove-Item -LiteralPath $absent -Force }
$states = @{}
foreach ($p in @($hl, $u16, $u8, $ro)) { $states[$p] = Get-FileState $p }
$fgOff = "(Mode=Off,NumFramesInterpolated=0,LocStr=" + $q + "Off" + $q + ")"
$entries = @(
    [pscustomobject]@{ path = $hl; section = "/Script/Phoenix.PhoenixGameSettings"; key = "FrameGeneration"; value = $fgOff },
    [pscustomobject]@{ path = $u16; section = "S"; key = "k"; value = "2" },
    [pscustomobject]@{ path = $u8; section = "S"; key = "k"; value = "3" },
    [pscustomobject]@{ path = $ro; section = "S"; key = "new"; value = "4" },
    [pscustomobject]@{ path = $absent; section = "S"; key = "k"; value = "5" })
$runA = Join-Path $iniDir "runA"
New-Item -ItemType Directory -Force -Path $runA | Out-Null
$st = Install-GameIni $entries $runA $pending "mopicselftest_noproc"
$hlText = (Read-IniText $hl).text
$hlLinesBefore = @($ascii.GetString($hlBytes) -split "`r`n")
$hlLinesAfter = @($hlText -split "`r`n")
$changed = @(for ($i = 0; $i -lt [math]::Max($hlLinesBefore.Count, $hlLinesAfter.Count); $i++) { if ($hlLinesBefore[$i] -cne $hlLinesAfter[$i]) { $i } })
Check "install: GameUserSettings.ini$(if ($realHlBefore) { ' (a copy of the real Hogwarts file)' }): exactly the FrameGeneration line changed, to the game's Off entry" ($changed.Count -eq 1 -and $hlLinesAfter[$changed[0]] -ceq "FrameGeneration=$fgOff" -and $hlLinesBefore[$changed[0]] -like "FrameGeneration=(Mode=Intel_XeFG*") ($changed -join ",")
Check "install: the value before recorded" ($st.entries[0].before -like "(Mode=Intel_XeFG*") $st.entries[0].before
$u16Bytes = [System.IO.File]::ReadAllBytes($u16)
Check "install: UTF-16 file stays UTF-16 with its BOM, its other text intact" ($u16Bytes[0] -eq 0xFF -and $u16Bytes[1] -eq 0xFE -and [System.Text.Encoding]::Unicode.GetString($u16Bytes, 2, $u16Bytes.Length - 2) -ceq "[S]`r`nk=2`r`nname=$Cn`r`n")
$u8Bytes = [System.IO.File]::ReadAllBytes($u8)
Check "install: UTF-8 BOM and LF line breaks kept" ($u8Bytes[0] -eq 0xEF -and [System.Text.Encoding]::UTF8.GetString($u8Bytes, 3, $u8Bytes.Length - 3) -ceq "[S]`nk=3`n")
Check "install: a read-only file gets the key and stays read-only for the game" (((Read-IniText $ro).text -ceq "[S]`r`nk=1`r`nnew=4") -and ((Get-Item -LiteralPath $ro).Attributes -band [System.IO.FileAttributes]::ReadOnly))
Check "install: a missing file is created" ((Test-Path -LiteralPath $absent) -and (Read-IniText $absent).text -ceq "[S]`r`nk=5`r`n")
Check "install: journal and originals in the pending folder, copies in game-ini-before" ((Test-Path -LiteralPath (Join-Path $pending "journal.json")) -and @(Get-ChildItem -LiteralPath $pending -File).Count -eq 5 -and @(Get-ChildItem -LiteralPath (Join-Path $runA "game-ini-before") -File).Count -eq 4)
# the game rewrites one file while it runs (UE saves its settings when it quits)
[System.IO.File]::AppendAllText($hl, "[Extra]`r`nwritten=by the game`r`n")
$failed = @(Restore-GameIni $st $runA)
Check "restore: no failures" ($failed.Count -eq 0) ($failed -join "; ")
$bad = @($states.Keys | Where-Object { (Get-FileState $_) -ne $states[$_] })
Check "restore: every file byte for byte, with its time and attributes" ($bad.Count -eq 0) (($bad | ForEach-Object { "$_ $(Get-FileState $_) vs $($states[$_])" }) -join "; ")
Check "restore: the file that wasn't there is gone again" (-not (Test-Path -LiteralPath $absent))
Check "restore: the files as the game left them in game-ini-after, the values read from them" (@(Get-ChildItem -LiteralPath (Join-Path $runA "game-ini-after") -File).Count -eq 5 -and $st.entries[0].after -ceq $fgOff -and (Get-Content -LiteralPath $st.files[0].after -Raw) -like "*written=by the game*") ($st.entries | ConvertTo-Json -Compress)
Check "restore: state per file (restored, no error)" (@($st.files | Where-Object { $_.restored -and -not $_.error }).Count -eq 5)
Check "restore: the pending folder removed" (-not (Test-Path -LiteralPath $pending))

# a harness that dies after the install: the next start puts the files back from the pending folder
$runB = Join-Path $iniDir "runB"
New-Item -ItemType Directory -Force -Path $runB | Out-Null
$st = Install-GameIni $entries $runB $pending "mopicselftest_noproc"
Remove-Item -LiteralPath $runB -Recurse -Force   # even without the run folder
[System.IO.File]::AppendAllText($u16, "x")
$note = Restore-PendingGameIni $pending
$bad = @($states.Keys | Where-Object { (Get-FileState $_) -ne $states[$_] })
Check "pending: an interrupted run's files put back at the next start" ($bad.Count -eq 0 -and -not (Test-Path -LiteralPath $absent) -and -not (Test-Path -LiteralPath $pending) -and $note -like "put back the game settings an interrupted run left changed*") "$note | $($bad -join ', ')"
Check "pending: the interrupted run's game stopped first" (($script:stopped -join ",") -eq "mopicselftest_noproc") ($script:stopped -join ",")
Check "pending: nothing to do -> empty note" ((Restore-PendingGameIni $pending) -eq "")
# a value that doesn't read back (a key with a line break): nothing stays changed, the pending folder goes
Throws "install: a value that doesn't read back fails and puts back what it changed" { Install-GameIni @([pscustomobject]@{ path = $u8; section = "S"; key = "k"; value = "a`nb" }) (Join-Path $iniDir "runC") $pending "mopicselftest_noproc" } "*reads back as*"
Check "install failure: the file as before, no pending folder" ((Get-FileState $u8) -eq $states[$u8] -and -not (Test-Path -LiteralPath $pending))
# files it couldn't write back unchanged are refused before anything is copied or changed
$u16NoBom = New-IniFile "utf16-nobom.ini" ([System.Text.Encoding]::Unicode.GetBytes("[S]`r`nk=1`r`n"))
$badU8 = New-IniFile "bad-utf8.ini" ([byte[]](@(0xEF, 0xBB, 0xBF) + @($ascii.GetBytes("[S]`r`nk=")) + @(0xC3, 0x28) + @($ascii.GetBytes("`r`n"))))
foreach ($p in @($u16NoBom, $badU8)) {
    $pState = Get-FileState $p
    Throws "install: $(Split-Path -Leaf $p) refused (it wouldn't be written back the same)" { Install-GameIni @([pscustomobject]@{ path = $p; section = "S"; key = "k"; value = "2" }) (Join-Path $iniDir "runF") $pending "mopicselftest_noproc" } "*can't be changed safely*"
    Check "install refused: $(Split-Path -Leaf $p) as before, no pending folder" ((Get-FileState $p) -eq $pState -and -not (Test-Path -LiteralPath $pending))
}
# a run whose files couldn't be put back leaves its journal: the next run's install must not take the changed files for
# the originals (it refuses; the harness retries the put-back before each install)
$runD = Join-Path $iniDir "runD"
New-Item -ItemType Directory -Force -Path $runD | Out-Null
$st = Install-GameIni @($entries[2]) $runD $pending "mopicselftest_noproc"
$journalHash = Get-Hash (Join-Path $pending "journal.json")
Throws "install: refused while an earlier run's originals are pending" { Install-GameIni @($entries[2]) (Join-Path $iniDir "runE") $pending "mopicselftest_noproc" } "*still holds the originals*"
Check "install refused: the pending journal and original untouched" ((Get-Hash (Join-Path $pending "journal.json")) -eq $journalHash -and (Get-Hash $st.files[0].backup) -eq ($states[$u8] -split "\|")[0])
$note = Restore-PendingGameIni $pending
Check "the put-back retried: the file as before, the pending folder gone" ((Get-FileState $u8) -eq $states[$u8] -and -not (Test-Path -LiteralPath $pending) -and $note -like "put back*") $note
if ($realHlBefore) { Check "the real Hogwarts GameUserSettings.ini untouched" ((Get-FileState $realHl) -eq $realHlBefore) }
Clear-ReadOnly $ro

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
if ($busy.Count -gt 0 -or $NoWindows) {
    Write-Host "SKIP  sections 4-5: $(if ($NoWindows) { '-NoWindows' } else { "$(@($busy | ForEach-Object Name) -join ', ') running (the harness would stop it)" })"
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
    # what run-test.ps1 dot-sources (HEAD's copy may predate it; a missing one is fine for it)
    foreach ($dep in @("steam-offline.ps1")) {
        Copy-Item -LiteralPath (Join-Path $Tools $dep) -Destination (Join-Path $cmp.new $dep)
        $depHead = (& cmd /c "git -C `"$Repo`" show HEAD:tools/mopic-test/$dep 2>nul") -join "`r`n"
        if ($depHead) { [System.IO.File]::WriteAllText((Join-Path $cmp.head $dep), $depHead + "`r`n", [System.Text.Encoding]::ASCII) }
    }
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
            # keys this branch added since the comparison point (HEAD may already have some of them)
            $allowed = @("game_ini", "perf", "eyes", "binocular")
            Check "cmp $($case.name) r$($i + 1): result.json keys = before + some of $($allowed -join ', ')" (@($added | Where-Object { $allowed -notcontains $_ }).Count -eq 0 -and $lost.Count -eq 0) "added $($added -join ','); lost $($lost -join ',')"
            $changed = @($dh[$i].keys | Where-Object { $dh[$i].values[$_] -cne $dn[$i].values[$_] })
            Check "cmp $($case.name) r$($i + 1): result.json values as before" ($changed.Count -eq 0) (($changed | ForEach-Object { "$_ head=$($dh[$i].values[$_]) new=$($dn[$i].values[$_])" }) -join "; ")
            Check "cmp $($case.name) r$($i + 1): recipe_vars empty, save null" ($dn[$i].values["recipe_vars"] -eq '""' -and $dn[$i].values["save"] -eq "null")
            # this copy has no perfreport.py next to it (and an empty python.exe): no numbers, the reason in summary.txt
            $perfLines = @($dn[$i].summary | Where-Object { $_ -like "perf*" })
            $launched = $null -ne $case.game
            Check "cmp $($case.name) r$($i + 1): perf null, $(if ($launched) { 'one perf line (not measured)' } else { 'no perf line (no game)' })" ($dn[$i].values["perf"] -eq "null" -and $(if ($launched) { $perfLines.Count -eq 1 -and $perfLines[0] -eq "perf: not measured (no .venv or no perfreport.py)" } else { $perfLines.Count -eq 0 })) ($perfLines -join " / ")
            $sumNew = @($dn[$i].summary | Where-Object { $_ -notlike "perf*" })
            $sumHead = @($dh[$i].summary | Where-Object { $_ -notlike "perf*" })
            Check "cmp $($case.name) r$($i + 1): summary.txt as before (but the perf line)" (($sumHead -join "`n") -ceq ($sumNew -join "`n")) ("head: " + ($dh[$i].summary -join " / ") + " | new: " + ($dn[$i].summary -join " / "))
            # the samplers' files: nvidia-smi's gpu.csv, PresentMon's output (or its "access denied"), perf-context.json,
            # monado's stats when a VR session happened to run
            $samplers = '^\\(gpu\.csv|presentmon[^\\]*|perf-context\.json|monado-frames\.csv)$'
            $filesNew = @($dn[$i].files | Where-Object { $_ -notmatch $samplers })
            $filesHead = @($dh[$i].files | Where-Object { $_ -notmatch $samplers })
            Check "cmp $($case.name) r$($i + 1): same files in the run folder (but the perf samplers')" (($filesHead -join ",") -eq ($filesNew -join ",")) ("head: " + ($dh[$i].files -join ",") + " | new: " + ($dn[$i].files -join ","))
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
Write-Host "== 5b. -GameIni end to end (the stand-in in its windowless exit mode: no window, no game)"
$busyNow = @(Get-Process -Name (@("UEVRInjector", "CrashReportClient") + $fakeNames) -ErrorAction SilentlyContinue)
$pendingReal = Join-Path $Tools "runs\game-ini-pending"
if ($busyNow.Count -gt 0 -or (Test-Path -LiteralPath $pendingReal)) {
    Write-Host "SKIP  section 5b: $(if ($busyNow.Count -gt 0) { "$(@($busyNow | ForEach-Object Name) -join ', ') running (the harness would stop it)" } else { "$pendingReal exists (an interrupted harness run: the next harness start puts its files back)" })"
} else {
    $e2eIniDir = Join-Path $Work "e2e-gameini"
    New-Item -ItemType Directory -Force -Path $e2eIniDir | Out-Null
    $standIn = Join-Path $Work "game\mopicselftest_game.exe"
    if (-not (Test-Path -LiteralPath $standIn)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $standIn) | Out-Null
        Add-Type -TypeDefinition (Get-Content -LiteralPath (Join-Path $Here "fakegame.cs") -Raw) -ReferencedAssemblies System.Windows.Forms, System.Drawing -OutputAssembly $standIn -OutputType WindowsApplication
    }
    $iniA = Join-Path $e2eIniDir "GameUserSettings.ini"
    $iniB = Join-Path $e2eIniDir "Engine.ini"
    [System.IO.File]::WriteAllBytes($iniA, [System.Text.Encoding]::ASCII.GetBytes("[/Script/Game.Settings]`r`nFrameGeneration=(Mode=Intel_XeFG)`r`nOther=1`r`n`r`n"))
    [System.IO.File]::WriteAllBytes($iniB, [System.Text.Encoding]::ASCII.GetBytes("[SystemSettings]`r`nr.X=0`r`n"))
    (Get-Item -LiteralPath $iniA).LastWriteTimeUtc = [DateTime]::new(2026, 9, 1, 0, 0, 0, [DateTimeKind]::Utc)
    $iniState = { "$(Get-Hash $iniA)|$((Get-Item -LiteralPath $iniA).LastWriteTimeUtc.Ticks)|$(Get-Hash $iniB)" }
    $iniBefore = & $iniState
    $iniSpec = "$iniA|/Script/Game.Settings|FrameGeneration=(Mode=Off,LocStr=" + [char]34 + "Off" + [char]34 + ");$iniB|SystemSettings|r.X=1"
    $baseIni = @("-File", (Join-Path $Tools "run-test.ps1"), "-Game", "Custom", "-ProcessName", "mopicselftest_game",
        "-LaunchTarget", $standIn, "-NoInject", "-NoPerf", "-LaunchTimeout", "30", "-EngineDir", $EngineDirReal)
    # one harness call; its run folders go into the work folder afterwards
    function Invoke-IniHarness([string[]]$argv, [string]$label, [string[]]$base = $baseIni) {
        $start = Get-Date
        $res = Invoke-PS ($base + $argv + @("-Label", $label))
        $dirs = @(Get-ChildItem -LiteralPath (Join-Path $Tools "runs") -Directory | Where-Object { $_.Name -like "*-Custom-$label-r*" -and $_.CreationTime -ge $start.AddSeconds(-2) } | Sort-Object Name)
        $moved = @()
        foreach ($d in $dirs) { $dest = Join-Path $Work "harness-runs\$($d.Name)"; New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null; Move-Item -LiteralPath $d.FullName -Destination $dest; $moved += $dest }
        return [pscustomobject]@{ code = $res.code; text = $res.text; flat = $res.flat; dirs = $moved; results = @($moved | ForEach-Object { $rp = Join-Path $_ "result.json"; if (Test-Path -LiteralPath $rp) { Read-Json $rp } }) }
    }

    # a crash: the "game" also writes the file while it runs (UE saves its settings when it quits)
    $env:MOPIC_FAKE_GAME = "exit:3:C0000005"
    $env:MOPIC_FAKE_GAME_TOUCH = $iniA
    $h = Invoke-IniHarness @("-Seconds", "20", "-GameIni", $iniSpec) "selftest-gameini-crash"
    Remove-Item Env:\MOPIC_FAKE_GAME, Env:\MOPIC_FAKE_GAME_TOUCH -ErrorAction SilentlyContinue
    $r = $h.results | Select-Object -First 1
    Check "e2e -GameIni CRASH: verdict, both files put back byte for byte (time too)" ($r -and $r.verdict -eq "CRASH" -and (& $iniState) -eq $iniBefore) $h.text
    if ($r) {
        $ge = @($r.game_ini.entries)
        Check "e2e -GameIni: result.json game_ini: values before and as the game left them, files restored" ($ge.Count -eq 2 -and $ge[0].before -eq "(Mode=Intel_XeFG)" -and $ge[0].after -eq ("(Mode=Off,LocStr=" + [char]34 + "Off" + [char]34 + ")") -and $ge[1].before -eq "0" -and $ge[1].after -eq "1" -and @($r.game_ini.files | Where-Object { $_.restored }).Count -eq 2) ($r.game_ini | ConvertTo-Json -Compress -Depth 5)
        $d = $h.dirs[0]
        Check "e2e -GameIni: game-ini-before (as before) and game-ini-after (with the game's write)" ((Get-Hash (Join-Path $d "game-ini-before\1-GameUserSettings.ini")) -eq ($iniBefore -split "\|")[0] -and (Get-Content -LiteralPath (Join-Path $d "game-ini-after\1-GameUserSettings.ini") -Raw) -like "*Mode=Off*autosave")
        $sum = @(Get-Content -LiteralPath (Join-Path $d "summary.txt") -Encoding UTF8)
        $iniLine = @($sum | Where-Object { $_.StartsWith("game ini: GameUserSettings.ini [/Script/Game.Settings] FrameGeneration=(Mode=Off") -and $_.Contains("was (Mode=Intel_XeFG); after the run kept") -and $_.EndsWith("| put back, sha256 ok") })
        Check "e2e -GameIni: summary.txt game ini lines" ($iniLine.Count -eq 1 -and @($sum | Where-Object { $_.StartsWith("game ini: Engine.ini [SystemSettings] r.X=1 (was 0;") }).Count -eq 1) ($sum -join " / ")
    }
    Check "e2e -GameIni: no pending folder left" (-not (Test-Path -LiteralPath $pendingReal))
    Check "e2e -GameIni with -KeepGame refused before any run" ((Invoke-IniHarness @("-GameIni", $iniSpec, "-KeepGame") "selftest-gameini-keep").flat -like "*can't be combined with -KeepGame*")
    Check "e2e -GameIni malformed refused before any run" ((Invoke-IniHarness @("-GameIni", "oops") "selftest-gameini-bad").flat -like "*-GameIni expects*")

    # -EyeSampler without a recipe: the sampler reads the Mopic display (whatever it shows) while the stand-in runs,
    # stops with it, and the report lands in result.json and summary.txt
    $env:MOPIC_FAKE_GAME = "exit:6:0"
    $h = Invoke-IniHarness @("-Seconds", "20", "-WaitForExit", "-EyeSampler") "selftest-eyes"
    Remove-Item Env:\MOPIC_FAKE_GAME -ErrorAction SilentlyContinue
    $r = $h.results | Select-Object -First 1
    $mopic = [bool](& $Py -c "import sys; sys.path.insert(0, r'$Tools'); import gamepilot; print(gamepilot.mopic_monitor() or '')" | Where-Object { $_ -match '\d' })
    if ($r -and $h.dirs.Count -ge 1) {
        $sum = @(Get-Content -LiteralPath (Join-Path $h.dirs[0] "summary.txt") -Encoding UTF8)
        $eyesLine = @($sum | Where-Object { $_ -like "eyes*" })
        if ($mopic) {
            Check "e2e -EyeSampler: PASS, ~120 samples a second while the stand-in ran, stopped with it, eyes block and line" ($r.verdict -eq "PASS" -and $r.eyes.samples -gt 300 -and $r.eyes.rate_hz -gt 100 -and $r.eyes.stopped_by -in @("game exited", "stop file") -and $eyesLine.Count -eq 1 -and $eyesLine[0].StartsWith("eyes[") -and $eyesLine[0].Contains("no segments")) (($eyesLine -join " / ") + " | " + ($r.eyes | ConvertTo-Json -Compress -Depth 3))
        } else {
            Check "e2e -EyeSampler without a Mopic display: no samples, said so" ($r.verdict -eq "PASS" -and $null -eq $r.eyes -and $eyesLine.Count -eq 1 -and $eyesLine[0] -like "eyes: no samples*") ($eyesLine -join " / ")
        }
        $leaf = Split-Path -Leaf $h.dirs[0]
        Check "e2e -EyeSampler: no sampler left running" (@(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like "*eyesampler.py*$leaf*" }).Count -eq 0)
    } else {
        Check "e2e -EyeSampler: a run folder with result.json" $false $h.text
    }

    # a harness killed while the game runs: its changes stay, the next harness start puts them back (and stops the game)
    $env:MOPIC_FAKE_GAME = "exit:90:0"
    $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass") + $baseIni + @("-Seconds", "80", "-GameIni", $iniSpec, "-Label", "selftest-gameini-killed")
    $killed = Start-Process -FilePath "powershell.exe" -ArgumentList (@($argv | ForEach-Object { ConvertTo-Argv $_ }) -join " ") -WindowStyle Hidden -PassThru
    Remove-Item Env:\MOPIC_FAKE_GAME -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline -and -not ((Test-Path -LiteralPath (Join-Path $pendingReal "journal.json")) -and (Get-Process -Name "mopicselftest_game" -ErrorAction SilentlyContinue))) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 2
    $changedWhileRunning = (& $iniState) -ne $iniBefore
    try { Stop-Process -Id $killed.Id -Force } catch { }
    $killed.WaitForExit(10000) | Out-Null
    Check "e2e killed harness: the files were changed and the journal written while the game ran" ($changedWhileRunning -and (Test-Path -LiteralPath (Join-Path $pendingReal "journal.json")) -and @(Get-Process -Name "mopicselftest_game" -ErrorAction SilentlyContinue).Count -eq 1)
    # the next harness start, without -GameIni: a launch that fails at once (HARNESS_ERROR), after the put-back
    $baseNext = @($baseIni[0..5]) + @("-LaunchTarget", (Join-Path $Work "nope\missing.exe")) + @($baseIni[8..($baseIni.Count - 1)])
    $h = Invoke-IniHarness @() "selftest-gameini-next" $baseNext
    $r = $h.results | Select-Object -First 1
    Check "e2e next harness start: files put back first, the interrupted run's game stopped, noted in run 1" ((& $iniState) -eq $iniBefore -and -not (Test-Path -LiteralPath $pendingReal) -and @(Get-Process -Name "mopicselftest_game" -ErrorAction SilentlyContinue).Count -eq 0 -and $r -and (@($r.notes) -join ";") -like "*put back the game settings an interrupted run left changed*" -and $h.flat -like "*put back the game settings an interrupted run left changed*") $h.text
    Get-ChildItem -LiteralPath (Join-Path $Tools "runs") -Directory | Where-Object { $_.Name -like "*-Custom-selftest-gameini-killed-r*" } | ForEach-Object { Move-Item -LiteralPath $_.FullName -Destination (Join-Path $Work "harness-runs\$($_.Name)") }
    Get-Process -Name "mopicselftest_game" -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
    # the killed harness never put back the stand-in's UEVR config.txt (only made for this self-test)
    foreach ($p in $persistentDirs) { foreach ($n in @("config.txt", "user_script.txt")) { $f = Join-Path $p $n; if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } } }
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

# a rung without "save" plays the game's own progress: no -SaveFile, and its recipe needs no save_slot
$noSaveLadder = Join-Path $Work "ladder-nosave.json"
Write-Json $noSaveLadder ([ordered]@{ game = "Wukong"; recipe = $recipeLadderNoSlot; rungs = @([ordered]@{ name = "demo"; tier = 1 }) })
$l = Invoke-Ladder $noSaveLadder @("-DryRun")
Check "ladder: a rung without save dry-runs without -SaveFile" ($l.code -eq 0 -and $l.flat -like "*demo (tier 1) <- (no save: the game's own progress)*" -and $l.flat -notlike "*-SaveFile*") $l.text
$noSaveSha = Join-Path $Work "ladder-nosave-sha.json"
Write-Json $noSaveSha ([ordered]@{ game = "Wukong"; recipe = $recipeLadderNoSlot; rungs = @([ordered]@{ name = "demo"; sha256 = ("0" * 64) }) })
$l = Invoke-Ladder $noSaveSha @("-DryRun")
Check "ladder: a rung with sha256 but no save refused" ($l.code -ne 0 -and $l.flat -like "*rung demo has a sha256 but no save*") $l.text

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

Write-Host "== 7c. the eye sampler on synthetic side-by-side frames (analysis\eyesampler.py), gamepilot's mopic-sbs crop"
foreach ($f in @((Join-Path $Tools "analysis\eyesampler.py"), (Join-Path $Here "selftest_eyes.py"))) {
    & { $ErrorActionPreference = "Continue"; & $Py -W error -m py_compile $f 2>&1 | ForEach-Object { Write-Host "      $_" } }
    Check "py_compile $(Split-Path -Leaf $f) (warnings as errors)" ($LASTEXITCODE -eq 0)
}
$pyOut = @(& { $ErrorActionPreference = "Continue"; & $Py -W error (Join-Path $Here "selftest_eyes.py") (Join-Path $Work "eyes") 2>&1 | ForEach-Object { "$_" } })
$pyOut | Where-Object { $_ -like "FAIL*" -or $_ -like "  line: *" -or $_ -like "Traceback*" -or $_ -match "Error" } | ForEach-Object { Write-Host "  $_" }
$pyOk = @($pyOut | Where-Object { $_ -like "ok *" }).Count
Check "eye sampler self-test ($pyOk checks passed)" ($LASTEXITCODE -eq 0 -and $pyOk -gt 0)

Write-Host "== 7d. both eyes' content compared (analysis\binocular.py) on synthetic stereo pairs with one-eye defects"
$binPy = Join-Path $Tools "analysis\binocular.py"
& { $ErrorActionPreference = "Continue"; & $Py -W error -m py_compile $binPy 2>&1 | ForEach-Object { Write-Host "      $_" } }
Check "py_compile binocular.py (warnings as errors)" ($LASTEXITCODE -eq 0)
$pyOut = @(& { $ErrorActionPreference = "Continue"; & $Py -W error $binPy "selftest" "--work" (Join-Path $Work "binocular") 2>&1 | ForEach-Object { "$_" } })
$pyOut | Where-Object { $_ -like "FAIL*" -or $_ -like "Traceback*" -or $_ -match "Error" } | ForEach-Object { Write-Host "  $_" }
$pyOk = @($pyOut | Where-Object { $_ -like "ok *" }).Count
Check "binocular self-test ($pyOk checks passed)" ($LASTEXITCODE -eq 0 -and $pyOk -gt 0)

Write-Host "== 8. the real save folder"
Check "real Wukong save folder unchanged by the self-test" ((Get-Listing $RealSaveDir) -join "`n" -eq ($realBefore -join "`n"))

Write-Host ""
Write-Host "self-test: $($script:passes) passed, $($script:fails) failed (work folder $Work)"
if ($script:fails -gt 0) { exit 1 }
exit 0
