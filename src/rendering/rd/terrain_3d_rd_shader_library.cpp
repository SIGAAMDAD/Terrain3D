// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_shader_library.h"
#include "instances/terrain_3d_instancer_bringup.h"

#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

typedef enum t3dRdShaderSourceStage_e : uint8_t {
	T3D_RD_SHADER_SOURCE_IDLE = 0u,
	T3D_RD_SHADER_SOURCE_REQUESTED,
	T3D_RD_SHADER_SOURCE_READY,
	T3D_RD_SHADER_SOURCE_FAILED,
} t3dRdShaderSourceStage_t;

static const char *const g_t3dRdShaderSourcePaths[] = {
	"res://addons/terrain_3d/rd/terrain_clipmap_near.glsl",
	"res://addons/terrain_3d/rd/terrain_clipmap_middle.glsl",
	"res://addons/terrain_3d/rd/terrain_clipmap_far.glsl",
	"res://addons/terrain_3d/rd/terrain_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_instances.glsl",
	"res://addons/terrain_3d/rd/terrain_instance_occluder_depth.glsl",
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	"res://addons/terrain_3d/rd/terrain_hlod_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_instance_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_instance_hiz_reduce.glsl",
#endif
	"res://addons/terrain_3d/rd/terrain_world_hiz_reduce.glsl",
	"res://addons/terrain_3d/rd/terrain_world_visibility.glsl",
	"res://addons/terrain_3d/rd/terrain_world_raster.glsl",
	"res://addons/terrain_3d/rd/terrain_ocean.glsl",
};

static uint32_t t3dRdShaderSourceCount( void ) {
	return ( uint32_t )T3D_ARRAY_COUNT( g_t3dRdShaderSourcePaths );
}

static uint64_t t3dRdShaderTicksMsec( void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_msec() : 0u;
}

static uint64_t t3dRdShaderTicksUsec( void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_usec() : 0u;
}

static uint32_t t3dRdExpectedShaderLoadCount( void ) {
	uint32_t nCount = 0u;
	nCount += 3u * T3D_TERRAIN_SHADER_PATH_COUNT * T3D_RD_FORWARD_SHADER_VARIANT_MAX;
	nCount += 3u * T3D_TERRAIN_SHADER_PATH_COUNT * T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
	nCount += 3u; // Near/middle/far compatibility aliases.
	nCount += 1u; // Terrain cull.
	nCount += T3D_RD_FORWARD_SHADER_VARIANT_MAX;
	nCount += 1u; // Instance color/bringup alias.
	nCount += 1u; // Instance occluder depth.
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	nCount += 3u; // HLOD cull, instance cull, instance Hi-Z reduce.
#endif
	nCount += 1u; // Unified-world Hi-Z reduce.
	nCount += 3u; // Unified-world visibility reset/cull/finalize.
	nCount += 2u * T3D_RD_FORWARD_SHADER_VARIANT_MAX;
	nCount += 2u; // Unified-world terrain/mesh shadow.
	nCount += 2u; // Ocean color/shadow.
	return nCount;
}

static String t3dRdShaderStatusName( const String &rPath, const StringName &rVersion ) {
	return rVersion.is_empty() ? rPath : rPath + String( " version=" ) + String( rVersion );
}

void Terrain3DRdShaderLibrary::logShaderFailure( const String &rPath, const StringName &rVersion, const uint64_t nStartedMsec ) {
	m_nShaderLoadFailureCount++;
	const uint64_t nNowMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nNowMsec >= nStartedMsec ? nNowMsec - nStartedMsec : 0u;
	UtilityFunctions::push_error( "[Terrain3D][ShaderLoad] FAIL [", m_nShaderLoadAttemptCount, "/", m_nShaderLoadExpectedCount,
	        "] ", t3dRdShaderStatusName( rPath, rVersion ), " elapsed=", nElapsedMsec, " ms :: ", m_lastError );
}

void Terrain3DRdShaderLibrary::logShaderReady( const String &rPath, const StringName &rVersion, const uint64_t nStartedMsec ) {
	m_nShaderLoadSuccessCount++;
	t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadSuccessCount );
	const uint64_t nNowMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nNowMsec >= nStartedMsec ? nNowMsec - nStartedMsec : 0u;
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] READY [", m_nShaderLoadAttemptCount, "/", m_nShaderLoadExpectedCount,
	        "] ", t3dRdShaderStatusName( rPath, rVersion ), " elapsed=", nElapsedMsec, " ms" );
}

bool Terrain3DRdShaderLibrary::abortShaderLoad( void ) {
	const uint64_t nNowMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nNowMsec >= m_nShaderLoadStartMsec ? nNowMsec - m_nShaderLoadStartMsec : 0u;
	const uint64_t nNowUsec = t3dRdShaderTicksUsec();
	m_nShaderLoadElapsedUsec = nNowUsec >= m_nShaderLoadStartUsec ? nNowUsec - m_nShaderLoadStartUsec : nElapsedMsec * 1000u;
	t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadSuccessCount );
	t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, nNowUsec );
	UtilityFunctions::push_error( "[Terrain3D][ShaderLoad] ABORT ready=", m_nShaderLoadSuccessCount, "/", m_nShaderLoadExpectedCount,
	        " attempted=", m_nShaderLoadAttemptCount, " failed=", m_nShaderLoadFailureCount, " generation=", m_nStartupGeneration,
	        " elapsed=", nElapsedMsec,
	        " ms :: ", m_lastError );
	return false;
}


void Terrain3DRdShaderLibrary::markStartupGpuPending( const uint32_t nGeneration ) {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nGeneration != m_nStartupGeneration || startup.m_nGeneration != nGeneration ) {
		return;
	}
	const t3dStartupStage_t eStage = startup.m_eStage;
	if ( eStage == T3D_STARTUP_CPU_READY || eStage == T3D_STARTUP_GPU_PENDING ) {
		t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_GPU_PENDING );
	}
}

void Terrain3DRdShaderLibrary::markStartupGpuReady( const uint32_t nGeneration ) {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nGeneration != m_nStartupGeneration || startup.m_nGeneration != nGeneration ) {
		return;
	}
	const t3dStartupStage_t eStage = startup.m_eStage;
	if ( eStage == T3D_STARTUP_CPU_READY || eStage == T3D_STARTUP_GPU_PENDING || eStage == T3D_STARTUP_GPU_READY ) {
		if ( eStage != T3D_STARTUP_GPU_READY ) {
			UtilityFunctions::print( "[Terrain3D][Startup][Shader] GPU READY generation=", nGeneration );
		}
		t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_GPU_READY );
	}
}

void Terrain3DRdShaderLibrary::markStartupCommitted( const uint32_t nGeneration ) {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nGeneration != m_nStartupGeneration || startup.m_nGeneration != nGeneration ) {
		return;
	}
	const t3dStartupStage_t eStage = startup.m_eStage;
	if ( eStage == T3D_STARTUP_GPU_READY || eStage == T3D_STARTUP_COMMITTED ) {
		t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadExpectedCount );
		t3dStartupDomainFinish( m_startupState, T3D_STARTUP_COMMITTED, t3dRdShaderTicksUsec() );
	}
}

Ref<RDShaderFile> Terrain3DRdShaderLibrary::findSourceFile( const String &rPath ) const {
	for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
		if ( rPath == String( g_t3dRdShaderSourcePaths[iSource] ) ) {
			return m_szSourceFiles[iSource];
		}
	}
	return Ref<RDShaderFile>();
}

Ref<RDShaderSPIRV> Terrain3DRdShaderLibrary::loadShader( const String &rPath, const StringName &rVersion ) {
	m_nShaderLoadAttemptCount++;
	const uint64_t nStartedMsec = t3dRdShaderTicksMsec();
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] RESOLVE [", m_nShaderLoadAttemptCount, "/", m_nShaderLoadExpectedCount,
	        "] ", t3dRdShaderStatusName( rPath, rVersion ) );
	const Ref<RDShaderFile> shader_file = findSourceFile( rPath );
	if ( shader_file.is_null() ) {
		m_lastError = "Threaded RD shader source is unavailable: " + rPath;
		logShaderFailure( rPath, rVersion, nStartedMsec );
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
			logShaderFailure( rPath, rVersion, nStartedMsec );
			return Ref<RDShaderSPIRV>();
		}
	}
	Ref<RDShaderSPIRV> spirv = shader_file->get_spirv( rVersion );
	if ( spirv.is_null() ) {
		m_lastError = "RD shader has no SPIR-V: " + rPath + ( rVersion.is_empty() ? String() : String( " version=" ) + String( rVersion ) );
		logShaderFailure( rPath, rVersion, nStartedMsec );
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
			logShaderFailure( rPath, rVersion, nStartedMsec );
			return Ref<RDShaderSPIRV>();
		}
	}
	logShaderReady( rPath, rVersion, nStartedMsec );
	return spirv;
}


bool Terrain3DRdShaderLibrary::beginLoadCold( const uint32_t nStartupGeneration ) {
	clear();
	m_nStartupGeneration = nStartupGeneration;
	m_nShaderLoadAttemptCount = 0u;
	m_nShaderLoadSuccessCount = 0u;
	m_nShaderLoadFailureCount = 0u;
	m_nShaderLoadExpectedCount = t3dRdExpectedShaderLoadCount();
	m_nShaderLoadStartMsec = t3dRdShaderTicksMsec();
	m_nShaderLoadStartUsec = t3dRdShaderTicksUsec();
	m_nShaderLoadElapsedUsec = 0u;
	m_nShaderSourceCount = t3dRdShaderSourceCount();
	m_nShaderSourceRequestCount = 0u;
	m_nShaderSourceReadyCount = 0u;
	m_nShaderSourceFailureCount = 0u;
	m_bLoadRequested = true;
	m_bLoadCancelled = false;
	if ( m_nShaderSourceCount > s_nShaderSourceCapacity ) {
		m_lastError = "RD shader source table exceeds fixed startup capacity";
		return abortShaderLoad();
	}
	t3dStartupDomainRequest( m_startupState, m_nStartupGeneration, m_nShaderLoadStartUsec, m_nShaderLoadExpectedCount );
	t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_PREPARING );
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( pLoader == NULL ) {
		m_lastError = "ResourceLoader is unavailable while requesting RD shader sources";
		return abortShaderLoad();
	}
	for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
		const String path = String( g_t3dRdShaderSourcePaths[iSource] );
		const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE || status == ResourceLoader::THREAD_LOAD_FAILED ) {
			const Error error = pLoader->load_threaded_request( path, "RDShaderFile", true, ResourceLoader::CACHE_MODE_REUSE );
			if ( error != OK ) {
				m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_FAILED;
				++m_nShaderSourceFailureCount;
				m_lastError = "Failed requesting threaded RD shader source: " + path + " error=" + String::num_int64( error );
				return abortShaderLoad();
			}
			++m_nShaderSourceRequestCount;
		}
		m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_REQUESTED;
	}
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] BEGIN generation=", m_nStartupGeneration, " unique_sources=", m_nShaderSourceCount,
	        " expected_variants=", m_nShaderLoadExpectedCount );
	UtilityFunctions::print( "[Terrain3D][Startup][Shader] REQUESTED generation=", m_nStartupGeneration );
	return true;
}

void Terrain3DRdShaderLibrary::serviceLoadCold( void ) {
	if ( !m_bLoadRequested || m_bLoadCancelled || isCpuReady( m_nStartupGeneration ) || isFailed( m_nStartupGeneration ) ) {
		return;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( pLoader == NULL ) {
		m_lastError = "ResourceLoader became unavailable while servicing RD shader sources";
		( void )abortShaderLoad();
		return;
	}
	uint32_t nReady = 0u;
	for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
		if ( m_szSourceStages[iSource] == T3D_RD_SHADER_SOURCE_READY ) {
			++nReady;
			continue;
		}
		const String path = String( g_t3dRdShaderSourcePaths[iSource] );
		const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
			continue;
		}
		if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
			m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_FAILED;
			++m_nShaderSourceFailureCount;
			m_lastError = "Threaded RD shader source failed: " + path + " status=" + String::num_int64( status );
			( void )abortShaderLoad();
			return;
		}
		const Ref<Resource> resource = pLoader->load_threaded_get( path );
		Ref<RDShaderFile> shaderFile = resource;
		if ( shaderFile.is_null() ) {
			m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_FAILED;
			++m_nShaderSourceFailureCount;
			m_lastError = "Threaded resource is not an RDShaderFile: " + path;
			( void )abortShaderLoad();
			return;
		}
		m_szSourceFiles[iSource] = shaderFile;
		m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_READY;
		++nReady;
	}
	m_nShaderSourceReadyCount = nReady;
	if ( nReady != m_nShaderSourceCount ) {
		return;
	}
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] SOURCES READY generation=", m_nStartupGeneration, " ready=", nReady, "/", m_nShaderSourceCount );
	if ( !resolveShaderBundleCold() ) {
		return;
	}
}

bool Terrain3DRdShaderLibrary::isCpuReady( const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nStartupGeneration != m_nStartupGeneration || startup.m_nGeneration != nStartupGeneration ) {
		return false;
	}
	const t3dStartupStage_t stage = startup.m_eStage;
	return stage == T3D_STARTUP_CPU_READY || stage == T3D_STARTUP_GPU_PENDING || stage == T3D_STARTUP_GPU_READY
	        || stage == T3D_STARTUP_COMMITTED;
}

bool Terrain3DRdShaderLibrary::isGpuReady( const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nStartupGeneration != m_nStartupGeneration || startup.m_nGeneration != nStartupGeneration ) {
		return false;
	}
	const t3dStartupStage_t stage = startup.m_eStage;
	return stage == T3D_STARTUP_GPU_READY || stage == T3D_STARTUP_COMMITTED;
}

bool Terrain3DRdShaderLibrary::isFailed( const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	return nStartupGeneration == m_nStartupGeneration && startup.m_nGeneration == nStartupGeneration
	        && startup.m_eStage == T3D_STARTUP_FAILED;
}

void Terrain3DRdShaderLibrary::cancelLoadCold( const uint32_t nStartupGeneration ) {
	if ( nStartupGeneration != m_nStartupGeneration || !t3dStartupDomainMatches( m_startupState, nStartupGeneration ) ) {
		return;
	}
	const t3dStartupStage_t stage = t3dStartupDomainGetStage( m_startupState );
	if ( stage == T3D_STARTUP_FAILED || stage == T3D_STARTUP_CANCELLED ) {
		return;
	}
	m_bLoadCancelled = true;
	m_bLoadRequested = false;
	for ( uint32_t iSource = 0u; iSource < s_nShaderSourceCapacity; ++iSource ) {
		m_szSourceFiles[iSource].unref();
		if ( m_szSourceStages[iSource] != T3D_RD_SHADER_SOURCE_FAILED ) {
			m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_IDLE;
		}
	}
	( void )t3dStartupDomainCancel( m_startupState, nStartupGeneration, t3dRdShaderTicksUsec() );
}

bool Terrain3DRdShaderLibrary::resolveShaderBundleCold( void ) {
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
	static const char *const terrain_one_sun_version_names[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
		"color_native_one_sun",
		"color_specular_one_sun",
		"color_motion_one_sun",
		"color_specular_motion_one_sun",
		"color_native_one_sun_radiance_array",
		"color_specular_one_sun_radiance_array",
		"color_motion_one_sun_radiance_array",
		"color_specular_motion_one_sun_radiance_array",
	};
	static const char *const terrain_compat_version_names[T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
		"compat_color_native",
		"compat_color_specular",
		"compat_color_motion",
		"compat_color_specular_motion",
		"compat_color_native_radiance_array",
		"compat_color_specular_radiance_array",
		"compat_color_motion_radiance_array",
		"compat_color_specular_motion_radiance_array",
		"compat_depth",
		"compat_depth_normal_roughness",
		"compat_depth_normal_roughness_voxel_gi",
	};
	static const char *const terrain_editor_version_names[T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
		"editor_color_native",
		"editor_color_specular",
		"editor_color_motion",
		"editor_color_specular_motion",
		"editor_color_native_radiance_array",
		"editor_color_specular_radiance_array",
		"editor_color_motion_radiance_array",
		"editor_color_specular_motion_radiance_array",
		"editor_depth",
		"editor_depth_normal_roughness",
		"editor_depth_normal_roughness_voxel_gi",
	};
	static const char *const terrain_compat_one_sun_version_names[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
		"compat_color_native_one_sun",
		"compat_color_specular_one_sun",
		"compat_color_motion_one_sun",
		"compat_color_specular_motion_one_sun",
		"compat_color_native_one_sun_radiance_array",
		"compat_color_specular_one_sun_radiance_array",
		"compat_color_motion_one_sun_radiance_array",
		"compat_color_specular_motion_one_sun_radiance_array",
	};
	static const char *const terrain_editor_one_sun_version_names[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
		"editor_color_native_one_sun",
		"editor_color_specular_one_sun",
		"editor_color_motion_one_sun",
		"editor_color_specular_motion_one_sun",
		"editor_color_native_one_sun_radiance_array",
		"editor_color_specular_one_sun_radiance_array",
		"editor_color_motion_one_sun_radiance_array",
		"editor_color_specular_motion_one_sun_radiance_array",
	};
	for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			m_forward.m_szTerrain[T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL][tier][variant] =
			        loadShader( terrain_paths[tier], StringName( version_names[variant] ) );
			m_forward.m_szTerrain[T3D_TERRAIN_SHADER_PATH_COMPATIBILITY_MIXED][tier][variant] =
			        loadShader( terrain_paths[tier], StringName( terrain_compat_version_names[variant] ) );
			m_forward.m_szTerrain[T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED][tier][variant] =
			        loadShader( terrain_paths[tier], StringName( terrain_editor_version_names[variant] ) );
			for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
				if ( m_forward.m_szTerrain[path][tier][variant].is_null() ) {
					return abortShaderLoad();
				}
			}
		}
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
			m_forward.m_szTerrainOneSun[T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL][tier][variant] =
			        loadShader( terrain_paths[tier], StringName( terrain_one_sun_version_names[variant] ) );
			m_forward.m_szTerrainOneSun[T3D_TERRAIN_SHADER_PATH_COMPATIBILITY_MIXED][tier][variant] =
			        loadShader( terrain_paths[tier], StringName( terrain_compat_one_sun_version_names[variant] ) );
			m_forward.m_szTerrainOneSun[T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED][tier][variant] =
			        loadShader( terrain_paths[tier], StringName( terrain_editor_one_sun_version_names[variant] ) );
			for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
				if ( m_forward.m_szTerrainOneSun[path][tier][variant].is_null() ) {
					return abortShaderLoad();
				}
			}
		}
	}
	m_near = loadShader( terrain_paths[0], StringName( "color" ) );
	m_middle = loadShader( terrain_paths[1], StringName( "color" ) );
	m_far = loadShader( terrain_paths[2], StringName( "color" ) );
	if ( m_near.is_null() || m_middle.is_null() || m_far.is_null() ) {
		return abortShaderLoad();
	}
	m_cull = loadShader( "res://addons/terrain_3d/rd/terrain_cull.glsl" );
	if ( m_cull.is_null() ) {
		return abortShaderLoad();
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		m_forward.m_szInstances[variant] = loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( "bringup" ) );
#else
		m_forward.m_szInstances[variant] =
		        loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( version_names[variant] ) );
#endif
		if ( m_forward.m_szInstances[variant].is_null() ) {
			return abortShaderLoad();
		}
	}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	m_instances = loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( "bringup" ) );
#else
	m_instances = loadShader( "res://addons/terrain_3d/rd/terrain_instances.glsl", StringName( "color" ) );
#endif
	if ( m_instances.is_null() ) {
		return abortShaderLoad();
	}
	m_instanceOccluderDepth = loadShader( "res://addons/terrain_3d/rd/terrain_instance_occluder_depth.glsl" );
	if ( m_instanceOccluderDepth.is_null() ) {
		return abortShaderLoad();
	}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	m_hlodCull = loadShader( "res://addons/terrain_3d/rd/terrain_hlod_cull.glsl" );
	m_instanceCull = loadShader( "res://addons/terrain_3d/rd/terrain_instance_cull.glsl" );
	m_instanceHiZReduce = loadShader( "res://addons/terrain_3d/rd/terrain_instance_hiz_reduce.glsl" );
	if ( m_hlodCull.is_null() || m_instanceCull.is_null() || m_instanceHiZReduce.is_null() ) {
		return abortShaderLoad();
	}
#endif
	m_worldHiZReduce = loadShader( "res://addons/terrain_3d/rd/terrain_world_hiz_reduce.glsl" );
	m_worldVisibilityReset = loadShader( "res://addons/terrain_3d/rd/terrain_world_visibility.glsl", StringName( "reset" ) );
	m_worldVisibilityCull = loadShader( "res://addons/terrain_3d/rd/terrain_world_visibility.glsl", StringName( "cull" ) );
	m_worldVisibilityFinalize = loadShader( "res://addons/terrain_3d/rd/terrain_world_visibility.glsl", StringName( "finalize" ) );
	if ( m_worldHiZReduce.is_null() || m_worldVisibilityReset.is_null() || m_worldVisibilityCull.is_null()
	        || m_worldVisibilityFinalize.is_null() ) {
		return abortShaderLoad();
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_worldRasterTerrainStrip[variant] = loadShader( "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
		        StringName( String( "terrain_strip_" ) + String( version_names[variant] ) ) );
		m_worldRasterMesh[variant] = loadShader( "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
		        StringName( String( "mesh_" ) + String( version_names[variant] ) ) );
		if ( m_worldRasterTerrainStrip[variant].is_null() || m_worldRasterMesh[variant].is_null() ) {
			return abortShaderLoad();
		}
	}
	m_worldRasterTerrainStripShadow =
	        loadShader( "res://addons/terrain_3d/rd/terrain_world_raster.glsl", StringName( "terrain_strip_shadow" ) );
	m_worldRasterMeshShadow = loadShader( "res://addons/terrain_3d/rd/terrain_world_raster.glsl", StringName( "mesh_shadow" ) );
	if ( m_worldRasterTerrainStripShadow.is_null() || m_worldRasterMeshShadow.is_null() ) {
		return abortShaderLoad();
	}

	m_oceanColor = loadShader( "res://addons/terrain_3d/rd/terrain_ocean.glsl", StringName( "color_native" ) );
	m_oceanShadow = loadShader( "res://addons/terrain_3d/rd/terrain_ocean.glsl", StringName( "shadow" ) );
	if ( m_oceanColor.is_null() || m_oceanShadow.is_null() ) {
		return abortShaderLoad();
	}

	m_lastError = String();
	const uint64_t nFinishedMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nFinishedMsec >= m_nShaderLoadStartMsec ? nFinishedMsec - m_nShaderLoadStartMsec : 0u;
	const uint64_t nFinishedUsec = t3dRdShaderTicksUsec();
	m_nShaderLoadElapsedUsec = nFinishedUsec >= m_nShaderLoadStartUsec ? nFinishedUsec - m_nShaderLoadStartUsec : nElapsedMsec * 1000u;
	t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadSuccessCount );
	t3dStartupDomainFinish( m_startupState, T3D_STARTUP_CPU_READY, nFinishedUsec );
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] COMPLETE ready=", m_nShaderLoadSuccessCount, "/", m_nShaderLoadExpectedCount,
	        " generation=", m_nStartupGeneration, " elapsed=", nElapsedMsec, " ms" );
	UtilityFunctions::print( "[Terrain3D][Startup][Shader] CPU READY generation=", m_nStartupGeneration,
	        " completed=", m_nShaderLoadSuccessCount, "/", m_nShaderLoadExpectedCount );
	return true;
}

void Terrain3DRdShaderLibrary::clear( void ) {
	for ( uint32_t iSource = 0u; iSource < s_nShaderSourceCapacity; ++iSource ) {
		m_szSourceFiles[iSource].unref();
		m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_IDLE;
	}
	m_nShaderSourceCount = 0u;
	m_nShaderSourceRequestCount = 0u;
	m_nShaderSourceReadyCount = 0u;
	m_nShaderSourceFailureCount = 0u;
	m_bLoadRequested = false;
	m_bLoadCancelled = false;
	m_near.unref();
	m_middle.unref();
	m_far.unref();
	m_cull.unref();
	m_instances.unref();
	m_hlodCull.unref();
	m_instanceCull.unref();
	m_instanceHiZReduce.unref();
	m_instanceOccluderDepth.unref();
	m_worldHiZReduce.unref();
	m_worldVisibilityReset.unref();
	m_worldVisibilityCull.unref();
	m_worldVisibilityFinalize.unref();
	for ( uint32_t i = 0u; i < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++i ) {
		m_worldRasterTerrainStrip[i].unref();
		m_worldRasterMesh[i].unref();
	}
	m_worldRasterTerrainStripShadow.unref();
	m_worldRasterMeshShadow.unref();
	m_oceanColor.unref();
	m_oceanShadow.unref();
	for ( uint32_t path = 0u; path < T3D_TERRAIN_SHADER_PATH_COUNT; ++path ) {
		for ( uint32_t tier = 0u; tier < 3u; ++tier ) {
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
				m_forward.m_szTerrain[path][tier][variant].unref();
			}
			for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
				m_forward.m_szTerrainOneSun[path][tier][variant].unref();
			}
		}
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_forward.m_szInstances[variant].unref();
	}

}

Dictionary Terrain3DRdShaderLibrary::getDiagnostics( void ) const {
	Dictionary result;
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	result["startup_generation"] = ( int64_t )( m_nStartupGeneration );
	const t3dStartupStage_t eStartupStage = startup.m_eStage;
	result["startup_stage"] = ( int64_t )( eStartupStage );
	result["startup_stage_name"] = String( t3dStartupStageName( eStartupStage ) );
	result["startup_state_generation"] = ( int64_t )( startup.m_nGeneration );
	result["startup_completed_units"] = ( int64_t )( startup.m_nCompletedUnits );
	result["startup_total_units"] = ( int64_t )( startup.m_nTotalUnits );
	result["startup_start_usec"] = ( int64_t )( startup.m_nStartUsec );
	result["startup_end_usec"] = ( int64_t )( startup.m_nEndUsec );
	result["resource_spirv_prepare_usec_last"] = ( int64_t )( m_nShaderLoadElapsedUsec );
	result["shader_compile_expected"] = ( int64_t )( m_nShaderLoadExpectedCount );
	result["shader_compile_attempted"] = ( int64_t )( m_nShaderLoadAttemptCount );
	result["shader_compile_ready"] = ( int64_t )( m_nShaderLoadSuccessCount );
	result["shader_compile_failed"] = ( int64_t )( m_nShaderLoadFailureCount );
	result["shader_completed"] = ( int64_t )( startup.m_nCompletedUnits );
	result["shader_total"] = ( int64_t )( startup.m_nTotalUnits );
	result["shader_cpu_ready"] = eStartupStage == T3D_STARTUP_CPU_READY || eStartupStage == T3D_STARTUP_GPU_PENDING
	        || eStartupStage == T3D_STARTUP_GPU_READY || eStartupStage == T3D_STARTUP_COMMITTED;
	result["shader_gpu_ready"] = eStartupStage == T3D_STARTUP_GPU_READY || eStartupStage == T3D_STARTUP_COMMITTED;
	result["shader_elapsed_ms"] = ( double )( t3dStartupDomainElapsedUsec( startup, t3dRdShaderTicksUsec() ) ) / 1000.0;
	result["shader_source_count"] = ( int64_t )( m_nShaderSourceCount );
	result["shader_source_requests"] = ( int64_t )( m_nShaderSourceRequestCount );
	result["shader_source_ready"] = ( int64_t )( m_nShaderSourceReadyCount );
	result["shader_source_failed"] = ( int64_t )( m_nShaderSourceFailureCount );
	result["near_loaded"] = m_near.is_valid();
	result["middle_loaded"] = m_middle.is_valid();
	result["far_loaded"] = m_far.is_valid();
	result["cull_loaded"] = m_cull.is_valid();
	result["instances_loaded"] = m_instances.is_valid();
	result["instance_occluder_depth_loaded"] = m_instanceOccluderDepth.is_valid();
	result["hlod_cull_loaded"] = m_hlodCull.is_valid();
	result["world_hiz_loaded"] = m_worldHiZReduce.is_valid();
	result["world_visibility_loaded"] =
	        m_worldVisibilityReset.is_valid() && m_worldVisibilityCull.is_valid() && m_worldVisibilityFinalize.is_valid();
	result["world_raster_terrain_strip_loaded"] = m_worldRasterTerrainStrip[0].is_valid() && m_worldRasterTerrainStripShadow.is_valid();
	result["world_raster_mesh_loaded"] = m_worldRasterMesh[0].is_valid() && m_worldRasterMeshShadow.is_valid();
	result["ocean_color_loaded"] = m_oceanColor.is_valid();
	result["ocean_shadow_loaded"] = m_oceanShadow.is_valid();
	result["terrain_shipping_source_variants_per_tier"] = ( int64_t )( T3D_RD_TERRAIN_FORWARD_SOURCE_VARIANT_COUNT );
	result["terrain_shipping_feature_class_count"] = ( int64_t )( T3D_TERRAIN_SHIPPING_FEATURE_CLASS_COUNT );
	result["terrain_shipping_specialization_constant_id"] = ( int64_t )( T3D_TERRAIN_SPEC_CONSTANT_SHIPPING_FEATURE_CLASS );
	result["terrain_lighting_model_class_count"] = ( int64_t )( T3D_TERRAIN_LIGHTING_MODEL_CLASS_COUNT );
	result["terrain_scene_integration_class_count"] = ( int64_t )( T3D_TERRAIN_SCENE_INTEGRATION_CLASS_COUNT );
	result["terrain_lighting_model_specialization_constant_id"] = ( int64_t )( T3D_TERRAIN_SPEC_CONSTANT_LIGHTING_MODEL_CLASS );
	result["terrain_scene_integration_specialization_constant_id"] = ( int64_t )( T3D_TERRAIN_SPEC_CONSTANT_SCENE_INTEGRATION_CLASS );
	result["terrain_dnr_output_class_count"] = ( int64_t )( T3D_TERRAIN_DNR_OUTPUT_CLASS_COUNT );
	result["terrain_dnr_output_specialization_constant_id"] = ( int64_t )( T3D_TERRAIN_SPEC_CONSTANT_DNR_OUTPUT_CLASS );
	result["terrain_middle_surface_class_count"] = ( int64_t )( T3D_TERRAIN_MIDDLE_SURFACE_CLASS_COUNT );
	result["terrain_middle_surface_specialization_constant_id"] = ( int64_t )( T3D_TERRAIN_SPEC_CONSTANT_MIDDLE_SURFACE_CLASS );
	result["terrain_shipping_specialization_uses_shared_spirv"] = true;
	result["last_error"] = m_lastError;
	return result;
}
