#!/usr/bin/env python3
"""UCMP-00..03 offline source/ownership gates, not Vulkan parity evidence."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[4]
def source(path):
    return (root / path).read_text(encoding='utf-8')

matrix = json.loads(source('src/diagnostics/tests/composed_surface/ucmp00_material_feature_matrix.json'))
assert len(matrix['features']) >= 19
assert {'static_candidate', 'dependency_conditional', 'runtime', 'unsupported'} == {
    feature['classification'] for feature in matrix['features']}
assert matrix['parity_capture_accepted'] is False

contract = source('src/rendering/rd/vt/core/terrain_3d_composed_surface_contract.h')
assert '#define T3D_CS_FILE_VERSION           1u' in contract
assert '#define T3D_CS_COMPOSITION_ABI        1u' in contract
assert 't3d_cs_import_region_mips_complete' in contract
assert 'nCount == 21u ? 4u' in contract
pipeline = source('src/authoring/vt/build/terrain_3d_composed_surface_v1_cold_pipeline.cpp')
assert 't3d_cs13_hash_integer( h, T3D_CS_SOURCE_RECIPE_REVISION )' in pipeline
editor = source('src/authoring/vt/integration/terrain_3d_composed_surface_editor_source.cpp')
for key in ('tile.m_nMip = mip', 'rPage.m_nMip', 'request.m_source.m_fBlendSharpness',
            'rMaterial.getMiddleMaterialLodBias()', 'm_nMaterialRevision',
            'm_pCurrent = t3d_cs_editor_current', 'm_bReferenceOnly = true'):
    assert key in editor, key
assert 'getControls( positions )' in editor
assert 'terrain_composed.t3sc.candidate' in editor

importer = source('src/authoring/vt/io/terrain_3d_vt_godot_cooker.cpp')
for key in ('t3d_cs_validate_sidecar_bytes( pSidecar',
            't3d_cs_import_region_mips_complete( records, count, tiles, signature )',
            't3d_cs_ucmp_prepare_channel_cold', 't3d_vt_page_builder_build_logical_channel_mips_u8',
            'source.m_PreparedChannels', 't3d_vt_composed_region_stable_key',
            'T3D_VT_CONSUMER_COMPOSED', 't3d_vt_godot_merge_packages_v1'):
    assert key in importer, key

coordinator = source('src/rendering/rd/core/terrain_3d_render_coordinator.cpp')
assert 'request.m_cook.m_bReferenceOnly' in coordinator
assert 'request.m_cook.m_source.m_bCanonicalMaterialParityAccepted' in coordinator
assert 't3d_vt_godot_import_composed_sidecar_v1(' in coordinator
assert 'T3D_CS_ACTIVE' in source('project/addons/terrain_3d/rd/terrain_composed_surface_sampling.glslinc')
assert 'T3D_TIER' in source('project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc')

static = source('src/authoring/vt/build/terrain_3d_composed_surface_static_v1.cpp')
bridge = source('src/authoring/vt/build/terrain_3d_composed_surface_v1_pack_sampler.cpp')
writer = source('src/authoring/vt/io/terrain_3d_composed_surface_sidecar_writer.cpp')
assert '#define T3D_CS_SOURCE_RECIPE_REVISION 3u' in contract
assert 'm_pReadTerrainSurface' in bridge and 't3d_cs12_bridge_surface' in bridge
assert 't3d_cs_resolve_auto' in static and 't3d_cs_detile' in static
assert 'std::isfinite( autoBase )' in editor and 'unsupported_auto_material_id' in editor
assert 'identical' in writer and 'memcmp( scratch, pBytes + offset, block )' in writer
assert 'getTextureDetiles()' in editor and 'getHeights( surfacePositions )' in editor
assert 'getNormals( surfacePositions )' in editor
assert 'autoShader && pageUsesAuto' in editor and 'pageUsesAuto |= ( value & 1u ) != 0u' in editor

print('UCMP Wave1 source/recipe/mip/V1 ownership and fail-closed cutover: PASS')
