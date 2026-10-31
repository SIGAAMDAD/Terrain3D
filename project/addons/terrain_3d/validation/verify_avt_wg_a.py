#!/usr/bin/env python3
"""WG-A source/contract checks. Requires no Godot runtime or render device."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[4]

def source(path):
    return (ROOT / path).read_text(encoding="utf-8")

def required(ok, message):
    if not ok:
        raise AssertionError(message)
    print("PASS", message)

def main():
    export = source("project/addons/terrain_3d/src/vt_demand_export_plugin.gd")
    coordinator = source("src/rendering/rd/core/terrain_3d_render_coordinator.cpp")
    contract = source("src/rendering/rd/vt/core/terrain_3d_vt_contract.h")
    region = source("src/authoring/vt/build/terrain_3d_st_godot_region_cooker.cpp")
    data = source("src/authoring/data/terrain_3d_data_cooking.cpp")
    megavt = source("src/authoring/vt/build/terrain_3d_incremental_megavt_cooker.cpp")
    importer = source("src/authoring/vt/io/terrain_3d_vt_godot_cooker.cpp")

    required('filename == "terrain3d_instances.t3dvt"' in export and
             'path_join( "terrain3d_instances.t3dvt" )' in coordinator and
             'path_join( "terrain3d_instances.t3dvt" )' in data,
             "Export includes exact V1 package with runtime/data authored path")
    for suffix in ('.t3sp', '.t3pm', '.vt_demand.t3vd'):
        required(suffix in export, f"Preserved {suffix} export")
    required('LEGACY_NATIVE_STEMS' in export and 'skip()' in export and
             'terrain3d_hlod_manifest.tres' in export,
             "Retained HLOD and editor/runtime DLL export filters")

    def macro(name):
        found = re.search(r'^#define\s+' + name + r'\s+(\d+)u', contract, re.M)
        required(bool(found), f"Defines {name}")
        return int(found.group(1))

    required(macro('T3D_VT_REGIONAL_SOURCE_RECIPE_VERSION') == 3, "Direct source recipe v3")
    required(not (ROOT / 'src/rendering/surface/terrain_3d_far_regional_cooker.cpp').exists() and
             'T3D_VT_CONSUMER_FAR' not in contract,
             "Legacy FAR consumer and its cooker are retired")
    required('m_nComposedAllocation' in region and
             'm_nComposedGeneration' in region and
             'region_height_cutout=terrain3d_surface.t3dst' in data,
             "Unified material generation drives the height/cutout package")
    required('t3d_vt_godot_cook_source_fragment' in megavt and
             't3d_vt_godot_import_far_region_directory' not in importer,
             "Shared V1 cold cook replaces indexed FAR intermediates")
    required(not (ROOT / 'src/rendering/rd/vt/io/terrain_3d_far_avt_region_file.h').exists(),
             "Retired regional FAR indexed-file reader deleted")

    required('t3dMegaVTLoadExportFragmentCold(' in megavt and
             't3dMegaVTSourceFingerprint( capture )' in megavt and
             't3dMegaVTPublishCachePairCold(' in megavt and
             't3d_vt_pack_open_baseline_view( candidate.ptr()' in megavt,
             "Incremental cook reuses validated source-keyed V1 fragments")
    required('publishedArtifactsMatchCold' in data and
             't3d_vt_demand_publish_triplet_cold' in data and
             't3dMegaVTFileEqualsBytesCold' in megavt,
             "Identical package/demand artifacts bypass publication")
    required('t3d_vt_godot_merge_packages( packages.data()' in megavt and
             't3d_vt_demand_publish_triplet_cold' in data,
             "Shared V1 material package and demand publication are retained")
    print('WG-A static contract checks passed (ST06 FAR-free representation)')

if __name__ == '__main__':
    try:
        main()
    except AssertionError as exc:
        print('FAIL', exc, file=sys.stderr)
        sys.exit(1)
