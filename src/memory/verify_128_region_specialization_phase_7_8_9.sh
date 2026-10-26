#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$(cd "$ROOT/.." && pwd)"
fail() { echo "Terrain3D 128 m specialization phases 7+8+9 gate: FAIL: $1" >&2; exit 1; }

CONSTANTS="$ROOT/Terrain3DConstants.h"
PAGE_FORMAT="$ROOT/data/terrain_3d_runtime_page_format.h"
PAGE_STORE_H="$ROOT/streaming/terrain_3d_runtime_page_store.h"
WORLD_H="$ROOT/data/terrain_3d_runtime_world_manifest.h"
COOK_H="$ROOT/authoring/terrain_3d_cook_database.h"
HLOD_H="$ROOT/hlod/terrain_3d_hlod_manifest.h"
REPO_H="$ROOT/data/terrain_3d_region_repository.h"
PERSIST_H="$ROOT/data/terrain_3d_region_persistence.h"
REGION_H="$ROOT/data/terrain_3d_region.h"
COLLISION_H="$ROOT/physics/terrain_3d_collision.h"
TERRAIN_H="$ROOT/terrain_3d.h"
DATA_H="$ROOT/data/terrain_3d_data.h"
BINDINGS="$ROOT/core/terrain_3d_bindings.cpp"
HLOD_BAKER="$PROJECT/project/addons/terrain_3d/tools/hlod_baker.gd"

# Phase 7: cooked artifacts are versioned against one fixed geometry signature.
grep -q '#define s_nRegionGeometrySignature' "$CONSTANTS" || fail "fixed region geometry signature is missing"
grep -q '#define s_nRuntimePageVersion ( 7u )' "$CONSTANTS" || fail "runtime page format was not bumped to v7"
grep -q '#define s_nRuntimeWorldManifestVersion ( 5u )' "$CONSTANTS" || fail "world manifest format was not bumped to v5"
grep -q '#define s_nGpuFarAtlasVersion ( 9u )' "$CONSTANTS" || fail "far atlas format was not bumped to v9"
grep -q '#define s_nCookDatabaseVersion ( 2u )' "$CONSTANTS" || fail "cook database format was not bumped to v2"
grep -q '#define s_nBlobVersion ( 2u )' "$CONSTANTS" || fail "runtime baked blob format was not bumped to v2"
grep -q '#define s_nHlodRuntimeVersion ( 2u )' "$CONSTANTS" || fail "HLOD runtime format was not bumped to v2"
grep -q 'uint32_t m_nReservedGeometry = 0u;' "$PAGE_FORMAT" || fail "runtime page header still serializes live region size"
if grep -Eq 'm_nRegionSize|m_nRegionMapSize' "$PAGE_STORE_H" "$WORLD_H" "$COOK_H" "$HLOD_H"; then
    fail "cooked runtime metadata still owns mutable region dimensions"
fi
grep -q 'int m_nFormat = 3;' "$HLOD_H" || fail "HLOD manifest resource format was not bumped to 3"
grep -q 'const BUILDER_VERSION: int = 5' "$HLOD_BAKER" || fail "HLOD builder signature was not bumped"
if grep -q 'terrain.region_size' "$HLOD_BAKER"; then fail "HLOD baker still reads live terrain region size"; fi

# Phase 8: large bounded region-coordinate tables use two-byte signed coordinates.
grep -q 'typedef struct t3dRegionCoord_s' "$CONSTANTS" || fail "packed region coordinate POD is missing"
grep -q 'static_assert( sizeof( t3dRegionCoord_t ) == 2u' "$CONSTANTS" || fail "packed coordinate size invariant is missing"
grep -q 't3dRegionCoord_t m_szRegionLocations\[s_nRegionSlots\] = {};' "$REPO_H" || fail "repository slot coordinates are not packed"
grep -q 'eastl::array<t3dRegionCoord_t, s_nRegionSlots> m_regionLocations = {};' "$PERSIST_H" || fail "staged repository coordinates are not packed"
grep -q 't3dRegionCoord_t m_szFullRegionSlots\[s_nRegionSlots\];' "$COLLISION_H" || fail "full-collision region slots are not packed"
grep -q 't3dRegionCoord_t m_location = t3dInvalidRegionCoord();' "$WORLD_H" || fail "runtime world records do not use packed region coordinates"

# Phase 9: no live region-size mutation path or historical 64/4096 logical-domain aliases remain.
if grep -Eq 'changeRegionSize|change_region_size' "$TERRAIN_H" "$DATA_H" "$BINDINGS"; then
    fail "legacy live region-size change API remains"
fi
if grep -q 'int m_nRegionSize' "$REGION_H"; then fail "Terrain3DRegion still stores mutable region size"; fi
grep -q 'return s_nRegionSize;' "$REGION_H" || fail "Terrain3DRegion compatibility getter is not fixed to 128"
if grep -Eq 's_nMinRegionSize|s_nMaxRegionMapSize|s_nMaxRegionSlots|s_nActiveMaxRegionMapSize|s_nActiveMaxRegionSlots' "$CONSTANTS"; then
    fail "historical generic region-domain aliases remain"
fi

# The only canonical dimensions are the 128/32/1024 set.
grep -q '#define s_nRegionSize ( 128 )' "$CONSTANTS" || fail "128 region size changed"
grep -q '#define s_nRegionMapSize ( 32 )' "$CONSTANTS" || fail "32-wide map changed"
grep -q '#define s_nRegionSlots ( 1024 )' "$CONSTANTS" || fail "1024-slot domain changed"

echo "Terrain3D 128 m specialization phases 7+8+9 gate: PASS"
