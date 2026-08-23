// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_SHADER_LIBRARY_H
#define TERRAIN3D_RD_SHADER_LIBRARY_H
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

class Terrain3DRdShaderLibrary {
private:
	Ref<RDShaderSPIRV> m_near;
	Ref<RDShaderSPIRV> m_middle;
	Ref<RDShaderSPIRV> m_far;
	Ref<RDShaderSPIRV> m_cull;
	Ref<RDShaderSPIRV> m_instances;
	Ref<RDShaderSPIRV> m_hlodCull;
	String m_lastError;

	Ref<RDShaderSPIRV> loadShader( const String &rPath );

public:
	bool load( void );
	void clear( void );

	EA_FORCE_INLINE bool isReady( void ) const {
		return m_near.is_valid() && m_middle.is_valid() && m_far.is_valid() && m_cull.is_valid() && m_instances.is_valid() && m_hlodCull.is_valid();
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getNear( void ) const { return m_near; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getMiddle( void ) const { return m_middle; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getFar( void ) const { return m_far; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getCull( void ) const { return m_cull; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstances( void ) const { return m_instances; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getHlodCull( void ) const { return m_hlodCull; }
	EA_FORCE_INLINE String getLastError( void ) const { return m_lastError; }
	Dictionary getDiagnostics( void ) const;
};

#endif // TERRAIN3D_RD_SHADER_LIBRARY_H
