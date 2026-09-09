#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
switch_probe = (root / 'src/instances/terrain_3d_instancer_bringup.h').read_text(encoding='utf-8')
if '#define T3D_INSTANCER_RD_PORT 1' in switch_probe and '#define T3D_INSTANCER_MINIMAL_RENDER_PATH 0' in switch_probe:
    print('PASS: historical bring-up gate superseded by integrated instancer RD port')
    sys.exit(0)
mesh_h = (root / 'src/rendering/rd/terrain_3d_rd_mesh_cache.h').read_text(encoding='utf-8')
mesh = (root / 'src/rendering/rd/terrain_3d_rd_mesh_cache.cpp').read_text(encoding='utf-8')
renderer = (root / 'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp').read_text(encoding='utf-8')
coordinator = (root / 'src/rendering/rd/terrain_3d_render_coordinator.cpp').read_text(encoding='utf-8')
instancer = (root / 'src/instances/terrain_3d_instancer.cpp').read_text(encoding='utf-8')
shader = (root / 'project/addons/terrain_3d/rd/terrain_instances.glsl').read_text(encoding='utf-8')
shader_library = (root / 'src/rendering/rd/terrain_3d_rd_shader_library.cpp').read_text(encoding='utf-8')
constants = (root / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text(encoding='utf-8')


def fail(message: str) -> None:
    print(f'Instancer direct-surface bring-up verification FAIL: {message}')
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
    fail(f'unterminated body for {signature}')
    return ''

build = 'instancer-direct-surface-bringup-2026-09-09'
if build not in constants or build not in plugin:
    fail('native/script build ID mismatch')

for token in ('getDirectSurfaceVertexArray', 'getDirectSurfaceIndexArray', 'getDirectSurfaceCount'):
    if token not in mesh_h:
        fail(f'missing direct surface accessor {token}')

rebuild = body('bool Terrain3DRdMeshCache::rebuildDirectSurfaces(', mesh)
for token in ('vertex_buffer_create', 'vertex_array_create', 'index_buffer_create', 'index_array_create'):
    if token not in rebuild:
        fail(f'direct surface build missing {token}')
if 'OWNER_INSTANCES' not in rebuild:
    fail('direct surface resources are not instance-owned')

ensure_arena = body('bool Terrain3DRdMeshCache::ensureArena(', mesh)
if ensure_arena.find('discardArrays();') < 0:
    fail('mesh arena rebuild does not discard child array views before parent buffers')

preview_draw = body('bool Terrain3DRdInstanceRenderer::drawPacket(', renderer)
for token in ('getDirectSurfaceVertexArray', 'getDirectSurfaceIndexArray', 'UINT16_MAX', 'UINT32_MAX', 'draw_list_draw('):
    if token not in preview_draw:
        fail(f'preview direct lane missing {token}')

forward_draw = body('bool Terrain3DRdInstanceRenderer::drawForwardView(', renderer)
for token in ('getDirectSurfaceVertexArray', 'getDirectSurfaceIndexArray', 'draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u );'):
    if token not in forward_draw:
        fail(f'Forward+ direct lane missing {token}')
if 'draw_list_draw_indirect' not in forward_draw:
    fail('production indirect submission disappeared')

pipelines = body('bool Terrain3DRdInstanceRenderer::ensureForwardPipelines(', renderer)
for token in ('set_enable_depth_test( false )', 'set_enable_depth_write( false )', 'COMPARE_OP_ALWAYS', 'POLYGON_CULL_DISABLED'):
    if token not in pipelines:
        fail(f'Forward+ bring-up pipeline missing {token}')

view_state = body('bool Terrain3DRdInstanceRenderer::ensureViewState(', renderer)
for token in ('set_enable_depth_test( false )', 'COMPARE_OP_ALWAYS', 'rd->free_rid( state.m_hFramebuffer )', 'rd->free_rid( state.m_szPipeline[pipeline_index] )'):
    if token not in view_state:
        fail(f'preview pipeline/resource safety missing {token}')

initialize = body('bool Terrain3DRdInstanceRenderer::initialize(', renderer)
if '#if !T3D_INSTANCER_MINIMAL_RENDER_PATH' not in initialize or '( void )rInstanceCullShader;' not in initialize:
    fail('minimal initialization still depends on GPU cull shader')

height = body('t3dUsableHeight_t Terrain3DInstancer::getUsableHeight(', instancer)
if '#if !T3D_INSTANCER_MINIMAL_RENDER_PATH' not in height or 'getRaycastResult' not in height:
    fail('editor physics raycast is not isolated from minimal bring-up mode')

if 'bringup = "#define T3D_INSTANCE_BRINGUP";' not in shader or 'vec4(1.0, 0.0, 1.0, 1.0)' not in shader:
    fail('compile-time solid-magenta bring-up shader version missing')
if 'StringName( "bringup" )' not in shader_library or '#if T3D_INSTANCER_MINIMAL_RENDER_PATH' not in shader_library:
    fail('shader library does not route minimal instance variants to the bring-up shader')
set1 = shader.find('layout(set = 1, binding = 0)')
set1_guard = shader.rfind('#ifndef T3D_INSTANCE_BRINGUP', 0, set1)
set1_guard_end = shader.find('#endif', set1_guard)
if set1 < 0 or set1_guard < 0 or set1_guard_end < set1:
    fail('bring-up fragment shader still statically depends on material texture set 1')

upload = body('bool Terrain3DRdMeshCache::upload(', mesh)
minimal_upload = upload.split('#endif', 1)[0] if '#if T3D_INSTANCER_MINIMAL_RENDER_PATH' in upload else ''
if 'rebuildDirectSurfaces( pSnapshot )' not in upload or 'return true;' not in upload:
    fail('mesh-cache bring-up upload does not terminate after standalone surface publication')
if 'RD direct-surface upload failed to retain mesh surface/LOD metadata' not in upload:
    fail('mesh-cache direct path is not isolated from production arena metadata upload')

uniform = body('bool Terrain3DRdInstanceRenderer::ensureUniformSet(', renderer)
if '#if !T3D_INSTANCER_MINIMAL_RENDER_PATH' not in uniform:
    fail('minimal set-0 still requires production material/motion descriptors')

if 'const bool bInstancePassEnabled = rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE' not in coordinator:
    fail('minimal instance lane is not opaque-only')

print('Instancer direct-surface bring-up verification PASS')
