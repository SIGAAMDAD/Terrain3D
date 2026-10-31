#!/usr/bin/env python3
"""Reproducible source census for the ST00 FAR retirement boundary.

Reads only production source; tests/docs are deliberately excluded. Strict
anchors cause source drift to fail the gate instead of silently omitting paths.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]
SCAN_DIRS = (ROOT / 'src', ROOT / 'project/addons/terrain_3d')
EXTENSIONS = {'.cpp', '.h', '.glsl', '.glslinc', '.gdshader', '.gdshaderinc', '.inc', '.gd', '.py', '.json', '.md', '.cs'}
PATTERNS = {
    'far_consumer_enum': r'\bT3D_VT_CONSUMER_FAR\b',
    'far_channel_mask': r'\bT3D_VT_FAR_(?:ALLOWED|REQUIRED)_CHANNEL_MASK\b',
    'far_region_cooker': r'\bTerrain3DFarRegionalCooker\b|\bfarRegionalCooker\b',
    'far_stable_key': r'\bt3d_vt_(?:is_)?far_region_stable_key\b',
    'far_gpu_record': r'\b(?:far_vt_allocation|far_vt_generation|m_nFarVTAllocation|m_nFarVTGeneration)\b',
    'far_source_dirty': r'\b(?:FarAvt|far_avt|isGpuFarAvtRegionCookDirty|getGpuFarAvtCookedPackageGeneration|invalidateGpuFarSurface)\b',
    'far_legacy_manifest': r'\bfar_surface_vt\b|terrain3d_far_vt',
    'far_residency': r'\b(?:stage_far_region_demand_hot|getFarSurfaceAllocationCold|m_szFarSurfaceAllocations|FarSurfaceAllocation)\b',
    'far_height_shader': r'\b(?:t3d_far_height|terrain_far_height_vertex_v1|t3d_far_vertex_height16|sample_far_height_or_background)\b',
    'far_fragment_shader': r'\b(?:sample_far_surface_v1|sample_far_surface|far_authored_color|far_surface_solid|far_surface_alpha|far_surface_material|decode_far_surface_bits)\b',
    'far_image_producer': r'\b(?:t3dFarRegionalCookFarSurfaceAlpha|cookOneRegionalV1Cold|T3D_VT_LOGICAL_CHANNEL_FAR_HEIGHT)\b',
    'far_publication': r'\b(?:m_nFarVTPublicationGeneration|m_nFarAvtSourcePublicationRevision)\b',
    'far_settings': r'\b(?:T3D_FAR_VT_[A-Z_]+|far_vt_[a-z_]+|far_surface_response_quality|getFarVt[A-Za-z]+|(?:set|get)FarSurfaceResponseQuality)\b',
    'far_dirty_ledger': r'\b(?:FAR_VT|TERRAIN3D_PROFILE_EVENT_FAR_VT_WORK)\b',
    'far_cooker_member': r'\bm_farRegionalCooker\b',
    'far_shader_support': r'\b(?:far_vt_world_contains|far_vt_world_uv|terrain_far_index_exact|t3d_far_vertex_[a-z_]+|T3D_FAR_VERTEX_[A-Z_]+)\b',
    'far_diagnostics': r'\b(?:far_height_lattice|far_height_[a-z_]+|far_color_[a-z_]+)\b',
}
REQUIRED = {
    'src/rendering/surface/terrain_3d_far_regional_cooker.cpp': ('far_region_cooker', 'far_image_producer'),
    'src/Terrain3DConstants.h': ('far_settings',),
    'src/data/runtime/terrain_3d_data_residency_adapter.cpp': ('far_cooker_member',),
    'src/integration/godot/terrain_3d_resources.cpp': ('far_source_dirty',),
    'src/rendering/material/terrain_3d_material.h': ('far_settings',),
    'src/diagnostics/tests/shader_retirement/verify_retirement.py': ('far_height_shader',),
    'project/addons/terrain_3d/validation/verify_far_intermediate_retired.py': ('far_consumer_enum',),
    'project/addons/terrain_3d/setting_metadata.json': ('far_settings',),
    'project/addons/terrain_3d/menu/terrain_menu.gd': ('far_settings',),
    'src/diagnostics/core/terrain_3d_runtime_diagnostics.cpp': ('far_settings',),
    'src/integration/godot/terrain_3d_bindings.cpp': ('far_settings',),
    'src/rendering/material/terrain_3d_material.cpp': ('far_settings',),
    'src/streaming/gpu/terrain_3d_gpu_dirty_ledger.cpp': ('far_dirty_ledger',),
    'src/streaming/gpu/terrain_3d_gpu_dirty_ledger.h': ('far_dirty_ledger',),
    'src/streaming/integration/terrain_3d_streaming_integration.cpp': ('far_settings',),
    'src/streaming/policy/terrain_3d_performance_profile_manifest.inc': ('far_settings',),
    'src/streaming/policy/terrain_3d_runtime_profile.cpp': ('far_settings',),
    'src/authoring/data/terrain_3d_data_cooking.cpp': ('far_region_cooker', 'far_legacy_manifest'),
    'src/data/runtime/terrain_3d_data_residency_adapter.h': ('far_region_cooker',),
    'src/streaming/residency/terrain_3d_region_residency.cpp': ('far_region_cooker',),
    'src/authoring/vt/build/terrain_3d_vt_cooker.cpp': ('far_consumer_enum',),
    'src/rendering/rd/vt/core/terrain_3d_vt_contract.h': ('far_consumer_enum', 'far_channel_mask'),
    'src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.cpp': ('far_consumer_enum', 'far_residency'),
    'src/rendering/rd/core/terrain_3d_render_publication_builder.cpp': ('far_gpu_record', 'far_source_dirty'),
    'src/Terrain3DTypes.h': ('far_gpu_record',),
    'project/addons/terrain_3d/rd/terrain_clipmap_contract.glslinc': ('far_gpu_record',),
    'project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc': ('far_fragment_shader', 'far_gpu_record'),
    'project/addons/terrain_3d/rd/terrain_far_height_vertex_v1.glslinc': ('far_height_shader', 'far_gpu_record'),
    'project/addons/terrain_3d/rd/terrain_clipmap_vertex_inc.glslinc': ('far_height_shader',),
    'project/addons/terrain_3d/rd/terrain_clipmap.glsl': ('far_height_shader',),
    'project/addons/terrain_3d/rd/terrain_world_raster.glsl': ('far_height_shader',),
}


def owner(path):
    if path.startswith('src/diagnostics/tests/') or '/validation/' in path:
        return 'validation_test'
    if path.startswith('project/addons/terrain_3d/rd/'):
        return 'shader'
    if path.startswith('src/rendering/surface/'):
        return 'far_cooker'
    if path.startswith(('src/authoring/', 'src/data/model/', 'src/data/runtime/')):
        return 'authoring_data'
    if path.startswith('src/rendering/rd/vt/'):
        return 'vt_reader_residency'
    if path.startswith('src/rendering/rd/core/'):
        return 'publication'
    if path.startswith('src/diagnostics/'):
        return 'diagnostics'
    if path.startswith('project/addons/terrain_3d/'):
        return 'editor_addon'
    if path.startswith('src/integration/'):
        return 'godot_binding'
    if path.startswith('src/rendering/material/'):
        return 'material_settings'
    if path.startswith('src/streaming/'):
        return 'streaming'
    if path.startswith(('src/Terrain3DTypes.h', 'src/Terrain3DEnums.h')):
        return 'global_abi'
    return 'indirect'


def scan():
    compiled = {k: re.compile(v) for k, v in PATTERNS.items()}
    entries = []
    for parent in SCAN_DIRS:
        for path in sorted(parent.rglob('*')):
            if not path.is_file() or path.suffix not in EXTENSIONS:
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel.startswith('src/diagnostics/tests/superterrain/'):
                continue
            try:
                data = path.read_bytes()
                content = data.decode('utf-8')
            except UnicodeDecodeError:
                continue
            matches = []
            for line_number, line in enumerate(content.splitlines(), 1):
                matches.extend({'line': line_number, 'kind': key, 'symbol': m.group(0)}
                               for key, expression in compiled.items() for m in expression.finditer(line))
            if matches:
                entries.append({'path': rel, 'owner': owner(rel), 'sha256': hashlib.sha256(data).hexdigest(),
                                'references': matches})
    by_path = {entry['path']: entry for entry in entries}
    missing = []
    for path, tags in REQUIRED.items():
        actual = {entry['kind'] for entry in by_path.get(path, {}).get('references', [])}
        for tag in tags:
            if tag not in actual:
                missing.append(f'{path}: {tag}')
    if missing:
        raise AssertionError('Unaccounted required FAR semantic(s):\n' + '\n'.join(missing))
    return {
        'schema': 'ST00_FAR_INVENTORY_V1',
        'scope': ['src excluding src/diagnostics/tests/superterrain', 'project/addons/terrain_3d'],
        'patterns': PATTERNS,
        'required_anchors': REQUIRED,
        'file_count': len(entries),
        'validation_test_file_count': sum(e['owner'] == 'validation_test' for e in entries),
        'reference_count': sum(len(entry['references']) for entry in entries),
        'files': entries,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true', help='Update frozen inventory')
    args = parser.parse_args()
    inventory = scan()
    target = Path(__file__).with_name('far_inventory.json')
    result = json.dumps(inventory, indent=2, sort_keys=True) + '\n'
    if args.write:
        target.write_text(result, encoding='utf-8')
    elif not target.is_file() or target.read_text(encoding='utf-8') != result:
        raise SystemExit('FAR source inventory changed: inspect and deliberately re-freeze before Wave 2')
    print(f'ST00 FAR inventory OK: {inventory["file_count"]} source files '
          f'({inventory["validation_test_file_count"]} validation scripts), '
          f'{inventory["reference_count"]} explicit references')


if __name__ == '__main__':
    main()
