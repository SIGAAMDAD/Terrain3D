#!/usr/bin/env python3
from pathlib import Path
import re
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


stream_h = text('src/rendering/rd/render_world/terrain_3d_render_world_streaming.h')
for field in ('pPageToJob', 'pFreeJobs', 'pQueuedJobs', 'pJobQueuePosition', 'pReadyJobs'):
    if field not in stream_h:
        failures.append(f'P10 streaming fixed index missing {field}')

stream_cpp = text('src/rendering/rd/render_world/terrain_3d_render_world_streaming.cpp')
find_existing = block(
    'src/rendering/rd/render_world/terrain_3d_render_world_streaming.cpp',
    'static uint32_t t3d_rw_stream_find_existing',
    'static uint32_t t3d_rw_stream_pop_free_job',
)
if 'pPageToJob[pVirtualPage]' not in find_existing or 'for (' in find_existing:
    failures.append('P10 existing-page job lookup is not direct O(1) indexing')

pop_free = block(
    'src/rendering/rd/render_world/terrain_3d_render_world_streaming.cpp',
    'static uint32_t t3d_rw_stream_pop_free_job',
    'static uint32_t t3d_rw_stream_find_replaceable',
)
if 'pFreeJobs[--rState.nFreeJobCount]' not in pop_free or 'for (' in pop_free:
    failures.append('P10 free-job acquisition is not O(1)')

claim = block(
    'src/rendering/rd/render_world/terrain_3d_render_world_streaming.cpp',
    'bool t3d_render_world_vt_streaming_claim_job_owner_hot',
    'bool t3d_render_world_vt_streaming_cancel_claim_owner_hot',
)
if 'pQueuedJobs[0]' not in claim:
    failures.append('P10 owner claim is not driven by the active priority queue')
if re.search(r'for\s*\([^\)]*nJobCapacity', claim):
    failures.append('P10 owner claim still scans full job capacity')

collect = block(
    'src/rendering/rd/render_world/terrain_3d_render_world_streaming.cpp',
    'static void t3d_rw_stream_collect_completions_owner_hot',
    'uint64_t t3d_render_world_vt_streaming_service_uploads_hot',
)
if 'nScratchLaneCount' not in collect or 'pScratchLaneOwner[lane]' not in collect:
    failures.append('P10 completion discovery is not scratch-lane driven')
if re.search(r'for\s*\([^\)]*nJobCapacity', collect):
    failures.append('P10 completion discovery still scans full job capacity')

vt_h = text('src/rendering/rd/render_world/terrain_3d_render_world_vt.h')
if 'pRetiredSlots' not in vt_h or 'nRetiredSlotCount' not in vt_h:
    failures.append('P10 explicit retired-slot list is missing')

retire = block(
    'src/rendering/rd/render_world/terrain_3d_render_world_vt_gpu.cpp',
    'bool Terrain3DRenderWorldVTGpu::service_retired_slots_hot',
    'bool Terrain3DRenderWorldVTGpu::upload_commit_hot',
)
if 'nRetiredSlotCount' not in retire or 'pRetiredSlots' not in retire:
    failures.append('P10 retired service is not driven by explicit retired-slot lists')
if 'while ( last < nCount' not in retire or 'nRunBytes' not in retire:
    failures.append('P10 page-table invalidations are not coalesced into contiguous runs')
if re.search(r'for\s*\([^\)]*nPhysicalSlotCapacity', retire):
    failures.append('P10 retired service still scans full physical-slot capacity')

readback = block(
    'src/rendering/rd/render_world/terrain_3d_render_world_vt_gpu.cpp',
    'bool Terrain3DRenderWorldVTGpu::schedule_request_readback_hot',
    'bool Terrain3DRenderWorldVTGpu::consume_request_readback_owner_hot',
)
if 's_nReadbackFrameCadence = 4u' not in readback or 'nReadbackCadenceSkipCount' not in readback:
    failures.append('P10 request readback is not decoupled from raster FPS')

runtime = text('src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp')
for needle in ('service_vt_stream_workers_hot()', 'claim_vt_stream_job_owner_hot', 'add_native_task', 'execute_vt_stream_job_async'):
    if needle not in runtime:
        failures.append(f'P10 production VT worker handoff missing {needle}')
if 'wait_vt_stream_workers_cold();' not in runtime:
    failures.append('P10 shutdown does not join outstanding VT workers')
if 'm_pStreamPageToJob' not in runtime or 'm_pRetiredSlots' not in runtime:
    failures.append('P10 fixed CPU image does not reserve streaming indices/retired lists')


contract = text('src/rendering/rd/render_world/terrain_3d_render_world_contract.cpp')
if 'pLimits.nMaxStreamJobs > 0xffffu' not in contract:
    failures.append('P10 uint16 job-index ABI is not enforced by the RenderWorld limit contract')
if 'pLimits.nMaxVtPhysicalAlbedoPages > 0xffffu' not in contract:
    failures.append('P10 uint16 retired-slot ABI is not enforced by the RenderWorld limit contract')

vt_cpp = text('src/rendering/rd/render_world/terrain_3d_render_world_vt.cpp')
precheck = vt_cpp.find('rState.nRetiredSlotCount[pReservation.nChannel] >= rState.Buffers.nPhysicalSlotCapacity[pReservation.nChannel]')
commit = vt_cpp.find('NewSlot.nVirtualPage = pReservation.nVirtualPage;')
if precheck < 0 or commit < 0 or precheck > commit:
    failures.append('P10 retired-list capacity is not validated before upload publication mutation')

build_id = text('src/Terrain3DConstants.h')
if not any(tag in build_id for tag in ('p10-vt-stream-index-worker-batching-2026-09-07', 'p11-instance-gpu-cull-dirty-ranges-2026-09-07', 'p11-runtime-page-header-padding-fix-2026-09-07', 'instancer-minimal-indirect-addressing-fix-2026-09-08', 'instancer-fault-reporting-texture-fallback-fix-2026-09-09', 'instancer-direct-surface-bringup-2026-09-09', 'instancer-rd-port-integrated-2026-09-09', 'instancer-authoritative-store-publication-2026-09-09', 'instancer-rd-arraymesh-surface-binding-p0-2026-09-09', 'memory-budget-zone-containment-p1-2026-09-09')):
    failures.append('P10-or-later native build provenance is not updated')

# P10 remains a required cumulative contract under P11.

if failures:
    print('P10 verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)

print('P10 verification PASS')
print('P10: direct page/job index + fixed free/queued/ready lists + retired batching + readback cadence + worker handoff present')
print('P10 cumulative contract remains present under P11 instance GPU-cull changes')
