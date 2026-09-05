#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]

def read(rel):
    return (root / rel).read_text(encoding='utf-8')

def fail(msg):
    print('FAIL:', msg)
    sys.exit(1)

def require(cond, msg):
    if not cond:
        fail(msg)

production = ''
for path in (root / 'src').rglob('*'):
    if path.suffix in {'.h', '.cpp'}:
        production += path.read_text(encoding='utf-8', errors='ignore')

for forbidden in (
    'g_t3dRuntimeAllocationPermission',
    't3dValidateRuntimeAllocationRequest',
    't3dRuntimeAllocationPhaseAllowsAllocation',
    'Terrain3DColdAllocationScope',
):
    require(forbidden not in production, f'permission-era symbol remains: {forbidden}')

memory = read('src/memory/terrain_3d_memory.h')
require('Terrain3DColdPhaseScope' in memory, 'cold phase diagnostic scope missing')
require('t3dRecordRuntimeEastlAllocation' in memory, 'EASTL phase telemetry missing')
require('t3dRecordRuntimeZoneAllocation' in memory, 'zone phase telemetry missing')
require('t3dRecordRuntimeRawAllocation' in memory, 'raw phase telemetry missing')
require('assert( false && "Terrain3D-owned allocation attempted from a hot runtime phase" )' not in memory,
        'global hot-phase allocation assert remains')

eastl = read('src/eastl_config.h')
zone = read('src/memory/terrain_3d_memory_zone.cpp')
linear = read('src/memory/terrain_3d_linear_arena.h')
require('t3dRecordRuntimeEastlAllocation( n )' in eastl, 'EASTL telemetry not recorded')
require('t3dRecordRuntimeZoneAllocation( nBytes )' in zone, 'zone telemetry not recorded')
require('t3dValidateRuntimeAllocationRequest' not in eastl + zone + linear, 'allocator layer still contains phase denial')
require('m_bCapacitySealed' in linear and 'sealCapacity' in linear and 'unsealCapacityCold' in linear,
        'linear arena capacity owner seal missing')

fixed = read('src/core/terrain_3d_fixed_buffer.h')
require('m_bSealed' in fixed and 'reserveCold' in fixed and 'unsealCold' in fixed and 'sealed( void ) const' in fixed,
        'fixed buffer owner seal API incomplete')
require('m_bSealed || nCapacity < m_nCount' in fixed, 'fixed buffer backing growth does not honor owner seal')

slab = read('src/core/terrain_3d_slab_array.h')
require('reserveCold' in slab and 'm_bSealed' in slab and 'beginColdRebuild' in slab,
        'high-water slab owner growth API incomplete')
require('if ( m_bSealed || nCapacity > SIZE_MAX / sizeof( T ) )' in slab,
        'high-water slab backing growth does not fail closed while sealed')

policy = read('src/memory/terrain_3d_memory_policy.cpp')
for key in ('eastl_allocation_events_by_phase', 'zone_allocation_events_by_phase', 'raw_allocation_events_by_phase'):
    require(key in policy, f'phase telemetry key missing: {key}')
phase_slice = policy[policy.find('const t3dRuntimeAllocationPhaseStats_t'):policy.find('const t3dMemoryZoneStats_t')]
for obsolete_key in ('current_allocation_permission', 'denied_checks', 'restricted_cold_overrides', 'explicit_cold_overrides'):
    require(obsolete_key not in phase_slice, f'obsolete allocation-policy telemetry remains: {obsolete_key}')

print('PASS: owner-controlled allocation patches A/B/C')
