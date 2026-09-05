#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D 128 m specialization phase 1 gate: FAIL: $1" >&2; exit 1; }

CONSTANTS="$ROOT/Terrain3DConstants.h"
LIMITS="$ROOT/core/terrain_3d_limits.h"
COLLISION_H="$ROOT/physics/terrain_3d_collision.h"
COLLISION_CPP="$ROOT/physics/terrain_3d_collision.cpp"

grep -q '#define s_nRegionSize ( 128 )' "$CONSTANTS" || fail "canonical region size is missing"
grep -q '#define s_nRegionShift ( 7 )' "$CONSTANTS" || fail "canonical region shift is missing"
grep -q '#define s_nRegionMask ( 127 )' "$CONSTANTS" || fail "canonical region mask is missing"
grep -q '#define s_nRegionMapSize ( 32 )' "$CONSTANTS" || fail "canonical region-map size is missing"
grep -q '#define s_nRegionMapShift ( 5 )' "$CONSTANTS" || fail "canonical region-map shift is missing"
grep -q '#define s_nRegionMapMask ( 31 )' "$CONSTANTS" || fail "canonical region-map mask is missing"
grep -q '#define s_nRegionSlots ( 1024 )' "$CONSTANTS" || fail "canonical region-slot count is missing"
grep -q '#define s_nRegionMapHalf ( 16 )' "$CONSTANTS" || fail "canonical region-map half extent is missing"
grep -q '#define s_nRegionTexels ( 16384 )' "$CONSTANTS" || fail "canonical region texel count is missing"
grep -q '#define s_nRegionHeightBytes ( 32768 )' "$CONSTANTS" || fail "canonical R16 height-page byte count is missing"
grep -q '#define s_nMaxCollisionShapeRecords ( 4096 )' "$CONSTANTS" || fail "independent collision shape capacity is missing"

grep -q 'static_assert( s_nRegionSlots == 1024' "$LIMITS" || fail "canonical region-slot assertion is missing"
grep -q 'static_assert( s_nMaxCollisionShapeRecords == 4096' "$LIMITS" || fail "collision shape-capacity assertion is missing"

grep -q 't3dCollisionShapeRecord_t m_szShapeRecords\[s_nMaxCollisionShapeRecords\];' "$COLLISION_H" || fail "shape records still use region-slot capacity"
grep -q 'int32_t m_gridScratch\[s_nMaxCollisionShapeRecords\] = {};' "$COLLISION_H" || fail "dynamic collision grid scratch still uses region-slot capacity"
grep -q 'eastl::fixed_vector<int32_t, s_nMaxCollisionShapeRecords, false> m_inactiveShapeIds;' "$COLLISION_H" || fail "inactive collision IDs still use region-slot capacity"
grep -q 'shape_count > s_nMaxCollisionShapeRecords' "$COLLISION_CPP" || fail "collision build bound does not use shape capacity"
grep -q 'grid_count > s_nMaxCollisionShapeRecords' "$COLLISION_CPP" || fail "dynamic collision grid bound does not use shape capacity"

# Region-indexed collision state uses the specialized 1024-slot domain after phase 2.
grep -q 'int16_t m_bakedRegionByMap\[s_nRegionSlots\] = {};' "$COLLISION_H" || fail "region-indexed baked map was changed out of phase"
grep -q 't3dRegionCoord_t m_szFullRegionSlots\[s_nRegionSlots\];' "$COLLISION_H" || fail "full-region slots are not packed after phase 8"
grep -q 'uint8_t m_szFullStreamingResident\[s_nRegionSlots\] = {};' "$COLLISION_H" || fail "full streaming residency was changed out of phase"

echo "Terrain3D 128 m specialization phase 1 gate: PASS"
