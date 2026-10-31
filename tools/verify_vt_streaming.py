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


stream_rel = 'src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.cpp'
stream_h_rel = 'src/rendering/rd/vt/streaming/terrain_3d_vt_streaming.h'
residency_rel = 'src/rendering/rd/vt/streaming/terrain_3d_vt_residency.cpp'
residency_h_rel = 'src/rendering/rd/vt/streaming/terrain_3d_vt_residency.h'
gpu_rel = 'src/rendering/rd/vt/gpu/terrain_3d_vt_gpu.cpp'
owner_rel = 'src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.cpp'

stream_h = text(stream_h_rel)
for field in ('pPageToJob', 'pFreeJobs', 'pQueuedJobs', 'pJobQueuePosition', 'pReadyJobs'):
    if field not in stream_h:
        failures.append(f'P10 streaming fixed index missing {field}')

find_existing = block(stream_rel, 'static uint32_t t3d_vt_stream_find_existing', 'static uint32_t t3d_vt_stream_pop_free_job')
if 'pPageToJob[pVirtualPage]' not in find_existing or 'for (' in find_existing:
    failures.append('P10 existing-page job lookup is not direct O(1) indexing')

pop_free = block(stream_rel, 'static uint32_t t3d_vt_stream_pop_free_job', 'static uint32_t t3d_vt_stream_find_replaceable')
if 'pFreeJobs[--rState.nFreeJobCount]' not in pop_free or 'for (' in pop_free:
    failures.append('P10 free-job acquisition is not O(1)')

claim = block(stream_rel, 'bool t3d_vt_streaming_claim_job_owner_hot', 'bool t3d_vt_streaming_cancel_claim_owner_hot')
if 'nPriorityBucketHead[iBucket]' not in claim and 'nPriorityBucketHead[nBucket]' not in claim and 'pQueuedJobs[0]' not in claim:
    failures.append('P10 owner claim is not driven by a fixed active priority queue')
if re.search(r'for\s*\([^\)]*nJobCapacity', claim):
    failures.append('P10 owner claim still scans full job capacity')

collect = block(stream_rel, 'static void t3d_vt_stream_collect_completions_owner_hot', 'uint64_t t3d_vt_streaming_service_uploads_hot')
if 'nScratchLaneCount' not in collect or ('pScratchLaneOwner[lane]' not in collect and 'pScratchLaneOwner[iLane]' not in collect):
    failures.append('P10 completion discovery is not scratch-lane driven')
if re.search(r'for\s*\([^\)]*nJobCapacity', collect):
    failures.append('P10 completion discovery still scans full job capacity')

residency_h = text(residency_h_rel)
if 'pRetiredPageSets' not in residency_h or 'nRetiredSlotCount' not in residency_h or 'pFreeSlots' not in residency_h:
    failures.append('P10/P17 fixed retirement/free-slot storage is missing')

retire = block(gpu_rel, 'bool Terrain3DVTGpu::service_retired_slots_hot', 'bool Terrain3DVTGpu::upload_page_set_commit_hot')
delegates_retirement = 't3d_vt_service_retired_slots_hot( rResidency, pFrameId )' in retire
services_page_set_ring = (
    'pRetiredPageSets' in retire
    and 'nRetireHead' in retire
    and 't3d_vt_retirement_ready_reference_debug' in retire
    and 't3d_vt_finalize_eviction_hot' in retire
)
if not delegates_retirement and not services_page_set_ring:
    failures.append('P10/P17 retired service does not route through the fixed delayed retirement ring')
if re.search(r'for\s*\([^\)]*nPhysicalPageSetCapacity', retire):
    failures.append('P10/P17 retired service still scans full physical-slot capacity')

readback = block(gpu_rel, 'bool Terrain3DVTGpu::schedule_request_readback_hot', 'bool Terrain3DVTGpu::consume_request_readback_owner_hot')
if 's_nReadbackFrameCadence = 4u' not in readback or 'nReadbackCadenceSkipCount' not in readback:
    failures.append('P10 request readback is not decoupled from raster FPS')

runtime = text('src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp')
for needle in ('service_vt_stream_workers_hot()', 'claim_vt_stream_job_owner_hot', 'add_native_task', 'execute_vt_stream_job_async'):
    if needle not in runtime:
        failures.append(f'P10 production VT worker handoff missing {needle}')
if 'wait_vt_stream_workers_cold();' not in runtime:
    failures.append('P10 shutdown does not join outstanding VT workers')

owner = text(owner_rel)
for needle in ('m_pStreamPageToJob', 'm_pStreamFreeJobs', 'm_pStreamQueuedJobs', 'm_pStreamJobQueuePosition', 'm_pStreamReadyJobs', 'm_pRetiredPageSets', 'm_pFreePhysicalSlots'):
    if needle not in owner:
        failures.append(f'P10/P16 VT owner CPU image does not reserve {needle}')

contract = text('src/rendering/rd/render_world/terrain_3d_render_world_contract.cpp')
if 'pLimits.nMaxStreamJobs > 0xffffu' not in contract:
    failures.append('P10 uint16 job-index ABI is not enforced by the RenderWorld limit contract')
if 'pLimits.nMaxVtPhysicalPageSets > 0xffffu' not in contract:
    failures.append('P10/P17 shared physical page-set capacity remains outside the compact slot contract')

residency_cpp = text(residency_rel)
precheck = residency_cpp.find('pState.Stats.nRetiredSlotCount >= pState.Buffers.nPhysicalPageSetCapacity')
commit = residency_cpp.find('NewSlot.m_nVirtualPage = pReservation.nVirtualPage;')
if precheck < 0 or commit < 0 or precheck > commit:
    failures.append('P10/P17 retirement capacity is not validated before upload publication mutation')

build_id = text('src/Terrain3DConstants.h')
if not any(tag in build_id for tag in (
    'p10-vt-stream-index-worker-batching-2026-09-07',
    'p11-instance-gpu-cull-dirty-ranges-2026-09-07',
    'p11-runtime-page-header-padding-fix-2026-09-07',
    'instancer-minimal-indirect-addressing-fix-2026-09-08',
    'instancer-fault-reporting-texture-fallback-fix-2026-09-09',
    'instancer-direct-surface-bringup-2026-09-09',
    'instancer-rd-port-integrated-2026-09-09',
    'instancer-authoritative-store-publication-2026-09-09',
    'instancer-rd-arraymesh-surface-binding-p0-2026-09-09',
    'memory-budget-zone-containment-p1-2026-09-09',
    'forward-color-blend-attachment-contract-fix-2026-09-09',
    'instancer-category-policy-p3-2026-09-09',
    'instancer-terrain-follow-p4-2026-09-09',
    'instancer-publication-p5-2026-09-09',
    'instancer-hlod-handoff-p6-2026-09-09',
    'instancer-occlusion-p7-2026-09-09',
    'mesh-derived-instance-physics-occlusion-p13-2-2026-09-10',
    'wg-d-p23-async-scheduler-2026-09-10',
    'wg-d-p24-residency-policy-2026-09-11',
)):
    failures.append('P10-or-later native build provenance is not updated')

if failures:
    print('P10 verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)

print('P10 verification PASS')
print('P10: direct page/job index + fixed queues + delayed retirement ring + cadence + worker handoff preserved under P17')
