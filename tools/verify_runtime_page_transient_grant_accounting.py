#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []

def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')

data = read('src/data/model/terrain_3d_data.cpp')
policy = read('src/memory/policy/terrain_3d_memory_policy.cpp')
constants = read('src/Terrain3DConstants.h')
terrain_h = read('src/terrain_3d.h')

m = re.search(r'size_t Terrain3DData::getMemoryNonPageTransientStagingBytes\( void \) const \{(.*?)\n\}', data, re.S)
if not m:
    failures.append('missing Terrain3DData::getMemoryNonPageTransientStagingBytes')
else:
    body = m.group(1)
    if 'm_mapEditService.getTransientScratchBytes()' not in body:
        failures.append('non-page transient accounting lost map-edit scratch')
    if 'm_farSurfaceAtlasCache.getImageBytes()' in body:
        failures.append('retained far-atlas images are still double-charged against transient reserve')

if 'const uint64_t fixed_far_atlas_bytes = getGpuFarAtlasBytes();' not in policy:
    failures.append('far-atlas fixed residency is not budgeted by calculateFixedMemoryBytes')
if 'setRuntimePagePreparedByteBudget( runtime_page_grant )' not in policy:
    failures.append('runtime-page prepared-byte grant is no longer owned by the central memory policy')
if 'getMemoryNonPageTransientStagingBytes()' not in policy:
    failures.append('runtime-page grant no longer subtracts genuine competing transient staging')

m_reserve = re.search(r'int m_nMemoryTransientReserveMb = (\d+);', terrain_h)
m_surface = re.search(r'#define s_nGpuCacheFarSurfaceMaxSide \( (\d+) \)', constants)
if not m_reserve or not m_surface:
    failures.append('could not resolve default transient reserve/far-surface cap')
else:
    reserve = int(m_reserve.group(1)) * 1024 * 1024
    side = int(m_surface.group(1))
    surface_bytes = side * side * 4
    if surface_bytes <= reserve:
        failures.append('regression fixture no longer reproduces retained far-atlas > transient-reserve pressure')

if failures:
    for failure in failures:
        print('FAIL:', failure)
    sys.exit(1)

print('PASS: retained far-atlas images stay in fixed residency and no longer zero the runtime-page transient grant')
