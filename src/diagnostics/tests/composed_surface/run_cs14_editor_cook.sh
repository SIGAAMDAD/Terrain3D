#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
unit="$root/src/diagnostics/tests/composed_surface"
clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined -I "$unit/cs13_stubs" -I "$root/src" \
  "$unit/test_cs14_editor_cook.cpp" \
  "$root/src/authoring/vt/integration/terrain_3d_composed_surface_editor_cook.cpp" \
  "$root/src/authoring/vt/integration/terrain_3d_composed_surface_edit_journal.cpp" \
  "$root/src/authoring/vt/integration/terrain_3d_composed_surface_recook_bridge.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_v3_cold_pipeline.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_v3_pack_sampler.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_static_v3.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_cooker.cpp" \
  "$root/src/authoring/vt/io/terrain_3d_composed_surface_sidecar_writer.cpp" \
  -o /tmp/t3d_cs14_editor_cook_test
/tmp/t3d_cs14_editor_cook_test
