# Steam offline mode for test runs (dot-sourced by run-test.ps1, run-ladder.ps1 and run-matrix.ps1).
#
# Tests run with Steam offline by default and put it back online afterwards. Steam has no command-line switch for
# it: it reads the logged-in account's "WantsOfflineMode" from config\loginusers.vdf when it starts (the menu's "Go
# Offline" writes the same key and restarts Steam). So a switch is: write the key (and "SkipOfflineModeWarning", or a
# dialog asks first), `steam.exe -shutdown`, start Steam again and wait until the account is logged in
# (HKCU\Software\Valve\Steam\ActiveProcess ActiveUser != 0). About half a minute each way.
#
# Batches hold offline mode across their runs: the outer script switches once and sets MOPIC_STEAM_OFFLINE_HELD,
# and run-test.ps1 then neither switches nor restores.

$SteamOfflineHeldVar = "MOPIC_STEAM_OFFLINE_HELD"

# A path with each part spelled as on disk. Steam records the path it was started from (SteamPath, SteamExe, its
# library list), so starting it from the registry's lowercase "c:/program files (x86)/steam" made that the spelling.
function Get-OnDiskPath([string]$path) {
    $full = [System.IO.Path]::GetFullPath($path)
    $root = [System.IO.Path]::GetPathRoot($full).ToUpperInvariant()
    $out = $root
    foreach ($part in $full.Substring($root.Length).Split([char]'\', [StringSplitOptions]::RemoveEmptyEntries)) {
        $hit = @(Get-ChildItem -LiteralPath $out -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ieq $part } | Select-Object -First 1)
        $out = Join-Path $out $(if ($hit.Count -gt 0) { $hit[0].Name } else { $part })
    }
    return $out
}

function Get-SteamDir {
    try { return Get-OnDiskPath ((Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath -replace '/', '\') } catch { return $null }
}

function Get-SteamLoginUsersPath {
    $dir = Get-SteamDir
    if (-not $dir) { return $null }
    $path = Join-Path $dir "config\loginusers.vdf"
    if (Test-Path -LiteralPath $path) { return $path }
    return $null
}

# The account Steam logs in with: @{ id; name; start; end } (line indices of its braces), or $null. That is the
# logged-in one (ActiveProcess ActiveUser = SteamID64 - 76561197960265728) while Steam runs, else the AutoLogin one,
# else the newest Timestamp (current clients write no "MostRecent").
function Get-SteamRecentAccount([string[]]$lines) {
    $accounts = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*"(\d{17})"\s*$' -and $i + 1 -lt $lines.Count -and $lines[$i + 1] -match '^\s*\{\s*$') {
            $id = $Matches[1]; $start = $i + 1; $depth = 0; $end = -1
            for ($j = $start; $j -lt $lines.Count; $j++) {
                if ($lines[$j] -match '^\s*\{\s*$') { $depth++ }
                elseif ($lines[$j] -match '^\s*\}\s*$') { $depth--; if ($depth -eq 0) { $end = $j; break } }
            }
            if ($end -lt 0) { break }
            $block = $lines[$start..$end]
            $field = { param($k) $l = $block | Where-Object { $_ -match "`"$k`"\s+`"([^`"]*)`"" } | Select-Object -First 1; if ($l) { $l -replace ".*`"$k`"\s+`"([^`"]*)`".*", '$1' } else { "" } }
            $accounts += @{ id = $id; name = (& $field "AccountName"); start = $start; end = $end
                auto = ((& $field "AutoLogin") -eq "1"); recent = ((& $field "MostRecent") -eq "1"); ts = [int64]("0" + (& $field "Timestamp")) }
            $i = $end
        }
    }
    if ($accounts.Count -eq 0) { return $null }
    try {
        $active = [int64](Get-ItemProperty "HKCU:\Software\Valve\Steam\ActiveProcess" -ErrorAction Stop).ActiveUser
        if ($active -ne 0) {
            $hit = @($accounts | Where-Object { [decimal]$_.id - [decimal]76561197960265728 -eq $active })
            if ($hit.Count -gt 0) { return $hit[0] }
        }
    } catch { }
    foreach ($pick in @({ $_.recent }, { $_.auto })) {
        $hit = @($accounts | Where-Object $pick)
        if ($hit.Count -gt 0) { return $hit[0] }
    }
    return ($accounts | Sort-Object { $_.ts } -Descending | Select-Object -First 1)
}

# $true / $false from loginusers.vdf (the state Steam started in, or will start in), $null when unknown.
function Get-SteamWantsOffline {
    $path = Get-SteamLoginUsersPath
    if (-not $path) { return $null }
    $lines = [System.IO.File]::ReadAllLines($path)
    $acct = Get-SteamRecentAccount $lines
    if (-not $acct) { return $null }
    foreach ($l in $lines[$acct.start..$acct.end]) { if ($l -match '"WantsOfflineMode"\s+"(\d)"') { return $Matches[1] -eq "1" } }
    return $false
}

function Set-SteamWantsOffline([bool]$offline) {
    $path = Get-SteamLoginUsersPath
    if (-not $path) { throw "Steam loginusers.vdf not found" }
    $lines = [System.Collections.Generic.List[string]]::new([System.IO.File]::ReadAllLines($path))
    $acct = Get-SteamRecentAccount $lines.ToArray()
    if (-not $acct) { throw "no MostRecent account in $path" }
    $value = $(if ($offline) { "1" } else { "0" })
    foreach ($key in @("WantsOfflineMode", "SkipOfflineModeWarning")) {
        $found = $false
        for ($i = $acct.start; $i -le $acct.end; $i++) {
            if ($lines[$i] -match "^(\s*)`"$key`"(\s+)`"\d`"") {
                $lines[$i] = "$($Matches[1])`"$key`"$($Matches[2])`"$value`""; $found = $true; break
            }
        }
        if (-not $found) { $lines.Insert($acct.end, "`t`t`"$key`"`t`t`"$value`""); $acct.end++ }
    }
    [System.IO.File]::WriteAllLines($path, $lines, (New-Object System.Text.UTF8Encoding($false)))
}

function Test-SteamLoggedIn {
    try { return [int](Get-ItemProperty "HKCU:\Software\Valve\Steam\ActiveProcess" -ErrorAction Stop).ActiveUser -ne 0 } catch { return $false }
}

# Ready for a game launch: online = the account is logged in (ActiveUser != 0); offline = ActiveUser stays 0 (checked
# 2026-10-08), so the running steam.exe has registered itself (ActiveProcess pid) and its web helper is up.
function Test-SteamReady([bool]$offline) {
    if (-not $offline) { return (Test-SteamLoggedIn) }
    $steam = @(Get-Process steam -ErrorAction SilentlyContinue)
    if ($steam.Count -eq 0 -or -not (Get-Process steamwebhelper -ErrorAction SilentlyContinue)) { return $false }
    try { $pid2 = [int](Get-ItemProperty "HKCU:\Software\Valve\Steam\ActiveProcess" -ErrorAction Stop).pid } catch { return $false }
    return @($steam | Where-Object { $_.Id -eq $pid2 }).Count -gt 0
}

function Restart-SteamClient([bool]$offline, [int]$TimeoutS = 120) {
    $dir = Get-SteamDir
    $exe = Join-Path $dir "steam.exe"
    if (Get-Process steam -ErrorAction SilentlyContinue) {
        Start-Process -FilePath $exe -ArgumentList "-shutdown" | Out-Null
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Process steam -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
        if (Get-Process steam -ErrorAction SilentlyContinue) { throw "Steam did not shut down within 60 s" }
    }
    # ActiveUser keeps the last value until the new client logs in or clears it
    $deadline = (Get-Date).AddSeconds(15)
    while ((Test-SteamLoggedIn) -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
    Start-Process -FilePath $exe -ArgumentList "-silent" | Out-Null
    $deadline = (Get-Date).AddSeconds($TimeoutS)
    while (-not (Test-SteamReady $offline) -and (Get-Date) -lt $deadline) { Start-Sleep 2 }
    if (-not (Test-SteamReady $offline)) { throw "Steam was not ready ($(if ($offline) { 'offline' } else { 'logged in' })) within $TimeoutS s" }
    Start-Sleep 10   # let the client settle before a game launch asks it for anything
}

# Puts Steam in the wanted mode. Returns $true when it had to switch (the caller restores later), $false when Steam
# already was in that mode or the batch around this run holds it.
function Enter-SteamMode([bool]$offline) {
    if ($offline -and [Environment]::GetEnvironmentVariable($SteamOfflineHeldVar) -eq "1") { return $false }
    $current = Get-SteamWantsOffline
    if ($null -eq $current) { Write-Warning "Steam: loginusers.vdf has no recent account; leaving Steam as it is"; return $false }
    if ($current -eq $offline -and (Test-SteamReady $offline)) { return $false }
    Write-Host "Steam: switching to $(if ($offline) { 'offline' } else { 'online' }) mode (restarts Steam)" -ForegroundColor Cyan
    Set-SteamWantsOffline $offline
    Restart-SteamClient $offline
    Write-Host "Steam: $(if ($offline) { 'offline' } else { 'online' })" -ForegroundColor Cyan
    return $true
}
