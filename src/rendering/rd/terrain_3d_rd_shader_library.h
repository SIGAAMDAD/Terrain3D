// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_SHADER_LIBRARY_H
#define TERRAIN3D_RD_SHADER_LIBRARY_H
#include "core/terrain_3d_compiler.h"
#include "rendering/rd/terrain_3d_rd_forward_shader_bundle.h"
#include "rendering/rd/terrain_3d_rd_types.h"

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
	Ref<RDShaderSPIRV> m_oceanColor;
	Ref<RDShaderSPIRV> m_oceanShadow;
	t3dRdForwardShaderBundle_t m_forward;
	String m_lastError;

	Ref<RDShaderSPIRV> loadShader( const String &rPath, const StringName &rVersion = StringName() );

public:
	bool load( void );
	void clear( void );

	EA_FORCE_INLINE bool isReady( void ) const {
		return m_near.is_valid() && m_middle.is_valid() && m_far.is_valid() && m_cull.is_valid() && m_instances.is_valid() && m_hlodCull.is_valid() && m_oceanColor.is_valid() && m_oceanShadow.is_valid();
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getNear( void ) const { return m_near; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getMiddle( void ) const { return m_middle; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getFar( void ) const { return m_far; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getCull( void ) const { return m_cull; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstances( void ) const { return m_instances; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getHlodCull( void ) const { return m_hlodCull; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getOceanColor( void ) const { return m_oceanColor; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getOceanShadow( void ) const { return m_oceanShadow; }
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getTerrainForward( const uint32_t nTier, const t3dRdForwardShaderVariant_t variant ) const {
		return nTier < 3u && variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX ? m_forward.m_szTerrain[nTier][variant] : Ref<RDShaderSPIRV>();
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstanceForward( const t3dRdForwardShaderVariant_t variant ) const {
		return variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX ? m_forward.m_szInstances[variant] : Ref<RDShaderSPIRV>();
	}
	EA_FORCE_INLINE const t3dRdForwardShaderBundle_t &getForwardBundle( void ) const { return m_forward; }
	EA_FORCE_INLINE String getLastError( void ) const { return m_lastError; }
	Dictionary getDiagnostics( void ) const;
};

#endif // TERRAIN3D_RD_SHADER_LIBRARY_H
