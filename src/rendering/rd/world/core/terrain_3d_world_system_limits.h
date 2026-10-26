// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_SYSTEM_LIMITS_H
#define TERRAIN3D_WORLD_SYSTEM_LIMITS_H

#include "Terrain3DTypes.h"

#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <stdint.h>

t3dWorldLimits_t t3d_world_make_prototype_limits();
t3dWorldLimits_t t3d_world_make_runtime_limits( const t3dWorldResourceLimits_t &rWorldLimits );
t3dWorldLimits_t t3d_world_make_preview_limits( uint32_t nMaterialCapacity, uint32_t nVirtualPageCapacity );
bool			 t3d_world_validate_limits( const t3dWorldLimits_t &pLimits );
uint64_t		 t3d_world_limits_signature( const t3dWorldLimits_t &pLimits );

#endif // TERRAIN3D_WORLD_SYSTEM_LIMITS_H
