#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../../.." && pwd)
out="${TMPDIR:-/tmp}/t3d_st01_recook_hierarchy_$$"
trap 'rm -f "$out"' EXIT HUP INT TERM
python3 "$root/src/diagnostics/tests/superterrain/material/test_material_revision_contract.py"
clang++ -std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -fsanitize=address,undefined \
    -I "$root/src" \
    "$root/src/diagnostics/tests/superterrain/material/test_recook_hierarchy.cpp" \
    "$root/src/authoring/vt/integration/terrain_3d_composed_surface_recook_bridge.cpp" \
    -Wl,--gc-sections -o "$out"
"$out"
