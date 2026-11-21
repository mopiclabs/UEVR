# Steam's HKCU\Software\Valve\Steam as two readers see it. Get-ItemProperty can read a packaged app's private hive
# (an agent shell inside an MSIX-packaged app saw RunningAppID 0 while a game ran); WMI StdRegProv reads the user's
# hive from the WMI service (on another PC it read ActiveProcess pid 0 while Get-ItemProperty read the real steam.exe
# pid). Neither view is trusted alone: a run treats either nonzero RunningAppID as busy, and the game-process list
# stays the primary evidence.
#   dot-source:  . (Join-Path $PSScriptRoot "steam_state.ps1"); Get-SteamRunningAppId; Format-SteamState (Get-SteamState)
#   watch:       powershell -File steam_state.ps1 -SteamWatchProcess b1-Win64-Shipping -SteamWatchLog <campaign.log> [-SteamWatchDelay 240]
#                (waits for the game process, logs both views once -SteamWatchDelay s into it, and exits)
# Parameter names are prefixed: dot-sourcing binds them in the caller's scope.
param([string]$SteamWatchProcess = "", [string]$SteamWatchLog = "", [int]$SteamWatchDelay = 240, [int]$SteamWatchTimeout = 1800)

$script:SteamKey = 'Software\Valve\Steam'
function Read-SteamWmi([string]$sub, [string]$name) {
    try {
        $r = Invoke-CimMethod -Namespace root\cimv2 -ClassName StdRegProv -MethodName GetDWORDValue -ErrorAction Stop `
            -Arguments @{ hDefKey = [uint32]2147483649; sSubKeyName = $sub; sValueName = $name }
        if ($r.ReturnValue -eq 0) { return [int64]$r.uValue }
    } catch { }
    return $null
}
function Read-SteamDirect([string]$sub, [string]$name) {
    try { return [int64](Get-ItemProperty "HKCU:\$sub" -Name $name -ErrorAction Stop).$name } catch { return $null }
}
function Get-SteamState {
    [pscustomobject]@{
        AppDirect  = Read-SteamDirect $script:SteamKey "RunningAppID"
        AppWmi     = Read-SteamWmi $script:SteamKey "RunningAppID"
        PidDirect  = Read-SteamDirect "$script:SteamKey\ActiveProcess" "pid"
        PidWmi     = Read-SteamWmi "$script:SteamKey\ActiveProcess" "pid"
        UserDirect = Read-SteamDirect "$script:SteamKey\ActiveProcess" "ActiveUser"
        UserWmi    = Read-SteamWmi "$script:SteamKey\ActiveProcess" "ActiveUser"
    }
}
function Format-SteamState($s) {
    $v = { param($x) if ($null -eq $x) { "?" } else { "$x" } }
    # ActiveUser is the Steam account id: logged only as set / 0, never its value
    $u = { param($x) if ($null -eq $x) { "?" } elseif ($x -ne 0) { "set" } else { "0" } }
    $steam = @(Get-Process steam -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ","
    "RunningAppID direct $(& $v $s.AppDirect) / wmi $(& $v $s.AppWmi); ActiveProcess pid direct $(& $v $s.PidDirect) / wmi $(& $v $s.PidWmi) (steam.exe $(if ($steam) { $steam } else { 'none' })); ActiveUser direct $(& $u $s.UserDirect) / wmi $(& $u $s.UserWmi)"
}
# Either view nonzero -> that id; both zero -> 0; neither readable -> -1.
function Get-SteamRunningAppId {
    $s = Get-SteamState
    $vals = @($s.AppDirect, $s.AppWmi | Where-Object { $null -ne $_ })
    if ($vals.Count -eq 0) { return -1 }
    $nz = @($vals | Where-Object { $_ -ne 0 })
    if ($nz.Count) { return [int64]$nz[0] }
    return 0
}

if ($SteamWatchProcess) {
    $end = (Get-Date).AddSeconds($SteamWatchTimeout)
    $p = $null
    while (-not $p -and (Get-Date) -lt $end) { $p = Get-Process $SteamWatchProcess -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not $p) { Start-Sleep 5 } }
    if ($p) {
        $at = $p.StartTime.AddSeconds($SteamWatchDelay)
        while ((Get-Date) -lt $at -and -not $p.HasExited) { Start-Sleep 5 }
        $l = "$(Get-Date -Format s) [steam] during play ($SteamWatchProcess pid $($p.Id), $([int]((Get-Date) - $p.StartTime).TotalSeconds) s in): $(Format-SteamState (Get-SteamState))"
    } else { $l = "$(Get-Date -Format s) [steam] no $SteamWatchProcess within $SteamWatchTimeout s: $(Format-SteamState (Get-SteamState))" }
    if ($SteamWatchLog) { Add-Content -Path $SteamWatchLog -Value $l -Encoding utf8 } else { Write-Host $l }
}