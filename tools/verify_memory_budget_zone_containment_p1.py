#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
budget = (root / 'src/memory/policy/terrain_3d_memory_budget.h').read_text(encoding='utf-8')
policy = (root / 'src/memory/policy/terrain_3d_memory_policy.cpp').read_text(encoding='utf-8')
data_h = (root / 'src/data/model/terrain_3d_data.h').read_text(encoding='utf-8')
data_cpp = (root / 'src/data/model/terrain_3d_data.cpp').read_text(encoding='utf-8')
constants = (root / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text(encoding='utf-8')


def fail(message: str) -> None:
    print(f'Memory budget zone-containment P1 verification FAIL: {message}')
    sys.exit(1)


def contains_code(text: str, token: str) -> bool:
    return re.sub(r'\s+', ' ', token).strip() in re.sub(r'\s+', ' ', text).strip()


for token in (
    'm_nAllocatorContainedFixedBytes',
    'result.m_nAllocatorContainedFixedBytes = allocator_contained < rInput.m_nAllocatorReserveBytes',
    'result.m_nFixedBytes = rInput.m_nFixedBytes - result.m_nAllocatorContainedFixedBytes;',
    'addChecked( result.m_nFixedBytes, result.m_nAllocatorReserveBytes, arithmetic_overflow )',
):
    if not contains_code(budget, token):
        fail(f'missing coordinator ownership split: {token}')

for token in (
    'calculateAllocatorContainedFixedMemoryBytes()',
    'getMemoryAllocatorContainedRepositoryBytes()',
    'm_pInstancer->getRetainedContainerBytes()',
    'm_pNodeStreamer->getRetainedCapacityBytes()',
    'input.m_nAllocatorContainedFixedBytes = calculateAllocatorContainedFixedMemoryBytes();',
    'allocator_contained_fixed_bytes',
):
    if not contains_code(policy, token):
        fail(f'missing policy containment accounting: {token}')

if 'getMemoryAllocatorContainedRepositoryBytes' not in data_h:
    fail('Terrain3DData does not expose allocator-contained repository bytes')
if not contains_code(data_cpp, 'return m_samplingStorage.getResidentReservedBytes();'):
    fail('resident sampling pool is not classified as allocator-contained')

native_build = re.search(r's_pszNativeBuildId \( "([^"]+)" \)', constants)
plugin_build = re.search(r'EXPECTED_NATIVE_BUILD_ID: String = "([^"]+)"', plugin)
if not native_build or not plugin_build or native_build.group(1) != plugin_build.group(1):
    fail('native/editor build IDs are not synchronized to P1')

print('Memory budget zone-containment P1 verification PASS')
