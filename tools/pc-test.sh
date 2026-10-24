#!/usr/bin/env bash
# Runs a scenario (tools/scenarios/<name>.txt, the steps of tools/pc-run.ps1) with build/explorer-lite.exe in the
# user's session on the PC, on a desktop of its own, and brings back build/test-<name>/ (results.json, the log, PNGs).
#   tools/pc-test.sh <name>
# Through a scheduled task (the only way from ssh into the user's session) whose action is conhost --headless, so no
# console window ever shows; the task is removed at the end, and so is the PC folder.
set -euo pipefail
cd "$(dirname "$0")/.."
name=${1:?scenario name (tools/scenarios/<name>.txt)}
steps=tools/scenarios/$name.txt
[ -f "$steps" ] || { echo "no $steps" >&2; exit 1; }
[ -f build/explorer-lite.exe ] || { echo "no build/explorer-lite.exe: just build-mac first" >&2; exit 1; }
rel=agent-work/explorer-lite-test
task=explorer-lite-test

pc ps - <<EOF >/dev/null
\$d = Join-Path \$env:USERPROFILE '$rel'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue \$d
New-Item -ItemType Directory -Force \$d | Out-Null
EOF
cleanup() {
  pc ps - <<EOF >/dev/null 2>&1 || true
Unregister-ScheduledTask -TaskName '$task' -Confirm:\$false -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path \$env:USERPROFILE '$rel')
EOF
}
trap cleanup EXIT
pc cp build/explorer-lite.exe "$rel/explorer-lite.exe" >/dev/null
pc cp tools/pc-run.ps1 "$rel/pc-run.ps1" >/dev/null
pc cp "$steps" "$rel/steps.txt" >/dev/null

echo "running $name in the user's session (own desktop)…"
pc ps - <<EOF
\$ErrorActionPreference = 'Stop'
\$d = Join-Path \$env:USERPROFILE '$rel'
\$sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
\$a = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument "--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File \`"\$d\\pc-run.ps1\`" -Dir \`"\$d\`" -Steps \`"\$d\\steps.txt\`""
\$p = New-ScheduledTaskPrincipal -UserId \$sid -LogonType Interactive
\$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -AllowStartIfOnBatteries
Register-ScheduledTask -TaskName '$task' -Action \$a -Principal \$p -Settings \$s -Force | Out-Null
Start-ScheduledTask -TaskName '$task'
'started'
\$done = Join-Path \$d 'done.txt'
for (\$t = 0; \$t -lt 600 -and -not (Test-Path \$done); \$t++) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path \$done)) { 'timeout: the scenario did not finish in 5 min'; exit 1 }
EOF
rm -rf "build/test-$name"
pc get "$rel/out" "build/test-$name" >/dev/null
cat "build/test-$name/results.json"
