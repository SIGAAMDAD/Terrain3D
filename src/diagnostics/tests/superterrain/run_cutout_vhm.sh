#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
# project source root is four parents from src/diagnostics/tests/superterrain
scratch=$(mktemp -d "${TMPDIR:-/tmp}/terrain3d_st03_st04_XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
cxx="${CXX:-clang++}"
common="-std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -I $root/src"
if [ "${T3D_ST_SANITIZE:-0}" = "1" ]; then
    common="$common -fsanitize=address,undefined -fno-omit-frame-pointer"
fi
"$cxx" $common "$root/src/diagnostics/tests/superterrain/cutout/test_cutout.cpp" \
    "$root/src/authoring/vt/cutout/terrain_3d_cutout_cooker.cpp" \
    "$root/src/rendering/rd/vt/cutout/terrain_3d_cutout_reader.cpp" -o "$scratch/st03"
"$scratch/st03" "$root/src/diagnostics/tests/superterrain/corpus"
"$cxx" $common "$root/src/diagnostics/tests/superterrain/vhm/test_vhm.cpp" \
    "$root/src/authoring/vhm/terrain_3d_vhm_cooker.cpp" \
    "$root/src/rendering/rd/vhm/terrain_3d_vhm_reader.cpp" \
    "$root/src/rendering/rd/vhm/terrain_3d_vhm_residency.cpp" -o "$scratch/st04"
"$scratch/st04" "$root/src/diagnostics/tests/superterrain/corpus"
