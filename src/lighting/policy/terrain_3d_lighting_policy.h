// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_LIGHTING_POLICY_H
#define TERRAIN3D_LIGHTING_POLICY_H

#include "Terrain3DConstants.h"
#include "data/model/terrain_3d_region.h"

#include <stdint.h>

// Godot exposes 20 VisualInstance3D render layers. Terrain3D reserves the
// highest five for environment-lighting policy. Four encode dynamic-shadow
// priority; the final bit tags streamed geometry that explicitly receives the
// managed directional light. Terrain does not need a shadow layer because the
// semantic policy intentionally excludes Terrain from dynamic casting.

inline uint32_t t3dLightingLayerForClass( const int nClass ) {
	switch ( nClass ) {
		case LIGHTING_CLASS_TERRAIN:
			return 0u;
		case LIGHTING_CLASS_FOLIAGE_CLUTTER:
			return T3D_LIGHTING_LAYER_FOLIAGE;
		case LIGHTING_CLASS_LARGE_OCCLUDER:
			return T3D_LIGHTING_LAYER_LARGE;
		case LIGHTING_CLASS_REGIONAL_OCCLUDER:
			return T3D_LIGHTING_LAYER_REGIONAL;
		case LIGHTING_CLASS_MEDIUM_OBSTACLE:
		default:
			return T3D_LIGHTING_LAYER_MEDIUM;
	}
}

#endif // TERRAIN3D_LIGHTING_POLICY_H
