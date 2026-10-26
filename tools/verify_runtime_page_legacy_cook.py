#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
data = (root / 'src/data/model/terrain_3d_data.cpp').read_text(encoding='utf-8')
menu = (root / 'project/addons/terrain_3d/menu/terrain_menu.gd').read_text(encoding='utf-8')
failures = []

inspect_begin = data.find('Dictionary Terrain3DData::inspectGpuRegionPages( void )')
cook_begin = data.find('Dictionary Terrain3DData::cookGpuRegionPages( void )')
bind_begin = data.find('void Terrain3DData::_bind_methods')
if inspect_begin < 0 or cook_begin < 0:
    failures.append('runtime-page inspect/cook functions missing')
    inspect = ''
    cook = ''
else:
    inspect = data[inspect_begin:cook_begin]
    cook = data[cook_begin:bind_begin if bind_begin > cook_begin else len(data)]

for needle in [
    'm_runtimeArtifacts.probePage( page_path, probe )',
    'probe.m_state == RUNTIME_PAGE_PROBE_LEGACY',
    'probe.m_state == RUNTIME_PAGE_PROBE_FUTURE',
    'result["legacy"] = legacy',
    'result["incompatible"] = incompatible',
    'result["safe_to_cook"] = future == 0',
]:
    if needle not in inspect:
        failures.append(f'inspection contract missing: {needle}')

legacy_pos = inspect.find('probe.m_state == RUNTIME_PAGE_PROBE_LEGACY')
sig_pos = inspect.find('readGpuRuntimePageSourceSignature( page_path, existing_signature )')
if legacy_pos < 0 or sig_pos < 0 or legacy_pos > sig_pos:
    failures.append('inspection still attempts current-format signature parsing before legacy classification')

for needle in [
    'm_runtimeArtifacts.probePage( page_path, page_probe )',
    'page_probe.m_state == RUNTIME_PAGE_PROBE_FUTURE',
    'm_runtimeArtifacts.validatePageForRegion( pRegion, runtime_page_config )',
    'const bool bLegacyArtifact = page_probe.m_state == RUNTIME_PAGE_PROBE_LEGACY',
    'legacy_recooked += bLegacyArtifact ? 1 : 0',
    'incompatible_recooked += bIncompatibleArtifact ? 1 : 0',
    'Cooked world manifest publication withheld because region-page cooking failed',
]:
    if needle not in cook:
        failures.append(f'cook replacement contract missing: {needle}')

old_bypass = 'bool bCurrent = bCookDatabaseLoaded && FileAccess::file_exists( page_path )'
if old_bypass in cook:
    failures.append('cook database still bypasses physical runtime-page validation')

if 'Future-version runtime pages: %d' not in menu:
    failures.append('editor cook UI does not block future-version page overwrite')
if 'legacy %d' not in menu or 'incompatible %d' not in menu:
    failures.append('editor cook UI does not distinguish legacy/incompatible pages')

if failures:
    for failure in failures:
        print('FAIL:', failure)
    sys.exit(1)
print('PASS: runtime-page legacy/incompatible cook replacement contract')
