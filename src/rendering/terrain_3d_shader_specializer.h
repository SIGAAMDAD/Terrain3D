// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_SHADER_SPECIALIZER_H
#define TERRAIN3D_SHADER_SPECIALIZER_H

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

typedef struct t3dShaderStaticState_s {
	int m_nTerrainSamplingMode = 0;
	int m_nTerrainNormalMode = 0;
	int m_nColorSamplingMode = 0;
	int m_nProjectionMode = 0;
	int m_nTextureSurfaceMode = 0;
	int m_nTerrainShadingMode = 0;
	int m_nDetilingMode = 0;
	int m_nWorldNoiseShaderMode = 0;
	bool m_bSampleSurfaceData = false;
	bool m_bOutputRoughness = false;
	bool m_bOutputSpecular = false;
	bool m_bOutputNormalMap = false;
	bool m_bOutputAo = false;
	bool m_bFastBlendCurve = false;
	bool m_bControlMapsImplicit = false;
	bool m_bColorMapsImplicit = false;
	bool m_bAutoShaderEnabled = false;
} t3dShaderStaticState_t;

// Reads a root Terrain3D shader template and recursively expands every shader
// include into one deterministic source unit. Repeated include occurrences are
// preserved because a few modules are intentionally used as inline snippets.
bool t3dExpandShaderIncludes(
	const godot::String &rRootPath,
	godot::String &rOutputSource,
	godot::String &rFailureSummary
);

// Resolves every Terrain3D-owned compile-time branch in an already-expanded
// source unit. Renderer/platform directives and locally-derived shader macros
// remain for Godot, but no T3D_* preprocessing state may survive this pass.
bool t3dAssembleShaderSource(
	const godot::String &rExpandedSource,
	const t3dShaderStaticState_t &rState,
	const godot::PackedStringArray &rEnabledFeatures,
	godot::String &rOutputSource,
	godot::String &rFailureSummary
);

// Enforces the final built-in shader contract before Shader::set_code().
bool t3dValidateAssembledShaderSource(
	const godot::String &rSource,
	godot::String &rFailureSummary
);

#endif // TERRAIN3D_SHADER_SPECIALIZER_H
