#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$(cd "$ROOT/.." && pwd)"
fail() { echo "Terrain3D 128 m specialization phases 4+5+6 gate: FAIL: $1" >&2; exit 1; }

TERRAIN_H="$ROOT/terrain_3d.h"
UTIL_H="$ROOT/core/terrain_3d_util.h"
REPOSITORY_H="$ROOT/data/terrain_3d_region_repository.h"
SAMPLER_H="$ROOT/data/terrain_3d_sampler.h"
SAMPLER_CPP="$ROOT/data/terrain_3d_sampler.cpp"
SAMPLING_H="$ROOT/data/terrain_3d_sampling_storage.h"
ACTIVE_H="$ROOT/memory/terrain_3d_active_storage.h"
ACTIVE_CPP="$ROOT/memory/terrain_3d_active_storage.cpp"
RESIDENT_H="$ROOT/memory/terrain_3d_resident_pool.h"
RD_TYPES="$ROOT/rendering/rd/terrain_3d_rd_types.h"
RD_COORD="$ROOT/rendering/rd/terrain_3d_render_coordinator.cpp"
COMPUTE_H="$ROOT/rendering/terrain_3d_compute.h"
COMPUTE_CPP="$ROOT/rendering/terrain_3d_compute.cpp"
VERTEX="$PROJECT/project/addons/terrain_3d/rd/terrain_clipmap_vertex_inc.glsl"
FRAGMENT="$PROJECT/project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glsl"
DISPLACEMENT="$PROJECT/project/addons/terrain_3d/compute/terrain_displacement.glsl"

# Phase 4: runtime geometry no longer owns mutable region dimensions.
if grep -q 'm_nRegionSize' "$TERRAIN_H"; then fail "Terrain3D still stores mutable region size"; fi
if grep -Eq 'm_nRegionSize|m_nRegionMapSize|m_regionSizev' "$REPOSITORY_H"; then fail "region repository still stores mutable geometry dimensions"; fi
grep -q 'return value == s_nRegionSize;' "$UTIL_H" || fail "region-size validator is not 128-only"
if grep -q 'configureGeometry' "$REPOSITORY_H"; then fail "repository still exposes mutable geometry configuration"; fi
if grep -R -n 'm_nTargetRegionSize' "$ROOT/data" >/dev/null; then fail "staged repository still transports a mutable target region size"; fi
if grep -R -n 'nExpectedRegionSize' "$ROOT/data" >/dev/null; then fail "runtime source loader still transports an expected region size"; fi

# Phase 5: active/resident/sampler layout is fixed at 128 without size arguments/state.
if grep -Eq 'm_nRegionSize|m_nRegionShift|m_nRegionMask|floorDiv' "$SAMPLER_H" "$SAMPLER_CPP"; then
    fail "sampler still carries generic region addressing state"
fi
grep -q 'nRegion = nGrid >> s_nRegionShift;' "$SAMPLER_CPP" || fail "sampler positive addressing is not shift-specialized"
grep -q 's_nRegionMask' "$SAMPLER_CPP" || fail "sampler local addressing does not use the 127 mask"
grep -q 's_nRegionTexels' "$SAMPLER_CPP" || fail "sampler does not use the fixed region texel count"
if grep -Eq 'm_nRegionSize|m_nTexelCount' "$RESIDENT_H"; then fail "resident pool still stores fixed layout dimensions"; fi
if grep -q 'nRegionSize' "$ACTIVE_H"; then
    fail "active storage API still accepts a region size"
fi
if grep -q 'int nRegionSize' "$SAMPLING_H"; then fail "sampling-storage API still accepts a region size"; fi
grep -q 'const int logical_count = s_nRegionSlots;' "$ACTIVE_CPP" || fail "active height replacement is not fixed to 1024 logical slots"
grep -q 'return ( size_t )( s_nRegionHeightBytes );' "$ACTIVE_CPP" || fail "active height page bytes are not the fixed 32 KiB constant"

# Phase 6: direct-RD frame/push interfaces and shaders do not transport dynamic 128/32 values.
if grep -Eq 'm_nRegionSize|m_nRegionMapSize' "$RD_TYPES"; then fail "RD frame snapshot still transports region dimensions"; fi
if grep -Eq 'm_fRegionSize|m_fRegionMapSize' "$RD_TYPES"; then fail "RD frame UBO still transports region dimensions"; fi
if grep -Eq 'm_nRegionSize|m_nRegionMapSize' "$COMPUTE_H"; then fail "compute request still transports region dimensions"; fi
if grep -Eq 'm_nRegionSize|m_nRegionMapSize' "$COMPUTE_CPP"; then fail "compute push preparation still transports region dimensions"; fi
grep -q 'sizeof( t3dDisplacementParams_t ) == 104u' "$COMPUTE_CPP" || fail "displacement push ABI was not reduced to the fixed-layout size"
if grep -Eq 'terrain_params\.[xy]|floor_div_i|floor_mod_i' "$VERTEX" "$FRAGMENT"; then fail "RD clipmap shader still reads dynamic region geometry"
fi
if grep -Eq 'region_size[[:space:]]*;|region_map_size[[:space:]]*;' "$DISPLACEMENT"; then fail "displacement push constants still contain dynamic region geometry"
fi
for shader in "$VERTEX" "$FRAGMENT"; do
    grep -q 'T3D_REGION_SIZE = 128' "$shader" || fail "clipmap shader is missing the 128 constant"
    grep -q 'T3D_REGION_SHIFT = 7' "$shader" || fail "clipmap shader is missing the region shift"
    grep -q 'T3D_REGION_MASK = 127' "$shader" || fail "clipmap shader is missing the region mask"
    grep -q 'T3D_REGION_MAP_SIZE = 32' "$shader" || fail "clipmap shader is missing the 32-wide map constant"
done
grep -q 'REGION_SIZE = 128' "$DISPLACEMENT" || fail "displacement shader is missing the 128 constant"
grep -q 'REGION_SHIFT = 7' "$DISPLACEMENT" || fail "displacement shader is missing the region shift"
grep -q 'REGION_MASK = 127' "$DISPLACEMENT" || fail "displacement shader is missing the region mask"
grep -q 'REGION_MAP_SIZE = 32' "$DISPLACEMENT" || fail "displacement shader is missing the 32-wide map constant"
grep -q 'texel >> REGION_SHIFT' "$DISPLACEMENT" || fail "displacement region addressing is not shift-specialized"
grep -q 'texel & ivec2(REGION_MASK)' "$DISPLACEMENT" || fail "displacement local addressing is not mask-specialized"
if grep -Eq '%[[:space:]]*s_nRegionMapSize|/[[:space:]]*s_nRegionMapSize|%[[:space:]]*region_map_size|/[[:space:]]*region_map_size' "$RD_COORD"; then
    fail "RD coordinator still divides/modulos by the fixed 32-wide region map"
fi

echo "Terrain3D 128 m specialization phases 4+5+6 gate: PASS"
