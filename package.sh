#!/usr/bin/env bash
# The installer and the portable zip of explorer-lite from build/explorer-lite.exe (`just build <version>`; CI calls
# this from .github/workflows/release.yml):
#   dist/explorer-lite-Setup-<version>.exe and dist/explorer-lite-<version>-windows-x64.zip (+ a .sha256 of each)
# Needs zip and makensis. Works on Linux and macOS.
#   ./package.sh VERSION      X.Y.Z or, for a prerelease, X.Y.Z-<branch>.N
set -euo pipefail
cd "$(dirname "$0")"
version=${1:?usage: ./package.sh X.Y.Z[-prerelease]}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$ ]] || { echo "bad version: $version" >&2; exit 1; }
numeric=${version%%-*} # NSIS's VIProductVersion only takes X.Y.Z.W
exe=build/explorer-lite.exe
out=dist/explorer-lite
rm -rf dist && mkdir -p "$out"
[ -f "$exe" ] || { echo "no $exe" >&2; exit 1; }
# the exe has to carry this version (update.h): otherwise the updater would offer this same version forever
# (exact: the string ends in its NUL and is not preceded by a digit or a dot, so 0.2.8 does not pass for 0.2.8-x.1)
LC_ALL=C grep -qE "(^|[^0-9.])${version//./\\.}\$" < <(LC_ALL=C tr '\0' '\n' < "$exe" 2>/dev/null) \
    || { echo "$exe is not $version (just build $version stamps it)" >&2; exit 1; }
cp "$exe" "$out/explorer-lite.exe"
# the zip also carries Windows Lite's explorer.exe stand-in (stub/explorer.cpp); the installer does not need it
[ -f build/explorer.exe ] || { echo "no build/explorer.exe" >&2; exit 1; }
cp build/explorer.exe "$out/explorer.exe"
(cd "$out" && zip -q -X "../explorer-lite-$version-windows-x64.zip" explorer-lite.exe explorer.exe)
makensis -V2 -NOCD -DVERSION="$version" -DVERSION_NUM="$numeric" installer/explorer-lite.nsi
# the installer's .sha256 is required by the updater (a release asset); the zip's, for whoever downloads it by hand
for f in "explorer-lite-Setup-$version.exe" "explorer-lite-$version-windows-x64.zip"; do
    (cd dist && { shasum -a 256 "$f" 2>/dev/null || sha256sum "$f"; }) | awk -v f="$f" '{print $1"  "f}' > "dist/$f.sha256"
done
ls -lh dist/explorer-lite-*
