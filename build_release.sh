#!/bin/sh
set -eu

# Stop immediately if configure or build fails; never package stale artifacts.
build_preset() {
    echo "Configuring $1 ..."
    cmake --preset "$1"
    echo "Building $1 ..."
    cmake --build --preset "$1"
}

build_preset Release

rm -rf out
mkdir -p out/switch/sphaira
cp build/Release/sphaira.nro out/switch/sphaira/sphaira.nro
(cd out && zip -r9 sphaira.zip switch)
