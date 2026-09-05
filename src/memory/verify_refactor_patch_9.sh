#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 9 gate: FAIL: $1" >&2; exit 1; }

REPO_CPP="$ROOT/data/terrain_3d_region_repository.cpp"
REPO_H="$ROOT/data/terrain_3d_region_repository.h"

bash "$ROOT/memory/verify_refactor_patch_8.sh" >/dev/null

grep -q 'eastl::array<bool, s_nRegionSlots> free_seen{};' "$REPO_CPP" || fail "free-slot validation scratch is not fixed-array storage"
grep -q 'eastl::array<bool, s_nRegionSlots> location_seen{};' "$REPO_CPP" || fail "location validation scratch is not fixed-array storage"
if grep -Eq 'bool freeSeen\[s_nRegionSlots\]|bool locationSeen\[s_nRegionSlots\]' "$REPO_CPP"; then
    fail "legacy repository validation raw arrays remain"
fi
# The 128-only specialization packs bounded region coordinates to signed bytes.
grep -q 't3dRegionCoord_t m_szRegionLocations\[s_nRegionSlots\] = {};' "$REPO_H" || fail "repository location slot table is not packed"
grep -q 'uint16_t m_szRegionMap\[s_nRegionSlots\] = {};' "$REPO_H" || fail "repository logical map was altered"

echo "Terrain3D refactor patch 9 gate: PASS"
