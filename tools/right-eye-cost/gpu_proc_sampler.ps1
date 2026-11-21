# Per-process GPU engine utilisation, once a second, from the Windows "GPU Engine" performance counters (read only,
# no admin): to see what other processes (mopic-openxr-service: compositor + interleaving, Mopic Hub: eye tracking,
# dwm) take of the GPU during a VR run against a flat run. Writes <out>.csv: time, qpc_ns, pid, process, luid,
# engtype, utilisation % (summed over that process's engines of that type and adapter). Stops after -Seconds or when
# <out>.stop exists.
#   powershell -ExecutionPolicy Bypass -File gpu_proc_sampler.ps1 -Out cells\gpu-<label> -Seconds 900
param([Parameter(Mandatory)][string]$Out, [int]$Seconds = 900)
$ErrorActionPreference = "SilentlyContinue"
$csv = "$Out.csv"
New-Item -ItemType Directory -Force (Split-Path $csv) | Out-Null
"time,qpc_ns,pid,process,luid,engtype,util" | Set-Content -Path $csv -Encoding ascii
$names = @{}
$end = (Get-Date).AddSeconds($Seconds)
Add-Type -Namespace GpuSamp -Name Q -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool QueryPerformanceCounter(out long v); [DllImport("kernel32.dll")] public static extern bool QueryPerformanceFrequency(out long v);'
$freq = [long]0; [void][GpuSamp.Q]::QueryPerformanceFrequency([ref]$freq)
while ((Get-Date) -lt $end -and -not (Test-Path "$Out.stop")) {
    $t0 = Get-Date
    $set = Get-Counter -Counter "\GPU Engine(*)\Utilization Percentage" -MaxSamples 1
    $q = [long]0; [void][GpuSamp.Q]::QueryPerformanceCounter([ref]$q)
    $qns = [long]([double]$q * 1e9 / [double]$freq)
    $agg = @{}
    foreach ($s in $set.CounterSamples) {
        if ($s.CookedValue -le 0) { continue }
        if ($s.InstanceName -match '^pid_(\d+)_luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_\d+_eng_\d+_engtype_(.+)$') {
            $key = "$($Matches[1])|$($Matches[2])|$($Matches[3])"
            $agg[$key] = $(if ($agg.ContainsKey($key)) { $agg[$key] } else { 0 }) + $s.CookedValue
        }
    }
    $lines = foreach ($k in $agg.Keys) {
        $p = $k.Split("|")
        $procId = [int]$p[0]
        if (-not $names.ContainsKey($procId)) { $proc = Get-Process -Id $procId; $names[$procId] = $(if ($proc) { $proc.ProcessName } else { "?" }) }
        "{0},{1},{2},{3},{4},{5},{6:N2}" -f $t0.ToString("HH:mm:ss"), $qns, $procId, $names[$procId], $p[1], $p[2], $agg[$k]
    }
    if ($lines) { Add-Content -Path $csv -Value $lines -Encoding ascii }
    $left = 1000 - ((Get-Date) - $t0).TotalMilliseconds
    if ($left -gt 0) { Start-Sleep -Milliseconds ([int]$left) }
}
