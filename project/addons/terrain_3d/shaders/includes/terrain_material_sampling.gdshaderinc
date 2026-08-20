// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
// ShaderInclude resources are parsed independently by the editor. Runtime-generated
// shaders inject the selected values before these guards, so local defaults only
// apply when this module is parsed without its Terrain3D parent shader.
#ifndef T3D_TERRAIN_SAMPLING_MODE
#define T3D_TERRAIN_SAMPLING_MODE 0
#endif
// T3D_STANDALONE_DEFAULTS_END


#ifdef T3D_ENABLE_TERRAIN_MATERIAL_SAMPLING
#if T3D_TERRAIN_SAMPLING_MODE == 0
#include "res://addons/terrain_3d/shaders/includes/near/material_sampling/adaptive.gdshaderinc"
#elif T3D_TERRAIN_SAMPLING_MODE == 1
#include "res://addons/terrain_3d/shaders/includes/near/material_sampling/cinematic.gdshaderinc"
#elif T3D_TERRAIN_SAMPLING_MODE == 3
#include "res://addons/terrain_3d/shaders/includes/near/material_sampling/weighted.gdshaderinc"
#elif T3D_TERRAIN_SAMPLING_MODE == 4
#include "res://addons/terrain_3d/shaders/includes/near/material_sampling/distance_adaptive.gdshaderinc"
#elif T3D_TERRAIN_SAMPLING_MODE == 5
#include "res://addons/terrain_3d/shaders/includes/near/material_sampling/two_corner.gdshaderinc"
#else
#include "res://addons/terrain_3d/shaders/includes/near/material_sampling/single.gdshaderinc"
#endif
#endif
