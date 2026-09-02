#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D direct EASTL type gate: FAIL: $1" >&2; exit 1; }

if grep -RInE --include='*.h' --include='*.cpp' \
  'Terrain3D(SharedVector|FixedVector|FixedArray|FixedBitSet|SharedVectorMap|SharedVectorSet)' "$ROOT"; then
  fail "Terrain3D container alias token remains"
fi
if grep -RInE --include='*.h' --include='*.cpp' \
  '^\s*using\s+[^=]+\s*=\s*eastl::(vector|fixed_vector|array|bitset|vector_map|vector_set)<' "$ROOT"; then
  fail "EASTL container type alias remains"
fi
[[ ! -e "$ROOT/core/terrain_3d_containers.h" ]] || fail "obsolete Terrain3D container alias header remains"
[[ -e "$ROOT/core/terrain_3d_eastl_helpers.h" ]] || fail "EASTL helper header missing"

grep -q 'eastl::fixed_vector<int32_t, s_nMaxRegionSlots, false> m_inactiveShapeIds;' "$ROOT/physics/terrain_3d_collision.h" || fail "collision list is not direct eastl::fixed_vector"
grep -q 'eastl::array<t3dInstanceMesh_t, s_nMaxMeshAssets> m_szMeshes;' "$ROOT/instances/terrain_3d_instance_store.h" || fail "mesh table is not direct eastl::array"
grep -q 'eastl::bitset<s_nMaxRegionSlots> m_managedRegions;' "$ROOT/hlod/terrain_3d_hlod_streamer.h" || fail "HLOD region mask is not direct eastl::bitset"
grep -q 'eastl::vector<t3dRenderCell_t, Terrain3DEastlAllocator> m_renderCells;' "$ROOT/instances/terrain_3d_instancer.h" || fail "instancer storage is not direct eastl::vector"
grep -q 'eastl::vector_map<t3dRenderCellKey_t, size_t, t3dRenderCellKeyLess_t, Terrain3DEastlAllocator> m_iRenderCellIndex;' "$ROOT/instances/terrain_3d_instancer.h" || fail "instancer index is not direct eastl::vector_map"
grep -q 'eastl::vector_set<t3dCellUpdateKey_t, t3dCellUpdateKeyLess_t, Terrain3DEastlAllocator> m_streamingUpdates;' "$ROOT/instances/terrain_3d_instancer.h" || fail "instancer set is not direct eastl::vector_set"

echo "Terrain3D direct EASTL type gate: PASS"
