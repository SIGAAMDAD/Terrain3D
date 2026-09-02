#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 6 gate: FAIL: $1" >&2; exit 1; }

MAP_EDIT_CPP="$ROOT/data/terrain_3d_map_edit_service.cpp"

bash "$ROOT/memory/verify_refactor_patch_5.sh" >/dev/null

grep -q 'eastl::fixed_vector<t3dDerivedMapComputeJob_t, s_nComputeBatchSize, false> jobs;' "$MAP_EDIT_CPP" || fail "derived compute jobs are not fixed-vector storage"
grep -q 'eastl::fixed_vector<size_t, s_nComputeBatchSize, false> task_indices;' "$MAP_EDIT_CPP" || fail "derived compute task indices are not fixed-vector storage"
grep -q 'while ( taskCursor < tasks.size() && jobs.size() < s_nComputeBatchSize )' "$MAP_EDIT_CPP" || fail "compute batch cap no longer follows fixed-vector size"
grep -q 'const size_t job_index = jobs.size();' "$MAP_EDIT_CPP" || fail "dense compute batch index is missing"
grep -q 'jobs.push_back( t3dDerivedMapComputeJob_t{} );' "$MAP_EDIT_CPP" || fail "compute job append is missing"
grep -q 'task_indices.push_back( currentTask );' "$MAP_EDIT_CPP" || fail "task-index append is missing"
grep -q 'generateDerivedMaps( jobs.data(), ( uint32_t )( jobs.size() ) )' "$MAP_EDIT_CPP" || fail "compute call does not use fixed-vector data/size"
if grep -Eq 't3dDerivedMapComputeJob_t jobs\[s_nComputeBatchSize\]|size_t taskIndices\[s_nComputeBatchSize\]|\bjobCount\b' "$MAP_EDIT_CPP"; then
    fail "legacy derived compute array/count bookkeeping remains"
fi

echo "Terrain3D refactor patch 6 gate: PASS"
