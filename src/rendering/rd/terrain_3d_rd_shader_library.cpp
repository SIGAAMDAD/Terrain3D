// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_shader_library.h"

#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

Ref<RDShaderSPIRV> Terrain3DRdShaderLibrary::loadShader( const String &rPath, const StringName &rVersion ) {
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		m_lastError = "ResourceLoader is unavailable while loading " + rPath;
		return Ref<RDShaderSPIRV>();
	}
	Ref<RDShaderFile> shader_file = loader->load( rPath );
	if ( shader_file.is_null() ) {
		m_lastError = "Failed to load RD shader file: " + rPath;
		return Ref<RDShaderSPIRV>();
	}
	Ref<RDShaderSPIRV> spirv = shader_file->get_spirv( rVersion );
	if ( spirv.is_null() ) {
		m_lastError = "RD shader has no SPIR-V: " + rPath + ( rVersion.is_empty() ? String() : String( " version=" ) + String( rVersion ) );
		return Ref<RDShaderSPIRV>();
	}
	const RenderingDevice::ShaderStage stages[] = {
		RenderingDevice::SHADER_STAGE_VERTEX,
		RenderingDevice::SHADER_STAGE_FRAGMENT,
		RenderingDevice::SHADER_STAGE_COMPUTE,
	};
	for ( const RenderingDevice::ShaderStage stage : stages ) {
		const String compile_error = spirv->get_stage_compile_error( stage );
		if ( !compile_error.is_empty() ) {
			m_lastError = "RD shader compile failed: " + rPath + " :: " + compile_error;
			return Ref<RDShaderSPIRV>();
		}
	}
	return spirv;
}

bool Terrain3DRdShaderLibrary::load( void ) {
	clear();
	static const char *const version_names[T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
		"color_native", "color_specular", "color_motion", "color_specular_motion",
		"depth", "depth_normal_roughness", "depth_normal_roughness_voxel_gi",
	};
	static const char *const terrain_paths[3] = {
		"res://addons/terrain_3d/rd/terrain_clipmap_near.glsl",
		"res://addons/terrain_3d/rd/terrain_clipmap_middle.glsl",
		"res://addons/terrain_3d/rd/terrain_clipmap_far.glsl",
	};
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			m_forward.m_szTerrain[tier][variant] = loadShader( terrain_paths[tier], StringName( version_names[variant] ) );
			if ( m_forward.m_szTerrain[tier][variant].is_null() ) return false;
		}
	}
	m_near = loadShader( terrain_paths[0], StringName( "color" ) );
	m_middle = loadShader( terrain_paths[1], StringName( "color" ) );
	m_far = loadShader( terrain_paths[2], StringName( "color" ) );
	if ( m_near.is_null() || m_middle.is_null() || m_far.is_null() ) return false;
	m_cull = loadShader( "res://addons/terrain_3d/rd/terrain_cull.glsl" );
	if ( m_cull.is_null() ) return false;
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_forward.m_szInstances[variant] = loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( version_names[variant] ) );
		if ( m_forward.m_szInstances[variant].is_null() ) return false;
	}
	m_instances = loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( "color" ) );
	if ( m_instances.is_null() ) return false;
	m_hlodCull = loadShader( "res://addons/terrain_3d/rd/terrain_hlod_cull.glsl" );
	if ( m_hlodCull.is_null() ) return false;
	m_lastError = String();
	return true;
}

void Terrain3DRdShaderLibrary::clear( void ) {
	m_near.unref();
	m_middle.unref();
	m_far.unref();
	m_cull.unref();
	m_instances.unref();
	m_hlodCull.unref();
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) m_forward.m_szTerrain[tier][variant].unref();
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) m_forward.m_szInstances[variant].unref();
}

Dictionary Terrain3DRdShaderLibrary::getDiagnostics( void ) const {
	Dictionary result;
	result["near_loaded"] = m_near.is_valid();
	result["middle_loaded"] = m_middle.is_valid();
	result["far_loaded"] = m_far.is_valid();
	result["cull_loaded"] = m_cull.is_valid();
	result["instances_loaded"] = m_instances.is_valid();
	result["hlod_cull_loaded"] = m_hlodCull.is_valid();
	result["last_error"] = m_lastError;
	return result;
}
