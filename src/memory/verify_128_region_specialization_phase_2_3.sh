#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D 128 m specialization phases 2+3 gate: FAIL: $1" >&2; exit 1; }

CONSTANTS="$ROOT/Terrain3DConstants.h"
LIMITS="$ROOT/core/terrain_3d_limits.h"
COLLISION_H="$ROOT/physics/terrain_3d_collision.h"
COLLISION_BUILDER_H="$ROOT/physics/terrain_3d_collision_surface_builder.h"
INSTANCE_LAYOUT_H="$ROOT/instances/terrain_3d_instance_layout.h"
INSTANCER_H="$ROOT/instances/terrain_3d_instancer.h"
NODE_H="$ROOT/streaming/terrain_3d_node_streamer.h"
REPOSITORY_H="$ROOT/data/terrain_3d_region_repository.h"
HLOD_H="$ROOT/hlod/terrain_3d_hlod_streamer.h"

# Phase 2: every active/runtime region domain is 32x32 / 1024 slots.
grep -q '#define s_nResidentWorldSize ( s_nRegionMapSize )' "$CONSTANTS" || fail "resident world width is not specialized to 32"
grep -q '#define s_nResidentWorldSlotCount ( s_nRegionSlots )' "$CONSTANTS" || fail "resident world slots are not specialized to 1024"
grep -q '#define s_nInstanceRegionTableCapacity ( ( size_t )( s_nRegionSlots ) )' "$CONSTANTS" || fail "instance region table capacity is not 1024"
grep -q '#define s_nManifestTopologyBytes ( s_nRegionSlots / 8u )' "$CONSTANTS" || fail "manifest topology bytes still use the legacy region domain"

# Representative high-cost fixed tables must use the specialized slot count.
grep -q 'm_szRegionRenderLists\[s_nInstanceRegionTableCapacity\]' "$INSTANCER_H" || fail "instancer render-region table is not specialized"
grep -q 't3dRegionRuntime_t m_szRegions\[s_nRegionSlots\]' "$NODE_H" || fail "node-streamer runtime table is not 1024 slots"
grep -q 'uint16_t m_szRegionMap\[s_nRegionSlots\]' "$REPOSITORY_H" || fail "repository logical map is not 1024 slots"
grep -q 'eastl::bitset<s_nRegionSlots> m_managedRegions;' "$HLOD_H" || fail "HLOD managed-region mask is not 1024 bits"

# There must be no active C++ storage or loop still keyed to the 4096-slot legacy symbol.
if grep -R --include='*.h' --include='*.cpp' -n 's_nMaxRegionSlots' "$ROOT" | grep -v '/Terrain3DConstants.h:' >/dev/null; then
    fail "runtime C++ still references s_nMaxRegionSlots"
fi

# 128-only instance geometry: 8 m minimum cell means at most 16 cells per axis.
grep -q '#define s_nInstanceMaxCellsPerAxis ( 16 )' "$CONSTANTS" || fail "instance max cells per axis is not 16"
grep -q '#define s_nInstanceMaxRegionSize ( s_nRegionSize )' "$CONSTANTS" || fail "instance maximum region size is not fixed to 128"
grep -q '#define s_nInstanceMinRegionSize ( s_nRegionSize )' "$CONSTANTS" || fail "instance minimum region size is not fixed to 128"
if grep -qE '\bnRegionSize\b' "$INSTANCE_LAYOUT_H"; then fail "instance layout still transports a runtime region size"; fi
grep -q 's_nRegionSize >> m_nCellShift' "$INSTANCE_LAYOUT_H" || fail "instance layout is not derived directly from fixed 128 geometry"

# Phase 3: collision region scratch is 128-only, while dynamic shape capacity stays independent at 4096.
grep -q '#define s_nMaxRegionBlendSamples ( s_nRegionSize )' "$CONSTANTS" || fail "collision blend curve is not fixed to 128 samples"
grep -q '#define s_nMaxCollisionAxisSamples ( s_nMaxRegionBlendSamples + 1 )' "$CONSTANTS" || fail "collision axis scratch is not 129 samples"
grep -q '#define s_nCollisionBakedCacheAxis ( 8 )' "$CONSTANTS" || fail "collision baked cache axis is not 8"
grep -q '#define s_nBakedCacheCapacity ( s_nCollisionBakedCacheAxis \* s_nCollisionBakedCacheAxis )' "$CONSTANTS" || fail "collision baked cache capacity is not derived from the 8x8 cache"
grep -q 'int32_t m_szBlendRegionX0\[s_nMaxCollisionAxisSamples\]' "$COLLISION_BUILDER_H" || fail "collision X0 blend scratch does not use the 129-sample bound"
grep -q 'real_t m_fRegionBlendCurve\[s_nMaxRegionBlendSamples\]' "$COLLISION_BUILDER_H" || fail "collision blend curve does not use the 128-sample bound"
grep -q 't3dCollisionRegionCacheEntry_t m_bakedRegionCache\[s_nBakedCacheCapacity\]' "$COLLISION_H" || fail "collision baked-region cache does not use the 64-entry capacity"
grep -q 't3dCollisionShapeRecord_t m_szShapeRecords\[s_nMaxCollisionShapeRecords\]' "$COLLISION_H" || fail "dynamic collision shape records lost their independent capacity"
grep -q '#define s_nMaxCollisionShapeRecords ( 4096 )' "$CONSTANTS" || fail "dynamic collision shape ceiling changed from 4096"

grep -q 's_nMaxCollisionAxisSamples == s_nRegionSize + 1' "$LIMITS" || fail "129-sample collision invariant is missing"
grep -q 's_nCollisionBakedCacheAxis == 8 && s_nBakedCacheCapacity == 64' "$LIMITS" || fail "64-entry collision cache invariant is missing"

echo "Terrain3D 128 m specialization phases 2+3 gate: PASS"
