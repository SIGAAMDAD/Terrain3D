// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_SHADER_SPECIALIZER_H
#define TERRAIN3D_SHADER_SPECIALIZER_H

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

typedef struct t3dshaderStaticState_s {
	int terrainSamplingMode = 0;
	int terrainNormalMode = 0;
	int colorSamplingMode = 0;
	int projectionMode = 0;
	int textureSurfaceMode = 0;
	int terrainShadingMode = 0;
	int detilingMode = 0;
	int worldNoiseShaderMode = 0;
	bool bSampleSurfaceData = false;
	bool bOutputRoughness = false;
	bool bOutputSpecular = false;
	bool bOutputNormalMap = false;
	bool bOutputAo = false;
	bool bFastBlendCurve = false;
	bool bControlMapsImplicit = false;
	bool bColorMapsImplicit = false;
	bool bAutoShaderEnabled = false;
} t3dshaderStaticState_t;

// Reads a root Terrain3D shader template and recursively expands every shader
// include into one deterministic source unit. Repeated include occurrences are
// preserved because a few modules are intentionally used as inline snippets.
bool terrain3dExpandShaderIncludes(
	const godot::String &rRootPath,
	godot::String &rOutputSource,
	godot::String &rFailureSummary
);

// Resolves every Terrain3D-owned compile-time branch in an already-expanded
// source unit. Renderer/platform directives and locally-derived shader macros
// remain for Godot, but no T3D_* preprocessing state may survive this pass.
bool terrain3dAssembleShaderSource(
	const godot::String &rExpandedSource,
	const t3dshaderStaticState_t &rState,
	const godot::PackedStringArray &rEnabledFeatures,
	godot::String &rOutputSource,
	godot::String &rFailureSummary
);

// Enforces the final built-in shader contract before Shader::set_code().
bool terrain3dValidateAssembledShaderSource(
	const godot::String &rSource,
	godot::String &rFailureSummary
);

#endif // TERRAIN3D_SHADER_SPECIALIZER_H
