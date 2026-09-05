#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
header = (root / 'src/data/terrain_3d_data.h').read_text(encoding='utf-8')
source = (root / 'src/data/terrain_3d_data.cpp').read_text(encoding='utf-8')

checks = [
    ('rehydrate declaration', r'\bError\s+rehydrateActiveStorage\s*\(\s*void\s*\)\s*;'),
    ('rehydrate definition', r'\bError\s+Terrain3DData::rehydrateActiveStorage\s*\(\s*void\s*\)\s*\{'),
    ('sync declaration', r'\bbool\s+syncActiveRegion\s*\(\s*const\s+Vector2i\s*&\s*\w+\s*,\s*const\s+Terrain3DRegion\s*\*\s*\w+\s*\)\s*;'),
    ('sync definition', r'\bbool\s+Terrain3DData::syncActiveRegion\s*\(\s*const\s+Vector2i\s*&[^,]+,\s*const\s+Terrain3DRegion\s*\*\s*(?:const\s+)?\w+\s*\)\s*\{'),
]

failed = False
for label, pattern in checks:
    text = header if 'declaration' in label else source
    count = len(re.findall(pattern, text, re.MULTILINE))
    if count != 1:
        print(f'FAIL: {label}: expected exactly 1 match, found {count}')
        failed = True

if 'm_samplingStorage.replaceActiveHeightPages( NULL, NULL, 0u )' not in source:
    print('FAIL: rehydrateActiveStorage is not using the fixed-128 active-page reset API')
    failed = True
if 'return m_samplingStorage.syncActiveRegion( vLocation, pRegion );' not in source:
    print('FAIL: Terrain3DData::syncActiveRegion does not delegate to sampling storage')
    failed = True

if failed:
    sys.exit(1)
print('PASS: Terrain3DData link-surface methods are declared and defined exactly once.')
