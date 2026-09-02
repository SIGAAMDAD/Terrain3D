#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 11 gate: FAIL: $1" >&2; exit 1; }

PAGE_CPP="$ROOT/streaming/terrain_3d_runtime_page_store.cpp"

bash "$ROOT/memory/verify_refactor_patch_10.sh" >/dev/null

grep -q 'const eastl::array<Vector2i, 3> height_dependency_offsets = {' "$PAGE_CPP" || fail "height dependency offsets are not fixed-array storage"
grep -q 'dependency < height_dependency_offsets.size()' "$PAGE_CPP" || fail "height dependency scan still duplicates the table extent"
if grep -Eq 'Vector2i height_dependency_offsets\[3\]|dependency < 3' "$PAGE_CPP"; then
    fail "legacy height dependency raw table/extent remains"
fi
# Runtime page channel payload table remains raw direct-index state by contract.
grep -q 'PackedByteArray m_szPayloads\[s_nRuntimePageChannelCount\];' "$PAGE_CPP" || fail "staged runtime-page channel table was altered"

echo "Terrain3D refactor patch 11 gate: PASS"
