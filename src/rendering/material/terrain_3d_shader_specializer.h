// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_SHADER_SPECIALIZER_H
#define TERRAIN3D_SHADER_SPECIALIZER_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include <stdint.h>

// WG-A P01-P03 frozen terrain shader pipeline contract. Keep this key bounded:
// only renderer-reachable combinations may be materialized as RDShaderFile versions.
// t3dTerrainShaderPassClass_e centralized in Terrain3DTypes.h.


// t3dTerrainMaterialBackend_e centralized in Terrain3DTypes.h.


// t3dTerrainShaderPath_e centralized in Terrain3DTypes.h.


// WG-C bounded draw-wide specialization. These are the only material feature
// decisions specialized in this wave; all other feature flags remain runtime.
// The 3-bit domain is intentionally exhaustive and bounded: 0..7.
// t3dTerrainShippingFeatureClass_e centralized in Terrain3DTypes.h.


// Shared fragment-stage specialization constant ID. Keep this stable so host
// pipeline creation and the GLSL contract cannot silently drift.


static_assert( T3D_TERRAIN_SHIPPING_FEATURE_MASK == T3D_TERRAIN_SHIPPING_FEATURE_CLASS_COUNT - 1u,
        "Terrain shipping specialization feature bits must remain a dense bounded 0..7 domain" );

// WG-G P14 bounded lighting specialization. Lighting model is tier-stable for
// a terrain draw and therefore belongs in pipeline specialization rather than
// the per-light BRDF branch graph. Compatibility/editor paths retain runtime
// model selection through the explicit 0xff sentinel.
// t3dTerrainLightingModelClass_e centralized in Terrain3DTypes.h.


// General Forward+ keeps dynamic clustered integrations when either decals or
// reflection probes are present. A second bounded class proves both systems
// absent and lets specialization delete both cluster walks. One-sun always
// selects NONE because those descriptors are already absent from that family.
// t3dTerrainSceneIntegrationClass_e centralized in Terrain3DTypes.h.



// WG-H P11/P12 bounded prepass/middle-tier specialization. DNR outputs are
// draw/tier stable; middle surface source is draw stable and selects either
// the authoritative baked composite or the exact detailed fallback.
// t3dTerrainDnrOutputClass_e centralized in Terrain3DTypes.h.


// t3dTerrainMiddleSurfaceClass_e centralized in Terrain3DTypes.h.



// t3dTerrainShaderPipelineKey_s centralized in Terrain3DTypes.h.


bool t3dTerrainShaderPipelineKeyIsValid( const t3dTerrainShaderPipelineKey_t &rKey );

#endif // TERRAIN3D_SHADER_SPECIALIZER_H
