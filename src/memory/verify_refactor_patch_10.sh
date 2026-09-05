#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 10 gate: FAIL: $1" >&2; exit 1; }

DATA_CPP="$ROOT/data/terrain_3d_data.cpp"

bash "$ROOT/memory/verify_refactor_patch_9.sh" >/dev/null

grep -q 'eastl::fixed_vector<const uint8_t \*, s_nRegionSlots, false> staged_height_pages;' "$DATA_CPP" || fail "staged height page pointers are not fixed-vector storage"
grep -q 'eastl::fixed_vector<int, s_nRegionSlots, false> staged_height_slots;' "$DATA_CPP" || fail "staged height slot IDs are not fixed-vector storage"
grep -q 'staged_height_pages.push_back( entry.m_stagedHeight.ptr() );' "$DATA_CPP" || fail "staged page append is missing"
grep -q 'staged_height_slots.push_back( entry.m_nActiveSlot );' "$DATA_CPP" || fail "staged slot append is missing"
grep -q 'staged_height_pages.data(), staged_height_slots.data(), staged_height_pages.size()' "$DATA_CPP" || fail "active-height publication no longer uses the synchronized fixed-vector span"
if grep -Eq 'pStagedHeightPages\[s_nRegionSlots\]|stagedHeightSlots\[s_nRegionSlots\]|\bstagedHeightCount\b' "$DATA_CPP"; then
    fail "legacy active-height staging array/count bookkeeping remains"
fi

echo "Terrain3D refactor patch 10 gate: PASS"
