# Installs explorer-lite for real in the user's session, as the updater would: the setup silently (/S), checked; then
# the resident in the tray (as Start with Windows does: --tray, no window); then the same setup again while it runs
# (the update path: --quit, files replaced, --updated). Started by tools/pc-install.sh through a scheduled task.
param([string]$Dir = $PSScriptRoot, [string]$Setup)
$ErrorActionPreference = 'Stop'
$out = Join-Path $Dir 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$app = Join-Path $env:LOCALAPPDATA 'Programs\explorer-lite'
$exe = Join-Path $app 'explorer-lite.exe'
$log = Join-Path $env:LOCALAPPDATA 'explorer-lite\explorer-lite.log'
$r = [ordered]@{ checks = [ordered]@{}; errors = @() }
function Check([string]$name, $ok, $value) { $r.checks[$name] = [ordered]@{ ok = [bool]$ok; value = "$value" }; if (-not $ok) { $script:r.errors += $name } }
function Resident { @(Get-CimInstance Win32_Process -Filter "Name='explorer-lite.exe'" | Where-Object { $_.ExecutablePath -eq $exe }) }
try {
    # Start-Process -Wait would also wait for what the setup starts (the reopened explorer-lite): WaitForExit
    $t0 = Get-Date
    $p = Start-Process -FilePath $Setup -ArgumentList '/S' -PassThru
    $p.WaitForExit()
    Check 'install exit code' ($p.ExitCode -eq 0) $p.ExitCode
    Check 'install seconds' $true ([math]::Round(((Get-Date) - $t0).TotalSeconds, 1))
    Check 'exe sha256' (Test-Path $exe) ((Get-FileHash $exe -Algorithm SHA256).Hash.ToLower())
    $run = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).'explorer-lite'
    Check 'Run key' ($run -eq "`"$exe`" --tray") $run
    Check 'Start menu shortcut' (Test-Path (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\explorer-lite.lnk')) ''
    $un = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\explorer-lite' -ErrorAction SilentlyContinue
    Check 'uninstall entry' ($un -and $un.DisplayVersion) $un.DisplayVersion

    if (-not (Resident)) { Start-Process -FilePath $exe -ArgumentList '--tray' }
    Start-Sleep -Seconds 3
    $res = @(Resident)
    Check 'resident running' ($res.Count -eq 1) ($res | ForEach-Object { $_.ProcessId })
    if ($res.Count -ge 1) {
        $pf = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -Filter "IDProcess=$($res[0].ProcessId)"
        Check 'resident private working set MB' $true ([math]::Round($pf.WorkingSetPrivate / 1MB, 1))
    }
    $pid1 = ($res | Select-Object -First 1).ProcessId

    # the update path: the same setup while it runs
    $p = Start-Process -FilePath $Setup -ArgumentList '/S' -PassThru
    $p.WaitForExit()
    Check 'reinstall exit code' ($p.ExitCode -eq 0) $p.ExitCode
    Start-Sleep -Seconds 3
    $res = @(Resident)
    Check 'resident back after reinstall' ($res.Count -eq 1 -and $res[0].ProcessId -ne $pid1) "$pid1 -> $(($res | ForEach-Object { $_.ProcessId }) -join ',')"
} catch {
    $r.errors += "$_"
} finally {
    if (Test-Path $log) { Get-Content $log -Tail 30 | Set-Content -Encoding UTF8 (Join-Path $out 'explorer-lite.log') }
    $r | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $out 'results.json')
    New-Item -ItemType File -Force (Join-Path $Dir 'done.txt') | Out-Null
}
