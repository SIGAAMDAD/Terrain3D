#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
tests="$root/src/diagnostics/tests/composed_surface"
output="${TMPDIR:-/tmp}/t3d_cs16_mode6_$$"
trap 'rm -f "$output"' EXIT HUP INT TERM
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
  -fsanitize=address,undefined -I "$tests/cs15_stubs" -I "$root/src" \
  "$tests/test_bc7_mode6.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_baseline_reference_decode.cpp" \
  -o "$output"
"$output"
python3 "$tests/test_pack_activation_contract.py"
