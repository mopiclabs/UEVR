# End-of-block check before handing the PC back: restore the engine folder's UEVRBackend.dll/.pdb from -BackupDir when
# they differ (run-test -Dll leaves the experiment build in the engine folder), stop the service runtime.ps1 started,
# then report game / harness / service processes, Steam's RunningAppID, the Hub client hash, the engine hashes, any
# experiment key left on in the test titles' UEVR configs, every save file in -SaveDir (hash@time), and keys left down.
# Make -BackupDir once, before the first -Dll run: copy the engine folder's UEVRBackend.dll and .pdb into it.
#   powershell -ExecutionPolicy Bypass -File free_check.ps1 -BackupDir <dir> [-SaveDir <Wukong save folder>]
param([Parameter(Mandatory)][string]$BackupDir,
    [string]$EngineDir = "$env:APPDATA\MOPIC\mopichub\engines\mopic-uevr\1.0.5.1",
    [string]$SaveDir = "",
    [string]$OutDir = "$env:LOCALAPPDATA\right-eye-cost")
$ErrorActionPreference = "Continue"
$client = "$env:APPDATA\MOPIC\OpenXR\src\xrt\targets\openxr\openxr_monado.dll"
function Get-Hash8($p) { if (Test-Path $p) { (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0, 8) } else { "missing" } }
$games = @("Polaris-Win64-Shipping", "SandFall-Win64-Shipping", "b1-Win64-Shipping", "Stray-Win64-Shipping", "HogwartsLegacy")
$running = @(Get-Process $games -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) { Write-Host "a game is still running: $($running.Name -join ', '); engine not touched"; exit 1 }
foreach ($f in "UEVRBackend.dll", "UEVRBackend.pdb") {
    if (-not (Test-Path "$BackupDir\$f")) { Write-Host "no $f in $BackupDir; engine $f not checked"; continue }
    if ((Get-Hash8 "$EngineDir\$f") -ne (Get-Hash8 "$BackupDir\$f")) { Copy-Item "$BackupDir\$f" "$EngineDir\$f" -Force; Write-Host "restored $f" }
}
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "runtime.ps1") -Mode stop -OutDir $OutDir | Out-Null
$svcExit = $LASTEXITCODE
$procs = @(Get-CimInstance Win32_Process | Where-Object {
    ($_.Name -match '^(powershell|pwsh)\.exe$' -and $_.CommandLine -match 'run-test\.ps1|runexp\.ps1|cost_run|walk_block') -or
    ($_.Name -match '^python' -and $_.CommandLine -match 'gamepilot|exitwatch') -or $_.Name -match 'mopic-openxr-service|monado-service' })
$left = @()
foreach ($g in "b1-Win64-Shipping", "SandFall-Win64-Shipping", "Polaris-Win64-Shipping", "Stray-Win64-Shipping") {
    $cfg = "$env:APPDATA\UnrealVRMod\$g\config.txt"
    if (Test-Path $cfg) {
        $on = @(Select-String -Path $cfg -Pattern '^((VR_NativeStereoFixTextureArraySubmit|VR_NativeStereoFixAsyncOpenXRWait|VR_NativeOpenXRAsyncWaitOnly)=true|VR_NativeStereoFixSamePass=false|VR_Experiment\w+=(?!false\s*$|0\s*$)\S+)' | ForEach-Object { $_.Line })
        if ($on.Count) { $left += "$g : $($on -join ',')" }
    }
}
. (Join-Path $PSScriptRoot "steam_state.ps1")
$rid = Get-SteamRunningAppId   # either registry view nonzero -> that id (steam_state.ps1)
Write-Host ("FREE CHECK {0}: processes {1}; service stop exit {2}; RunningAppID {3}; Hub client {4}; engine UEVRBackend.dll {5}, pdb {6}; experiment keys left on: {7}" -f
    (Get-Date -Format "HH:mm:ss"), $(if ($procs.Count) { ($procs | ForEach-Object { "$($_.Name) $($_.ProcessId)" }) -join ", " } else { "none" }),
    $svcExit, $rid, (Get-Hash8 $client), (Get-Hash8 "$EngineDir\UEVRBackend.dll"), (Get-Hash8 "$EngineDir\UEVRBackend.pdb"), $(if ($left.Count) { $left -join " | " } else { "none" }))
if ($SaveDir -and (Test-Path $SaveDir)) {
    $slots = Get-ChildItem $SaveDir -Filter "*.sav" | Sort-Object Name | ForEach-Object { "$($_.Name)=$(Get-Hash8 $_.FullName)@$($_.LastWriteTime.ToString('HH:mm:ss'))" }
    Write-Host ("SAVES: " + ($slots -join " "))
}
Add-Type -Namespace FreeKeys -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);' -ErrorAction SilentlyContinue
$down = @(1..254 | Where-Object { ([FreeKeys.U]::GetAsyncKeyState($_) -band 0x8000) -ne 0 } | ForEach-Object { "0x{0:X2}" -f $_ })
Write-Host ("STEAM: " + (Format-SteamState (Get-SteamState)))
Write-Host ("KEYS DOWN: " + $(if ($down.Count) { $down -join " " } else { "none" }))
