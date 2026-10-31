#!/usr/bin/env python3
"""Cold semantic source contract; not a substitute for Vulkan reference captures."""
from pathlib import Path

root = Path(__file__).resolve().parents[5]
src = (root / 'src/authoring/vt/integration/terrain_3d_composed_surface_editor_source.cpp').read_text()
for symbol in (
    'getTextureAsset( int( i ) )',
    'getAlbedoColor()', 'getUvScale()', 'getNormalDepth()', 'getRoughness()',
    'getMetallic()', 'getAoStrength()', 'getAoLightAffect()',
    'getTextureDetiles()', 'getMiddleAutoShaderEnabled()',
    'getProjectionEnabled()', 'm_nMaterialRevision',
    'candidate_missing_or_stale_mip_page', 'source_changed_before_candidate_acceptance',
    'request.m_bReferenceOnly = true',
    'Terrain3DFilePublication publication', 'publication.prepare()',
    'publication.publish()', 'publication.commit()',
    'publication.rollback()', 'FileAccess::get_file_as_bytes( stagedPath )',
):
    assert symbol in src, symbol
rt = (root / 'src/authoring/vt/integration/terrain_3d_composed_surface_recook_bridge.cpp').read_text()
for symbol in ('allPages', 'presentMask', '( presentMask & bit )',
               'nPages != allPages', 'page.m_nMip >= allMips',
               'expectedMask == 0u && presentMask'):
    assert symbol in rt, symbol
print('ST01 authored-source and fail-closed recook integration: PASS')
