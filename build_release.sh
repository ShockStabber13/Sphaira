#!/bin/sh
set -eu

# Reuse existing CMake configuration and built dependencies for quick edits.
# CMake's build system automatically reconfigures if its build files change.
cd "$(dirname "$0")"

if [ ! -f build/Release/CMakeCache.txt ] || [ "${1:-}" = "--reconfigure" ]; then
    echo "Configuring Release ..."
    cmake --preset Release
fi

echo "Building Release (incremental) ..."
cmake --build --preset Release

# Package only if the build above succeeded.
rm -rf out
mkdir -p out/switch/sphaira
cp build/Release/sphaira.nro out/switch/sphaira/sphaira.nro
(cd out && zip -q -r9 sphaira.zip switch)
echo "Release ready: out/sphaira.zip"
