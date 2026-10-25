# explorer-lite commands. `build` runs where it is called (the Mac, CI); from the Linux dev box use build-mac and
# package-mac, which send the work to the Mac (`mac sync` mirror ~/agent-work/mirror/explorer-lite). Nothing starts on
# a machine above 70 % CPU (machine-load; waits up to 10 min).

set shell := ["bash", "-euo", "pipefail", "-c"]

mac_dir := "agent-work/mirror/explorer-lite"
# outside the mirror: `mac sync` deletes what is not in this tree
mac_cache := "agent-work/explorer-lite-cache"
libs := "-luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32 -lcomctl32 -lshlwapi -ldwmapi -lversion"

# build/explorer-lite.exe (zig c++), stamped as VERSION when given
build version="":
    #!/usr/bin/env bash
    set -euo pipefail
    v="{{version}}"
    def=()
    if [ -n "$v" ]; then
      [[ $v =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || { echo "bad version: $v" >&2; exit 1; }
      n=${v%%-*}
      def=(-DEXPLORER_LITE_VERSION="\"$v\"" -DEXPLORER_LITE_VERSION_RC="${n//./,},0")
    fi
    python3 i18n/generar.py >/dev/null
    mkdir -p build
    ZIG_LOCAL_CACHE_DIR=${ZIG_LOCAL_CACHE_DIR:-build/zig-cache} zig c++ -target x86_64-windows-gnu -O2 -std=c++17 \
      -municode -fno-exceptions -fno-rtti -s -DUNICODE -D_UNICODE -DNOMINMAX \
      -Wall -Wno-unused-function -Wno-missing-braces -Wno-unknown-pragmas "${def[@]}" \
      src/*.cpp res/explorer-lite.rc -o build/explorer-lite.exe -Wl,--subsystem,windows {{libs}}
    # Windows Lite's C:\Windows\explorer.exe: hands explorer.exe's launches to explorer-lite (stub/explorer.cpp)
    ZIG_LOCAL_CACHE_DIR=${ZIG_LOCAL_CACHE_DIR:-build/zig-cache} zig c++ -target x86_64-windows-gnu -O2 -std=c++17 \
      -municode -fno-exceptions -fno-rtti -s -DUNICODE -D_UNICODE -DNOMINMAX -Wall \
      stub/explorer.cpp -o build/explorer.exe -Wl,--subsystem,windows -lshell32 -lole32
    # a GUI never opens a console: the PE subsystem has to be 2 (windows)
    for exe in build/explorer-lite.exe build/explorer.exe; do
      python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(); pe=struct.unpack('<I',d[0x3c:0x40])[0]; s=struct.unpack('<H',d[pe+92:pe+94])[0]; sys.exit(0 if s==2 else 'not a GUI exe (subsystem %d)' % s)" "$exe"
    done
    ls -l build/explorer-lite.exe build/explorer.exe

# build on the Mac and bring back build/explorer-lite.exe
build-mac version="": (_cpu "mac")
    mac sync . explorer-lite >/dev/null
    mac sh 'cd ~/{{mac_dir}} && export PATH=/opt/homebrew/bin:$PATH && ZIG_LOCAL_CACHE_DIR=$HOME/{{mac_cache}}/zig just build {{version}}'
    mkdir -p build
    mac get {{mac_dir}}/build/explorer-lite.exe build/explorer-lite.exe
    mac get {{mac_dir}}/build/explorer.exe build/explorer.exe

# build on the PC when the Mac is busy (its zig in %LOCALAPPDATA%\fp-tools\zig) and bring back build/explorer-lite.exe
build-pc: (_cpu "windows")
    python3 i18n/generar.py >/dev/null
    pc sync . explorer-lite >/dev/null
    pc ps tools/pc-build.ps1
    mkdir -p build
    pc get 'AppData/Local/fp-build/explorer-lite/build/explorer-lite.exe' build/explorer-lite.exe
    ls -l build/explorer-lite.exe

# installer + zip (+ .sha256) of VERSION into dist/ (CI does this for releases)
package version: (build version)
    ./package.sh {{version}}

# the same, built and packaged on the Mac
package-mac version: (_cpu "mac")
    mac sync . explorer-lite >/dev/null
    mac sh 'cd ~/{{mac_dir}} && export PATH=/opt/homebrew/bin:$PATH && ZIG_LOCAL_CACHE_DIR=$HOME/{{mac_cache}}/zig just package {{version}}'
    mkdir -p dist
    mac get '{{mac_dir}}/dist/explorer-lite-*' dist/

# a scenario (tools/scenarios/<name>.txt) with build/explorer-lite.exe on the PC, in the user's session on a desktop of
# its own; results in build/test-<name>/
test-pc name: (_cpu "windows")
    tools/pc-test.sh {{name}}

# installs dist/explorer-lite-Setup-VERSION.exe for real (silently, then again as an update) and checks it; results in
# build/install/
install-pc version: (_cpu "windows")
    tools/pc-install.sh {{version}}

# translations: the strings in the code (i18n/fuente.json) and every language has them all
i18n:
    python3 i18n/extraer.py
    python3 i18n/comprobar.py es
    python3 i18n/generar.py

# what main would publish now (version and notes; nothing is published)
release-notes mode="ci":
    .github/release.sh plan {{mode}}

# wait until the machine answers and is at or below 70 % CPU
_cpu host:
    #!/usr/bin/env bash
    set -uo pipefail
    for _ in $(seq 40); do
      cpu=$(machine-load get {{host}} | jq -r '.cpu | floor') || { echo "{{host}} does not answer" >&2; exit 1; }
      [ "$cpu" -gt 70 ] || exit 0
      echo "{{host}} at ${cpu}% CPU: waiting" >&2
      sleep 15
    done
    echo "{{host}} stayed above 70 % CPU" >&2
    exit 1
