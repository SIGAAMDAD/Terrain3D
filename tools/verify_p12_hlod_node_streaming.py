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


# P12A: HLOD selection and retirement must be generation/list driven.
hlod_h = text('src/hlod/runtime/terrain_3d_hlod_streamer.h')
for needle in (
    'm_nDesiredGeneration',
    'm_activeEntries',
    'm_loadingEntries',
    'm_retiringEntries',
    'm_loadCandidates',
    'm_nTransitionEntriesExaminedLastTick',
):
    if needle not in hlod_h:
        failures.append(f'P12 HLOD transition state missing {needle}')

hlod_update = block(
    'src/hlod/runtime/terrain_3d_hlod_streamer.cpp',
    'void Terrain3DHLODStreamer::updateResidency',
    'uint8_t Terrain3DHLODStreamer::getNodeMinimumLevel',
)
for needle in ('beginSelectionGeneration()', 'm_loadingEntries', 'm_loadCandidates', 'm_retiringEntries'):
    if needle not in hlod_update:
        failures.append(f'P12 HLOD update does not use {needle}')
if re.search(r'for\s*\([^\)]*m_entries\.size\(\)', hlod_update):
    failures.append('P12 HLOD updateResidency still performs a full manifest-entry scan')
if 'm_bDesired' in hlod_h or 'entry.m_bDesired' in hlod_update:
    failures.append('P12 legacy per-entry desired clear flag remains in HLOD hot selection')

# P12B: movable-node ownership migration is no longer mandatory every physics tick.
node_h = text('src/streaming/runtime/terrain_3d_node_streamer.h')
node_update = block(
    'src/streaming/runtime/terrain_3d_node_streamer.cpp',
    'void Terrain3DNodeStreamer::updateResidency',
    'Dictionary Terrain3DNodeStreamer::getStats',
)
for needle in ('m_nMovableMigrationCadence = 4u', 'm_nMovableMigrationTick', 'm_bMigrationScanSkippedLastTick'):
    if needle not in node_h:
        failures.append(f'P12 node migration cadence state missing {needle}')
for needle in ('bMigrationDue', 'm_nMovableMigrationCadence'):
    if needle not in node_update:
        failures.append(f'P12 node migration throttling missing {needle}')
if 'migration_scan_skipped_last_tick' not in text('src/streaming/runtime/terrain_3d_node_streamer.cpp'):
    failures.append('P12 node migration skip telemetry is missing')

if failures:
    print('P12 verification FAIL')
    for failure in failures:
        print(f' - {failure}')
    sys.exit(1)

print('P12 verification PASS')
print('generation/list-driven HLOD transitions + throttled movable-node ownership migration present')
