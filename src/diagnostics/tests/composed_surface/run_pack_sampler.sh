#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
test_dir="$root/src/diagnostics/tests/composed_surface"
stubs="$test_dir/cs12_stubs"
clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined -I "$stubs" -I "$root/src" \
  "$test_dir/test_pack_sampler.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_baseline_pack_sampler.cpp" \
  -o /tmp/t3d_comp_surf_pack_sampler_test
/tmp/t3d_comp_surf_pack_sampler_test
