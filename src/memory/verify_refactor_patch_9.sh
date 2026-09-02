#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 9 gate: FAIL: $1" >&2; exit 1; }

REPO_CPP="$ROOT/data/terrain_3d_region_repository.cpp"
REPO_H="$ROOT/data/terrain_3d_region_repository.h"

bash "$ROOT/memory/verify_refactor_patch_8.sh" >/dev/null

grep -q 'eastl::array<bool, s_nMaxRegionSlots> free_seen{};' "$REPO_CPP" || fail "free-slot validation scratch is not fixed-array storage"
grep -q 'eastl::array<bool, s_nMaxRegionSlots> location_seen{};' "$REPO_CPP" || fail "location validation scratch is not fixed-array storage"
if grep -Eq 'bool freeSeen\[s_nMaxRegionSlots\]|bool locationSeen\[s_nMaxRegionSlots\]' "$REPO_CPP"; then
    fail "legacy repository validation raw arrays remain"
fi
# Persistent repository identity tables remain raw by contract.
grep -q 'Vector2i m_szRegionLocations\[s_nMaxRegionSlots\] = {};' "$REPO_H" || fail "repository location slot table was altered"
grep -q 'uint16_t m_szRegionMap\[s_nMaxRegionSlots\] = {};' "$REPO_H" || fail "repository logical map was altered"

echo "Terrain3D refactor patch 9 gate: PASS"
