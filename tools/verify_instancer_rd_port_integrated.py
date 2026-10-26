#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
switch = (root / 'src/instances/terrain_3d_instancer_bringup.h').read_text(encoding='utf-8')
mesh = (root / 'src/rendering/rd/instances/terrain_3d_rd_mesh_cache.cpp').read_text(encoding='utf-8')
mesh_h = (root / 'src/rendering/rd/instances/terrain_3d_rd_mesh_cache.h').read_text(encoding='utf-8')
renderer = (root / 'src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp').read_text(encoding='utf-8')
coordinator = (root / 'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text(encoding='utf-8')
constants = (root / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text(encoding='utf-8')

def fail(message: str) -> None:
    print(f'Instancer RD-port integration FAIL: {message}')
    sys.exit(1)

def body(signature: str, text: str) -> str:
    start = text.find(signature)
    if start < 0:
        fail(f'missing {signature}')
    brace = text.find('{', start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == '{':
            depth += 1
        elif text[index] == '}':
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    fail(f'unterminated {signature}')
    return ''

native_build = re.search(r's_pszNativeBuildId \( "([^"]+)" \)', constants)
plugin_build = re.search(r'EXPECTED_NATIVE_BUILD_ID: String = "([^"]+)"', plugin)
native_revision = re.search(r's_nNativeApiRevision \( (\d+)LL \)', constants)
plugin_revision = re.search(r'EXPECTED_NATIVE_API_REVISION: int = (\d+)', plugin)
if not native_build or not plugin_build or native_build.group(1) != plugin_build.group(1):
    fail('native/script build IDs are not synchronized')
if not native_revision or not plugin_revision or native_revision.group(1) != plugin_revision.group(1):
    fail('native/script API revisions are not synchronized')
if int(native_revision.group(1)) < 2026090901:
    fail('native/script API revision predates the integrated P3 lineage')
if '#define T3D_INSTANCER_RD_PORT 1' not in switch:
    fail('RD-port switch is not enabled')
if '#define T3D_INSTANCER_MINIMAL_RENDER_PATH 0' not in switch:
    fail('old minimal/magenta path is still active')
shared_arena = '#define T3D_INSTANCER_SHARED_MESH_ARENA 1' in switch

if shared_arena:
    for token in ('getSurfaceVertexArray', 'getSurfaceIndexArray', 'getSurfaceViewCount'):
        if token not in mesh_h:
            fail(f'missing shared-arena surface-view accessor {token}')
    upload = body('bool Terrain3DRdMeshCache::upload(', mesh)
    for token in ('ensureArena( m_hVertexArena', 'ensureArena( m_hIndex32Arena',
                  'uploadChangedChunks( m_hVertexArena', 'rebuildSurfaceViews( pSnapshot )', 't3dVectorCopyCold( m_materials'):
        if token not in upload:
            fail(f'production shared-arena mesh upload missing {token}')
else:
    for token in ('getDirectSurfaceVertexArray', 'getDirectSurfaceIndexArray', 'getDirectSurfaceCount'):
        if token not in mesh_h:
            fail(f'missing direct-surface accessor {token}')
    upload = body('bool Terrain3DRdMeshCache::upload(', mesh)
    for token in ('syncTextures( pSnapshot )', 'rebuildDirectSurfaces( pSnapshot )', 't3dVectorCopyCold( m_materials'):
        if token not in upload:
            fail(f'production direct-surface mesh upload missing {token}')

buffers = body('bool Terrain3DRdInstanceRenderer::ensureBuffers(', renderer)
p10_production = '#define T3D_INSTANCER_GPU_CULL_PRODUCTION 1' in switch
if p10_production:
    if 'const bool replace_indirect = !m_hIndirectBuffer.is_valid()' not in buffers or 'RESOURCE_INDIRECT_BUFFER' not in buffers:
        fail('P10 direct-surface RD port does not own its production indirect buffer')
else:
    if '#if T3D_INSTANCER_RD_PORT' not in buffers or 'const bool replace_indirect = false;' not in buffers:
        fail('pre-P10 RD port unexpectedly requires the indirect command buffer')
if 'm_hPreviousInstanceBuffer' not in buffers or 'm_hMaterialBuffer' not in buffers:
    fail('RD port dropped production motion/material buffers')

if 'bool Terrain3DRdInstanceRenderer::uploadShadowVisibility( void )' not in renderer:
    fail('shadow visible-index upload is still undefined')

prepare = body('bool Terrain3DRdInstanceRenderer::prepareForwardPass(', renderer)
for token in ('buildPackets( *pScene', 'uploadMaterials()'):
    if token not in prepare:
        fail(f'RD-port prepare path missing {token}')
if p10_production:
    if 'const bool gpu_visibility = T3D_INSTANCER_GPU_CULL_PRODUCTION != 0 && !m_bDirectDrawDebug;' not in prepare:
        fail('P10 RD port does not select GPU visibility with a CPU debug override')
else:
    for token in ('#if T3D_INSTANCER_RD_PORT', 'const bool gpu_visibility = false;'):
        if token not in prepare:
            fail(f'CPU RD-port prepare path missing {token}')

draw = body('bool Terrain3DRdInstanceRenderer::drawForwardView(', renderer)
for token in ('getForwardMaterialBinding', 'packet.m_nFirstVisible', 'draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u );'):
    if token not in draw:
        fail(f'production CPU reference path missing {token}')
if shared_arena:
    for token in ('getSurfaceVertexArray', 'getSurfaceIndexArray', 'm_meshCache.getVertexArray()',
                  'm_meshCache.getIndexArray()', 'draw_list_draw_indirect('):
        if token not in draw:
            fail(f'shared-arena production/reference draw split missing {token}')
else:
    for token in ('getDirectSurfaceVertexArray', 'getDirectSurfaceIndexArray'):
        if token not in draw:
            fail(f'production direct draw path missing {token}')

helper = body('static Terrain3DHLODStreamer *t3dRdGetInstanceHlodStreamer(', coordinator)
if '#if T3D_INSTANCER_MINIMAL_RENDER_PATH' not in helper or 'getHlodStreamer()' not in helper:
    fail('existing HLOD publication contract was changed by the instancer port')
if 't3dRdInstanceCellVisible( cell, frustums[0], rCamera, true )' not in renderer:
    fail('CPU direct path does not preserve HLOD frustum visibility')

print('Instancer RD-port integration PASS')
