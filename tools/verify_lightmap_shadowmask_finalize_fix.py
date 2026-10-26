#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
baker = (root / "project/addons/terrain_3d/tools/region_lightmap_baker.gd").read_text(encoding="utf-8")
data = (root / "src/data/model/terrain_3d_data.cpp").read_text(encoding="utf-8")
gpu = (root / "src/streaming/cache/terrain_3d_gpu_region_cache.cpp").read_text(encoding="utf-8")

errors = []

def require(condition, message):
    if not condition:
        errors.append(message)

slice_start = baker.find("func _shadowmask_slice_image")
slice_end = baker.find("func _nearest_power_of_two", slice_start)
slice_body = baker[slice_start:slice_end]
require(slice_start >= 0 and slice_end > slice_start, "shadowmask slice helper is missing")
require("readable.is_compressed()" in slice_body, "shadowmask slice is not checked for compression")
require("readable.decompress()" in slice_body, "compressed shadowmask slice is not decompressed")
require(slice_body.find("readable.decompress()") < slice_body.find("return readable"),
        "shadowmask slice can return before decompression")

extract_start = baker.find("func _extract_terrain_shadow_mask")
extract_end = baker.find("func _filter_baked_user_data", extract_start)
extract_body = baker[extract_start:extract_end]
require("source.get_region(" in extract_body, "terrain shadowmask extraction no longer crops the atlas")
require(slice_body.find("readable.decompress()") >= 0,
        "get_region still receives compressed shadowmask data")

for source, label in ((data, "Terrain3DData"), (gpu, "Terrain3DGpuRegionCache")):
    helper = source[source.find("makeTerrainShadowMaskImage"):]
    helper = helper[:helper.find("makeGpuRenderControlImage")]
    require("FileAccess::file_exists( shadow_path )" in helper,
            f"{label} still attempts ResourceLoader::load on a missing shadowmask path")
    require("Terrain3DUtil::getFilledImage" in helper,
            f"{label} has no fully-lit terrain-shadow fallback")

cook_start = data.find("bool Terrain3DData::writeGpuRuntimePage")
cook_end = data.find("int Terrain3DData::getRegionSlotReserve", cook_start)
cook_body = data[cook_start:cook_end]
require("makeTerrainShadowMaskImage( pRegion )" in cook_body,
        "runtime-page cook still loads the terrain shadow sidecar directly")
require("ResourceLoader::get_singleton()->load( lighting->getTerrainShadowMaskPath() )" not in cook_body,
        "runtime-page cook still hard-fails through direct shadowmask ResourceLoader load")
require("optional render enhancement" in cook_body,
        "runtime-page cook does not document optional-lighting failure semantics")

if errors:
    for error in errors:
        print("FAIL:", error)
    sys.exit(1)
print("Lightmap shadowmask finalize verification PASS")
