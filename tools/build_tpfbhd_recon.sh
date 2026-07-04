#!/usr/bin/env bash
# Host (x86-64 Linux) build of the offline BHF4 (.tpfbhd/.tpfbdt) recon tool. NOT the cross-compile.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=tools/tpfbhd_recon
g++ -std=c++17 -O2 -Wall \
    -Isrc -Ithird_party \
    tools/tpfbhd_recon.cpp \
    src/worldmap/msbe_parser.cpp \
    src/stb_image_impl.cpp \
    -ldl \
    -o "$OUT"
echo "built: $OUT"
