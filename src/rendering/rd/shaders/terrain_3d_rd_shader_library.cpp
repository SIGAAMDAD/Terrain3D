// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"

#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"

#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#include <new>

typedef struct t3dRdShaderSharedState_s {

	Terrain3DMutex m_sourceMutex;
	Ref<RDShaderFile> m_sourceFiles[T3D_RD_SHADER_COUNT];
	Ref<RDShaderSPIRV> m_spirv[T3D_RD_SHADER_COUNT];
	Terrain3DRdShader m_physical[T3D_RD_SHADER_COUNT];
	t3dAtomicU32_t m_nPhysicalPrewarmCursor{ 0u };
	t3dAtomicU32_t m_nPhysicalReadyCount{ 0u };
	t3dAtomicU32_t m_nPhysicalFailureCount{ 0u };
	t3dAtomicU32_t m_nPhysicalMissAfterReadyCount{ 0u };
	t3dAtomicU32_t m_nPhysicalDemandCreateCount { 0u };
	t3dAtomicU32_t m_nPhysicalDemandFailureCount { 0u };
	t3dAtomicU32_t m_nPhysicalUsers{ 0u };
	t3dAtomicU64_t m_nPhysicalDeviceGeneration{ 0u };
	bool m_bPhysicalMetadataPrepared = false;
} t3dRdShaderSharedState_t;

// Keep the Windows loader path free of Godot Ref/RID/mutex constructors.
// This pointer is trivially initialized by the CRT; the state itself is created
// only after GDExtension initialization has established the Godot interface.
static t3dRdShaderSharedState_t *s_pRdShaderSharedState = NULL;

static t3dRdShaderSharedState_t *t3dRdShaderSharedStateGet( void ) {
	return s_pRdShaderSharedState;
}

static t3dRdShaderSharedState_t &t3dRdShaderSharedState( void ) {
	return *s_pRdShaderSharedState;
}

bool Terrain3DRdShaderLibrary::initializeSharedState( void ) {
	if ( s_pRdShaderSharedState ) { return true; }
	s_pRdShaderSharedState = new ( std::nothrow ) t3dRdShaderSharedState_t();
	return s_pRdShaderSharedState != NULL;
}

void Terrain3DRdShaderLibrary::shutdownSharedState( void ) {
	if ( !s_pRdShaderSharedState ) { return; }
	for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) {
		s_pRdShaderSharedState->m_physical[id].abandon();
	}
	delete s_pRdShaderSharedState;
	s_pRdShaderSharedState = NULL;
}

bool Terrain3DRdShaderLibrary::physicalCacheReady( void ) {
	const t3dRdShaderSharedState_t *const pState = t3dRdShaderSharedStateGet();
	return pState && pState->m_nPhysicalUsers != 0u
	        && pState->m_nPhysicalPrewarmCursor == T3D_RD_SHADER_COUNT
	        && pState->m_nPhysicalFailureCount == 0u;
}

// t3dRdShaderSourceStage_e centralized in Terrain3DTypes.h.

static uint64_t t3dRdShaderTicksMsec( void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_msec() : 0u;
}

static uint64_t t3dRdShaderTicksUsec( void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_usec() : 0u;
}

static bool t3dRdShaderCatalogEntryEnabled( const t3dRdShaderId_t id ) {
	return true;
}

static uint32_t t3dRdExpectedPhysicalShaderCount( void ) {
	uint32_t nCount = 0u;
	for ( uint32_t value = 0u; value < T3D_RD_SHADER_COUNT; ++value ) {
		const t3dRdShaderId_t id = ( t3dRdShaderId_t )value;
		if ( !t3dRdShaderCatalogEntryEnabled( id ) ) { continue; }
		if ( t3dRdShaderCatalogGet( id ).m_domain == T3D_RD_SHADER_GLOBAL_DEVICE ) ++nCount;
	}
	return nCount;
}

static uint32_t t3dRdExpectedShaderLoadCount( void ) {
	uint32_t nCount = 0u;
	for ( uint32_t value = 0u; value < T3D_RD_SHADER_COUNT; ++value ) {
		if ( t3dRdShaderCatalogEntryEnabled( ( t3dRdShaderId_t )value ) ) ++nCount;
	}
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
	if ( !t3dRdShaderSharedStateGet() ) { return Ref<RDShaderFile>(); }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	for ( uint32_t iSource = 0u; iSource < m_nShaderSourceCount; ++iSource ) {
		if ( rPath == String( t3dRdShaderCatalogSourcePath( iSource ) ) ) {
			Terrain3DScopedLock lock( state.m_sourceMutex );
			return state.m_sourceFiles[iSource];
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

Ref<RDShaderSPIRV> Terrain3DRdShaderLibrary::loadCatalogShader( const t3dRdShaderId_t id ) {
	if ( !t3dRdShaderSharedStateGet() ) { return Ref<RDShaderSPIRV>(); }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( id >= T3D_RD_SHADER_COUNT ) { return Ref<RDShaderSPIRV>(); }
	const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
	if ( !entry.m_path ) { return Ref<RDShaderSPIRV>(); }
	Ref<RDShaderSPIRV> cached;
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
		if ( state.m_spirv[id].is_null() ) state.m_spirv[id] = cached;
		return state.m_spirv[id];
	}
	return cached;
}

Ref<RDShaderSPIRV> Terrain3DRdShaderLibrary::getCatalogShader( const t3dRdShaderId_t id ) const {
	return borrowSpirv( id );
}

Ref<RDShaderSPIRV> Terrain3DRdShaderLibrary::borrowSpirv( const t3dRdShaderId_t id ) {
	if ( !t3dRdShaderSharedStateGet() ) { return Ref<RDShaderSPIRV>(); }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( id >= T3D_RD_SHADER_COUNT ) { return Ref<RDShaderSPIRV>(); }
	Terrain3DScopedLock lock( state.m_sourceMutex );
	return state.m_spirv[id];
}

Ref<RDShaderSPIRV> Terrain3DRdShaderLibrary::resolveComputeShader( const t3dRdShaderId_t id ) {
	if ( id >= T3D_RD_SHADER_COUNT ) { return Ref<RDShaderSPIRV>(); }
	const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
	return entry.m_category == T3D_RD_SHADER_CATEGORY_COMPUTE ? borrowSpirv( id ) : Ref<RDShaderSPIRV>();
}

static void t3dRdSetDynamicShaderBuffers( const Ref<RDShaderSPIRV> &spirv, const PackedInt64Array &bindings ) {
	if ( spirv.is_valid() && spirv->has_method( StringName( "set_dynamic_buffers" ) ) ) {
		spirv->call( StringName( "set_dynamic_buffers" ), bindings );
	}
}

RID Terrain3DRdShaderLibrary::borrowPhysical( const t3dRdShaderId_t id ) {
	if ( !t3dRdShaderSharedStateGet() ) { return RID(); }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( id >= T3D_RD_SHADER_COUNT || !physicalCacheReady() ) { return RID(); }
	const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
	if ( entry.m_domain != T3D_RD_SHADER_GLOBAL_DEVICE || !state.m_physical[id].isValid() ) {
		++state.m_nPhysicalMissAfterReadyCount;
		UtilityFunctions::push_error( "[Terrain3D][ShaderCache] Missing physical shader after READY, catalog ID=", ( int64_t )id );
		return RID();
	}
	return state.m_physical[id].rid();
}

RID Terrain3DRdShaderLibrary::acquirePhysicalCold( Terrain3DRdContext& context, const t3dRdShaderId_t id )
{
	if ( !t3dRdShaderSharedStateGet() || id >= T3D_RD_SHADER_COUNT || !physicalCacheReady() ) {
		return RID();
	}
	t3dRdShaderSharedState_t& state = t3dRdShaderSharedState();
	if ( !context.getDevice() || context.getDeviceGeneration() != state.m_nPhysicalDeviceGeneration || t3dRdShaderCatalogGet( id ).m_domain != T3D_RD_SHADER_GLOBAL_DEVICE ) {
		return RID();
	}
	if ( state.m_physical[ id ].isValid() ) {
		return borrowPhysical( id );
	}
	const Ref<RDShaderSPIRV> spirv = borrowSpirv( id );
	const String name = String( "T3D.ShaderCache." ) + String::num_uint64( id );
	context.beginResourceRebuild();
	const bool created = spirv.is_valid() && state.m_physical[ id ].createTracked( context, T3D_RD_CONTEXT_OWNER_SHADER_CACHE, name, spirv );
	context.endResourceRebuild();
	if ( !created ) {
		++state.m_nPhysicalDemandFailureCount;
		return RID();
	}
	++state.m_nPhysicalReadyCount;
	++state.m_nPhysicalDemandCreateCount;
	return state.m_physical[ id ].rid();
}

bool Terrain3DRdShaderLibrary::isPhysicalReady( void ) const {
	if ( !t3dRdShaderSharedStateGet() ) { return false; }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	return m_bPhysicalUser && m_nPhysicalDeviceGeneration == state.m_nPhysicalDeviceGeneration
	        && state.m_nPhysicalPrewarmCursor == T3D_RD_SHADER_COUNT && state.m_nPhysicalFailureCount == 0u;
}

bool Terrain3DRdShaderLibrary::servicePhysicalPrewarm( Terrain3DRdContext &context, const uint32_t budget ) {
	if ( !t3dRdShaderSharedStateGet() ) { return false; }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !context.getDevice() || !isCpuReady( m_nStartupGeneration ) ) { return false; }
	const uint64_t generation = context.getDeviceGeneration();
	if ( state.m_nPhysicalDeviceGeneration != generation ) {
		for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) state.m_physical[id].abandon();
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
	if ( state.m_nPhysicalFailureCount != 0u ) { return false; }
	if ( isPhysicalReady() ) { return true; }
	if ( !state.m_bPhysicalMetadataPrepared ) {
		bool persistent = Terrain3DRdGpuBuffer::supportsPersistentMapping( context.getDevice() );
		for ( uint32_t id = 0u; id < T3D_RD_SHADER_NEAR_ALIAS && persistent; ++id ) {
			const Ref<RDShaderSPIRV> spirv = borrowSpirv( ( t3dRdShaderId_t )id );
			persistent = spirv.is_valid() && spirv->has_method( StringName( "set_dynamic_buffers" ) );
		}
		const Ref<RDShaderSPIRV> near_spirv = borrowSpirv( T3D_RD_SHADER_NEAR_ALIAS );
		const Ref<RDShaderSPIRV> middle_spirv = borrowSpirv( T3D_RD_SHADER_MIDDLE_ALIAS );
		const Ref<RDShaderSPIRV> far_spirv = borrowSpirv( T3D_RD_SHADER_FAR_ALIAS );
		const Ref<RDShaderSPIRV> cull_spirv = borrowSpirv( T3D_RD_SHADER_TERRAIN_CULL );
		persistent = persistent && near_spirv.is_valid() && middle_spirv.is_valid() && far_spirv.is_valid() && cull_spirv.is_valid()
		        && near_spirv->has_method( StringName( "set_dynamic_buffers" ) )
		        && middle_spirv->has_method( StringName( "set_dynamic_buffers" ) )
		        && far_spirv->has_method( StringName( "set_dynamic_buffers" ) )
		        && cull_spirv->has_method( StringName( "set_dynamic_buffers" ) );
		if ( persistent ) {
			PackedInt64Array terrain;
			static const int64_t s_terrainDynamicBindings[] = {
				0, 1, 8, 9, 13, 23, ( int64_t )( 2ULL << 32u )
			};
			for ( const int64_t binding : s_terrainDynamicBindings ) terrain.push_back( binding );
			for ( uint32_t id = 0u; id < T3D_RD_SHADER_NEAR_ALIAS; ++id ) {
				t3dRdSetDynamicShaderBuffers( borrowSpirv( ( t3dRdShaderId_t )id ), terrain );
			}
			t3dRdSetDynamicShaderBuffers( near_spirv, terrain );
			t3dRdSetDynamicShaderBuffers( middle_spirv, terrain );
			t3dRdSetDynamicShaderBuffers( far_spirv, terrain );
			PackedInt64Array cull;
			cull.push_back( 0 );
			cull.push_back( 9 );
			t3dRdSetDynamicShaderBuffers( cull_spirv, cull );
			PackedInt64Array world_mesh;
			world_mesh.push_back( 0 );
			world_mesh.push_back( ( int64_t )( 2ULL << 32u ) );
			PackedInt64Array world_strip = terrain;
			for ( uint32_t id = T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE; id < T3D_RD_SHADER_WORLD_RASTER_MESH_BASE; ++id ) {
				t3dRdSetDynamicShaderBuffers( borrowSpirv( ( t3dRdShaderId_t )id ), world_strip );
			}
			for ( uint32_t id = T3D_RD_SHADER_WORLD_RASTER_MESH_BASE; id < T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW; ++id ) {
				t3dRdSetDynamicShaderBuffers( borrowSpirv( ( t3dRdShaderId_t )id ), world_mesh );
			}
			t3dRdSetDynamicShaderBuffers( borrowSpirv( T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW ), world_strip );
			t3dRdSetDynamicShaderBuffers( borrowSpirv( T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW ), world_mesh );
		}
		if ( Terrain3DRdGpuBuffer::supportsPersistentMapping( context.getDevice() ) ) {
			PackedInt64Array displacement;
			displacement.push_back( 4 );
			displacement.push_back( 5 );
			t3dRdSetDynamicShaderBuffers( borrowSpirv( T3D_RD_SHADER_DISPLACEMENT_COMPUTE ), displacement );
		}
		state.m_bPhysicalMetadataPrepared = true;
	}
	const uint32_t limit = budget > 0u ? budget : 1u;
	for ( uint32_t work = 0u; work < limit && state.m_nPhysicalPrewarmCursor < T3D_RD_SHADER_COUNT; ++state.m_nPhysicalPrewarmCursor ) {
		const uint32_t nCursor = state.m_nPhysicalPrewarmCursor.load();
		const t3dRdShaderId_t id = ( t3dRdShaderId_t )nCursor;
		if ( !t3dRdShaderCatalogEntryEnabled( id ) || !t3dRdShaderNeedsStartupPhysical( id ) ) {
			continue;
		}
		const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
		if ( entry.m_domain != T3D_RD_SHADER_GLOBAL_DEVICE ) { continue; }
		const Ref<RDShaderSPIRV> spirv = borrowSpirv( id );
		if ( spirv.is_null() ) {
			++state.m_nPhysicalFailureCount;
			m_lastError = String( "Physical RD shader cache is missing catalog SPIR-V: id=" ) + String::num_uint64( id );
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

void Terrain3DRdShaderLibrary::releasePhysical( Terrain3DRdContext &context ) {
	if ( !t3dRdShaderSharedStateGet() ) { m_bPhysicalUser = false; return; }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
	if ( !m_bPhysicalUser ) { return; }
	m_bPhysicalUser = false;
	if ( m_nPhysicalDeviceGeneration != state.m_nPhysicalDeviceGeneration || state.m_nPhysicalUsers == 0u ) { return; }
	--state.m_nPhysicalUsers;
	if ( state.m_nPhysicalUsers != 0u ) { return; }
	for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) {
		if ( context.getDevice() ) state.m_physical[id].releaseTracked( context );
		else state.m_physical[id].abandon();
	}
	state.m_nPhysicalPrewarmCursor = 0u;
	state.m_nPhysicalReadyCount = 0u;
	state.m_nPhysicalFailureCount = 0u;
	state.m_nPhysicalMissAfterReadyCount = 0u;
	state.m_nPhysicalDemandCreateCount = 0u;
	state.m_nPhysicalDemandFailureCount = 0u;
	state.m_bPhysicalMetadataPrepared = false;
}

bool Terrain3DRdShaderLibrary::beginLoadCold( const uint32_t nStartupGeneration )
{
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
		const String path = String( t3dRdShaderCatalogSourcePath( iSource ) );
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
	if ( !t3dRdShaderSharedStateGet() ) { return; }
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
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
		const String path = String( t3dRdShaderCatalogSourcePath( iSource ) );
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
		{
			Terrain3DScopedLock lock( state.m_sourceMutex );
			if ( state.m_sourceFiles[iSource].is_null() ) state.m_sourceFiles[iSource] = shaderFile;
		}
		m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_READY;
		++nReady;
	}
	m_nShaderSourceReadyCount = nReady;
	if ( nReady != m_nShaderSourceCount ) {
		return;
	}
	UtilityFunctions::print( "[Terrain3D][ShaderLoad] SOURCES READY generation=", m_nStartupGeneration, " ready=", nReady, "/", m_nShaderSourceCount );
	if ( !resolveCatalogCold() ) {
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
	for ( uint32_t iSource = 0u; iSource < T3D_RD_SHADER_SOURCE_CAPACITY; ++iSource ) {
		if ( m_szSourceStages[iSource] != T3D_RD_SHADER_SOURCE_FAILED ) {
			m_szSourceStages[iSource] = T3D_RD_SHADER_SOURCE_IDLE;
		}
	}
	( void )t3dStartupDomainCancel( m_startupState, nStartupGeneration, t3dRdShaderTicksUsec() );
}

bool Terrain3DRdShaderLibrary::resolveCatalogCold( void ) {
	for ( uint32_t index = 0u; index < T3D_RD_SHADER_COUNT; ++index ) {
		if ( loadCatalogShader( ( t3dRdShaderId_t )index ).is_null() ) { return abortShaderLoad(); }
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

Dictionary Terrain3DRdShaderLibrary::getDiagnostics( void ) const {
	if ( !t3dRdShaderSharedStateGet() ) {
		Dictionary unavailable;
		unavailable["shared_state_initialized"] = false;
		return unavailable;
	}
	t3dRdShaderSharedState_t &state = t3dRdShaderSharedState();
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
	result["physical_shader_ready"] = ( int64_t )( state.m_nPhysicalReadyCount );
	result["physical_shader_expected"] = ( int64_t )( t3dRdExpectedPhysicalShaderCount() );
	result["physical_shader_failed"] = ( int64_t )( state.m_nPhysicalFailureCount );
	result["physical_shader_generation"] = ( int64_t )( state.m_nPhysicalDeviceGeneration );
	result["physical_shader_cache_users"] = ( int64_t )( state.m_nPhysicalUsers );
	result["physical_shader_misses_after_ready"] = ( int64_t )( state.m_nPhysicalMissAfterReadyCount );
	result[ "physical_shader_demand_created" ] = (int64_t)( state.m_nPhysicalDemandCreateCount );
	result[ "physical_shader_demand_failed" ] = (int64_t)( state.m_nPhysicalDemandFailureCount );
	uint32_t startup_physical_count = 0u;
	for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) {
		startup_physical_count += t3dRdShaderNeedsStartupPhysical( (t3dRdShaderId_t)id ) ? 1u : 0u;
	}
	result[ "physical_shader_startup_expected" ] = (int64_t)startup_physical_count;
	uint32_t catalog_by_category[5]{};
	uint32_t spirv_by_category[5]{};
	uint32_t physical_by_category[5]{};
	uint32_t catalog_by_domain[2]{};
	uint32_t spirv_by_domain[2]{};
	uint32_t physical_by_domain[2]{};
	{
		Terrain3DScopedLock lock( state.m_sourceMutex );
		for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) {
			const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( ( t3dRdShaderId_t )id );
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
		result[String( "catalog_count_" ) + categories[i]] = ( int64_t )catalog_by_category[i];
		result[String( "spirv_ready_" ) + categories[i]] = ( int64_t )spirv_by_category[i];
		result[String( "physical_ready_" ) + categories[i]] = ( int64_t )physical_by_category[i];
	}
	result["catalog_shader_count"] = ( int64_t )( catalog_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE] + catalog_by_domain[T3D_RD_SHADER_LOCAL_DEVICE] );
	result["catalog_global_count"] = ( int64_t )catalog_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE];
	result["catalog_local_count"] = ( int64_t )catalog_by_domain[T3D_RD_SHADER_LOCAL_DEVICE];
	result["spirv_ready_global"] = ( int64_t )spirv_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE];
	result["spirv_ready_local"] = ( int64_t )spirv_by_domain[T3D_RD_SHADER_LOCAL_DEVICE];
	result["physical_ready_global"] = ( int64_t )physical_by_domain[T3D_RD_SHADER_GLOBAL_DEVICE];
	result["physical_ready_local"] = ( int64_t )physical_by_domain[T3D_RD_SHADER_LOCAL_DEVICE];
	result["near_loaded"] = getShader( T3D_RD_SHADER_NEAR_ALIAS ).is_valid();
	result["middle_loaded"] = getShader( T3D_RD_SHADER_MIDDLE_ALIAS ).is_valid();
	result["far_loaded"] = getShader( T3D_RD_SHADER_FAR_ALIAS ).is_valid();
	result["cull_loaded"] = getShader( T3D_RD_SHADER_TERRAIN_CULL ).is_valid();
	result["instances_loaded"] = getShader( T3D_RD_SHADER_INSTANCE_ALIAS ).is_valid();
	result["instance_occluder_depth_loaded"] = getShader( T3D_RD_SHADER_INSTANCE_OCCLUDER_DEPTH ).is_valid();
	result["hlod_cull_loaded"] = getShader( T3D_RD_SHADER_HLOD_CULL ).is_valid();
	result["world_hiz_loaded"] = getShader( T3D_RD_SHADER_WORLD_HIZ_REDUCE ).is_valid();
	result["world_visibility_loaded"] =
	        getWorldVisibilityReset().is_valid() && getWorldVisibilityCull().is_valid() && getWorldVisibilityFinalize().is_valid();
	result["world_raster_terrain_strip_loaded"] = getWorldRasterTerrainStrip( ( t3dRdForwardShaderVariant_t )0 ).is_valid() && getWorldRasterTerrainStripShadow().is_valid();
	result["world_raster_mesh_loaded"] = getWorldRasterMesh( ( t3dRdForwardShaderVariant_t )0 ).is_valid() && getWorldRasterMeshShadow().is_valid();
	result["ocean_color_loaded"] = getShader( T3D_RD_SHADER_OCEAN_COLOR ).is_valid();
	result["ocean_shadow_loaded"] = getShader( T3D_RD_SHADER_OCEAN_SHADOW ).is_valid();
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

// M38 extracted non-template header implementations.
void Terrain3DRdShaderLibrary::setStartupGeneration( const uint32_t nGeneration ) {
		m_nStartupGeneration = nGeneration;
		t3dStartupDomainRequest( m_startupState, nGeneration, 0u, 0u );
	}

bool Terrain3DRdShaderLibrary::isReady( void ) const {
		if ( !isCpuReady( m_nStartupGeneration ) ) { return false; }
		for ( uint32_t id = 0u; id < T3D_RD_SHADER_COUNT; ++id ) {
			if ( getCatalogShader( ( t3dRdShaderId_t )id ).is_null() ) { return false; }
		}
		return true;
	}
