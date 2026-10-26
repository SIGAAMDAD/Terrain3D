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
    contract = source("src/rendering/rd/vt/core/terrain_3d_far_avt_contract.h")
    region = source("src/rendering/rd/vt/io/terrain_3d_far_avt_region_file.h")
    cooker = source("src/rendering/surface/terrain_3d_far_regional_cooker.cpp")
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

    required(macro('T3D_FAR_AVT_SCHEMA_VERSION') == 2, "Legacy schema 1 invalidated")
    required(macro('T3D_FAR_AVT_MIP_COUNT') == 1, "Only mip0 authored")
    required(macro('T3D_FAR_AVT_CHANNEL_COUNT') == 3, "All three FAR channels retained")
    required('T3D_FAR_AVT_REGION_RECORD_COUNT' in region and
             't3dFarAvtValidateRecord' in region and
             'record.m_nLevel != mip' in region,
             "Indexed reader checks complete ordered three-record contract")
    required('T3D_FAR_AVT_SCHEMA_VERSION' in cooker and
             't3dFarAvtOpenRegionCold' in cooker and
             't3dFarAvtReadTileCold' in cooker and
             't3dColdIndexPublishFile' in cooker,
             "Writer hashes schema, validates payload before reuse and atomically publishes")
    required('nMip >= T3D_FAR_AVT_MIP_COUNT' in region,
             "Reader rejects nonzero mips")

    # Machine-audit references (tests and comments excluded, report for review).
    readers = []
    for path in (ROOT / 'src').rglob('*'):
        if path.suffix not in ('.cpp', '.h'):
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        if 't3dFarAvtReadTileCold(' in text:
            readers.append(path.relative_to(ROOT).as_posix())
    required(set(readers) == {
        'src/rendering/rd/vt/io/terrain_3d_far_avt_region_file.h',
        'src/rendering/surface/terrain_3d_far_regional_cooker.cpp',
        'src/authoring/vt/io/terrain_3d_vt_godot_cooker.cpp',
    }, f"All FAR tile reader call sites accounted for: {readers}")
    required(re.search(r'c \+ 1u,\s*0u,', importer) is not None,
             "V1 FAR importer consumes mip zero only")
    required('for ( uint32_t mip = 0u; mip < T3D_FAR_AVT_MIP_COUNT; ++mip )' in cooker,
             "Region writer respects reduced record count")

    required('t3dMegaVTLoadExportFragmentCold(' in megavt and
             't3dMegaVTSourceFingerprint( capture )' in megavt and
             't3dMegaVTPublishCachePairCold(' in megavt and
             't3d_vt_pack_open_view_v1( candidate.ptr()' in megavt,
             "Incremental cook reuses validated source-keyed V1 fragments")
    required('publishedArtifactsMatchCold' in data and
             't3d_vt_demand_publish_triplet_cold' in data and
             't3dMegaVTFileEqualsBytesCold' in megavt,
             "Identical package/demand artifacts bypass publication")
    required('t3d_vt_godot_merge_packages_v1( packages.data()' in megavt and
             't3d_vt_godot_merge_packages_v1( parts, 2u' in data,
             "Established V1 combined merge retained")
    required('m_nAvtLastCookWrittenRegions' in cooker and
             'm_nAvtLastCookReusedRegions' in cooker and
             'm_nAvtLastCookWrittenBytes' in cooker,
             "FAR cold-write counters are present")
    print('WG-A static contract checks passed')

if __name__ == '__main__':
    try:
        main()
    except AssertionError as exc:
        print('FAIL', exc, file=sys.stderr)
        sys.exit(1)
