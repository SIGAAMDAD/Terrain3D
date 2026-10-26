#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
unit="$root/src/diagnostics/tests/composed_surface"
# Headless decoder validation: no Godot binary required.
clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined -I "$unit/cs15_stubs" -I "$root/src" \
  "$unit/test_cs15_reference_decoder.cpp" \
  "$root/src/authoring/vt/build/terrain_3d_composed_surface_v1_reference_decode.cpp" \
  -o /tmp/t3d_cs15_reference_decode_test
/tmp/t3d_cs15_reference_decode_test
# Native source compile checks against full Godot 4.7 generated bindings.
godotcpp=${T3D_GODOT_CPP_ROOT:-"$root/../godot-cpp"}
if [ -f "$godotcpp/gen/include/godot_cpp/classes/image.hpp" ]; then
  for source in \
    "$unit/test_cs15_native_pack.cpp" \
    "$root/src/data/formats/terrain_3d_vt_pack_reader.cpp" \
    "$root/src/authoring/vt/io/terrain_3d_vt_pack_writer.cpp" \
    "$root/src/authoring/vt/build/terrain_3d_composed_surface_v1_pack_sampler.cpp" \
    "$root/src/authoring/vt/build/terrain_3d_composed_surface_v1_reference_decode.cpp" \
    "$root/src/authoring/vt/build/terrain_3d_composed_surface_v1_godot_decode.cpp"; do
    clang++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -fsyntax-only \
      -DGDEXTENSION -I "$root/src" -I "$godotcpp/include" \
      -I "$godotcpp/gen/include" "$source"
  done
  echo "CS15 native Godot 4.7 syntax tests PASS (runtime linking unavailable)"
else
  echo "Godot 4.7 generated headers unavailable: native syntax verification skipped" >&2
fi
