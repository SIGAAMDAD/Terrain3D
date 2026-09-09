#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
budget = (root / 'src/memory/terrain_3d_memory_budget.h').read_text(encoding='utf-8')
policy = (root / 'src/memory/terrain_3d_memory_policy.cpp').read_text(encoding='utf-8')
data_h = (root / 'src/data/terrain_3d_data.h').read_text(encoding='utf-8')
data_cpp = (root / 'src/data/terrain_3d_data.cpp').read_text(encoding='utf-8')
constants = (root / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text(encoding='utf-8')


def fail(message: str) -> None:
    print(f'Memory budget zone-containment P1 verification FAIL: {message}')
    sys.exit(1)

for token in (
    'm_nAllocatorContainedFixedBytes',
    'result.m_nAllocatorContainedFixedBytes = allocator_contained < rInput.m_nAllocatorReserveBytes',
    'result.m_nFixedBytes = rInput.m_nFixedBytes - result.m_nAllocatorContainedFixedBytes;',
    'addChecked( result.m_nFixedBytes, result.m_nAllocatorReserveBytes, arithmetic_overflow )',
):
    if token not in budget:
        fail(f'missing coordinator ownership split: {token}')

for token in (
    'calculateAllocatorContainedFixedMemoryBytes()',
    'getMemoryAllocatorContainedRepositoryBytes()',
    'm_pInstancer->getRetainedContainerBytes()',
    'm_pNodeStreamer->getRetainedCapacityBytes()',
    'input.m_nAllocatorContainedFixedBytes = calculateAllocatorContainedFixedMemoryBytes();',
    'allocator_contained_fixed_bytes',
):
    if token not in policy:
        fail(f'missing policy containment accounting: {token}')

if 'getMemoryAllocatorContainedRepositoryBytes' not in data_h:
    fail('Terrain3DData does not expose allocator-contained repository bytes')
if 'return m_samplingStorage.getResidentReservedBytes();' not in data_cpp:
    fail('resident sampling pool is not classified as allocator-contained')

build_id = 'forward-color-blend-attachment-contract-fix-2026-09-09'
if build_id not in constants or build_id not in plugin:
    fail('native/editor build IDs are not synchronized to P1')

print('Memory budget zone-containment P1 verification PASS')
