#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 7 gate: FAIL: $1" >&2; exit 1; }

INSTANCER_CPP="$ROOT/instances/terrain_3d_instancer.cpp"
PAGE_CPP="$ROOT/streaming/terrain_3d_runtime_page_store.cpp"

bash "$ROOT/memory/verify_refactor_patch_6.sh" >/dev/null

grep -q 'eastl::array<uint32_t, s_nInstanceCacheMaxPages> largest_counts{};' "$INSTANCER_CPP" || fail "instancer ranking scratch is not fixed-array storage"
grep -q 'largest_counts.data(), largest_counts.size(), pRegion->getInstanceCount()' "$INSTANCER_CPP" || fail "instancer ranking helper no longer receives fixed-array data/size"
grep -q 'eastl::array<uint32_t, s_nMaxMeshAssets> mesh_counts{};' "$PAGE_CPP" || fail "runtime-page mesh-count scratch is not fixed-array storage"
grep -q 'mesh_counts.data(),' "$PAGE_CPP" || fail "instance decoder no longer receives fixed-array data"
if grep -Eq 'uint32_t largest_counts\[s_nInstanceCacheMaxPages\]|memset\( largest_counts' "$INSTANCER_CPP"; then
    fail "legacy instancer ranking raw array/zeroing remains"
fi
if grep -Eq 'uint32_t mesh_counts\[s_nMaxMeshAssets\]|memset\( mesh_counts' "$PAGE_CPP"; then
    fail "legacy runtime-page mesh-count raw array/zeroing remains"
fi

# Persistent indexed tables remain raw by contract; this patch must not convert them.
grep -q 't3dCollisionShapeRecord_t m_szShapeRecords\[s_nMaxRegionSlots\];' "$ROOT/physics/terrain_3d_collision.h" || fail "collision slot table was altered"
grep -q 'uint16_t m_szRegionMap\[s_nMaxRegionSlots\] = {};' "$ROOT/data/terrain_3d_region_repository.h" || fail "repository region map was altered"
grep -q 't3dRegionRuntime_t m_szRegions\[s_nMaxRegionSlots\];' "$ROOT/streaming/terrain_3d_node_streamer.h" || fail "node-streamer region table was altered"

echo "Terrain3D refactor patch 7 gate: PASS"
