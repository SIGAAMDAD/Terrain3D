// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_FORWARD_SHADER_BUNDLE_H
#define TERRAIN3D_RD_FORWARD_SHADER_BUNDLE_H

#include "rendering/rd/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>

using namespace godot;

typedef struct t3dRdForwardShaderBundle_s {
	Ref<RDShaderSPIRV> m_szTerrain[3][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	Ref<RDShaderSPIRV> m_szInstances[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
} t3dRdForwardShaderBundle_t;

#endif // TERRAIN3D_RD_FORWARD_SHADER_BUNDLE_H
