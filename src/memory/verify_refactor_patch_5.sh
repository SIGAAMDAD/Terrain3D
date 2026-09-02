#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 5 gate: FAIL: $1" >&2; exit 1; }

INSTANCER_CPP="$ROOT/instances/terrain_3d_instancer.cpp"

bash "$ROOT/memory/verify_refactor_patch_4.sh" >/dev/null

grep -q 'eastl::fixed_vector<int, s_nMeshMaxLodCount, false> valid_lods;' "$INSTANCER_CPP" || fail "valid LOD scratch is not fixed-vector storage"
grep -q 'valid_lods.push_back( lod );' "$INSTANCER_CPP" || fail "valid LOD append is missing"
grep -q 'index + 1u < valid_lods.size()' "$INSTANCER_CPP" || fail "valid LOD adjacency scan does not use fixed-vector size"
if grep -Eq 'int valid_lods\[s_nMeshMaxLodCount\]|\bvalid_count\b' "$INSTANCER_CPP"; then
    fail "legacy valid-LOD array/count bookkeeping remains"
fi
if grep -nE 'std::(vector|deque|list|map|unordered_map|unique_ptr|shared_ptr)' "$INSTANCER_CPP"; then
    fail "STL ownership container introduced in instancer mechanics"
fi

echo "Terrain3D refactor patch 5 gate: PASS"
