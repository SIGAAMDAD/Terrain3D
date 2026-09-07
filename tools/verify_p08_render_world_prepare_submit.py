#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []

def text(rel):
    return (ROOT / rel).read_text(encoding='utf-8')

def require(rel, needle, label):
    if needle not in text(rel):
        failures.append(f'{label}: missing {needle!r} in {rel}')

def forbid_block(rel, start, end, needle, label):
    data = text(rel)
    a = data.find(start)
    if a < 0:
        failures.append(f'{label}: missing block start {start!r}')
        return
    b = data.find(end, a + len(start))
    block = data[a:] if b < 0 else data[a:b]
    if needle in block:
        failures.append(f'{label}: forbidden {needle!r} in block')

# P08 contract: compute visibility/VT preparation is distinct from raster submission.
require('src/rendering/rd/render_world/terrain_3d_render_world.h',
        'prepare_camera_visibility_hot', 'P08 compute prepare API')
require('src/rendering/rd/render_world/terrain_3d_render_world.h',
        'submit_camera_pass_hot', 'P08 raster submit API')
require('src/rendering/rd/render_world/terrain_3d_render_world.h',
        'nComputePrepareCount', 'P08 prepare telemetry')
require('src/rendering/rd/render_world/terrain_3d_render_world.h',
        'nComputeReuseCount', 'P08 reuse telemetry')
require('src/rendering/rd/render_world/terrain_3d_render_world.h',
        'nRasterSubmitCount', 'P08 submit telemetry')

runtime_h = text('src/rendering/rd/render_world/terrain_3d_render_world_runtime.h')
for field in (
    'm_nRenderFrame',
    'm_nViewportKey',
    'm_nViewStateHash',
    'm_nSceneGeneration',
    'm_nTerrainGeneration',
    'm_nCullingGeneration',
    'm_nViewCount',
):
    if field not in runtime_h:
        failures.append(f'P08 prepared-view key missing {field}')

# Pass kind and draw list must not participate in compute-preparation reuse.
key_start = runtime_h.find('struct t3dRenderWorldPreparedViewKey_t')
key_end = runtime_h.find('class Terrain3DRenderWorldRuntime', key_start)
key_block = runtime_h[key_start:key_end] if key_start >= 0 and key_end >= 0 else ''
for forbidden in ('m_passKind', 'passKind', 'm_nDrawList', 'drawList'):
    if forbidden in key_block:
        failures.append(f'P08 prepared-view key incorrectly contains pass-specific field {forbidden}')

runtime_cpp = text('src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp')
render_start = runtime_cpp.find('bool Terrain3DRenderWorldRuntime::render_main_camera_hot')
render_end = runtime_cpp.find('bool Terrain3DRenderWorldRuntime::is_initialized', render_start)
render_block = runtime_cpp[render_start:render_end] if render_start >= 0 and render_end >= 0 else ''
for needle in (
    'm_preparedViewKey.matches( key )',
    'm_RenderWorld.record_compute_reuse_hot()',
    'm_RenderWorld.prepare_camera_visibility_hot',
    'm_RenderWorld.submit_camera_pass_hot',
):
    if needle not in render_block:
        failures.append(f'P08 runtime render path missing {needle!r}')
if render_block.count('m_RenderWorld.submit_camera_pass_hot') != 1:
    failures.append('P08 runtime must issue exactly one raster submit per callback')

# Prepared state must be invalidated on shutdown/release and generation-keyed for scene/terrain changes.
require('src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp',
        'invalidate_prepared_view();', 'P08 prepared-view invalidation')
require('src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp',
        'm_nScenePublicationGeneration', 'P08 scene generation')
require('src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp',
        'm_nTerrainPublicationGeneration', 'P08 terrain generation')

# Keep the hot-path implementation allocation-free and stack/local-state only.
forbidden_hot = ('PackedByteArray', 'PackedInt32Array', 'eastl::vector', 'memnew', 'new ')
for marker in forbidden_hot:
    if marker in render_block:
        failures.append(f'P08 render hot path contains allocation/container marker {marker!r}')

# Coordinator diagnostics must expose actual reuse behavior.
for key in (
    'render_world_compute_prepares',
    'render_world_compute_reuses',
    'render_world_raster_submits',
):
    require('src/rendering/rd/terrain_3d_render_coordinator.cpp', key, f'P08 diagnostic {key}')

if failures:
    print('P08 render-world prepare/submit verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)

print('P08 render-world prepare/submit verification PASS')
