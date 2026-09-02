#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 8 gate: FAIL: $1" >&2; exit 1; }

NODE_CPP="$ROOT/streaming/terrain_3d_node_streamer.cpp"
NODE_H="$ROOT/streaming/terrain_3d_node_streamer.h"

bash "$ROOT/memory/verify_refactor_patch_7.sh" >/dev/null

grep -q 'eastl::fixed_vector<Node3D \*, s_nMaxRegionSlots, false> new_roots;' "$NODE_CPP" || fail "new root scratch is not fixed-vector storage"
grep -q 'new_roots.push_back( pNewRoot );' "$NODE_CPP" || fail "new root scratch append is missing"
grep -q 'for ( Node3D \*const pRoot : new_roots )' "$NODE_CPP" || fail "partial allocation cleanup does not cover the fixed-vector prefix"
grep -q 'eastl::array<uint64_t, s_nMaxRegionSlots / 64> leased_roots{};' "$NODE_CPP" || fail "leased-root bitmap scratch is not fixed-array storage"
if grep -Eq 'Node3D \*newRoots\[s_nMaxRegionSlots\]|uint64_t leasedRoots\[s_nMaxRegionSlots / 64\]|memset\( leasedRoots' "$NODE_CPP"; then
    fail "legacy NodeStreamer cold scratch remains"
fi
# Stable indexed root ownership remains raw by contract.
grep -q 'Node3D \*m_szRegionRootPool\[s_nMaxRegionSlots\] = {};' "$NODE_H" || fail "persistent root-pool indexing was altered"

echo "Terrain3D refactor patch 8 gate: PASS"
