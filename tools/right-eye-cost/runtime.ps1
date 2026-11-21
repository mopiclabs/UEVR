# Mopic OpenXR runtime as installed by Mopic Hub (no runtime files changed or installed). Its client starts
# src\xrt\targets\service\mopic-openxr-service.exe on demand as a child of the game. For per-eye captures (SBS) the same
# installed service is started here first with MOPIC_MODE=sbs and MOPIC_PREACQ_FOLLOW_SDK=false; "normal" starts it here
# without them. Only the service started here (pid in -OutDir\service-pid.txt) is ever stopped; any other one is
# reported (exit 5), never ended. With IPC_EXIT_WHEN_IDLE=true set for the user, a service exits 5 s after its last
# client; -KeepAlive starts this one service without it (the environment of this process only).
#   powershell -File runtime.ps1 -Mode sbs | normal | stop | status [-KeepAlive] [-OutDir <dir>]
param([ValidateSet("sbs", "normal", "stop", "status")][string]$Mode = "status", [switch]$KeepAlive,
    [string]$OutDir = "$env:LOCALAPPDATA\right-eye-cost")
$ErrorActionPreference = "Stop"
$svcDir = "$env:APPDATA\MOPIC\OpenXR\src\xrt\targets\service"
$exe = "$svcDir\mopic-openxr-service.exe"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir "campaign.log"
$pidFile = Join-Path $OutDir "service-pid.txt"
$svcLog = Join-Path $OutDir ("service-logs\$Mode-" + (Get-Date -Format "MMdd-HHmmss") + ".log")
function Note($m) { $l = "$(Get-Date -Format s) [runtime] $m"; Write-Host $l; Add-Content -Path $log -Value $l -Encoding utf8 }
function Get-Svc { @(Get-Process mopic-openxr-service, mopic-openxr-service_huhd, monado-service -ErrorAction SilentlyContinue) }
function Describe($s) { ($s | ForEach-Object { "$($_.Name) pid $($_.Id) start $($_.StartTime.ToString('s'))" }) -join ", " }
function Get-Mine { if (Test-Path $pidFile) { [int](Get-Content $pidFile -TotalCount 1) } else { 0 } }
function Stop-Mine {
    $mine = Get-Mine
    if ($mine) {
        $p = Get-Process -Id $mine -ErrorAction SilentlyContinue
        if ($p -and $p.Name -eq "mopic-openxr-service") {
            $p | Stop-Process -Force
            for ($i = 0; $i -lt 20 -and (Get-Process -Id $mine -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 250 }
            Note "stopped my service pid $mine"
        }
        Remove-Item $pidFile -ErrorAction SilentlyContinue
    }
}
function Test-Foreign {
    $other = @(Get-Svc)
    if ($other.Count -gt 0) { Note "service I did not start is running: $(Describe $other) (left alone; report it, don't end it)"; return $true }
    return $false
}

if ($Mode -eq "status") { $s = Get-Svc; Note ("service: " + $(if ($s) { Describe $s } else { "none" }) + "; mine: $(Get-Mine)"); return }
Stop-Mine
if (Test-Foreign) { exit 5 }
if ($Mode -eq "stop") { Note "no service left"; return }
New-Item -ItemType Directory -Force (Split-Path $svcLog) | Out-Null
Remove-Item Env:MOPIC_MODE, Env:MOPIC_PREACQ_FOLLOW_SDK -ErrorAction SilentlyContinue
if ($Mode -eq "sbs") { $env:MOPIC_MODE = "sbs"; $env:MOPIC_PREACQ_FOLLOW_SDK = "false" }
if ($KeepAlive) { Remove-Item Env:IPC_EXIT_WHEN_IDLE, Env:IPC_EXIT_ON_DISCONNECT -ErrorAction SilentlyContinue }
# stdout/stderr to a file in -OutDir; it can hold license text: grep it, never print or share it whole
$p = Start-Process -FilePath $exe -WorkingDirectory $svcDir -WindowStyle Hidden -RedirectStandardOutput $svcLog -RedirectStandardError "$svcLog.err" -PassThru
Remove-Item Env:MOPIC_MODE, Env:MOPIC_PREACQ_FOLLOW_SDK -ErrorAction SilentlyContinue
Set-Content -Path $pidFile -Value $p.Id
Start-Sleep 6
if ($p.HasExited) { Note "service pid $($p.Id) exited at once (code $($p.ExitCode))"; Remove-Item $pidFile -ErrorAction SilentlyContinue; exit 3 }
Note "service started ($Mode$(if ($KeepAlive) { ", no idle exit" })) pid $($p.Id), output in $svcLog"
