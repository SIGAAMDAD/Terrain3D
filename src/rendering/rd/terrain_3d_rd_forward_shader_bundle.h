// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_FORWARD_SHADER_BUNDLE_H
#define TERRAIN3D_RD_FORWARD_SHADER_BUNDLE_H

#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/terrain_3d_shader_specializer.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>

using namespace godot;

// WG-C: source SPIR-V remains shared across the eight shipping feature classes.
// The specialization dimension is applied at pipeline creation, not stored as
// duplicated RDShaderFile versions in this bundle.
// WG-G extends that same shared-SPIR-V pipeline specialization contract with
// bounded lighting-model and scene-integration constants.
static const uint32_t T3D_RD_TERRAIN_FORWARD_SOURCE_VARIANT_COUNT =
        T3D_RD_FORWARD_SHADER_VARIANT_MAX + T3D_RD_FORWARD_COLOR_VARIANT_COUNT;

typedef struct t3dRdForwardShaderBundle_s {
	Ref<RDShaderSPIRV> m_szTerrain[T3D_TERRAIN_SHADER_PATH_COUNT][3][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	// GPU-P06: terrain-only color family. The shared instance/World
	// variant ABI remains unchanged; depth/prepass variants stay common.
	Ref<RDShaderSPIRV> m_szTerrainOneSun[T3D_TERRAIN_SHADER_PATH_COUNT][3][T3D_RD_FORWARD_COLOR_VARIANT_COUNT];
	Ref<RDShaderSPIRV> m_szInstances[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
} t3dRdForwardShaderBundle_t;

#endif // TERRAIN3D_RD_FORWARD_SHADER_BUNDLE_H
