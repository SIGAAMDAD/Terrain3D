#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
unit="$root/src/diagnostics/tests/composed_surface"
output="${TMPDIR:-/tmp}/t3d_ucmp_static_$$"
trap 'rm -f "$output"' EXIT HUP INT TERM
clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined -I "$root/src" \
  "$unit/test_ucmp_wave1_static_features.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_static_v1.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_cooker.cpp" \
  -o "$output"
"$output"
