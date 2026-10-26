#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
shader = (root / "project/addons/terrain_3d/shaders/includes/none_functions.gdshaderinc").read_text(encoding="utf-8")
sync = (root / "src/core/utility/terrain_3d_sync.h").read_text(encoding="utf-8")
lightmap = (root / "project/addons/terrain_3d/tools/region_lightmap_baker.gd").read_text(encoding="utf-8")

errors = []

def require(condition, message):
    if not condition:
        errors.append(message)

require("ivec2T3D_REGION_MAP_HALF" not in shader,
        "none_functions.gdshaderinc still contains invalid ivec2T3D_REGION_MAP_HALF token")
require("ivec2 half_map = ivec2(T3D_REGION_MAP_HALF);" in shader,
        "none_functions.gdshaderinc is missing the corrected ivec2 constructor")

require("sizeof( T ) == 2u" in sync,
        "MSVC atomic width guard does not admit 16-bit atomics")
for intrinsic in (
    "_InterlockedCompareExchange16",
    "_InterlockedExchange16",
    "_InterlockedExchangeAdd16",
    "_InterlockedOr16",
    "_InterlockedAnd16",
):
    require(intrinsic in sync, f"missing MSVC 16-bit atomic intrinsic: {intrinsic}")
require("typedef t3dAtomic_t<uint16_t> t3dAtomicU16_t;" in sync,
        "missing t3dAtomicU16_t alias")

require("ProjectSettings.localize_path" in lightmap,
        "regional lightmap output path is not normalized to a project-local path")
require('not normalized.begins_with("res://")' in lightmap,
        "regional lightmap output does not reject non-res:// paths")
require("ResourceLoader.load(" in lightmap and '"LightmapGIData", ResourceLoader.CACHE_MODE_IGNORE' in lightmap,
        "working LightmapGIData is not reloaded after save")
require("light_data.resource_path.simplify_path() != working_data_path.simplify_path()" in lightmap,
        "reloaded LightmapGIData path is not validated")
require("The prepared LightmapGIData has no valid res:// bake path" in lightmap,
        "editor bake path does not validate prepared LightmapGIData resource_path")

prepare_pos = lightmap.find("var output_ready: Dictionary = prepare_output_directory(terrain)")
paths_pos = lightmap.find("var output_paths: Dictionary = _region_output_paths(terrain, location)")
require(prepare_pos >= 0 and paths_pos >= 0 and prepare_pos < paths_pos,
        "prepare_region_bake still computes region output paths before normalizing/creating the output directory")

if errors:
    for error in errors:
        print("FAIL:", error)
    sys.exit(1)
print("Lightmap/shader/atomic verification PASS")
