#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []


def text(rel):
    return (ROOT / rel).read_text(encoding='utf-8')


def block(rel, start, end):
    data = text(rel)
    a = data.find(start)
    if a < 0:
        failures.append(f'missing block start {start!r} in {rel}')
        return ''
    b = data.find(end, a + len(start))
    if b < 0:
        failures.append(f'missing block end {end!r} in {rel}')
        return data[a:]
    return data[a:b]

instance_h = text('src/rendering/rd/terrain_3d_rd_instance_renderer.h')
for needle in ('m_dirtyRanges', 'm_dirtyCells', 'm_bTopologyChanged'):
    if needle not in instance_h:
        failures.append(f'instance snapshot ownership metadata missing {needle}')

instancer = block(
    'src/instances/terrain_3d_instancer.cpp',
    'bool Terrain3DInstancer::buildRdInstanceSnapshot',
    'Vector2i Terrain3DInstancer::getCellForLocalPosition',
)
for needle in ('m_dirtyRanges.pushBack', 'm_dirtyCells.pushBack', 'previous.m_nRevision != cell.m_nRevision'):
    if needle not in instancer:
        failures.append(f'instancer does not publish owner-known dirtiness: {needle}')

upload_scene = block(
    'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp',
    'bool Terrain3DRdInstanceRenderer::uploadScene',
    'bool Terrain3DRdInstanceRenderer::prepareMotionHistory',
)
if 'm_dirtyRanges' not in upload_scene or 'buffer_update' not in upload_scene:
    failures.append('renderer scene upload is not dirty-range driven')
if 'memcmp' in upload_scene:
    failures.append('renderer still rediscovers instance dirtiness with memcmp')

renderer_cpp = text('src/rendering/rd/terrain_3d_rd_instance_renderer.cpp')
for needle in ('buildGpuPacketTopology', 'dispatchInstanceCull', 'draw_list_draw_indirect'):
    if needle not in renderer_cpp:
        failures.append(f'GPU instance production path missing {needle}')
forward = block(
    'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp',
    'bool Terrain3DRdInstanceRenderer::prepareForwardPass',
    'bool Terrain3DRdInstanceRenderer::drawForwardView',
)
if 'const bool gpu_visibility = !m_bDirectDrawDebug;' not in forward:
    failures.append('production GPU visibility is not selected outside debug mode')
if 'dispatchInstanceCull' not in forward:
    failures.append('Forward+ production path does not dispatch GPU instance culling')
if ('Failed to build Terrain3D debug/reference instance packets' not in forward
        or 'Failed to upload Terrain3D debug/reference instance visibility' not in forward):
    failures.append('CPU reference culling is not isolated to the debug/reference branch')
if 'm_nGpuCullReuses' not in forward:
    failures.append('equivalent main-view GPU culling is not reused')

motion = block(
    'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp',
    'bool Terrain3DRdInstanceRenderer::prepareMotionHistory',
    'bool Terrain3DRdInstanceRenderer::uploadMaterials',
)
if 'm_motionDirtyRanges' not in motion or 'uploadPreviousMotionRanges' not in motion:
    failures.append('motion history is not range-driven on stable topology')
if 'm_dirtyInstanceChunks' in renderer_cpp:
    failures.append('legacy dirty-chunk rediscovery remains in renderer')

shader = text('project/addons/terrain_3d/rd/terrain_instance_cull.glsl')
for needle in ('atomicAdd(main_commands', 'atomicAdd(shadow_commands', 'main_visible[', 'shadow_visible['):
    if needle not in shader:
        failures.append(f'instance cull shader missing GPU output operation {needle}')

coordinator = text('src/rendering/rd/terrain_3d_render_coordinator.cpp')
for needle in (
    'm_shaderLibrary.getInstanceCull()',
    'm_cachedRegularInstanceScene.m_dirtyRanges.capacity() < regular_cells',
    'm_cachedInstanceScene.m_dirtyRanges.capacity() < combined_cells',
    'combined.m_dirtyRanges.append(',
    'rSnapshot.m_dirtyRanges.capacity() * sizeof( t3dRdInstanceDirtyRange_t )',
):
    if needle not in coordinator:
        failures.append(f'coordinator P11 integration missing {needle}')

shader_lib = text('src/rendering/rd/terrain_3d_rd_shader_library.cpp')
if 'terrain_instance_cull.glsl' not in shader_lib:
    failures.append('shader library does not load terrain_instance_cull.glsl')

if failures:
    print('P11 verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)

print('P11 verification PASS')
print('owner dirty ranges + production GPU instance visibility/indirect + range motion history present')
print('coordinator cold capacity includes P11 publication metadata')
