#!/usr/bin/env bash
# Host (x86-64 Linux) build of the offline menu-texture extractor. NOT the cross-compile.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=tools/menu_tex_extract
g++ -std=c++17 -O2 -Wall \
    -Isrc -Ithird_party \
    tools/menu_tex_extract.cpp \
    src/worldmap/msbe_parser.cpp \
    src/stb_image_impl.cpp \
    -ldl \
    -o "$OUT"
echo "built: $OUT"
