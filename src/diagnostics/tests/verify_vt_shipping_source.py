#!/usr/bin/env python3
"""Source-level P09/P10 contract checks. Runtime acceptance is a separate Godot test."""
from pathlib import Path
import sys

SRC = Path(__file__).resolve().parents[2]

def check(relative, required=(), forbidden=()):
    content = (SRC / relative).read_text(encoding='utf-8')
    for term in required:
        if term not in content:
            raise AssertionError(f'{relative}: missing {term!r}')
    for term in forbidden:
        if term in content:
            raise AssertionError(f'{relative}: stale {term!r}')

check('terrain_3d.cpp', (
    'publishRuntimeMaterialViewCold()',
    'commitStartupActivationCold',
), (
    'm_assets->clearTextures()',
    'free_editor_textures enabled, reloading Assets path',
))
check('assets/catalog/terrain_3d_assets.cpp', (
    'm_textureList = runtime_list;',
    'createRuntimeMaterialViewCold()',
    'm_nFrozenMaterialSourceSignature = signature;',
))
check('assets/resources/terrain_3d_texture_asset.cpp', (
    'Terrain3DTextureAsset::createRuntimeMaterialViewCold',
    'result->m_nId = m_nId;',
    'result->m_fUvScale = m_fUvScale;',
), (
    'result->m_albedoTexture = m_albedoTexture',
    'result->m_packedPbrResource = m_packedPbrResource',
))
check('rendering/rd/core/terrain_3d_render_coordinator.cpp', (
    'loadPackageFileCold( path )',
    'vt_demand_index_path',
    'gpu_stream_transfer_budget',
    'published_no_sync',
))
check('diagnostics/core/terrain_3d_runtime_diagnostics.cpp', (
    'authoring_texture_residency',
    'streaming_renderer',
))
check('rendering/rd/vt/runtime/terrain_3d_virtual_texture.cpp', (
    't3d_vt_pack_runtime_open_file_cold',
    'vt_package_file_backed',
))
check('diagnostics/tests/systems/virtual_texturing_tests.cpp', (
    'runtime_material_view_preserves_identity_and_scalar_settings',
))
print('P09-P10 shipping source contracts: PASS')
