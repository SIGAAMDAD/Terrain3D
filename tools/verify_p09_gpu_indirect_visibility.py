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

resource_plan = text('src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.h')
for needle in (
    'T3D_RW_INDIRECT_VIS_INSTANCE_OFFSET = 48u',
    'T3D_RW_INDIRECT_VIS_CLUSTER_OFFSET = 60u',
    'T3D_RW_INDIRECT_VT_WORLD_OFFSET = 72u',
    'T3D_RW_INDIRECT_VT_CLUTTER_OFFSET = 84u',
    'T3D_RW_INDIRECT_VT_FOLIAGE_OFFSET = 96u',
    'T3D_RW_INDIRECT_BUFFER_BYTES = 108u',
):
    if needle not in resource_plan:
        failures.append(f'P09 indirect-buffer ABI missing: {needle}')

resource_plan_cpp = text('src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.cpp')
if 'rPlan.nBufferBytes[T3D_RW_GPU_BUFFER_INDIRECT_ARGS] = T3D_RW_INDIRECT_BUFFER_BYTES;' not in resource_plan_cpp:
    failures.append('P09 indirect buffer size is not driven by the shared ABI')

contract_cpp = text('src/rendering/rd/render_world/terrain_3d_render_world_contract.cpp')
if '+ T3D_RW_INDIRECT_BUFFER_BYTES;' not in contract_cpp:
    failures.append('P09 minimum GPU visibility memory plan does not include the full indirect ABI')

visibility = block(
    'src/rendering/rd/render_world/terrain_3d_render_world_gpu_visibility.cpp',
    'bool Terrain3DRenderWorldGpuVisibility::dispatch_internal_hot',
    'bool Terrain3DRenderWorldGpuVisibility::is_initialized',
)
if 'pFrame.nCellCount' not in visibility:
    failures.append('P09 cell dispatch is not based on the live CPU cell count')
if 'compute_list_dispatch_indirect( nList, hIndirect, T3D_RW_INDIRECT_VIS_INSTANCE_OFFSET )' not in visibility:
    failures.append('P09 instance visibility stage is not GPU-indirect')
if 'compute_list_dispatch_indirect( nList, hIndirect, T3D_RW_INDIRECT_VIS_CLUSTER_OFFSET )' not in visibility:
    failures.append('P09 cluster visibility stage is not GPU-indirect')
for legacy in ('pLimits.nMaxVisibleCells + 63u', 'pLimits.nMaxVisibleInstances + 63u'):
    if legacy in visibility:
        failures.append(f'P09 sealed-capacity dispatch remains in visibility hot path: {legacy}')
for telemetry in ('nLaunchedVariableGroups', 'nRequiredVariableGroups', 'nIndirectDispatchCount'):
    if telemetry not in visibility:
        failures.append(f'P09 visibility telemetry missing {telemetry}')

args_shader = text('project/addons/terrain_3d/rd/render_world_visibility_args.glsl')
for word_offset in ('write_dispatch(12u', 'write_dispatch(15u', 'write_dispatch(18u', 'write_dispatch(21u', 'write_dispatch(24u'):
    if word_offset not in args_shader:
        failures.append(f'P09 args shader missing indirect triplet {word_offset}')

vt_gpu = block(
    'src/rendering/rd/vt/gpu/terrain_3d_vt_gpu.cpp',
    'bool Terrain3DVTGpu::dispatch_prediction_hot',
    'bool Terrain3DVTGpu::schedule_request_readback_hot',
)
if vt_gpu.count('compute_list_dispatch_indirect') < 1:
    failures.append('P09 VT prediction tiers are not dispatched indirectly')
if 'pLimits.nMaxVisibleClusters' in vt_gpu:
    failures.append('P09 VT prediction hot path still launches from sealed visible-cluster capacity')
if 'nVariableTierDispatchCount' not in vt_gpu:
    failures.append('P09 VT indirect dispatch telemetry is missing')

render_world = text('src/rendering/rd/render_world/terrain_3d_render_world.cpp')
if 'dispatch_m04_hot( pLimits, pFrame )' not in render_world:
    failures.append('P09 main visibility call does not pass the live frame')
if 'dispatch_visibility_m05_hot( *m_pVisibility, pLimits, pFrame )' not in render_world:
    failures.append('P09 Hi-Z visibility call does not pass the live frame')

# P09 remains a required cumulative contract. Later patches may extend independent subsystems.

if failures:
    print('P09 verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)

print('P09 verification PASS')
print('P09: live cell dispatch + GPU-indirect visibility continuation + GPU-indirect VT prediction are present')
print('P09 cumulative contract remains present under P10/P11 extensions')
