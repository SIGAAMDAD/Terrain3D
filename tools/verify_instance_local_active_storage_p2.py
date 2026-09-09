#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / 'src/memory/terrain_3d_active_storage.h').read_text(encoding='utf-8')
cpp = (ROOT / 'src/memory/terrain_3d_active_storage.cpp').read_text(encoding='utf-8')
sampling_h = (ROOT / 'src/data/terrain_3d_sampling_storage.h').read_text(encoding='utf-8')
sampling_cpp = (ROOT / 'src/data/terrain_3d_sampling_storage.cpp').read_text(encoding='utf-8')
data_cpp = (ROOT / 'src/data/terrain_3d_data.cpp').read_text(encoding='utf-8')
constants = (ROOT / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
plugin = (ROOT / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text(encoding='utf-8')

errors = []

def require(text, pattern, label):
    if re.search(pattern, text, re.S) is None:
        errors.append('missing ' + label)

def forbid(text, pattern, label):
    if re.search(pattern, text, re.S) is not None:
        errors.append('forbidden ' + label)

require(header, r'class\s+Terrain3DActiveStorage\s*\{', 'instance active-storage class')
require(sampling_h, r'Terrain3DActiveStorage\s+m_activeStorage\s*;', 'sampling-owned active storage member')
require(header, r't3dActivePhysicalPage_t\s+m_szPhysicalPages\s*\[\s*s_nActivePhysicalPageCount\s*\]', 'instance physical page bank')
require(header, r't3dAtomic_t<int>\s+m_szLogicalToPhysical\s*\[\s*s_nRegionSlots\s*\]', 'instance logical page map')
require(header, r't3dMutex_t\s+m_writerMutex\s*;', 'instance writer mutex')
require(header, r'Terrain3DActiveStorage\s*\*m_pStorage', 'lease storage owner pointer')
require(sampling_h, r'return\s+m_activeStorage\.isActive\(\)', 'sampling active-state delegation')
require(sampling_h, r'm_activeStorage\.acquire\(\s*rLocation\s*\)', 'sampling lease acquisition delegation')
require(sampling_cpp, r'm_activeStorage\.bind\(\s*pBacking\s*,\s*nBackingBytes\s*\)', 'instance backing bind')
require(sampling_cpp, r'm_activeStorage\.release\(\)', 'instance storage release')
require(cpp, r'Terrain3DActiveStorage::bind', 'instance bind implementation')
require(cpp, r'Terrain3DActiveStorage::acquire', 'instance acquire implementation')
require(cpp, r'm_pStorage->releaseReader\(\s*m_iPage\s*\)', 'lease releases the correct instance')

# The P2 purpose is to remove all mutable process-global active-store state and owner arbitration.
for symbol in [
    's_szPhysicalPages', 's_szLogicalToPhysical', 's_pActiveHeightBackingRaw',
    's_pActiveHeightBacking', 's_nActiveOwnerState', 's_nActiveOwnerGeneration',
    's_pActiveOwner', 's_activeWriterMutex'
]:
    forbid(cpp, r'\bstatic\b[^;\n]*\b' + re.escape(symbol) + r'\b', 'process-global ' + symbol)

for symbol in [
    't3dActiveStorageClaim', 't3dActiveStorageBind', 't3dActiveStorageRelease',
    't3dActiveStorageReset', 't3dActiveStorageIsOwner', 't3dActiveStorageAcquire',
    't3dActiveStorageContains', 't3dActiveStorageSyncRegion', 't3dActiveStorageSyncHeightPage'
]:
    forbid(header + cpp + sampling_cpp, r'\b' + symbol + r'\b', 'legacy singleton API ' + symbol)

for stale in ['process-static active terrain storage', 'Only one Terrain3DData owner is legal']:
    if stale in data_cpp:
        errors.append('stale singleton diagnostic: ' + stale)

build_id = 'instance-local-active-storage-p2-2026-09-09'
if build_id not in constants or build_id not in plugin:
    errors.append('native/editor build id mismatch for P2')

if errors:
    print('FAIL: instance-local active storage P2')
    for error in errors:
        print(' - ' + error)
    sys.exit(1)
print('PASS: instance-local active storage P2')
