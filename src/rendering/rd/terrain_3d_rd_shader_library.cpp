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
	if ( !rVersion.is_empty() ) {
		const TypedArray<StringName> versions = shader_file->get_version_list();
		bool found = false;
		for ( int64_t i = 0; i < versions.size(); ++i ) {
			if ( StringName( versions[i] ) == rVersion ) {
				found = true;
				break;
			}
		}
		if ( !found ) {
			m_lastError = "RD shader is missing required version: " + rPath + " version=" + String( rVersion );
			return Ref<RDShaderSPIRV>();
		}
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
	for ( size_t iStage = 0u; iStage < T3D_ARRAY_COUNT( stages ); iStage++ ) {
		const RenderingDevice::ShaderStage stage = ( stages )[iStage];
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
		"color_native",
		"color_specular",
		"color_motion",
		"color_specular_motion",
		"color_native_radiance_array",
		"color_specular_radiance_array",
		"color_motion_radiance_array",
		"color_specular_motion_radiance_array",
		"depth",
		"depth_normal_roughness",
		"depth_normal_roughness_voxel_gi",
	};
	static const char *const terrain_paths[3] = {
		"res://addons/terrain_3d/rd/terrain_clipmap_near.glsl",
		"res://addons/terrain_3d/rd/terrain_clipmap_middle.glsl",
		"res://addons/terrain_3d/rd/terrain_clipmap_far.glsl",
	};
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			m_forward.m_szTerrain[tier][variant] = loadShader( terrain_paths[tier], StringName( version_names[variant] ) );
			if ( m_forward.m_szTerrain[tier][variant].is_null() ) {
				return false;
			}
		}
	}
	m_near = loadShader( terrain_paths[0], StringName( "color" ) );
	m_middle = loadShader( terrain_paths[1], StringName( "color" ) );
	m_far = loadShader( terrain_paths[2], StringName( "color" ) );
	if ( m_near.is_null() || m_middle.is_null() || m_far.is_null() ) {
		return false;
	}
	m_cull = loadShader( "res://addons/terrain_3d/rd/terrain_cull.glsl" );
	if ( m_cull.is_null() ) {
		return false;
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_forward.m_szInstances[variant] =
		        loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( version_names[variant] ) );
		if ( m_forward.m_szInstances[variant].is_null() ) {
			return false;
		}
	}
	m_instances = loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( "color" ) );
	if ( m_instances.is_null() ) {
		return false;
	}
	m_hlodCull = loadShader( "res://addons/terrain_3d/rd/terrain_hlod_cull.glsl" );
	if ( m_hlodCull.is_null() ) {
		return false;
	}
	m_oceanColor = loadShader( "res://addons/terrain_3d/rd/terrain_ocean.glsl", StringName( "color_native" ) );
	m_oceanShadow = loadShader( "res://addons/terrain_3d/rd/terrain_ocean.glsl", StringName( "shadow" ) );
	if ( m_oceanColor.is_null() || m_oceanShadow.is_null() ) {
		return false;
	}

	static const char *const render_world_visibility_paths[T3D_RD_RW_VISIBILITY_COUNT] = {
		"res://addons/terrain_3d/rd/render_world_visibility_reset.glsl",
		"res://addons/terrain_3d/rd/render_world_visibility_cell.glsl",
		"res://addons/terrain_3d/rd/render_world_visibility_instance.glsl",
		"res://addons/terrain_3d/rd/render_world_visibility_cluster.glsl",
		"res://addons/terrain_3d/rd/render_world_visibility_args.glsl",
	};
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_VISIBILITY_COUNT; ++shader ) {
		m_renderWorld.m_szVisibility[shader] = loadShader( render_world_visibility_paths[shader] );
		if ( m_renderWorld.m_szVisibility[shader].is_null() ) {
			return false;
		}
	}

	static const char *const render_world_hiz_paths[T3D_RD_RW_HIZ_COUNT] = {
		"res://addons/terrain_3d/rd/render_world_hiz_reduce.glsl",
		"res://addons/terrain_3d/rd/render_world_hiz_cluster.glsl",
	};
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_HIZ_COUNT; ++shader ) {
		m_renderWorld.m_szHiZ[shader] = loadShader( render_world_hiz_paths[shader] );
		if ( m_renderWorld.m_szHiZ[shader].is_null() ) {
			return false;
		}
	}

	static const char *const render_world_vt_paths[T3D_RD_RW_VT_COUNT] = {
		"res://addons/terrain_3d/rd/render_world_vt_reset.glsl",
		"res://addons/terrain_3d/rd/render_world_vt_predict.glsl",
		"res://addons/terrain_3d/rd/render_world_vt_compact.glsl",
	};
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_VT_COUNT; ++shader ) {
		m_renderWorld.m_szVT[shader] = loadShader( render_world_vt_paths[shader] );
		if ( m_renderWorld.m_szVT[shader].is_null() ) {
			return false;
		}
	}

	for ( uint32_t variant = 0u; variant < T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
		m_renderWorld.m_szRaster[variant] =
		        loadShader( "res://addons/terrain_3d/rd/render_world_raster.glsl", StringName( version_names[variant] ) );
		if ( m_renderWorld.m_szRaster[variant].is_null() ) {
			return false;
		}
	}

	static const char *const render_world_aux_versions[T3D_RD_RW_AUX_COUNT] = { "shadow", "motion", "aux" };
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_AUX_COUNT; ++shader ) {
		m_renderWorld.m_szAux[shader] =
		        loadShader( "res://addons/terrain_3d/rd/render_world_aux.glsl", StringName( render_world_aux_versions[shader] ) );
		if ( m_renderWorld.m_szAux[shader].is_null() ) {
			return false;
		}
	}

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
	m_oceanColor.unref();
	m_oceanShadow.unref();
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			m_forward.m_szTerrain[tier][variant].unref();
		}
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_forward.m_szInstances[variant].unref();
	}
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_VISIBILITY_COUNT; ++shader ) {
		m_renderWorld.m_szVisibility[shader].unref();
	}
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_HIZ_COUNT; ++shader ) {
		m_renderWorld.m_szHiZ[shader].unref();
	}
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_VT_COUNT; ++shader ) {
		m_renderWorld.m_szVT[shader].unref();
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
		m_renderWorld.m_szRaster[variant].unref();
	}
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_AUX_COUNT; ++shader ) {
		m_renderWorld.m_szAux[shader].unref();
	}
}

Dictionary Terrain3DRdShaderLibrary::getDiagnostics( void ) const {
	Dictionary result;
	result["near_loaded"] = m_near.is_valid();
	result["middle_loaded"] = m_middle.is_valid();
	result["far_loaded"] = m_far.is_valid();
	result["cull_loaded"] = m_cull.is_valid();
	result["instances_loaded"] = m_instances.is_valid();
	result["hlod_cull_loaded"] = m_hlodCull.is_valid();
	result["ocean_color_loaded"] = m_oceanColor.is_valid();
	result["ocean_shadow_loaded"] = m_oceanShadow.is_valid();
	uint32_t render_world_loaded = 0u;
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_VISIBILITY_COUNT; ++shader ) {
		render_world_loaded += m_renderWorld.m_szVisibility[shader].is_valid() ? 1u : 0u;
	}
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_HIZ_COUNT; ++shader ) {
		render_world_loaded += m_renderWorld.m_szHiZ[shader].is_valid() ? 1u : 0u;
	}
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_VT_COUNT; ++shader ) {
		render_world_loaded += m_renderWorld.m_szVT[shader].is_valid() ? 1u : 0u;
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
		render_world_loaded += m_renderWorld.m_szRaster[variant].is_valid() ? 1u : 0u;
	}
	for ( uint32_t shader = 0u; shader < T3D_RD_RW_AUX_COUNT; ++shader ) {
		render_world_loaded += m_renderWorld.m_szAux[shader].is_valid() ? 1u : 0u;
	}
	result["render_world_loaded"] = ( int64_t )( render_world_loaded );
	result["render_world_expected"] = ( int64_t )( T3D_RD_RW_VISIBILITY_COUNT + T3D_RD_RW_HIZ_COUNT + T3D_RD_RW_VT_COUNT
	        + T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT + T3D_RD_RW_AUX_COUNT );
	result["last_error"] = m_lastError;
	return result;
}
