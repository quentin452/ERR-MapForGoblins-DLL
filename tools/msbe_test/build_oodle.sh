#!/usr/bin/env bash
# Build the KRAK (Oodle) offline MSBE verifier. Prefers the NATIVE path (no Wine):
# dlopen's ERR's native liboo2corelinux64.so.9. Falls back to the Windows/Wine exe only if
# you have no native .so.
# Output: tools/msbe_test/msbe_oodle_native
# Usage:  ./build_oodle.sh [optional: a KRAK .msb.dcx to parse after building]
#   The script auto-locates liboo2corelinux64.so.9; override with OODLE_SO=/path/to/it.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cxx="$(command -v clang++ || command -v g++)"
out="$here/msbe_oodle_native"

echo "[build_oodle.sh] $cxx -> $out (native, -ldl)"
"$cxx" -std=c++20 -O2 -Wall \
    -I"$root/src" -I"$root/third_party" \
    "$here/msbe_oodle_native.cpp" \
    "$root/src/worldmap/msbe_parser.cpp" \
    "$root/src/stb_image_impl.cpp" \
    -ldl -o "$out"
echo "[build_oodle.sh] ok"

# Locate the native Oodle .so (ERR ships it). Override via OODLE_SO=...
so="${OODLE_SO:-}"
if [[ -z "$so" ]]; then
    for cand in \
        "$HOME"/Games/*/internals/launcher/liboo2corelinux64.so.9 \
        "$HOME"/Games/*/mod/menu/deploy/liboo2corelinux64.so.9 \
        /home/*/Games/*/internals/launcher/liboo2corelinux64.so.9; do
        [[ -f "$cand" ]] && { so="$cand"; break; }
    done
fi

if [[ -n "$so" ]]; then
    echo "[build_oodle.sh] native Oodle: $so"
else
    echo "[build_oodle.sh] WARN: no liboo2corelinux64.so.9 found — set OODLE_SO=/path. (Wine"
    echo "                fallback: build tools/msbe_test/msbe_oodle_test.cpp w/ clang-cl+xwin,"
    echo "                run under wine with the game's oo2core_6_win64.dll.)"
fi

if [[ $# -ge 1 && -n "$so" ]]; then
    echo "[build_oodle.sh] running on: $1"
    "$out" "$so" "$1"
fi
