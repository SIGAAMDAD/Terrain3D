#!/usr/bin/env python3
from pathlib import Path
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

upload = block(
    'src/streaming/terrain_3d_gpu_region_cache.cpp',
    'bool Terrain3DGpuRegionCache::uploadGpuRegionLayer',
    'bool Terrain3DGpuRegionCache::isRuntimePageWanted',
)

# A missing prepared page can fail closed only outside editor Authoring.
missing = upload.find('if ( !bPreparedAttempted )')
if missing < 0:
    failures.append('missing prepared-page admission branch')
else:
    window = upload[missing:missing + 1200]
    if 'isEditorAuthoringMode()' not in window:
        failures.append('missing prepared-page branch does not preserve editor Authoring live authority')
    if 'TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE' not in window:
        failures.append('shipping/runtime no-ready-page stall contract disappeared')

# If a cooked image cannot be published into the current persistent texture
# layout, Authoring retries exactly through the non-cooked path.
retry = 'return uploadGpuRegionLayer( nLayer, vLocation, eMapType, false );'
if upload.count(retry) < 2:
    failures.append('editor Authoring does not retry both batched and direct incompatible cooked publications from live maps')

# The fallback must remain inside editor checks; shipping/runtime must never
# silently reconstruct authored pages from the HOT_FRAME path.
for occurrence in [i for i in range(len(upload)) if upload.startswith(retry, i)]:
    prefix = upload[max(0, occurrence - 260):occurrence]
    if 'isEditorAuthoringMode()' not in prefix:
        failures.append('live cooked-page retry is not editor-Authoring guarded')
        break

# Runtime source discovery is cached by every signature that participates in
# source validation. A signature change must invalidate a previous absent-page
# decision and any matching capacity-failure latch.
header = text('src/streaming/terrain_3d_runtime_page_store.h')
store = text('src/streaming/terrain_3d_runtime_page_store.cpp')
for symbol in (
    'm_nSourceMaterialSignature',
    'm_nSourceCollisionSignature',
    'm_nCapacityFailureMaterialSignature',
    'm_nCapacityFailureCollisionSignature',
):
    if symbol not in header:
        failures.append(f'runtime-page source cache missing signature state: {symbol}')
for needle in (
    'rState.m_config.m_nMaterialSourceSignature == m_nSourceMaterialSignature',
    'rState.m_config.m_nCollisionSurfaceSignature == m_nSourceCollisionSignature',
    'rState.m_config.m_nMaterialSourceSignature == m_nCapacityFailureMaterialSignature',
    'rState.m_config.m_nCollisionSurfaceSignature == m_nCapacityFailureCollisionSignature',
    'm_nSourceMaterialSignature = rState.m_config.m_nMaterialSourceSignature;',
    'm_nSourceCollisionSignature = rState.m_config.m_nCollisionSurfaceSignature;',
):
    if needle not in store:
        failures.append(f'runtime-page source cache invalidation contract missing: {needle}')

if failures:
    print('P07 authoring-page fallback verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)
print('P07 authoring-page fallback verification PASS')
