// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_RENDER_WORLD_SHADER_BUNDLE_H
#define TERRAIN3D_RD_RENDER_WORLD_SHADER_BUNDLE_H

#include "Terrain3DConstants.h"
#include <godot_cpp/classes/rd_shader_spirv.hpp>

#include <stdint.h>

using namespace godot;

enum t3dRdRenderWorldVisibilityShader_t : uint32_t {
	T3D_RD_RW_VISIBILITY_RESET = 0u,
	T3D_RD_RW_VISIBILITY_CELL,
	T3D_RD_RW_VISIBILITY_INSTANCE,
	T3D_RD_RW_VISIBILITY_CLUSTER,
	T3D_RD_RW_VISIBILITY_ARGS,
	T3D_RD_RW_VISIBILITY_COUNT,
};

enum t3dRdRenderWorldHiZShader_t : uint32_t {
	T3D_RD_RW_HIZ_REDUCE = 0u,
	T3D_RD_RW_HIZ_CLUSTER,
	T3D_RD_RW_HIZ_COUNT,
};

enum t3dRdRenderWorldVTShader_t : uint32_t {
	T3D_RD_RW_VT_RESET = 0u,
	T3D_RD_RW_VT_PREDICT,
	T3D_RD_RW_VT_COMPACT,
	T3D_RD_RW_VT_COUNT,
};

enum t3dRdRenderWorldAuxShader_t : uint32_t {
	T3D_RD_RW_AUX_SHADOW = 0u,
	T3D_RD_RW_AUX_MOTION,
	T3D_RD_RW_AUX_GENERIC,
	T3D_RD_RW_AUX_COUNT,
};


typedef struct t3dRdRenderWorldShaderBundle_s {
	Ref<RDShaderSPIRV> m_szVisibility[T3D_RD_RW_VISIBILITY_COUNT];
	Ref<RDShaderSPIRV> m_szHiZ[T3D_RD_RW_HIZ_COUNT];
	Ref<RDShaderSPIRV> m_szVT[T3D_RD_RW_VT_COUNT];
	Ref<RDShaderSPIRV> m_szRaster[T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT];
	Ref<RDShaderSPIRV> m_szAux[T3D_RD_RW_AUX_COUNT];
} t3dRdRenderWorldShaderBundle_t;

#endif // TERRAIN3D_RD_RENDER_WORLD_SHADER_BUNDLE_H
