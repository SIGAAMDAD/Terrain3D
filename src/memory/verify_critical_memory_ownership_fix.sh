#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D EASTL memory ownership gate: FAIL: $1" >&2; exit 1; }

bash "$ROOT/memory/verify_runtime_container_ownership.sh" >/dev/null

STORE_H="$ROOT/instances/terrain_3d_instance_store.h"
STORE_CPP="$ROOT/instances/terrain_3d_instance_store.cpp"
INSTANCER_H="$ROOT/instances/terrain_3d_instancer.h"
INSTANCER_CPP="$ROOT/instances/terrain_3d_instancer.cpp"
RD_H="$ROOT/rendering/rd/terrain_3d_rd_context.h"
RD_CPP="$ROOT/rendering/rd/terrain_3d_rd_context.cpp"

# Allocation-bearing EASTL containers remain routed through Terrain3DEastlAllocator.
grep -q '#include "eastl_config.h"' "$ROOT/core/terrain_3d_eastl_helpers.h" || fail "central EASTL allocator configuration include missing"
grep -q 'eastl::vector<t3dPackedTransform_t, Terrain3DEastlAllocator>' "$STORE_H" || fail "runtime EASTL vector does not state Terrain3DEastlAllocator"
if grep -nE 'std::(vector|deque|list|map|unordered_map|set|unordered_set)' "$STORE_H" "$INSTANCER_H" "$ROOT/rendering/rd/terrain_3d_rd_instance_renderer.h"; then
  fail "STL ownership container introduced in runtime ownership targets"
fi

# Instance-store copies remain transactional despite switching from shared slabs
# to EASTL-owned per-cell payload vectors.
grep -q 'bool copyFrom' "$STORE_H" || fail "instance-store copy cannot report failure"
grep -q 'Terrain3DInstanceStore replacement;' "$STORE_CPP" || fail "instance-store copy assignment is not transactional"
grep -q 'Terrain3DInstanceStore compacted;' "$STORE_CPP" || fail "instance-store compaction is not off-side"
grep -q 't3dContainerReserveCold( destination_mesh.m_cells' "$STORE_CPP" || fail "instance-store copy does not preflight cell capacity"
grep -q 't3dVectorPushBackWithinCapacity( destination_mesh.m_cells' "$STORE_CPP" || fail "instance-store copy bypasses bounded append"

# Sorted EASTL indexes must establish capacity during the explicit cold service.
grep -q 't3dContainerReserveCold( m_iRenderCellIndex' "$INSTANCER_CPP" || fail "instancer vector_map capacity is not established cold"
grep -q 't3dContainerReserveCold( m_logicalCellCounts' "$INSTANCER_CPP" || fail "logical vector_map capacity is not established cold"

# RD physical owner accounting from the prior ownership repair remains intact.
grep -q 'core/terrain_3d_eastl_helpers.h' "$RD_H" || fail "RD context does not include container policy"
python3 - "$RD_H" "$RD_CPP" <<'PY'
from pathlib import Path
import re, sys
h=Path(sys.argv[1]).read_text(); c=Path(sys.argv[2]).read_text()
if not re.search(r'bool registerResource\( const RID &rRid,\s*const String &rName,\s*ResourceType type,\s*MemoryCategory memoryCategory,\s*uint64_t nBytes,\s*ResourceOwner owner \);', h, re.S):
    raise SystemExit('typed RD registration does not require ResourceOwner')
if not re.search(r'bool Terrain3DRdContext::registerResourceInternal\([^)]*const ResourceOwner owner \)', c, re.S):
    raise SystemExit('RD internal owner-aware registration definition missing')
for token in ('m_szOwnerCreates','m_szOwnerFrees','m_szOwnerLive','m_szOwnerLiveBytes','m_szOwnerSteadyStateViolations'):
    if token not in c: raise SystemExit('RD owner accounting missing: '+token)
PY

echo "Terrain3D EASTL memory ownership gate: PASS"
