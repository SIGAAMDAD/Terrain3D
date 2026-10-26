// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_FORWARD_VARIANT_H
#define TERRAIN3D_RD_FORWARD_VARIANT_H

#include "Terrain3DTypes.h"

#include "rendering/rd/core/terrain_3d_rd_types.h"

// t3dRdForwardVariantCapability_e centralized in Terrain3DTypes.h.


bool t3dRdSelectForwardVariant( const t3dRdForwardPassContext_t &rContext,
        const uint32_t nCapabilities,
        t3dRdForwardShaderVariant_t &rVariant );

#endif // TERRAIN3D_RD_FORWARD_VARIANT_H
