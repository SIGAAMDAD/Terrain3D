#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../../.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/terrain3d_st06_XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
cxx="${CXX:-clang++}"
common="-std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -I $root/src"
if [ "${T3D_ST_SANITIZE:-0}" = "1" ]; then
    common="$common -fsanitize=address,undefined -fno-omit-frame-pointer"
fi
"$cxx" $common \
    "$root/src/diagnostics/tests/superterrain/publication/test_st06_package.cpp" \
    "$root/src/rendering/rd/superterrain/terrain_3d_st_region_package.cpp" \
    "$root/src/rendering/rd/superterrain/terrain_3d_st_publication.cpp" \
    "$root/src/authoring/vt/build/terrain_3d_st_region_source_cooker.cpp" \
    "$root/src/authoring/vhm/terrain_3d_vhm_cooker.cpp" \
    "$root/src/authoring/vt/cutout/terrain_3d_cutout_cooker.cpp" \
    "$root/src/rendering/rd/vhm/terrain_3d_vhm_reader.cpp" \
    "$root/src/rendering/rd/vhm/terrain_3d_vhm_residency.cpp" \
    "$root/src/rendering/rd/vt/cutout/terrain_3d_cutout_reader.cpp" \
    -o "$scratch/st06"
"$scratch/st06" "$root/src/diagnostics/tests/superterrain/corpus"
