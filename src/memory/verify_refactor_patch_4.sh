#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D refactor patch 4 gate: FAIL: $1" >&2; exit 1; }

MATERIAL_H="$ROOT/rendering/terrain_3d_material.h"
MATERIAL_CPP="$ROOT/rendering/terrain_3d_material.cpp"

# Patches 1-3 remain cumulative prerequisites.
bash "$ROOT/memory/verify_refactor_patch_3.sh" >/dev/null

# Candidate generation now carries one bounded list rather than pointer + count pairs.
grep -q 'eastl::fixed_vector<t3dShaderVariantState_t, s_nMaxShaderVariantPrewarmLimit, false> &rStates' "$MATERIAL_H" || fail "shader prewarm helper does not accept fixed state storage"
if grep -Eq 'append(VariantState|DimensionStates|PotentialBranchStates)\( t3dShaderVariantState_t \*|int &nCount|int& nCount' "$MATERIAL_H" "$MATERIAL_CPP"; then
    fail "legacy shader prewarm pointer/count helper bookkeeping remains"
fi

# Both prewarm and shader-pack preparation use bounded state lists.
state_list_count="$(grep -c 'eastl::fixed_vector<t3dShaderVariantState_t, s_nMaxShaderVariantPrewarmLimit, false> states;' "$MATERIAL_CPP")"
[ "$state_list_count" -eq 2 ] || fail "expected two fixed shader prewarm state scratch lists"

grep -q 'eastl::fixed_vector<t3dShaderVariantKey_t, s_nMaxShaderVariantPrewarmLimit, false> seen_main;' "$MATERIAL_CPP" || fail "main prewarm key set is not fixed-vector storage"
grep -q 'eastl::fixed_vector<t3dShaderVariantKey_t, s_nMaxShaderVariantPrewarmLimit, false> seen_buffer;' "$MATERIAL_CPP" || fail "buffer prewarm key set is not fixed-vector storage"
grep -q 'eastl::fixed_vector<t3dShaderVariantKey_t, s_nMaxShaderVariantPrewarmLimit \* 2, false> seen;' "$MATERIAL_CPP" || fail "shader-pack seen-key set is not fixed-vector storage"

if grep -Eq 't3dShaderVariantState_t states\[s_nMaxShaderVariantPrewarmLimit\]|t3dShaderVariantKey_t seen(_main|_buffer)?\[' "$MATERIAL_CPP"; then
    fail "raw shader prewarm scratch arrays remain"
fi
if grep -Eq '\b(state_count|seen_main_count|seen_buffer_count|seen_count)\b' "$MATERIAL_CPP"; then
    fail "manual shader prewarm scratch counts remain"
fi

# Existing configured soft cap still gates state appends; physical overflow stays disabled.
grep -q 'const int capacity = MIN( m_nShaderVariantPrewarmLimit, s_nMaxShaderVariantPrewarmLimit );' "$MATERIAL_CPP" || fail "shader prewarm configured cap changed"
grep -q 'if ( ( int )( rStates.size() ) >= capacity )' "$MATERIAL_CPP" || fail "shader prewarm append cap is not checked"
grep -q 'rStates.push_back( eState );' "$MATERIAL_CPP" || fail "shader prewarm state append is missing"

# Deduplication remains first-seen linear scanning and fixed-cap appending.
grep -q 'seen_main.push_back( main_key );' "$MATERIAL_CPP" || fail "main prewarm seen-key append changed"
grep -q 'seen_buffer.push_back( buffer_key );' "$MATERIAL_CPP" || fail "buffer prewarm seen-key append changed"
grep -q 'seen.push_back( key );' "$MATERIAL_CPP" || fail "shader-pack main seen-key append changed"
grep -q 'seen.push_back( buffer_key );' "$MATERIAL_CPP" || fail "shader-pack buffer seen-key append changed"

# Keep this patch free of STL ownership substitutions.
if grep -nE 'std::(vector|deque|list|map|unordered_map|unique_ptr|shared_ptr)' "$MATERIAL_H" "$MATERIAL_CPP"; then
    fail "STL ownership container introduced in shader prewarm mechanics"
fi

echo "Terrain3D refactor patch 4 gate: PASS"
