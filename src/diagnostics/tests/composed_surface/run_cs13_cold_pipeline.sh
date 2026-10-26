#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
unit="$root/src/diagnostics/tests/composed_surface"
clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined -I "$unit/cs13_stubs" -I "$root/src" \
  "$unit/test_cs13_cold_pipeline.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_v1_cold_pipeline.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_v1_pack_sampler.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_static_v1.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_cooker.cpp" \
  "$root/src/authoring/vt/io/terrain_3d_composed_surface_sidecar_writer.cpp" \
  -o /tmp/t3d_cs13_cold_pipeline_test
/tmp/t3d_cs13_cold_pipeline_test
