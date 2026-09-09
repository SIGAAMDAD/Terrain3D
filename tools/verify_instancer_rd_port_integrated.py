#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
switch = (root / 'src/instances/terrain_3d_instancer_bringup.h').read_text(encoding='utf-8')
mesh = (root / 'src/rendering/rd/terrain_3d_rd_mesh_cache.cpp').read_text(encoding='utf-8')
mesh_h = (root / 'src/rendering/rd/terrain_3d_rd_mesh_cache.h').read_text(encoding='utf-8')
renderer = (root / 'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp').read_text(encoding='utf-8')
coordinator = (root / 'src/rendering/rd/terrain_3d_render_coordinator.cpp').read_text(encoding='utf-8')
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

builds = (
    'instancer-rd-port-integrated-2026-09-09',
    'instancer-authoritative-store-publication-2026-09-09',
    'instancer-rd-arraymesh-surface-binding-p0-2026-09-09',
    'memory-budget-zone-containment-p1-2026-09-09',
    'instance-local-active-storage-p2-2026-09-09',
)
if not any(build in constants and build in plugin for build in builds):
    fail('native/script build IDs do not identify the integrated port lineage')
if '#define T3D_INSTANCER_RD_PORT 1' not in switch:
    fail('RD-port switch is not enabled')
if '#define T3D_INSTANCER_MINIMAL_RENDER_PATH 0' not in switch:
    fail('old minimal/magenta path is still active')

for token in ('getDirectSurfaceVertexArray', 'getDirectSurfaceIndexArray', 'getDirectSurfaceCount'):
    if token not in mesh_h:
        fail(f'missing direct-surface accessor {token}')

upload = body('bool Terrain3DRdMeshCache::upload(', mesh)
for token in ('syncTextures( pSnapshot )', 'rebuildDirectSurfaces( pSnapshot )', 't3dVectorCopyCold( m_materials'):
    if token not in upload:
        fail(f'production direct-surface mesh upload missing {token}')

buffers = body('bool Terrain3DRdInstanceRenderer::ensureBuffers(', renderer)
if '#if T3D_INSTANCER_RD_PORT' not in buffers or 'const bool replace_indirect = false;' not in buffers:
    fail('RD port still requires the indirect command buffer')
if 'm_hPreviousInstanceBuffer' not in buffers or 'm_hMaterialBuffer' not in buffers:
    fail('RD port dropped production motion/material buffers')

if 'bool Terrain3DRdInstanceRenderer::uploadShadowVisibility( void )' not in renderer:
    fail('shadow visible-index upload is still undefined')

prepare = body('bool Terrain3DRdInstanceRenderer::prepareForwardPass(', renderer)
for token in ('#if T3D_INSTANCER_RD_PORT', 'const bool gpu_visibility = false;', 'buildPackets( *pScene', 'uploadMaterials()'):
    if token not in prepare:
        fail(f'CPU RD-port prepare path missing {token}')

draw = body('bool Terrain3DRdInstanceRenderer::drawForwardView(', renderer)
for token in (
    'getDirectSurfaceVertexArray',
    'getDirectSurfaceIndexArray',
    'getForwardMaterialBinding',
    'packet.m_nFirstVisible',
    'draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u );',
):
    if token not in draw:
        fail(f'production direct draw path missing {token}')

helper = body('static Terrain3DHLODStreamer *t3dRdGetInstanceHlodStreamer(', coordinator)
if '#if T3D_INSTANCER_MINIMAL_RENDER_PATH' not in helper or 'getHlodStreamer()' not in helper:
    fail('existing HLOD publication contract was changed by the instancer port')
if 't3dRdInstanceCellVisible( cell, frustums[0], rCamera, true )' not in renderer:
    fail('CPU direct path does not preserve HLOD frustum visibility')

print('Instancer RD-port integration PASS')
