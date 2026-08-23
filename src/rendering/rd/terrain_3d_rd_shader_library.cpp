// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_shader_library.h"

#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

Ref<RDShaderSPIRV> Terrain3DRdShaderLibrary::loadShader( const String &rPath ) {
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
	Ref<RDShaderSPIRV> spirv = shader_file->get_spirv();
	if ( spirv.is_null() ) {
		m_lastError = "RD shader has no SPIR-V: " + rPath;
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
	m_near = loadShader( "res://addons/terrain_3d/rd/terrain_clipmap_near.glsl" );
	if ( m_near.is_null() ) return false;
	m_middle = loadShader( "res://addons/terrain_3d/rd/terrain_clipmap_middle.glsl" );
	if ( m_middle.is_null() ) return false;
	m_far = loadShader( "res://addons/terrain_3d/rd/terrain_clipmap_far.glsl" );
	if ( m_far.is_null() ) return false;
	m_cull = loadShader( "res://addons/terrain_3d/rd/terrain_cull.glsl" );
	if ( m_cull.is_null() ) return false;
	m_instances = loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl" );
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
