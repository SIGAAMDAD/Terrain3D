// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_LIGHTING_POLICY_H
#define TERRAIN3D_LIGHTING_POLICY_H

#include "Terrain3DConstants.h"

#include <cstdint>

#include "data/terrain_3d_region.h"

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
			return s_nLightingLayerFoliage;
		case LIGHTING_CLASS_LARGE_OCCLUDER:
			return s_nLightingLayerLarge;
		case LIGHTING_CLASS_REGIONAL_OCCLUDER:
			return s_nLightingLayerRegional;
		case LIGHTING_CLASS_MEDIUM_OBSTACLE:
		default:
			return s_nLightingLayerMedium;
	}
}

#endif // TERRAIN3D_LIGHTING_POLICY_H
