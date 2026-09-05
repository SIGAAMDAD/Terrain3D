#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 1 gate: FAIL: $1" >&2; exit 1; }

REPO_H="$ROOT/data/terrain_3d_region_repository.h"
REPO_CPP="$ROOT/data/terrain_3d_region_repository.cpp"
DATA_CPP="$ROOT/data/terrain_3d_data.cpp"
EDIT_CPP="$ROOT/data/terrain_3d_map_edit_service.cpp"
LIGHT_CPP="$ROOT/lighting/terrain_3d_lightmap_streamer.cpp"
AUDIT="$(cd "$ROOT/.." && pwd)/PATCH_8_RAW_STORAGE_AUDIT.tsv"

# Repository reconciliation lists own their size through non-overflowing inline storage.
grep -q 'eastl::fixed_vector<Vector2i, s_nRegionSlots, false> m_removedLocations;' "$REPO_H" || fail "removed-location list is not fixed vector storage"
grep -q 'eastl::fixed_vector<int, s_nRegionSlots, false> m_addedSlots;' "$REPO_H" || fail "added-slot list is not fixed vector storage"
grep -q 'eastl::fixed_vector<Vector2i, s_nRegionSlots, false> uniqueLocations;' "$REPO_CPP" || fail "unique reconciliation locations still use array+count storage"

if grep -Eq 'm_szRemovedLocations|m_nRemovedLocationCount|m_szAddedSlots|m_nAddedSlotCount|uniqueLocationCount' "$REPO_H" "$REPO_CPP" "$DATA_CPP"; then
    fail "legacy reconciliation array/count bookkeeping remains"
fi

grep -q 'for ( const Vector2i location : reconcile.m_removedLocations )' "$DATA_CPP" || fail "removed reconciliation list is not consumed directly"
grep -q 'for ( const int slot : reconcile.m_addedSlots )' "$DATA_CPP" || fail "added reconciliation list is not consumed directly"

# Small local candidate lists must not retain separate logical counts.
grep -q 'eastl::fixed_vector<Vector2i, 3, false> candidates;' "$EDIT_CPP" || fail "map-edit candidate list is not fixed vector storage"
if grep -Eq 'candidateCount|eastl::array<Vector2i, 3> candidates' "$EDIT_CPP"; then
    fail "map-edit candidate array/count boilerplate remains"
fi

grep -q 'eastl::fixed_vector<t3dLightmapCandidate_t, s_nLightmapActiveInstanceLimit, false> candidates;' "$LIGHT_CPP" || fail "lightmap candidate list is not fixed vector storage"
if grep -Eq 'candidate_count|t3dLightmapCandidate_t candidates\[s_nLightmapActiveInstanceLimit\]' "$LIGHT_CPP"; then
    fail "lightmap candidate array/count boilerplate remains"
fi

# The two converted declarations no longer belong in the raw-storage retain inventory.
if grep -Eq $'\tm_szRemovedLocations\t|\tm_szAddedSlots\t' "$AUDIT"; then
    fail "raw-storage audit still lists converted reconciliation arrays"
fi

# Patch 8 fixed vectors must remain inline-only.

# No STL ownership was introduced by this patch.
if grep -nE 'std::(vector|deque|list|map|unordered_map|unique_ptr|shared_ptr)' "$REPO_H" "$REPO_CPP" "$DATA_CPP" "$EDIT_CPP" "$LIGHT_CPP"; then
    fail "STL ownership container introduced in touched source"
fi

echo "Terrain3D refactor patch 1 gate: PASS"
