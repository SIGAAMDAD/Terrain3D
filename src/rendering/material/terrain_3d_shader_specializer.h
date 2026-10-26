// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_SHADER_SPECIALIZER_H
#define TERRAIN3D_SHADER_SPECIALIZER_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"
#include "rendering/material/terrain_3d_material_tile_class.h"

#include <stdint.h>

// WG-A P01-P03 frozen terrain shader pipeline contract. Keep this key bounded:
// only renderer-reachable combinations may be materialized as RDShaderFile versions.

// WG-C bounded draw-wide specialization. These are the only material feature
// decisions specialized by this shader; all other feature flags remain runtime.
// The 3-bit domain is intentionally exhaustive and bounded: 0..7.

// Shared fragment-stage specialization constant ID. Keep this stable so host
// pipeline creation and the GLSL contract cannot silently drift.

static_assert( T3D_TERRAIN_SHIPPING_FEATURE_MASK == T3D_TERRAIN_SHIPPING_FEATURE_CLASS_COUNT - 1u,
			   "Terrain shipping specialization feature bits must remain a dense bounded 0..7 domain" );

// WG-G P14 bounded lighting specialization. Lighting model is tier-stable for
// a terrain draw and therefore belongs in pipeline specialization rather than
// the per-light BRDF branch graph. Compatibility/editor paths retain runtime
// model selection through the explicit 0xff sentinel.

// General Forward+ keeps dynamic clustered integrations when either decals or
// reflection probes are present. A second bounded class proves both systems
// absent and lets specialization delete both cluster walks. One-sun always
// selects NONE because those descriptors are already absent from that family.

// WG-H P11/P12 bounded prepass/middle-tier specialization. DNR outputs are
// draw/tier stable; middle surface source is draw stable and selects either
// the authoritative baked composite or the exact detailed fallback.

// Batch certification never promotes a subset of a GPU-driven draw to a new
// shader key. Unknown/general inputs share the existing bounded generic class.
inline uint8_t t3dTerrainMaterialBatchClassCold(
    const t3dMaterialTileRegionClasses_t *pRegions,
    uint32_t nRegionCount, const uint32_t *pExpectedRevisions,
    bool bCompleteDrawCoverage, bool bStaticControls ) {
    if ( !bCompleteDrawCoverage || !bStaticControls ) return T3D_MATERIAL_TILE_UNKNOWN;
    return t3d_material_tiles_certify_batch( pRegions, nRegionCount, pExpectedRevisions );
}

// No new specialization IDs or permutations. A proven draw coverage mask may
// select an existing bounded material class; otherwise canonical shaders win.
inline uint8_t t3dTerrainCoveredMaterialBatchClassCold(
    const t3dMaterialTileRegionClasses_t *pRegions,
    uint32_t nRegionCount, const uint32_t *pExpectedRevisions,
    const uint64_t *pWholeDrawTileCoverage, bool bCompleteDrawCoverage,
    bool bStaticControls ) {
    if ( !bCompleteDrawCoverage || !bStaticControls ) return T3D_MATERIAL_TILE_UNKNOWN;
    return t3d_material_tiles_certify_covered_batch( pRegions, nRegionCount,
        pExpectedRevisions, pWholeDrawTileCoverage );
}

bool t3dTerrainShaderPipelineKeyIsValid( const t3dTerrainShaderPipelineKey_t &rKey );

#endif // TERRAIN3D_SHADER_SPECIALIZER_H
