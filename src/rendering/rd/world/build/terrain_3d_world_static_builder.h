// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_STATIC_BUILDER_H
#define TERRAIN3D_WORLD_STATIC_BUILDER_H

#include "Terrain3DTypes.h"

#include "rendering/rd/world/geometry/terrain_3d_world_geometry.h"
#include "rendering/rd/world/runtime/terrain_3d_world_publication.h"

class Terrain3DWorldStaticBuilder
{
public:
	static bool buildCold( const t3dWorldStaticBuildSource_t &rSource, Terrain3DWorldPublication &rTarget );
};

#endif // TERRAIN3D_WORLD_STATIC_BUILDER_H
