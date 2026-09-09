#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
renderer = (root / 'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp').read_text(encoding='utf-8')
renderer_h = (root / 'src/rendering/rd/terrain_3d_rd_instance_renderer.h').read_text(encoding='utf-8')
mesh = (root / 'src/rendering/rd/terrain_3d_rd_mesh_cache.cpp').read_text(encoding='utf-8')
texture = (root / 'src/rendering/rd/terrain_3d_rd_texture_manager.cpp').read_text(encoding='utf-8')
texture_h = (root / 'src/rendering/rd/terrain_3d_rd_texture_manager.h').read_text(encoding='utf-8')
coordinator = (root / 'src/rendering/rd/terrain_3d_render_coordinator.cpp').read_text(encoding='utf-8')
constants = (root / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text(encoding='utf-8')


def fail(message: str) -> None:
    print(f'Instance fault-reporting / texture-fallback verification FAIL: {message}')
    sys.exit(1)


def body(signature: str, text: str) -> str:
    start = text.find(signature)
    if start < 0:
        fail(f'missing {signature}')
    brace = text.find('{', start)
    if brace < 0:
        fail(f'missing body for {signature}')
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    fail(f'unterminated body for {signature}')
    return ''


expected_builds = (
    'instancer-rd-port-integrated-2026-09-09',
    'instancer-authoritative-store-publication-2026-09-09',
    'instancer-rd-arraymesh-surface-binding-p0-2026-09-09',
    'memory-budget-zone-containment-p1-2026-09-09',
    'forward-color-blend-attachment-contract-fix-2026-09-09',
    'instancer-authoritative-visible-index-fix-2026-09-09',
)
if not any(build in constants and build in plugin for build in expected_builds):
    fail('native/script build ID contract does not identify this patch lineage')

prepare = body('bool Terrain3DRdInstanceRenderer::prepareForwardPass(', renderer)
for token in (
    'const bool scene_empty = pScene->m_instances.empty() && pScene->m_cells.empty();',
    'const bool scene_incomplete = pScene->m_instances.empty() != pScene->m_cells.empty();',
    'RD instance publication contains renderable scene records but no renderable mesh publication',
    'RD minimal instance prepare could not resolve any published mesh source keys',
):
    if token not in prepare:
        fail(f'Forward+ prepare fault line missing: {token}')

preview = body('bool Terrain3DRdInstanceRenderer::renderPreview(', renderer)
if 'RD editor preview mesh-cache upload failed:' not in preview:
    fail('preview mesh-cache failure is not reported separately')
if 'Failed to upload RD editor preview mesh/material data' in preview:
    fail('preview still collapses mesh/material failures into one generic error')

materials = body('bool Terrain3DRdInstanceRenderer::uploadMaterials(', renderer)
binding_call = materials.find('rebuildMaterialBindings()')
commit = materials.find('m_nUploadedMeshGeneration = m_meshCache.getSourceGeneration();')
if binding_call < 0 or commit < 0 or commit < binding_call:
    fail('material mesh-generation is committed before material bindings succeed')

sync = body('bool Terrain3DRdMeshCache::syncTextures(', mesh)
if 'retaining mesh geometry with fallback materials' not in sync:
    fail('missing texture manager still aborts mesh geometry instead of falling back')
if 'createTexture( source.m_layout' not in sync or 'false );' not in sync:
    fail('imported instance material textures are not using non-fatal creation semantics')

if 'bool bFailureFatal = true' not in texture_h:
    fail('texture creation API lacks explicit fatal/non-fatal policy')
render_create = body('void Terrain3DRdTextureManager::renderCreateTexture(', texture)
if 'if ( bFailureFatal )' not in render_create:
    fail('texture manager does not protect shared context from optional texture failure')
if 'Terrain3DRdContext::OWNER_UNSPECIFIED' not in render_create:
    fail('texture resources bypass context owner inference')

for token in (
    'mesh_upload_attempts',
    'mesh_upload_failures',
    'mesh_texture_fallbacks',
    'mesh_last_error',
):
    if token not in renderer:
        fail(f'instance diagnostics missing {token}')

if 'Forward+ PREPARE produced zero draws for a non-empty published scene' in coordinator:
    fail('successful pass-specific zero-draw PREPARE is still treated as a fault')
for token in (
    '[RD instances] Forward+ PREPARE failed; pass=',
    'packet_input_cells=',
    'resolved_cells=',
    'unresolved_cells=',
    'mesh_upload_failures=',
    'texture_fallbacks=',
):
    if token not in coordinator:
        fail(f'coordinator stage report missing {token}')

for token in (
    'getMeshTextureFallbackCount',
    'getMeshCacheLodCount',
    'getMeshCacheSurfaceCount',
):
    if token not in renderer_h:
        fail(f'low-overhead fault counter getter missing {token}')

print('Instance fault-reporting / texture-fallback verification PASS')
