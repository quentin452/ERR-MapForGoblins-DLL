#!/usr/bin/env bash
# Build the DFLT (zlib/stb) offline MSBE verifier — native Linux, no game, no Wine.
# Output: tools/msbe_test/msbe_test
# Usage:  ./build.sh [optional: a .msb.dcx to parse right after building]
#   e.g.  ./build.sh "<ERR>/mod/map/MapStudio/m10_00_00_00.msb.dcx"   (expect 113 treasures)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cxx="$(command -v clang++ || command -v g++)"
out="$here/msbe_test"

echo "[build.sh] $cxx -> $out"
"$cxx" -std=c++20 -O2 -Wall \
    -I"$root/src" -I"$root/third_party" \
    "$here/msbe_test.cpp" \
    "$root/src/worldmap/msbe_parser.cpp" \
    "$root/src/stb_image_impl.cpp" \
    -o "$out"
echo "[build.sh] ok"

if [[ $# -ge 1 ]]; then
    echo "[build.sh] running on: $1"
    "$out" "$1"
fi
