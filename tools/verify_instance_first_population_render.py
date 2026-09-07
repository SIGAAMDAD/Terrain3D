#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
cpp = (root / 'src/instances/terrain_3d_instancer.cpp').read_text(encoding='utf-8')
hdr = (root / 'src/instances/terrain_3d_instancer.h').read_text(encoding='utf-8')

def fail(msg: str) -> None:
    print(f'Instance first-population render verification FAIL: {msg}')
    sys.exit(1)

def body(signature: str, text: str) -> str:
    start = text.find(signature)
    if start < 0:
        fail(f'missing {signature}')
    brace = text.find('{', start)
    if brace < 0:
        fail(f'missing body for {signature}')
    depth = 0
    for i in range(brace, len(text)):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[brace:i + 1]
    fail(f'unterminated body for {signature}')
    return ''

if 'void requestRuntimeCapacityForMutation( size_t nAdditionalRecords );' not in hdr:
    fail('mutation capacity request is not declared')

retain = body('static bool t3dRetainMutatedInstancePage(', cpp)
if 'ensureInstanceRegionResident' not in retain or 'bTemporary = false;' not in retain:
    fail('temporary populated page is not promoted into resident cache')

capacity = body('void Terrain3DInstancer::requestRuntimeCapacityForMutation(', cpp)
if 'm_bRuntimeStorageRebuildPending = true;' not in capacity:
    fail('first large mutation does not queue owner-controlled cold capacity rebuild')
if 'm_szLogicalMeshInstanceTotals' not in capacity:
    fail('capacity request is not based on current logical record population')

capacity_miss = body('bool Terrain3DInstancer::reportRuntimeCapacityExceeded(', cpp)
if 'm_bRuntimeStorageRebuildPending = true;' not in capacity_miss:
    fail('runtime capacity miss does not arm owner-controlled cold rebuild')

logical = body('void Terrain3DInstancer::setLogicalCellCount(', cpp)
if 'm_bRuntimeStorageRebuildPending = true;' not in logical:
    fail('logical-cell saturation is not recoverable through cold rebuild')

global_append = body('void Terrain3DInstancer::appendGlobalNative(', cpp)
if 'requestRuntimeCapacityForMutation( m_placementPending.size() );' not in global_append:
    fail('global placement does not preflight dynamic record capacity')
retain_pos = global_append.find('t3dRetainMutatedInstancePage')
end_pos = global_append.find('t3dEndInstancePageAccess')
if retain_pos < 0 or end_pos < 0 or retain_pos > end_pos:
    fail('global first-population page is evicted before residency promotion')
if 'm_touchedRegions.push_back( region )' not in global_append:
    fail('promoted global placement is not scheduled for render update')

region_append = body('void Terrain3DInstancer::appendRegionNative(', cpp)
if 'requestRuntimeCapacityForMutation( nTransformCount );' not in region_append:
    fail('region placement does not preflight dynamic record capacity')
retain_pos = region_append.find('t3dRetainMutatedInstancePage')
update_pos = region_append.find('updateMmis( nMeshId, regionLocation )')
end_pos = region_append.find('t3dEndInstancePageAccess')
if retain_pos < 0 or update_pos < 0 or end_pos < 0 or not (retain_pos < update_pos < end_pos):
    fail('region first-population promotion/update/cleanup ordering is invalid')

print('Instance first-population render verification PASS')
