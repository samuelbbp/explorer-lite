# The build of `just build` (same flags), on the PC: run by `just build-pc` in the mirror that `pc sync` keeps.
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $env:LOCALAPPDATA 'fp-build\explorer-lite')
New-Item -ItemType Directory -Force build | Out-Null
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $env:LOCALAPPDATA 'fp-build\explorer-lite-zig-cache'
$zig = Join-Path $env:LOCALAPPDATA 'fp-tools\zig\zig.exe'
$src = @(Get-ChildItem src -Filter *.cpp | ForEach-Object { "src/$($_.Name)" })
$libs = '-luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32 -lcomctl32 -lshlwapi -ldwmapi -lversion' -split ' '
& $zig c++ -target x86_64-windows-gnu -O2 -std=c++17 -municode -fno-exceptions -fno-rtti -s -DUNICODE -D_UNICODE -DNOMINMAX `
    -Wall -Wno-unused-function -Wno-missing-braces -Wno-unknown-pragmas @src res/explorer-lite.rc -o build/explorer-lite.exe `
    '-Wl,--subsystem,windows' @libs 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { "build failed ($LASTEXITCODE)"; exit 1 }
