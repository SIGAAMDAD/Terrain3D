#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
h = (root / 'src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h').read_text()
cpp = (root / 'src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.cpp').read_text()

checks = [
    ('coordinator slots are cold plain pointers', 'Terrain3DRenderCoordinator *m_szCoordinators[s_nMaxCoordinators]{};' in h),
    ('cold coordinator mutation mutex exists', 'm_coordinatorMutex' in h),
    ('prepared coordinator mask exists', 'm_nPreparedCoordinatorMask' in h),
    ('dispatch accepts immutable mask snapshot', 'dispatchToCoordinators( const t3dRdForwardPassContext_t &rContext, uint64_t nCoordinatorMask )' in h),
    ('hot dispatch has no per-slot atomic loads', 'm_szCoordinators[index].load' not in cpp),
    ('attach publishes pointer through release mask', 'm_szCoordinators[i] = pCoordinator;' in cpp and 'm_nCoordinatorMask.fetch_or' in cpp),
    ('detach removes publication before sync', 'm_nCoordinatorMask.fetch_and( ~removed_mask' in cpp and 'Terrain3DRdContext::forceSync();' in cpp),
    ('prepare snapshots coordinator mask once', 'const uint64_t coordinator_mask = m_nCoordinatorMask.load( T3D_MEMORY_ORDER_ACQUIRE );' in cpp),
    ('prepare stores exact recipient set', 'm_nPreparedCoordinatorMask = coordinator_mask;' in cpp),
    ('submit reuses prepared recipient set', 'dispatchToCoordinators( *m_pPreparedContext, m_nPreparedCoordinatorMask );' in cpp),
    ('shutdown avoids redundant second force sync', 'No second\n\t// force_sync is required' in cpp),
]

failed = [name for name, ok in checks if not ok]
if failed:
    for name in failed:
        print('FAIL:', name)
    raise SystemExit(1)
print('Terrain3D separate render-thread adapter verification PASS')
