#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT_ROOT="$(cd "$ROOT/.." && pwd)"
fail() { echo "Terrain3D refactor patch 3 gate: FAIL: $1" >&2; exit 1; }

MATERIAL_H="$ROOT/rendering/terrain_3d_material.h"
MATERIAL_CPP="$ROOT/rendering/terrain_3d_material.cpp"
AUDIT="$PROJECT_ROOT/PATCH_8_RAW_STORAGE_AUDIT.tsv"

# Patches 1 and 2 remain cumulative prerequisites.
bash "$ROOT/memory/verify_refactor_patch_2.sh" >/dev/null

# Shader variant domains are compact bounded LRU lists, not stable indexed tables.
grep -q 'eastl::fixed_vector<t3dShaderVariantEntry_t, s_nMaxShaderVariantCacheLimit, false> m_runtimeShaderVariantEntries;' "$MATERIAL_H" || fail "runtime shader variants are not fixed-vector storage"
grep -q 'eastl::fixed_vector<t3dShaderVariantEntry_t, s_nMaxEditorShaderVariantCacheLimit, false> m_editorShaderVariantEntries;' "$MATERIAL_H" || fail "editor shader variants are not fixed-vector storage"
grep -q 'eastl::fixed_vector<t3dShaderVariantEntry_t, s_nMaxBufferShaderVariantCacheLimit, false> m_bufferShaderVariantEntries;' "$MATERIAL_H" || fail "buffer shader variants are not fixed-vector storage"

if grep -Eq 'm_sz(Runtime|Editor|Buffer)ShaderVariantEntries|m_n(Runtime|Editor|Buffer)ShaderVariantCount' "$MATERIAL_H" "$MATERIAL_CPP"; then
    fail "legacy shader variant array/count bookkeeping remains"
fi

# Existing physical hard-cap checks must still guard every fixed-vector append.
grep -q 'if ( getShaderVariantCountForDomain( domain ) >= physical_capacity )' "$MATERIAL_CPP" || fail "shader variant physical-capacity guard changed"
grep -q 'm_runtimeShaderVariantEntries.push_back( t3dShaderVariantEntry_t{} );' "$MATERIAL_CPP" || fail "runtime shader variant append is missing"
grep -q 'm_editorShaderVariantEntries.push_back( t3dShaderVariantEntry_t{} );' "$MATERIAL_CPP" || fail "editor shader variant append is missing"
grep -q 'm_bufferShaderVariantEntries.push_back( t3dShaderVariantEntry_t{} );' "$MATERIAL_CPP" || fail "buffer shader variant append is missing"

# LRU mechanics still shift entries and retire exactly one tail element.
grep -q 'for ( int index = iIndex; index + 1 < count; ++index )' "$MATERIAL_CPP" || fail "shader variant erase shift changed"
grep -q 'for ( int index = count - 1; index > 0; --index )' "$MATERIAL_CPP" || fail "shader variant MRU insertion shift changed"
for symbol in m_runtimeShaderVariantEntries m_editorShaderVariantEntries m_bufferShaderVariantEntries; do
    grep -q "$symbol.pop_back();" "$MATERIAL_CPP" || fail "$symbol does not retire through fixed-vector size"
    grep -q "$symbol.clear();" "$MATERIAL_CPP" || fail "$symbol is not cleared through fixed-vector lifetime"
done

# Converted raw arrays must no longer be retained by the audit.
if grep -Eq $'\tm_sz(Runtime|Editor|Buffer)ShaderVariantEntries\t' "$AUDIT"; then
    fail "raw-storage audit still lists converted shader variant arrays"
fi

# No STL ownership substitution is allowed.
if grep -nE 'std::(vector|deque|list|map|unordered_map|unique_ptr|shared_ptr)' "$MATERIAL_H" "$MATERIAL_CPP"; then
    fail "STL ownership container introduced in material shader caches"
fi

echo "Terrain3D refactor patch 3 gate: PASS"
