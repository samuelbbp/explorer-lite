#!/usr/bin/env bash
# Installs dist/explorer-lite-Setup-<version>.exe for real in the user's session (tools/pc-install.ps1, through a
# scheduled task whose action is conhost --headless: nothing shows) and brings back build/install/ (results, log).
#   tools/pc-install.sh <version>
set -euo pipefail
cd "$(dirname "$0")/.."
version=${1:?version}
setup=dist/explorer-lite-Setup-$version.exe
[ -f "$setup" ] || { echo "no $setup: just package-mac $version first" >&2; exit 1; }
rel=agent-work/explorer-lite-install
task=explorer-lite-install

pc ps - <<PS >/dev/null
\$d = Join-Path \$env:USERPROFILE '$rel'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue \$d
New-Item -ItemType Directory -Force \$d | Out-Null
PS
cleanup() {
  pc ps - <<PS >/dev/null 2>&1 || true
Unregister-ScheduledTask -TaskName '$task' -Confirm:\$false -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path \$env:USERPROFILE '$rel')
PS
}
trap cleanup EXIT
pc cp "$setup" "$rel/setup.exe" >/dev/null
pc cp tools/pc-install.ps1 "$rel/pc-install.ps1" >/dev/null

pc ps - <<PS
\$ErrorActionPreference = 'Stop'
\$d = Join-Path \$env:USERPROFILE '$rel'
\$sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
\$a = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument "--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File \`"\$d\\pc-install.ps1\`" -Dir \`"\$d\`" -Setup \`"\$d\\setup.exe\`""
\$p = New-ScheduledTaskPrincipal -UserId \$sid -LogonType Interactive
\$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -AllowStartIfOnBatteries
Register-ScheduledTask -TaskName '$task' -Action \$a -Principal \$p -Settings \$s -Force | Out-Null
Start-ScheduledTask -TaskName '$task'
\$done = Join-Path \$d 'done.txt'
for (\$t = 0; \$t -lt 300 -and -not (Test-Path \$done); \$t++) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path \$done)) { 'timeout'; exit 1 }
PS
rm -rf build/install
pc get "$rel/out" build/install >/dev/null
cat build/install/results.json
