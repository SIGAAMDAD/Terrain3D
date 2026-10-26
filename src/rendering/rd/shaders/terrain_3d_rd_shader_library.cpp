// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"

#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"

#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#include <new>

typedef struct t3dRdShaderSharedState_s
{
	Terrain3DMutex		 m_sourceMutex;
	Ref< RDShaderFile >	 m_sourceFiles[T3D_RD_SHADER_COUNT];
	Ref< RDShaderSPIRV > m_spirv[T3D_RD_SHADER_COUNT];
	Terrain3DRdShader	 m_physical[T3D_RD_SHADER_COUNT];
	t3dAtomicU32_t		 m_nPhysicalPrewarmCursor{ 0u };
	t3dAtomicU32_t		 m_nPhysicalReadyCount{ 0u };
	t3dAtomicU32_t		 m_nPhysicalFailureCount{ 0u };
	t3dAtomicU32_t		 m_nPhysicalMissAfterReadyCount{ 0u };
	t3dAtomicU32_t		 m_nPhysicalDemandCreateCount{ 0u };
	t3dAtomicU32_t		 m_nPhysicalDemandFailureCount{ 0u };
	t3dAtomicU32_t		 m_nPhysicalUsers{ 0u };
	t3dAtomicU64_t		 m_nPhysicalDeviceGeneration{ 0u };
	bool				 m_bPhysicalMetadataPrepared = false;
	bool m_bCompactClusters = false;
	bool m_bUnifiedCookedPreview = false;
	bool m_bEnvironmentFields = false;
	bool m_bTerrainGpuTimestamp = false;
	String				 m_instanceDiagnosticSuffix;
	String				 m_instanceDiagnosticMode = "production";
	uint32_t			 m_nInstanceDiagnosticMode = 0u;
} t3dRdShaderSharedState_t;

// Keep the Windows loader path free of Godot Ref/RID/mutex constructors.
// This pointer is trivially initialized by the CRT; the state itself is created
// only after GDExtension initialization has established the Godot interface.
static t3dRdShaderSharedState_t *s_pRdShaderSharedState = NULL;

static t3dRdShaderSharedState_t *t3dRdShaderSharedStateGet(
	void ) {
	return s_pRdShaderSharedState;
}

static t3dRdShaderSharedState_t &t3dRdShaderSharedState(
	void ) {
	return *s_pRdShaderSharedState;
}

// Cold-start environment settings override SCons-configured defaults.
static bool t3dRdEnvironmentFlagOrDefault( OS *const pOS, const char *const pName,
	const bool bDefault ) {
	const String setting = pOS ? pOS->get_environment( pName ) : String();
	return setting.is_empty() ? bDefault : setting == "1";
}

bool Terrain3DRdShaderLibrary::initializeSharedState(
	void ) {
	if ( s_pRdShaderSharedState ) {
		return true;
	}
	s_pRdShaderSharedState = new ( std::nothrow ) t3dRdShaderSharedState_t();
	if ( s_pRdShaderSharedState ) {
		// Compact clusters are a supported renderer profile, not a shader trial.
		// Standard Forward+ lighting is the default and remains a legal fallback.
		ProjectSettings *const pSettings = ProjectSettings::get_singleton();
		s_pRdShaderSharedState->m_bCompactClusters = pSettings &&
			pSettings->has_setting( "terrain3d/rendering/compact_clusters" ) &&
			bool( pSettings->get_setting( "terrain3d/rendering/compact_clusters" ) );
		s_pRdShaderSharedState->m_bUnifiedCookedPreview = true;
		OS *const pOS = OS::get_singleton();
		s_pRdShaderSharedState->m_bEnvironmentFields = pOS &&
			pOS->get_environment( "TERRAIN3D_ENVIRONMENT_FIELDS" ) == "1";
		if ( pOS ) {
			s_pRdShaderSharedState->m_bTerrainGpuTimestamp = t3dRdEnvironmentFlagOrDefault(
				pOS, "TERRAIN3D_TERRAIN_GPU_TIMESTAMPS", T3D_DEFAULT_TERRAIN_GPU_TIMESTAMPS != 0u );
		}
		// The legacy preview instance cull retains the grouped shader.
		// Command-ownership/exclusive trials are not production dispatch paths.
#ifdef DEBUG_ENABLED
		// Cold selection is process-wide because the physical shader cache is shared.
		const String mode = OS::get_singleton()->get_environment( "TERRAIN3D_INSTANCE_GPU_DIAGNOSTIC" ).to_lower();
		if ( mode == "constant" || mode == "opacity" ) {
			s_pRdShaderSharedState->m_instanceDiagnosticMode = mode;
			s_pRdShaderSharedState->m_instanceDiagnosticSuffix = "_diag_" + mode;
			s_pRdShaderSharedState->m_nInstanceDiagnosticMode = mode == "constant" ? 1u : 2u;
		} else if ( mode == "scalar_lit" ) {
			// Production now includes the authored-surface/scalar-only policy (P01).
			s_pRdShaderSharedState->m_instanceDiagnosticMode = mode;
			s_pRdShaderSharedState->m_nInstanceDiagnosticMode = 3u;
		}
#endif
	}
	return s_pRdShaderSharedState != NULL;
}

void Terrain3DRdShaderLibrary::shutdownSharedState(
	void ) {
	if ( !s_pRdShaderSharedState ) {
		return;
	}
	for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) {
		s_pRdShaderSharedState->m_physical[id].abandon();
	}
	delete s_pRdShaderSharedState;
	s_pRdShaderSharedState = NULL;
}

uint32_t Terrain3DRdShaderLibrary::getInstanceGpuDiagnosticMode(
	void ) {
	const auto *state = t3dRdShaderSharedStateGet();
	return state ? state->m_nInstanceDiagnosticMode : 0u;
}

bool Terrain3DRdShaderLibrary::isUnifiedCookedPreviewEnabledCold( void ) {
    const t3dRdShaderSharedState_t *const pState = t3dRdShaderSharedStateGet();
    return pState && pState->m_bUnifiedCookedPreview;
}

bool Terrain3DRdShaderLibrary::isCompactClustersEnabledCold( void ) {
	const t3dRdShaderSharedState_t *const pState = t3dRdShaderSharedStateGet();
	return pState && pState->m_bCompactClusters;
}

bool Terrain3DRdShaderLibrary::isEnvironmentFieldsEnabledCold( void ) {
	const t3dRdShaderSharedState_t *const pState = t3dRdShaderSharedStateGet();
	return pState && pState->m_bEnvironmentFields;
}

bool Terrain3DRdShaderLibrary::isTerrainGpuTimestampEnabledCold( void ) {
	const auto *const pState = t3dRdShaderSharedStateGet();
	return pState && pState->m_bTerrainGpuTimestamp;
}

// Preserve existing internal query entrypoints for current callers.
// Grouped, small, and scan dispatch are normal shaders in the same WORLD owner.
bool Terrain3DRdShaderLibrary::isGroupedCullEnabledCold( void ) {
	return t3dRdShaderSharedStateGet() != NULL;
}

bool Terrain3DRdShaderLibrary::isWorldSmallCullEnabledCold( void ) {
	return t3dRdShaderSharedStateGet() != NULL;
}

bool Terrain3DRdShaderLibrary::isWorldScanEnabledCold( void ) {
	return t3dRdShaderSharedStateGet() != NULL;
}

bool Terrain3DRdShaderLibrary::isInstanceCommandOwnerEnabledCold( void ) {
	// The preview shader family keeps its stable grouped-cull fallback.
	return false;
}

bool Terrain3DRdShaderLibrary::isInstanceExclusiveEnabledCold( void ) {
	return false;
}

// The former multi-feedback GPU pilot has no production producer. Keep the
// borrowed API returning false until its unused experimental service is retired.
bool Terrain3DRdShaderLibrary::isMultiFeedbackExperimentEnabledCold( void ) {
    return false;
}

bool Terrain3DRdShaderLibrary::isPageReductionEnabledCold( void ) {
	return t3dRdShaderSharedStateGet() != NULL;
}

bool Terrain3DRdShaderLibrary::physicalCacheReady(
	void ) {
	const t3dRdShaderSharedState_t *const pState = t3dRdShaderSharedStateGet();
	return pState && pState->m_nPhysicalUsers != 0u && pState->m_nPhysicalPrewarmCursor == t3dRdShaderCatalogActiveCount() &&
		   pState->m_nPhysicalFailureCount == 0u;
}

// t3dRdShaderSourceStage_e centralized in Terrain3DTypes.h.

static uint64_t t3dRdShaderTicksMsec(
	void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_msec() : 0u;
}

static uint64_t t3dRdShaderTicksUsec(
	void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_usec() : 0u;
}

// Shader ID slots remain append-stable. Only real, reachable shader versions
// are loaded and physically prewarmed; retired slots have no catalog source.
static uint32_t t3dRdExpectedPhysicalShaderCount(
	void ) {
	uint32_t nCount = 0u;
	for ( uint32_t value = 0u; value < t3dRdShaderCatalogActiveCount(); ++value ) {
		const t3dRdShaderId_t id = t3dRdShaderCatalogActiveIdAt( value );
		if ( !t3dRdShaderCatalogIsActive( id ) ) {
			continue;
		}
		if ( t3dRdShaderCatalogGet( id ).m_domain == T3D_RD_SHADER_GLOBAL_DEVICE )
			++nCount;
	}
	return nCount;
}

static uint32_t t3dRdExpectedShaderLoadCount(
	void ) {
	return t3dRdShaderCatalogActiveCount();
}

static String t3dRdShaderStatusName(
	const String	 &rPath,
	const StringName &rVersion ) {
	return rVersion.is_empty() ? rPath : rPath + String( " version=" ) + String( rVersion );
}

void Terrain3DRdShaderLibrary::logShaderFailure(
	const String	 &rPath,
	const StringName &rVersion,
	const uint64_t	  nStartedMsec ) {
	m_nShaderLoadFailureCount++;
	const uint64_t nNowMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nNowMsec >= nStartedMsec ? nNowMsec - nStartedMsec : 0u;
	UtilityFunctions::push_error( "[Terrain3D][ShaderLoad] FAIL [",
								  m_nShaderLoadAttemptCount,
								  "/",
								  m_nShaderLoadExpectedCount,
								  "] ",
								  t3dRdShaderStatusName( rPath, rVersion ),
								  " elapsed=",
								  nElapsedMsec,
								  " ms :: ",
								  m_lastError );
}

void Terrain3DRdShaderLibrary::logShaderReady(
	const String	 &rPath,
	const StringName &rVersion,
	const uint64_t	  nStartedMsec ) {
	m_nShaderLoadSuccessCount++;
	t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadSuccessCount );
	const uint64_t nNowMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nNowMsec >= nStartedMsec ? nNowMsec - nStartedMsec : 0u;
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] READY [",
							 m_nShaderLoadAttemptCount,
							 "/",
							 m_nShaderLoadExpectedCount,
							 "] ",
							 t3dRdShaderStatusName( rPath, rVersion ),
							 " elapsed=",
							 nElapsedMsec,
							 " ms" );
}

bool Terrain3DRdShaderLibrary::abortShaderLoad(
	void ) {
	const uint64_t nNowMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nNowMsec >= m_nShaderLoadStartMsec ? nNowMsec - m_nShaderLoadStartMsec : 0u;
	const uint64_t nNowUsec = t3dRdShaderTicksUsec();
	m_nShaderLoadElapsedUsec =
		nNowUsec >= m_nShaderLoadStartUsec ? nNowUsec - m_nShaderLoadStartUsec : nElapsedMsec * 1000u;
	t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadSuccessCount );
	t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, nNowUsec );
	UtilityFunctions::push_error( "[Terrain3D][ShaderLoad] ABORT ready=",
								  m_nShaderLoadSuccessCount,
								  "/",
								  m_nShaderLoadExpectedCount,
								  " attempted=",
								  m_nShaderLoadAttemptCount,
								  " failed=",
								  m_nShaderLoadFailureCount,
								  " generation=",
								  m_nStartupGeneration,
								  " elapsed=",
								  nElapsedMsec,
								  " ms :: ",
								  m_lastError );
	return false;
}

void Terrain3DRdShaderLibrary::markStartupGpuPending(
	const uint32_t nGeneration ) {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nGeneration != m_nStartupGeneration || startup.m_nGeneration != nGeneration ) {
		return;
	}
	const t3dStartupStage_t eStage = startup.m_eStage;
	if ( eStage == T3D_STARTUP_CPU_READY || eStage == T3D_STARTUP_GPU_PENDING ) {
		t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_GPU_PENDING );
	}
}

void Terrain3DRdShaderLibrary::markStartupGpuReady(
	const uint32_t nGeneration ) {
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

void Terrain3DRdShaderLibrary::markStartupCommitted(
	const uint32_t nGeneration ) {
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

Ref< RDShaderFile > Terrain3DRdShaderLibrary::findSourceFile(
	const String &rPath ) const {
	if ( !t3dRdShaderSharedStateGet() ) {
		return Ref< RDShaderFile >();
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
		if ( rPath == String( t3dRdShaderCatalogSourcePath( iSource ) ) ) {
			Terrain3DScopedLock lock( state.m_sourceMutex );
			return state.m_sourceFiles[iSource];
		}
	}
	return Ref< RDShaderFile >();
}

Ref< RDShaderSPIRV > Terrain3DRdShaderLibrary::loadShader(
	const String	 &rPath,
	const StringName &rVersion ) {
	StringName	selected_version = rVersion;
	const auto &diagnostic_state = t3dRdShaderSharedState();
	if ( !diagnostic_state.m_instanceDiagnosticSuffix.is_empty() &&
		 ( rPath.ends_with( "/terrain_instances.glsl" ) ||
		   ( rPath.ends_with( "/terrain_world_raster.glsl" ) && String( rVersion ).contains( "mesh_" ) ) ) &&
		 rVersion != StringName( "bringup" ) ) {
		selected_version = StringName( String( rVersion ) + diagnostic_state.m_instanceDiagnosticSuffix );
	}
	m_nShaderLoadAttemptCount++;
	const uint64_t nStartedMsec = t3dRdShaderTicksMsec();
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] RESOLVE [",
							 m_nShaderLoadAttemptCount,
							 "/",
							 m_nShaderLoadExpectedCount,
							 "] ",
							 t3dRdShaderStatusName( rPath, rVersion ) );
	const Ref< RDShaderFile > shader_file = findSourceFile( rPath );
	if ( shader_file.is_null() ) {
		m_lastError = "Threaded RD shader source is unavailable: " + rPath;
		logShaderFailure( rPath, rVersion, nStartedMsec );
		return Ref< RDShaderSPIRV >();
	}
	if ( !selected_version.is_empty() ) {
		const TypedArray< StringName > versions = shader_file->get_version_list();
		bool						   found = false;
		for ( int64_t i = 0; i < versions.size(); ++i ) {
			if ( StringName( versions[i] ) == selected_version ) {
				found = true;
				break;
			}
		}
		if ( !found ) {
			m_lastError = "RD shader is missing required version: " + rPath + " version=" + String( rVersion );
			logShaderFailure( rPath, rVersion, nStartedMsec );
			return Ref< RDShaderSPIRV >();
		}
	}
	Ref< RDShaderSPIRV > spirv = shader_file->get_spirv( selected_version );
	if ( spirv.is_null() ) {
		m_lastError = "RD shader has no SPIR-V: " + rPath +
					  ( rVersion.is_empty() ? String() : String( " version=" ) + String( rVersion ) );
		logShaderFailure( rPath, rVersion, nStartedMsec );
		return Ref< RDShaderSPIRV >();
	}
	const RenderingDevice::ShaderStage stages[] = {
		RenderingDevice::SHADER_STAGE_VERTEX,
		RenderingDevice::SHADER_STAGE_FRAGMENT,
		RenderingDevice::SHADER_STAGE_COMPUTE,
	};
	for ( size_t iStage = 0u; iStage < T3D_ARRAY_COUNT( stages ); iStage++ ) {
		const RenderingDevice::ShaderStage stage = ( stages )[iStage];
		const String					   compile_error = spirv->get_stage_compile_error( stage );
		if ( !compile_error.is_empty() ) {
			m_lastError = "RD shader compile failed: " + rPath + " :: " + compile_error;
			logShaderFailure( rPath, rVersion, nStartedMsec );
			return Ref< RDShaderSPIRV >();
		}
	}
	logShaderReady( rPath, rVersion, nStartedMsec );
	return spirv;
}

Ref< RDShaderSPIRV > Terrain3DRdShaderLibrary::loadCatalogShader(
	const t3dRdShaderId_t id ) {
	if ( !t3dRdShaderSharedStateGet() ) {
		return Ref< RDShaderSPIRV >();
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( id >= T3D_RD_SHADER_COUNT ) {
		return Ref< RDShaderSPIRV >();
	}
	const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
	if ( !entry.m_path ) {
		return Ref< RDShaderSPIRV >();
	}
	Ref< RDShaderSPIRV > cached;
	{
		Terrain3DScopedLock lock( state.m_sourceMutex );
		cached = state.m_spirv[id];
	}
	if ( cached.is_valid() ) {
		++m_nShaderLoadAttemptCount;
		++m_nShaderLoadSuccessCount;
		t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadSuccessCount );
		return cached;
	}
	cached = loadShader( String( entry.m_path ), entry.m_version ? StringName( entry.m_version ) : StringName() );
	if ( cached.is_valid() ) {
		Terrain3DScopedLock lock( state.m_sourceMutex );
		if ( state.m_spirv[id].is_null() )
			state.m_spirv[id] = cached;
		return state.m_spirv[id];
	}
	return cached;
}

Ref< RDShaderSPIRV > Terrain3DRdShaderLibrary::getCatalogShader(
	const t3dRdShaderId_t id ) const {
	return borrowSpirv( id );
}

Ref< RDShaderSPIRV > Terrain3DRdShaderLibrary::borrowSpirv(
	const t3dRdShaderId_t id ) {
	if ( !t3dRdShaderSharedStateGet() ) {
		return Ref< RDShaderSPIRV >();
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !t3dRdShaderCatalogIsActive( id ) ) {
		return Ref< RDShaderSPIRV >();
	}
	Terrain3DScopedLock lock( state.m_sourceMutex );
	return state.m_spirv[id];
}

Ref< RDShaderSPIRV > Terrain3DRdShaderLibrary::resolveComputeShader(
	const t3dRdShaderId_t id ) {
	if ( id >= T3D_RD_SHADER_COUNT ) {
		return Ref< RDShaderSPIRV >();
	}
	const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
	return entry.m_category == T3D_RD_SHADER_CATEGORY_COMPUTE ? borrowSpirv( id ) : Ref< RDShaderSPIRV >();
}

static void t3dRdSetDynamicShaderBuffers(
	const Ref< RDShaderSPIRV > &spirv,
	const PackedInt64Array	   &bindings ) {
	if ( spirv.is_valid() && spirv->has_method( StringName( "set_dynamic_buffers" ) ) ) {
		spirv->call( StringName( "set_dynamic_buffers" ), bindings );
	}
}

RID Terrain3DRdShaderLibrary::borrowPhysical(
	const t3dRdShaderId_t id ) {
	if ( !t3dRdShaderSharedStateGet() ) {
		return RID();
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !t3dRdShaderCatalogIsActive( id ) || !physicalCacheReady() ) {
		return RID();
	}
	const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
	if ( entry.m_domain != T3D_RD_SHADER_GLOBAL_DEVICE || !state.m_physical[id].isValid() ) {
		++state.m_nPhysicalMissAfterReadyCount;
		UtilityFunctions::push_error( "[Terrain3D][ShaderCache] Missing physical shader after READY, catalog ID=",
									  ( int64_t ) id );
		return RID();
	}
	return state.m_physical[id].rid();
}

RID Terrain3DRdShaderLibrary::acquirePhysicalCold(
	Terrain3DRdContext	 &context,
	const t3dRdShaderId_t id ) {
	if ( !t3dRdShaderSharedStateGet() || !t3dRdShaderCatalogIsActive( id ) || !physicalCacheReady() ) {
		return RID();
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !context.getDevice() || context.getDeviceGeneration() != state.m_nPhysicalDeviceGeneration ||
		 t3dRdShaderCatalogGet( id ).m_domain != T3D_RD_SHADER_GLOBAL_DEVICE ) {
		return RID();
	}
	if ( state.m_physical[id].isValid() ) {
		return borrowPhysical( id );
	}
	const Ref< RDShaderSPIRV > spirv = borrowSpirv( id );
	const String			   name = String( "T3D.ShaderCache." ) + String::num_uint64( id );
	context.beginResourceRebuild();
	const bool created = spirv.is_valid() &&
						 state.m_physical[id].createTracked( context, T3D_RD_CONTEXT_OWNER_SHADER_CACHE, name, spirv );
	context.endResourceRebuild();
	if ( !created ) {
		++state.m_nPhysicalDemandFailureCount;
		return RID();
	}
	++state.m_nPhysicalReadyCount;
	++state.m_nPhysicalDemandCreateCount;
	return state.m_physical[id].rid();
}

void Terrain3DRdShaderLibrary::capturePhysicalCountersOnRenderThread(
	uint64_t &rReady,
	uint64_t &rExpected,
	uint64_t &rMisses ) const {
	rReady = 0u;
	rExpected = t3dRdExpectedPhysicalShaderCount();
	rMisses = 0u;
	if ( !t3dRdShaderSharedStateGet() ) {
		return;
	}
	const t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	rReady = state.m_nPhysicalReadyCount;
	rMisses = state.m_nPhysicalMissAfterReadyCount;
}

bool Terrain3DRdShaderLibrary::isPhysicalReady(
	void ) const {
	if ( !t3dRdShaderSharedStateGet() ) {
		return false;
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	return m_bPhysicalUser && m_nPhysicalDeviceGeneration == state.m_nPhysicalDeviceGeneration &&
		   state.m_nPhysicalPrewarmCursor == t3dRdShaderCatalogActiveCount() && state.m_nPhysicalFailureCount == 0u;
}

bool Terrain3DRdShaderLibrary::servicePhysicalPrewarm(
	Terrain3DRdContext &context,
	const uint32_t		budget ) {
	if ( !t3dRdShaderSharedStateGet() ) {
		return false;
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !context.getDevice() || !isCpuReady( m_nStartupGeneration ) ) {
		return false;
	}
	const uint64_t generation = context.getDeviceGeneration();
	if ( state.m_nPhysicalDeviceGeneration != generation ) {
		for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id )
			state.m_physical[id].abandon();
		state.m_nPhysicalPrewarmCursor = 0u;
		state.m_nPhysicalReadyCount = 0u;
		state.m_nPhysicalFailureCount = 0u;
		state.m_nPhysicalMissAfterReadyCount = 0u;
		state.m_nPhysicalDemandCreateCount = 0u;
		state.m_nPhysicalDemandFailureCount = 0u;
		state.m_nPhysicalUsers = 0u;
		state.m_bPhysicalMetadataPrepared = false;
		state.m_nPhysicalDeviceGeneration = generation;
	}
	if ( !m_bPhysicalUser || m_nPhysicalDeviceGeneration != generation ) {
		m_bPhysicalUser = true;
		m_nPhysicalDeviceGeneration = generation;
		++state.m_nPhysicalUsers;
	}
	if ( state.m_nPhysicalFailureCount != 0u ) {
		return false;
	}
	if ( isPhysicalReady() ) {
		return true;
	}
	if ( !state.m_bPhysicalMetadataPrepared ) {
		// Physical shader RIDs capture dynamic descriptor metadata at creation.
		// Prepare it here, before prewarming, using the negotiated arena mode.
		if ( !context.initializeSharedArenas() ) {
			++state.m_nPhysicalFailureCount;
			m_lastError = String( "Shader prewarm shared arena initialization failed: " ) + context.getLastFailure();
			t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, t3dRdShaderTicksUsec() );
			return false;
		}
		for ( uint32_t id = T3D_RD_SHADER_TERRAIN_FORWARD_BASE; id <= T3D_RD_SHADER_TERRAIN_CULL; ++id ) {
			context.markHotShader( borrowSpirv( ( t3dRdShaderId_t ) id ) );
		}
		for ( uint32_t id = T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE; id <= T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW;
			  ++id ) {
			context.markHotShader( borrowSpirv( ( t3dRdShaderId_t ) id ) );
		}
		context.markHotShader( borrowSpirv( T3D_RD_SHADER_WORLD_VISIBILITY_RESET ), 2 );
		context.markHotShader( borrowSpirv( isGroupedCullEnabledCold()
			? T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED
			: T3D_RD_SHADER_WORLD_VISIBILITY_CULL ) );
		if ( isWorldSmallCullEnabledCold() )
			context.markHotShader( borrowSpirv( T3D_RD_SHADER_WORLD_VISIBILITY_CULL_SMALL ), 0 );
		if ( isWorldScanEnabledCold() ) {
			context.markHotShader( borrowSpirv( T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_CLASSIFY ), 0 );
			context.markHotShader( borrowSpirv( T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_PREFIX ), 0 );
			context.markHotShader( borrowSpirv( T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_SCATTER ), 0 );
			context.markHotShader( borrowSpirv( T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_FINALIZE ), 2 );
		}
		context.markHotShader( borrowSpirv( T3D_RD_SHADER_WORLD_VISIBILITY_FINALIZE ), 2 );
		if ( Terrain3DRdGpuBuffer::supportsPersistentMapping( context.getDevice() ) ) {
			PackedInt64Array displacement;
			displacement.push_back( 4 );
			displacement.push_back( 5 );
			t3dRdSetDynamicShaderBuffers( borrowSpirv( T3D_RD_SHADER_DISPLACEMENT_COMPUTE ), displacement );
		}
		state.m_bPhysicalMetadataPrepared = true;
	}
	const uint32_t limit = budget > 0u ? budget : 1u;
	for ( uint32_t work = 0u; work < limit && state.m_nPhysicalPrewarmCursor < t3dRdShaderCatalogActiveCount();
		  ++state.m_nPhysicalPrewarmCursor ) {
		const uint32_t		  nCursor = state.m_nPhysicalPrewarmCursor.load();
		const t3dRdShaderId_t id = t3dRdShaderCatalogActiveIdAt( nCursor );
		if ( !t3dRdShaderCatalogIsActive( id ) || !t3dRdShaderNeedsStartupPhysical( id ) ) {
			continue;
		}
		const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
		if ( entry.m_domain != T3D_RD_SHADER_GLOBAL_DEVICE ) {
			continue;
		}
		const Ref< RDShaderSPIRV > spirv = borrowSpirv( id );
		if ( spirv.is_null() ) {
			++state.m_nPhysicalFailureCount;
			m_lastError =
				String( "Physical RD shader cache is missing catalog SPIR-V: id=" ) + String::num_uint64( id );
			t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, t3dRdShaderTicksUsec() );
			return false;
		}
		const String name = String( "T3D.ShaderCache." ) + String::num_uint64( id );
		if ( !state.m_physical[id].createTracked( context, T3D_RD_CONTEXT_OWNER_SHADER_CACHE, name, spirv ) ) {
			++state.m_nPhysicalFailureCount;
			m_lastError = String( "Physical RD shader creation failed: " ) + name;
			t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, t3dRdShaderTicksUsec() );
			return false;
		}
		++state.m_nPhysicalReadyCount;
		++work;
	}
	return true;
}

void Terrain3DRdShaderLibrary::releasePhysical(
	Terrain3DRdContext &context ) {
	if ( !t3dRdShaderSharedStateGet() ) {
		m_bPhysicalUser = false;
		return;
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !m_bPhysicalUser ) {
		return;
	}
	m_bPhysicalUser = false;
	if ( m_nPhysicalDeviceGeneration != state.m_nPhysicalDeviceGeneration || state.m_nPhysicalUsers == 0u ) {
		return;
	}
	--state.m_nPhysicalUsers;
	if ( state.m_nPhysicalUsers != 0u ) {
		return;
	}
	for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) {
		if ( context.getDevice() )
			state.m_physical[id].releaseTracked( context );
		else
			state.m_physical[id].abandon();
	}
	state.m_nPhysicalPrewarmCursor = 0u;
	state.m_nPhysicalReadyCount = 0u;
	state.m_nPhysicalFailureCount = 0u;
	state.m_nPhysicalMissAfterReadyCount = 0u;
	state.m_nPhysicalDemandCreateCount = 0u;
	state.m_nPhysicalDemandFailureCount = 0u;
	state.m_bPhysicalMetadataPrepared = false;
}

bool Terrain3DRdShaderLibrary::beginLoadCold(
	const uint32_t nStartupGeneration ) {
	if ( !t3dRdShaderSharedStateGet() ) {
		m_lastError = "RD shader shared state is not initialized";
		return false;
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	clear();
	m_nStartupGeneration = nStartupGeneration;
	m_nShaderLoadAttemptCount = 0u;
	m_nShaderLoadSuccessCount = 0u;
	m_nShaderLoadFailureCount = 0u;
	m_nShaderLoadExpectedCount = t3dRdExpectedShaderLoadCount();
	m_nShaderLoadStartMsec = t3dRdShaderTicksMsec();
	m_nShaderLoadStartUsec = t3dRdShaderTicksUsec();
	m_nShaderLoadElapsedUsec = 0u;
	m_nShaderSourceCount = t3dRdShaderCatalogSourceCount();
	m_nShaderSourceRequestCount = 0u;
	m_nShaderSourceReadyCount = 0u;
	m_nShaderSourceFailureCount = 0u;
	m_bLoadRequested = true;
	m_bLoadCancelled = false;
	if ( m_nShaderSourceCount > T3D_RD_SHADER_SOURCE_CAPACITY ) {
		m_lastError = "RD shader source table exceeds fixed startup capacity";
		return abortShaderLoad();
	}
	t3dStartupDomainRequest( m_startupState, m_nStartupGeneration, m_nShaderLoadStartUsec, m_nShaderLoadExpectedCount );
	t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_PREPARING );
	// Every active catalog shader must be requested as a source before SPIR-V
	// resolution begins. Check the entire table, including editor-only programs.
	for ( uint32_t iActive = 0u; iActive < t3dRdShaderCatalogActiveCount(); ++iActive ) {
		const t3dRdShaderId_t id = t3dRdShaderCatalogActiveIdAt( iActive );
		const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
		bool bSourceRegistered = false;
		for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
			if ( String( entry.m_path ) == String( t3dRdShaderCatalogSourcePath( iSource ) ) ) {
				bSourceRegistered = true;
				break;
			}
		}
		if ( !bSourceRegistered ) {
			m_lastError = "Active RD shader source is missing from startup catalog: id=" +
				String::num_int64( ( int64_t ) id ) + " path=" + String( entry.m_path );
			++m_nShaderSourceFailureCount;
			return abortShaderLoad();
		}
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( pLoader == NULL ) {
		m_lastError = "ResourceLoader is unavailable while requesting RD shader sources";
		return abortShaderLoad();
	}
	for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
		{
			Terrain3DScopedLock lock( state.m_sourceMutex );
			if ( state.m_sourceFiles[iSource].is_valid() ) {
				m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_READY;
				continue;
			}
		}
		const String						   path = String( t3dRdShaderCatalogSourcePath( iSource ) );
		const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE || status == ResourceLoader::THREAD_LOAD_FAILED ) {
			const Error error =
				pLoader->load_threaded_request( path, "RDShaderFile", true, ResourceLoader::CACHE_MODE_REUSE );
			if ( error != OK ) {
				m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_FAILED;
				++m_nShaderSourceFailureCount;
				m_lastError =
					"Failed requesting threaded RD shader source: " + path + " error=" + String::num_int64( error );
				return abortShaderLoad();
			}
			++m_nShaderSourceRequestCount;
		}
		m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_REQUESTED;
	}
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] BEGIN generation=",
							 m_nStartupGeneration,
							 " unique_sources=",
							 m_nShaderSourceCount,
							 " expected_variants=",
							 m_nShaderLoadExpectedCount );
	UtilityFunctions::print( "[Terrain3D][Startup][Shader] REQUESTED generation=", m_nStartupGeneration );
	return true;
}

void Terrain3DRdShaderLibrary::serviceLoadCold(
	void ) {
	if ( !t3dRdShaderSharedStateGet() ) {
		return;
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !m_bLoadRequested || m_bLoadCancelled || isCpuReady( m_nStartupGeneration ) ||
		 isFailed( m_nStartupGeneration ) ) {
		return;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( pLoader == NULL ) {
		m_lastError = "ResourceLoader became unavailable while servicing RD shader sources";
		( void ) abortShaderLoad();
		return;
	}
	uint32_t nReady = 0u;
	for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
		if ( m_szSourceStages[iSource] == T3D_RD_SHADER_SOURCE_READY ) {
			++nReady;
			continue;
		}
		const String						   path = String( t3dRdShaderCatalogSourcePath( iSource ) );
		const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
			continue;
		}
		if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
			m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_FAILED;
			++m_nShaderSourceFailureCount;
			m_lastError = "Threaded RD shader source failed: " + path + " status=" + String::num_int64( status );
			( void ) abortShaderLoad();
			return;
		}
		const Ref< Resource > resource = pLoader->load_threaded_get( path );
		Ref< RDShaderFile >	  shaderFile = resource;
		if ( shaderFile.is_null() ) {
			m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_FAILED;
			++m_nShaderSourceFailureCount;
			m_lastError = "Threaded resource is not an RDShaderFile: " + path;
			( void ) abortShaderLoad();
			return;
		}
		{
			Terrain3DScopedLock lock( state.m_sourceMutex );
			if ( state.m_sourceFiles[iSource].is_null() )
				state.m_sourceFiles[iSource] = shaderFile;
		}
		m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_READY;
		++nReady;
	}
	m_nShaderSourceReadyCount = nReady;
	if ( nReady != m_nShaderSourceCount ) {
		return;
	}
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] SOURCES READY generation=",
							 m_nStartupGeneration,
							 " ready=",
							 nReady,
							 "/",
							 m_nShaderSourceCount );
	if ( !resolveCatalogCold() ) {
		return;
	}
}

bool Terrain3DRdShaderLibrary::isCpuReady(
	const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nStartupGeneration != m_nStartupGeneration || startup.m_nGeneration != nStartupGeneration ) {
		return false;
	}
	const t3dStartupStage_t stage = startup.m_eStage;
	return stage == T3D_STARTUP_CPU_READY || stage == T3D_STARTUP_GPU_PENDING || stage == T3D_STARTUP_GPU_READY ||
		   stage == T3D_STARTUP_COMMITTED;
}

bool Terrain3DRdShaderLibrary::isGpuReady(
	const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nStartupGeneration != m_nStartupGeneration || startup.m_nGeneration != nStartupGeneration ) {
		return false;
	}
	const t3dStartupStage_t stage = startup.m_eStage;
	return stage == T3D_STARTUP_GPU_READY || stage == T3D_STARTUP_COMMITTED;
}

bool Terrain3DRdShaderLibrary::isFailed(
	const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	return nStartupGeneration == m_nStartupGeneration && startup.m_nGeneration == nStartupGeneration &&
		   startup.m_eStage == T3D_STARTUP_FAILED;
}

void Terrain3DRdShaderLibrary::cancelLoadCold(
	const uint32_t nStartupGeneration ) {
	if ( nStartupGeneration != m_nStartupGeneration ||
		 !t3dStartupDomainMatches( m_startupState, nStartupGeneration ) ) {
		return;
	}
	const t3dStartupStage_t stage = t3dStartupDomainGetStage( m_startupState );
	if ( stage == T3D_STARTUP_FAILED || stage == T3D_STARTUP_CANCELLED ) {
		return;
	}
	m_bLoadCancelled = true;
	m_bLoadRequested = false;
	for ( uint32_t iSource = 0u; iSource < T3D_RD_SHADER_SOURCE_CAPACITY; ++iSource ) {
		if ( m_szSourceStages[iSource] != T3D_RD_SHADER_SOURCE_FAILED ) {
			m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_IDLE;
		}
	}
	( void ) t3dStartupDomainCancel( m_startupState, nStartupGeneration, t3dRdShaderTicksUsec() );
}

bool Terrain3DRdShaderLibrary::resolveCatalogCold(
	void ) {
	for ( uint32_t index = 0u; index < t3dRdShaderCatalogActiveCount(); ++index ) {
		const t3dRdShaderId_t id = t3dRdShaderCatalogActiveIdAt( index );
		// Startup totals and physical prewarming already exclude retired shader IDs.
		// The CPU resolver must use the same catalog policy or retired entries
		// with no shader path cause a false startup failure after valid loads.
		if ( !t3dRdShaderCatalogIsActive( id ) ) {
			continue;
		}
		const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
        UtilityFunctions::print( "[Terrain3D][ShaderTrace] source=", entry.m_path,
            " version=", entry.m_version ? entry.m_version : "", " id=", ( int64_t ) id,
            " generation=", ( int64_t ) m_nStartupGeneration );
		if ( loadCatalogShader( id ).is_null() ) {
			return abortShaderLoad();
		}
	}

	m_lastError = String();
	const uint64_t nFinishedMsec = t3dRdShaderTicksMsec();
	const uint64_t nElapsedMsec = nFinishedMsec >= m_nShaderLoadStartMsec ? nFinishedMsec - m_nShaderLoadStartMsec : 0u;
	const uint64_t nFinishedUsec = t3dRdShaderTicksUsec();
	m_nShaderLoadElapsedUsec =
		nFinishedUsec >= m_nShaderLoadStartUsec ? nFinishedUsec - m_nShaderLoadStartUsec : nElapsedMsec * 1000u;
	t3dStartupDomainSetCompletedUnits( m_startupState, m_nShaderLoadSuccessCount );
	t3dStartupDomainFinish( m_startupState, T3D_STARTUP_CPU_READY, nFinishedUsec );
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] COMPLETE ready=",
							 m_nShaderLoadSuccessCount,
							 "/",
							 m_nShaderLoadExpectedCount,
							 " generation=",
							 m_nStartupGeneration,
							 " elapsed=",
							 nElapsedMsec,
							 " ms" );
	UtilityFunctions::print( "[Terrain3D][Startup][Shader] CPU READY generation=",
							 m_nStartupGeneration,
							 " completed=",
							 m_nShaderLoadSuccessCount,
							 "/",
							 m_nShaderLoadExpectedCount );
	return true;
}

void Terrain3DRdShaderLibrary::clear(
	void ) {
	for ( uint32_t iSource = 0u; iSource < T3D_RD_SHADER_SOURCE_CAPACITY; ++iSource ) {
		m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_IDLE;
	}
	m_nShaderSourceCount = 0u;
	m_nShaderSourceRequestCount = 0u;
	m_nShaderSourceReadyCount = 0u;
	m_nShaderSourceFailureCount = 0u;
	m_bLoadRequested = false;
	m_bLoadCancelled = false;
}

Dictionary Terrain3DRdShaderLibrary::getDiagnostics(
	void ) const {
	if ( !t3dRdShaderSharedStateGet() ) {
		Dictionary unavailable;
		unavailable["shared_state_initialized"] = false;
		return unavailable;
	}
	t3dRdShaderSharedState_t		&state = t3dRdShaderSharedState();
	Dictionary						 result;
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	result["world_instance_alpha_coverage"] = "canonical";
	result["terrain_gpu_timestamp_capture_enabled"] = isTerrainGpuTimestampEnabledCold();
	result["instance_gpu_diagnostic_mode"] = state.m_instanceDiagnosticMode;
	result["instance_gpu_diagnostic_requires_restart"] = true;
	result["instance_gpu_diagnostic_gpu_time_available"] = false;
	result["startup_generation"] = ( int64_t ) ( m_nStartupGeneration );
	const t3dStartupStage_t eStartupStage = startup.m_eStage;
	result["startup_stage"] = ( int64_t ) ( eStartupStage );
	result["startup_stage_name"] = String( t3dStartupStageName( eStartupStage ) );
	result["startup_state_generation"] = ( int64_t ) ( startup.m_nGeneration );
	result["startup_completed_units"] = ( int64_t ) ( startup.m_nCompletedUnits );
	result["startup_total_units"] = ( int64_t ) ( startup.m_nTotalUnits );
	result["startup_start_usec"] = ( int64_t ) ( startup.m_nStartUsec );
	result["startup_end_usec"] = ( int64_t ) ( startup.m_nEndUsec );
	result["resource_spirv_prepare_usec_last"] = ( int64_t ) ( m_nShaderLoadElapsedUsec );
	result["shader_compile_expected"] = ( int64_t ) ( m_nShaderLoadExpectedCount );
	result["shader_compile_attempted"] = ( int64_t ) ( m_nShaderLoadAttemptCount );
	result["shader_compile_ready"] = ( int64_t ) ( m_nShaderLoadSuccessCount );
	result["shader_compile_failed"] = ( int64_t ) ( m_nShaderLoadFailureCount );
	result["shader_completed"] = ( int64_t ) ( startup.m_nCompletedUnits );
	result["shader_total"] = ( int64_t ) ( startup.m_nTotalUnits );
	result["shader_cpu_ready"] = eStartupStage == T3D_STARTUP_CPU_READY || eStartupStage == T3D_STARTUP_GPU_PENDING ||
								 eStartupStage == T3D_STARTUP_GPU_READY || eStartupStage == T3D_STARTUP_COMMITTED;
	result["shader_gpu_ready"] = eStartupStage == T3D_STARTUP_GPU_READY || eStartupStage == T3D_STARTUP_COMMITTED;
	result["shader_elapsed_ms"] =
		( double ) ( t3dStartupDomainElapsedUsec( startup, t3dRdShaderTicksUsec() ) ) / 1000.0;
	result["shader_source_count"] = ( int64_t ) ( m_nShaderSourceCount );
	result["shader_source_requests"] = ( int64_t ) ( m_nShaderSourceRequestCount );
	result["shader_source_ready"] = ( int64_t ) ( m_nShaderSourceReadyCount );
	result["shader_source_failed"] = ( int64_t ) ( m_nShaderSourceFailureCount );
	result["physical_shader_ready"] = ( int64_t ) ( state.m_nPhysicalReadyCount );
	result["physical_shader_expected"] = ( int64_t ) ( t3dRdExpectedPhysicalShaderCount() );
	result["physical_shader_failed"] = ( int64_t ) ( state.m_nPhysicalFailureCount );
	result["physical_shader_generation"] = ( int64_t ) ( state.m_nPhysicalDeviceGeneration );
	result["physical_shader_cache_users"] = ( int64_t ) ( state.m_nPhysicalUsers );
	result["physical_shader_misses_after_ready"] = ( int64_t ) ( state.m_nPhysicalMissAfterReadyCount );
	result["physical_shader_demand_created"] = ( int64_t ) ( state.m_nPhysicalDemandCreateCount );
	result["physical_shader_demand_failed"] = ( int64_t ) ( state.m_nPhysicalDemandFailureCount );
	uint32_t startup_physical_count = 0u;
	const uint32_t nActiveShaders = t3dRdShaderCatalogActiveCount();
	for ( uint32_t index = 0u; index < nActiveShaders; ++index ) {
		const t3dRdShaderId_t id = t3dRdShaderCatalogActiveIdAt( index );
		startup_physical_count += t3dRdShaderNeedsStartupPhysical( id ) ? 1u : 0u;
	}
	result["physical_shader_startup_expected"] = ( int64_t ) startup_physical_count;
	uint32_t catalog_by_category[5]{};
	uint32_t spirv_by_category[5]{};
	uint32_t physical_by_category[5]{};
	uint32_t catalog_by_domain[2]{};
	uint32_t spirv_by_domain[2]{};
	uint32_t physical_by_domain[2]{};
	{
		Terrain3DScopedLock lock( state.m_sourceMutex );
		for ( uint32_t index = 0u; index < nActiveShaders; ++index ) {
			const t3dRdShaderId_t id = t3dRdShaderCatalogActiveIdAt( index );
			const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
			++catalog_by_category[entry.m_category];
			++catalog_by_domain[entry.m_domain];
			if ( state.m_spirv[id].is_valid() ) {
				++spirv_by_category[entry.m_category];
				++spirv_by_domain[entry.m_domain];
			}
			if ( state.m_physical[id].isValid() ) {
				++physical_by_category[entry.m_category];
				++physical_by_domain[entry.m_domain];
			}
		}
	}
	static const char *categories[5] = { "terrain", "instance", "world", "ocean", "compute" };
	for ( uint32_t i = 0u; i < 5u; ++i ) {
		result[String( "catalog_count_" ) + categories[i]] = ( int64_t ) catalog_by_category[i];
		result[String( "spirv_ready_" ) + categories[i]] = ( int64_t ) spirv_by_category[i];
		result[String( "physical_ready_" ) + categories[i]] = ( int64_t ) physical_by_category[i];
	}
	result["catalog_shader_count"] =
		( int64_t ) ( catalog_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE] + catalog_by_domain[T3D_RD_SHADER_LOCAL_DEVICE] );
	result["catalog_global_count"] = ( int64_t ) catalog_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE];
	result["catalog_local_count"] = ( int64_t ) catalog_by_domain[T3D_RD_SHADER_LOCAL_DEVICE];
	result["spirv_ready_global"] = ( int64_t ) spirv_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE];
	result["spirv_ready_local"] = ( int64_t ) spirv_by_domain[T3D_RD_SHADER_LOCAL_DEVICE];
	result["physical_ready_global"] = ( int64_t ) physical_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE];
	result["physical_ready_local"] = ( int64_t ) physical_by_domain[T3D_RD_SHADER_LOCAL_DEVICE];
	result["unified_material_loaded"] = getShader( T3D_RD_SHADER_UCMP_ALIAS ).is_valid();
	result["instances_loaded"] = getShader( T3D_RD_SHADER_INSTANCE_ALIAS ).is_valid();
	result["instance_occluder_depth_loaded"] = getShader( T3D_RD_SHADER_INSTANCE_OCCLUDER_DEPTH ).is_valid();
	result["hlod_cull_loaded"] = getShader( T3D_RD_SHADER_HLOD_CULL ).is_valid();
	result["world_hiz_loaded"] = getShader( T3D_RD_SHADER_WORLD_HIZ_REDUCE ).is_valid();
	result["world_visibility_loaded"] = getWorldVisibilityReset().is_valid() && getWorldVisibilityCull().is_valid() &&
										getWorldVisibilityFinalize().is_valid();
	result["world_raster_terrain_strip_loaded"] =
		getWorldRasterTerrainStrip( ( t3dRdForwardShaderVariant_t ) 0 ).is_valid() &&
		getWorldRasterTerrainStripShadow().is_valid();
	result["world_raster_mesh_loaded"] =
		getWorldRasterMesh( ( t3dRdForwardShaderVariant_t ) 0 ).is_valid() && getWorldRasterMeshShadow().is_valid();
	result["ocean_color_loaded"] = getShader( T3D_RD_SHADER_OCEAN_COLOR ).is_valid();
	result["ocean_shadow_loaded"] = getShader( T3D_RD_SHADER_OCEAN_SHADOW ).is_valid();
	result["terrain_shipping_source_variants_per_tier"] = ( int64_t ) ( T3D_RD_TERRAIN_FORWARD_SOURCE_VARIANT_COUNT );
	result["terrain_shipping_feature_class_count"] = ( int64_t ) ( T3D_TERRAIN_SHIPPING_FEATURE_CLASS_COUNT );
	result["terrain_shipping_specialization_constant_id"] =
		( int64_t ) ( T3D_TERRAIN_SPEC_CONSTANT_SHIPPING_FEATURE_CLASS );
	result["terrain_lighting_model_class_count"] = ( int64_t ) ( T3D_TERRAIN_LIGHTING_MODEL_CLASS_COUNT );
	result["terrain_scene_integration_class_count"] = ( int64_t ) ( T3D_TERRAIN_SCENE_INTEGRATION_CLASS_COUNT );
	result["terrain_lighting_model_specialization_constant_id"] =
		( int64_t ) ( T3D_TERRAIN_SPEC_CONSTANT_LIGHTING_MODEL_CLASS );
	result["terrain_scene_integration_specialization_constant_id"] =
		( int64_t ) ( T3D_TERRAIN_SPEC_CONSTANT_SCENE_INTEGRATION_CLASS );
	result["terrain_dnr_output_class_count"] = ( int64_t ) ( T3D_TERRAIN_DNR_OUTPUT_CLASS_COUNT );
	result["terrain_dnr_output_specialization_constant_id"] =
		( int64_t ) ( T3D_TERRAIN_SPEC_CONSTANT_DNR_OUTPUT_CLASS );
	result["terrain_middle_surface_class_count"] = ( int64_t ) ( T3D_TERRAIN_MIDDLE_SURFACE_CLASS_COUNT );
	result["terrain_middle_surface_specialization_constant_id"] =
		( int64_t ) ( T3D_TERRAIN_SPEC_CONSTANT_MIDDLE_SURFACE_CLASS );
	result["terrain_shipping_specialization_uses_shared_spirv"] = true;
	result["last_error"] = m_lastError;
	return result;
}

// M38 extracted non-template header implementations.
void Terrain3DRdShaderLibrary::setStartupGeneration(
	const uint32_t nGeneration ) {
	m_nStartupGeneration = nGeneration;
	t3dStartupDomainRequest( m_startupState, nGeneration, 0u, 0u );
}

bool Terrain3DRdShaderLibrary::isReady(
	void ) const {
	if ( !isCpuReady( m_nStartupGeneration ) ) {
		return false;
	}
	const uint32_t nActiveShaders = t3dRdShaderCatalogActiveCount();
	for ( uint32_t index = 0u; index < nActiveShaders; ++index ) {
		const t3dRdShaderId_t shaderId = t3dRdShaderCatalogActiveIdAt( index );
		if ( getCatalogShader( shaderId ).is_null() ) {
			return false;
		}
	}
	return true;
}
