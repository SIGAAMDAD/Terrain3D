#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT_ROOT="$(cd "$ROOT/.." && pwd)"
fail() { echo "Terrain3D refactor patch 2 gate: FAIL: $1" >&2; exit 1; }

LIGHT_H="$ROOT/lighting/terrain_3d_lightmap_streamer.h"
LIGHT_CPP="$ROOT/lighting/terrain_3d_lightmap_streamer.cpp"
AUDIT="$PROJECT_ROOT/PATCH_8_RAW_STORAGE_AUDIT.tsv"

# Patch 1 remains a required cumulative prerequisite.
bash "$ROOT/memory/verify_refactor_patch_1.sh" >/dev/null

# Active regional lightmaps are a logical bounded list, not a stable slot table.
grep -q 'eastl::fixed_vector<t3dActivePackage_t, s_nLightmapActiveInstanceLimit, false> m_activePackages;' "$LIGHT_H" || fail "active lightmap packages are not fixed-vector storage"
if grep -Eq 'm_szActive|m_nActiveCount' "$LIGHT_H" "$LIGHT_CPP"; then
    fail "legacy active-package array/count bookkeeping remains"
fi

# The exact engine/package cap remains authoritative and overflow allocation is forbidden.
grep -q 'if ( m_activePackages.full() )' "$LIGHT_CPP" || fail "active-package capacity is not checked through fixed storage"
grep -q 'm_activePackages.push_back( active );' "$LIGHT_CPP" || fail "active-package activation does not append to fixed storage"
grep -q 'm_activePackages.pop_back();' "$LIGHT_CPP" || fail "active-package deactivation does not retire fixed storage"

# The fixed node pool stays independent; Patch 2 must not collapse object ownership.
grep -q 'eastl::fixed_vector<LightmapGI \*, s_nLightmapActiveInstanceLimit, false> m_freeNodes;' "$LIGHT_H" || fail "fixed LightmapGI node pool was altered"

# Converted raw storage should no longer be retained by the audit.
if grep -Eq $'src/lighting/terrain_3d_lightmap_streamer.h\t[0-9]+\tm_szActive\t' "$AUDIT"; then
    fail "raw-storage audit still lists converted active-package array"
fi

# Keep this patch free of STL ownership substitutions.
if grep -nE 'std::(vector|deque|list|map|unordered_map|unique_ptr|shared_ptr)' "$LIGHT_H" "$LIGHT_CPP"; then
    fail "STL ownership container introduced in lightmap streamer"
fi

echo "Terrain3D refactor patch 2 gate: PASS"
