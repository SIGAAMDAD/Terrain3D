// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "authoring/editor/terrain_3d_editor.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_integration_helpers.h"
#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "lighting/policy/terrain_3d_lighting_policy.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "simd/core/terrain_3d_parallel.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"

#include <EASTL/numeric_limits.h>
#include <assert.h>
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"
#include "terrain_3d.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

static EA_FORCE_INLINE uint64_t t3dSaturatingAddMemoryBytes( const uint64_t nA, const uint64_t nB ) {
	return nA > eastl::numeric_limits<uint64_t>::max() - nB ? eastl::numeric_limits<uint64_t>::max() : nA + nB;
}

t3dAllocatorOverflowPolicy_t Terrain3D::getRuntimeAllocatorOverflowPolicyInternal( void ) const {
	return TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
}

bool Terrain3D::initializeRuntimeMemory( void ) {
	if ( m_bRuntimeMemoryReady ) {
		if ( m_runtimeMemoryZone.state() != T3D_MEMORY_ZONE_ACTIVE ) {
			return false;
		}
		if ( m_pData
		        && !m_pData->bindRuntimeAllocatorInternal( &m_runtimeMemoryZone,
		                TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT ) ) {
			return false;
		}
		if ( m_pData && m_pData->isActiveStorageOwnerInternal() ) {
			return true;
		}
		const t3dMemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
		return m_pData && active_span.valid() && m_pData->bindActiveStorageInternal( active_span.m_pData, active_span.m_nSize );
	}
	// Worker and container allocations may bind explicitly to this private runtime
	// zone. Active and resident page banks retain independent storage and must
	// never depend on a process-global allocator router.
	const size_t active_bytes = t3dActiveStorageRequiredBytes();
	const size_t zone_bytes = calculateRuntimeAllocatorZoneBytes();
	if ( active_bytes == 0u || zone_bytes < T3D_SMALL_PAGE_BYTES
	        || active_bytes > eastl::numeric_limits<size_t>::max() - zone_bytes - 2u * T3D_ARENA_DEFAULT_BASE_ALIGNMENT ) {
		return false;
	}
	Terrain3DMemoryArenaLayout layout( active_bytes + zone_bytes + 2u * T3D_ARENA_DEFAULT_BASE_ALIGNMENT );
	if ( !layout.reserveLow( active_bytes, T3D_ACTIVE_STORAGE_ALIGNMENT, m_runtimeActiveHeightSlice )
	        || !layout.reserveLow( zone_bytes, T3D_ARENA_DEFAULT_BASE_ALIGNMENT, m_runtimeZoneSlice ) || !layout.seal()
	        || !m_runtimeMemoryArena.initialize( layout ) || !m_runtimeMemoryArena.commit() ) {
		return false;
	}
	const t3dMemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
	const t3dMemoryArenaSpan_t zone_span = m_runtimeMemoryArena.resolve( m_runtimeZoneSlice );
	if ( !m_pData || !active_span.valid() || !zone_span.valid()
	        || !m_runtimeMemoryZone.initialize( zone_span.m_pData, zone_span.m_nSize ) ) {
		m_runtimeMemoryArena.release();
		return false;
	}
	const t3dAllocatorOverflowPolicy_t overflow_policy =
	        TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
	if ( !m_pData->bindRuntimeAllocatorInternal( &m_runtimeMemoryZone, overflow_policy )
	        || !m_pData->bindActiveStorageInternal( active_span.m_pData, active_span.m_nSize ) || !m_runtimeMemoryArena.seal() ) {
		m_pData->deactivateRuntimeStorage( this );
		m_runtimeMemoryZone.beginDraining();
		m_runtimeMemoryZone.release();
		m_runtimeMemoryArena.release();
		return false;
	}
	m_bRuntimeMemoryReady = true;
	return true;
}

bool Terrain3D::beginRuntimeMemoryDrain( void ) {
	if ( !m_bRuntimeMemoryReady ) {
		return true;
	}
	const t3dMemoryZoneState_t state = m_runtimeMemoryZone.state();
	if ( state == T3D_MEMORY_ZONE_ACTIVE ) {
		return m_runtimeMemoryZone.beginDraining();
	}
	return state == T3D_MEMORY_ZONE_DRAINING || state == T3D_MEMORY_ZONE_DEAD || state == T3D_MEMORY_ZONE_UNINITIALIZED;
}

bool Terrain3D::releaseRuntimeMemory( void ) {
	if ( !m_bRuntimeMemoryReady ) {
		return true;
	}
	if ( !beginRuntimeMemoryDrain() || ( m_pData && m_pData->isActiveStorageOwnerInternal() ) ) {
		return false;
	}
	if ( m_runtimeMemoryZone.liveAllocations() != 0u ) {
		return false;
	}
#if !defined( NDEBUG )
	assert( m_runtimeMemoryZone.liveAllocations() == 0u );
#endif
	if ( !m_runtimeMemoryZone.release() ) {
		return false;
	}
	m_runtimeMemoryArena.release();
	m_runtimeActiveHeightSlice = {};
	m_runtimeZoneSlice = {};
	m_bRuntimeMemoryReady = false;
	return true;
}

bool Terrain3D::shutdownRuntimeMemoryImage( void ) {
	if ( !m_bRuntimeMemoryReady ) {
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_COLD;
		return true;
	}
	m_runtimeLifecycleState = TERRAIN3D_RUNTIME_SHUTTING_DOWN;
	if ( releaseRuntimeMemory() ) {
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_COLD;
		return true;
	}
	const t3dMemoryZoneStats_t stats = m_runtimeMemoryZone.getStats();
	LOG( ERROR,
	        "[lifecycle] Final runtime memory drain failed: live allocations=",
	        stats.m_nLiveAllocationCount,
	        "; live requested bytes=",
	        stats.m_nLiveRequestedBytes,
	        "; live physical bytes=",
	        stats.m_nLivePhysicalBytes,
	        "; zone_state=",
	        ( int )( m_runtimeMemoryZone.state() ) );
	m_runtimeLifecycleState = TERRAIN3D_RUNTIME_DRAIN_FAILED;
	return false;
}

void Terrain3D::markMemoryBudgetDirty( void ) {
	m_bMemoryBudgetPolicyDirty = true;
}

const char *Terrain3D::memoryLimitReasonName( const t3dMemoryLimitReason_t reason ) {
	switch ( reason ) {
		case TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET:
			return "total_budget";
		case TERRAIN3D_MEMORY_LIMIT_FIXED_CAP:
			return "fixed_cap";
		case TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE:
			return "manual_override";
		case TERRAIN3D_MEMORY_LIMIT_DISABLED:
			return "disabled";
		case TERRAIN3D_MEMORY_LIMIT_NONE:
		default:
			return "none";
	}
}

size_t Terrain3D::calculateRuntimeAllocatorZoneBytes( void ) const {
	const uint64_t total_bytes = ( uint64_t )m_nTotalMemoryBudgetMb * T3D_MEMORY_MIB;
	const uint64_t transient_bytes = ( uint64_t )m_nMemoryTransientReserveMb * T3D_MEMORY_MIB;
	const size_t active_bytes = t3dActiveStorageRequiredBytes();
	uint64_t target = total_bytes / T3D_RUNTIME_ALLOCATOR_BUDGET_DIVISOR;
	target = MAX( target, ( uint64_t )T3D_RUNTIME_ALLOCATOR_MIN_BYTES );
	target = MIN( target, ( uint64_t )T3D_RUNTIME_ALLOCATOR_MAX_BYTES );
	const uint64_t fixed_floor = ( uint64_t )active_bytes + transient_bytes;
	if ( total_bytes <= fixed_floor + T3D_SMALL_PAGE_BYTES ) {
		return 0u;
	}
	target = MIN( target, total_bytes - fixed_floor );
	target &= ~( ( uint64_t )T3D_SMALL_PAGE_BYTES - 1u );
	return target >= T3D_SMALL_PAGE_BYTES && target <= SIZE_MAX ? ( size_t )target : 0u;
}

uint64_t Terrain3D::calculateFixedMemoryBytes( void ) const {
	uint64_t bytes = sizeof( *this );
	const uint64_t render_reserve = MIN(uint64_t(m_nTotalMemoryBudgetMb) * T3D_MEMORY_MIB / 4u, 128ull * T3D_MEMORY_MIB);
	uint64_t render_owned = m_pRenderCoordinator ? m_pRenderCoordinator->getRetainedCpuBytes() : 0u;
	if (m_pRenderCoordinator) {
		render_owned = t3dSaturatingAddMemoryBytes(render_owned, m_pRenderCoordinator->getWorldGpuBufferBytes());
		render_owned = t3dSaturatingAddMemoryBytes(render_owned, m_pRenderCoordinator->getWorldHiZGpuBytes());
		render_owned = t3dSaturatingAddMemoryBytes(render_owned, m_pRenderCoordinator->getVirtualTextureGpuBytes());
	}
	// VT images in the runtime zone are paid for by the allocator reserve.
	const uint64_t vt_zone = m_pRenderCoordinator ? m_pRenderCoordinator->getVirtualTextureCpuBytes() : 0u;
	render_owned = render_owned > vt_zone ? render_owned - vt_zone : 0u;
	bytes = t3dSaturatingAddMemoryBytes(bytes, MAX(render_reserve, render_owned));
	if ( m_pData ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pData->getMemoryFixedRepositoryBytes() ) );
		const uint64_t fixed_far_atlas_bytes = getGpuFarAtlasBytes();
		bytes = t3dSaturatingAddMemoryBytes( bytes, fixed_far_atlas_bytes );
	}
	if ( m_assets.is_valid() ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_assets->getGeneratedTexturePayloadBytes() ) );
	}
	if ( m_pTerrainMesher ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pTerrainMesher->getVisibleGeometryPayloadBytes() ) );
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pTerrainMesher->getShadowGeometryPayloadBytes() ) );
	}
	if ( m_pInstancer ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( sizeof( Terrain3DInstancer ) ) );
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pInstancer->getActiveMultimeshBytes() ) );
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pInstancer->getRetainedContainerBytes() ) );
	}
	if ( m_pCollision ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( sizeof( Terrain3DCollision ) ) );
	}
	if ( m_pNodeStreamer ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( sizeof( Terrain3DNodeStreamer ) ) );
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pNodeStreamer->getRetainedCapacityBytes() ) );
	}
	if ( m_pLightmapStreamer ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( sizeof( Terrain3DLightmapStreamer ) ) );
	}
	if ( m_pRuntimeProfileHistory ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( sizeof( t3dRuntimeProfileHistory_t ) ) );
	}
	return bytes;
}

uint64_t Terrain3D::calculateAllocatorContainedFixedMemoryBytes( void ) const {
	uint64_t bytes = 0u;
	if ( m_pData ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pData->getMemoryAllocatorContainedRepositoryBytes() ) );
	}
	if ( m_pInstancer ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pInstancer->getRetainedContainerBytes() ) );
	}
	if ( m_pNodeStreamer ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pNodeStreamer->getRetainedCapacityBytes() ) );
	}
	return bytes;
}

uint64_t Terrain3D::calculateTransientMemoryBytes( void ) const {
	uint64_t bytes = 0u;
	if ( m_pData ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pData->getMemoryTransientStagingBytes() ) );
	}
	if ( m_pTerrainMesher ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pTerrainMesher->getStreamingBufferBytes() ) );
	}
	if ( m_pCollision ) {
		bytes = t3dSaturatingAddMemoryBytes( bytes, ( uint64_t )( m_pCollision->getRetainedBufferBytes() ) );
	}
	return bytes;
}

t3dMemoryBudgetInput_t Terrain3D::buildMemoryBudgetInput( void ) const {
	t3dMemoryBudgetInput_t input;
	input.m_nTotalBudgetBytes = ( uint64_t )( m_nTotalMemoryBudgetMb )*T3D_MEMORY_MIB;
	input.m_nFixedBytes = calculateFixedMemoryBytes();
	input.m_nAllocatorContainedFixedBytes = calculateAllocatorContainedFixedMemoryBytes();
	input.m_nAllocatorReserveBytes = m_runtimeMemoryZone.capacity() > 0u ? ( uint64_t )m_runtimeMemoryZone.capacity()
	                                                                     : ( uint64_t )calculateRuntimeAllocatorZoneBytes();
	input.m_nTransientReserveBytes = ( uint64_t )( m_nMemoryTransientReserveMb )*T3D_MEMORY_MIB;

	t3dMemorySubsystemInput_t &gpu = input.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.m_bEnabled = effectiveGpuRegionCacheEnabled();
	gpu.m_bAutomatic = m_bGpuRegionCacheBudgetAuto;
	gpu.m_nManualBytes = ( uint64_t )( m_nGpuRegionCacheBudgetMb )*T3D_MEMORY_MIB;
	gpu.m_nMinimumBytes = m_pData ? ( uint64_t )( m_pData->getGpuRegionCacheMinimumBytes() ) : T3D_MEMORY_MIB;
	gpu.m_nHardCapBytes =
	        m_pData ? ( uint64_t )( m_pData->getGpuRegionCacheHardCapBytes() ) : ( uint64_t )( T3D_GPU_REGION_CACHE_MAX_BUDGET_MB )*T3D_MEMORY_MIB;
	gpu.m_nWeight = 5u;

	t3dMemorySubsystemInput_t &cpu = input.m_szSubsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES];
	cpu.m_bEnabled = effectiveCpuRegionCacheEnabled();
	cpu.m_bAutomatic = m_bCpuRegionCacheBudgetAuto;
	cpu.m_nManualBytes = ( uint64_t )( m_nCpuRegionCacheBudgetMb )*T3D_MEMORY_MIB;
	cpu.m_nMinimumBytes = m_pData ? ( uint64_t )( m_pData->getCpuRegionCacheMinimumBytes() ) : 4u * T3D_MEMORY_MIB;
	cpu.m_nHardCapBytes = m_pData ? ( uint64_t )( m_pData->getCpuRegionCacheHardCapBytes() ) : 128u * T3D_MEMORY_MIB;
	cpu.m_nWeight = 4u;

	t3dMemorySubsystemInput_t &instances = input.m_szSubsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES];
	instances.m_bEnabled = effectiveInstanceRegionCacheEnabled();
	instances.m_bAutomatic = m_bInstanceRegionCacheBudgetAuto;
	instances.m_nManualBytes = ( uint64_t )( m_nInstanceRegionCacheBudgetMb )*T3D_MEMORY_MIB;
	instances.m_nMinimumBytes = T3D_MEMORY_MIB;
	instances.m_nHardCapBytes = 128u * T3D_MEMORY_MIB;
	instances.m_nWeight = 2u;

	t3dMemorySubsystemInput_t &lighting = input.m_szSubsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING];
	lighting.m_bEnabled = m_bLightmapStreamingEnabled;
	lighting.m_bAutomatic = m_bLightmapResidencyBudgetAuto;
	lighting.m_nManualBytes = ( uint64_t )( m_nLightmapResidencyBudgetMb )*T3D_MEMORY_MIB;
	lighting.m_nMinimumBytes = T3D_MEMORY_MIB;
	lighting.m_nHardCapBytes = 1024u * T3D_MEMORY_MIB;
	lighting.m_nWeight = 2u;

	t3dMemorySubsystemInput_t &hlod = input.m_szSubsystems[TERRAIN3D_MEMORY_HLOD_PROXIES];
	hlod.m_bEnabled = m_bHlodEnabled;
	hlod.m_bAutomatic = m_bHlodMemoryBudgetAuto;
	hlod.m_nManualBytes = ( uint64_t )( m_nHlodMemoryBudgetMb )*T3D_MEMORY_MIB;
	hlod.m_nMinimumBytes = 4u * T3D_MEMORY_MIB;
	hlod.m_nHardCapBytes = 4096u * T3D_MEMORY_MIB;
	hlod.m_nWeight = 2u;

	t3dMemorySubsystemInput_t &shader = input.m_szSubsystems[TERRAIN3D_MEMORY_SHADER_CACHE];
	// M27: the material source/variant cache is retired. Production RD shaders are
	// owned by the fixed catalog cache and do not consume a user-sized material budget.
	shader.m_bEnabled = false;
	shader.m_bAutomatic = false;
	shader.m_nManualBytes = 0u;
	shader.m_nMinimumBytes = 0u;
	shader.m_nHardCapBytes = 0u;
	shader.m_nWeight = 0u;
	return input;
}

void Terrain3D::applyMemoryBudgetResult( const t3dMemoryBudgetResult_t &rResult, const bool bForce ) {
	const uint64_t previous_gpu_bytes = m_nEffectiveGpuRegionCacheBudgetBytes;
	const int previous_effective_mb[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_nEffectiveGpuRegionCacheBudgetMb,
		m_nEffectiveCpuRegionCacheBudgetMb,
		m_nEffectiveInstanceRegionCacheBudgetMb,
		m_nEffectiveLightmapResidencyBudgetMb,
		m_nEffectiveHlodMemoryBudgetMb,
		m_nEffectiveShaderCacheBudgetMb,
	};
	const uint64_t managed_gpu_page_budget_bytes = rResult.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_nEffectiveBytes;
	const int gpu_mb = managed_gpu_page_budget_bytes == 0u
	        ? 0
	        : ( int )( managed_gpu_page_budget_bytes / T3D_MEMORY_MIB + ( managed_gpu_page_budget_bytes % T3D_MEMORY_MIB != 0u ? 1u : 0u ) );
	// Whole-MiB APIs must round DOWN: a zero grant stays zero.
	const int cpu_mb = int(rResult.m_szSubsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES].m_nEffectiveBytes / T3D_MEMORY_MIB);
	const int instance_mb = int(rResult.m_szSubsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES].m_nEffectiveBytes / T3D_MEMORY_MIB);
	const int lightmap_mb = int(rResult.m_szSubsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING].m_nEffectiveBytes / T3D_MEMORY_MIB);
	const int hlod_mb = int(rResult.m_szSubsystems[TERRAIN3D_MEMORY_HLOD_PROXIES].m_nEffectiveBytes / T3D_MEMORY_MIB);
	const int shader_mb = 0;

	const int hysteresis_mb = ( int )( T3D_MEMORY_POLICY_HYSTERESIS_BYTES / T3D_MEMORY_MIB );
	const uint64_t gpu_delta = previous_gpu_bytes > managed_gpu_page_budget_bytes ? previous_gpu_bytes - managed_gpu_page_budget_bytes
	                                                                              : managed_gpu_page_budget_bytes - previous_gpu_bytes;
	const bool bGpuChanged = bForce || managed_gpu_page_budget_bytes < previous_gpu_bytes || gpu_delta >= T3D_MEMORY_POLICY_HYSTERESIS_BYTES;
	const bool bCpuChanged = bForce || cpu_mb < m_nEffectiveCpuRegionCacheBudgetMb || abs( cpu_mb - m_nEffectiveCpuRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bInstanceChanged = bForce || instance_mb < m_nEffectiveInstanceRegionCacheBudgetMb || abs( instance_mb - m_nEffectiveInstanceRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bLightmapChanged = bForce || lightmap_mb < m_nEffectiveLightmapResidencyBudgetMb || abs( lightmap_mb - m_nEffectiveLightmapResidencyBudgetMb ) >= hysteresis_mb;
	const bool bHlodChanged = bForce || hlod_mb < m_nEffectiveHlodMemoryBudgetMb || abs( hlod_mb - m_nEffectiveHlodMemoryBudgetMb ) >= hysteresis_mb;
	const bool bShaderChanged = bForce || abs( shader_mb - m_nEffectiveShaderCacheBudgetMb ) >= hysteresis_mb;

	if ( m_pData ) {
		uint64_t non_page_transient = ( uint64_t )m_pData->getMemoryNonPageTransientStagingBytes();
		if ( m_pTerrainMesher ) {
			non_page_transient = t3dSaturatingAddMemoryBytes( non_page_transient, ( uint64_t )m_pTerrainMesher->getStreamingBufferBytes() );
		}
		if ( m_pCollision ) {
			non_page_transient = t3dSaturatingAddMemoryBytes( non_page_transient, ( uint64_t )m_pCollision->getRetainedBufferBytes() );
		}
		const uint64_t runtime_page_grant =
		        rResult.m_nTransientReserveBytes > non_page_transient ? rResult.m_nTransientReserveBytes - non_page_transient : 0u;
		m_pData->setRuntimePagePreparedByteBudget( runtime_page_grant );
	}

	if ( bGpuChanged ) {
		m_nEffectiveGpuRegionCacheBudgetBytes = managed_gpu_page_budget_bytes;
		m_nEffectiveGpuRegionCacheBudgetMb = gpu_mb;
		if ( m_pData ) {
			reconfigureGpuRegionCache();
		}
	}
	if ( bCpuChanged ) {
		m_nEffectiveCpuRegionCacheBudgetMb = cpu_mb;
		if ( m_pData ) {
			m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(),
			        m_nEffectiveCpuRegionCacheBudgetMb,
			        m_nCpuRegionLoadBudgetLayers,
			        m_nCpuRegionLoadBudgetMb );
		}
	}
	if ( bInstanceChanged ) {
		m_nEffectiveInstanceRegionCacheBudgetMb = instance_mb;
		if ( m_pData ) {
			m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
			        m_nEffectiveInstanceRegionCacheBudgetMb,
			        m_nInstanceRegionLoadBudgetPages,
			        m_nInstanceRegionLoadBudgetMb );
		}
	}
	if ( bLightmapChanged ) {
		m_nEffectiveLightmapResidencyBudgetMb = lightmap_mb;
		if ( m_pLightmapStreamer ) {
			m_pLightmapStreamer->setResidencyBudgetMb( m_nEffectiveLightmapResidencyBudgetMb );
		}
	}
	if ( bHlodChanged ) {
		m_nEffectiveHlodMemoryBudgetMb = hlod_mb;
		if ( m_pHlodStreamer ) {
			m_pHlodStreamer->setMemoryBudgetMb( m_nEffectiveHlodMemoryBudgetMb );
		}
	}
	if ( bShaderChanged ) {
		m_nEffectiveShaderCacheBudgetMb = 0;
	}

	const int current_effective_mb[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_nEffectiveGpuRegionCacheBudgetMb,
		m_nEffectiveCpuRegionCacheBudgetMb,
		m_nEffectiveInstanceRegionCacheBudgetMb,
		m_nEffectiveLightmapResidencyBudgetMb,
		m_nEffectiveHlodMemoryBudgetMb,
		m_nEffectiveShaderCacheBudgetMb,
	};
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		if ( index == TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES ) {
			if ( m_nEffectiveGpuRegionCacheBudgetBytes == previous_gpu_bytes ) {
				continue;
			}
		} else if ( current_effective_mb[index] == previous_effective_mb[index] ) {
			continue;
		}
		emit_signal( "cache_budget_changed",
		        StringName( s_pszMemorySubsystemNames[index] ),
		        ( int64_t )( rResult.m_szSubsystems[( size_t )( index )].m_nEffectiveBytes ),
		        StringName( memoryLimitReasonName( rResult.m_szSubsystems[( size_t )( index )].m_limitingReason ) ) );
	}
}

void Terrain3D::serviceMemoryBudgetPolicy( const bool bForce ) {
	Terrain3DMemoryAccountScope memory_scope(m_memoryAccount.get());
	if ( !bForce ) {
		++m_nMemoryBudgetPolicyFrame;
		if ( !m_bMemoryBudgetPolicyDirty && ( m_nMemoryBudgetPolicyFrame % ( uint64_t )( T3D_MEMORY_POLICY_INTERVAL_FRAMES ) ) != 0u ) {
			return;
		}
	}
	const t3dMemoryBudgetInput_t input = buildMemoryBudgetInput();
	const t3dMemoryBudgetResult_t result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	applyMemoryBudgetResult( result, bForce || m_nMemoryBudgetPolicyGeneration == 0u );
	const bool bPressure = result.m_bHardOvercommit || result.m_nHeadroomBytes < T3D_MEMORY_POLICY_HYSTERESIS_BYTES;
	if ( bPressure != m_bMemoryPressureEventState || result.m_bHardOvercommit != m_bMemoryOvercommitEventState ) {
		m_bMemoryPressureEventState = bPressure;
		m_bMemoryOvercommitEventState = result.m_bHardOvercommit;
		emit_signal( "memory_pressure_changed", bPressure, result.m_bHardOvercommit, ( int64_t )( result.m_nHeadroomBytes ) );
	}
	m_nMemoryBudgetResult = result;
	m_bMemoryBudgetPolicyDirty = false;
	++m_nMemoryBudgetPolicyGeneration;
}

void Terrain3D::forceMemoryBudgetRebalance( void ) {
	serviceMemoryBudgetPolicy( true );
	notify_property_list_changed();
}

void Terrain3D::setTotalMemoryBudgetMb( const int nBudgetMb ) {
	m_memoryBudgetChangeError = String();
	const int requested_mb = CLAMP(nBudgetMb, 32, 4096);
	if (requested_mb == m_nTotalMemoryBudgetMb) { return; }
	Terrain3DMemoryAccount *const account = m_memoryAccount.get();
	if (!account) { return; }
	Terrain3DMemoryAccountScope memory_scope(account);
	Terrain3DColdPhaseScope cold_scope;
	const int previous_mb = m_nTotalMemoryBudgetMb;
	const bool had_runtime = m_bRuntimeMemoryReady;
	const uint64_t minimum_bytes = uint64_t(t3dActiveStorageRequiredBytes())
	        + T3D_RUNTIME_ALLOCATOR_MIN_BYTES + uint64_t(m_nMemoryTransientReserveMb) * T3D_MEMORY_MIB;
	if (had_runtime && uint64_t(requested_mb) * T3D_MEMORY_MIB < minimum_bytes) {
		m_memoryBudgetChangeError = "Total limit is below active storage, the minimum allocator and the transient reserve.";
		LOG(ERROR, "Requested memory budget is below the active storage, minimum allocator and transient reserve: ", minimum_bytes);
		return;
	}
	if (had_runtime) {
		// Stop readers and join workers before retiring their allocator backing.
		// Resource-owned maps and encoded instance pages remain in the repository.
		deactivateRuntime();
		destroyRenderCoordinator(true);
		destroyTerrainMesher(true);
		destroyOceanMesher(true);
		destroyCollision(true);
		if ((m_pData && !m_pData->releaseRuntimeAllocatorStorageInternal()) || !shutdownRuntimeMemoryImage()) {
			m_memoryBudgetChangeError = "Runtime storage could not be drained; the total budget was not changed.";
			LOG(ERROR, "Memory budget rebuild could not drain runtime storage; requested budget was not applied");
			return;
		}
	}
	if (!account->setLimit(uint64_t(requested_mb) * T3D_MEMORY_MIB)) {
		m_memoryBudgetChangeError = "Total limit is below retained authored/shared storage; the previous budget was restored.";
		LOG(ERROR, "Memory budget cannot be smaller than retained authored/shared storage; retained bytes=", account->stats().committed);
		m_nTotalMemoryBudgetMb = previous_mb;
	} else {
		m_nTotalMemoryBudgetMb = requested_mb;
	}
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if (had_runtime) {
		// Rebuild only runtime consumers. Startup recognizes the loaded repository.
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_COLD;
		initializeInternal();
		if (is_inside_tree() && m_bIsInsideWorld) { set_physics_process(true); }
	}
	notify_property_list_changed();
	update_configuration_warnings();
}

bool Terrain3D::setRenderMemoryBudgetMb(Terrain3DMemoryAccount::Domain domain, int budget_mb) {
	Terrain3DMemoryAccount *account = m_memoryAccount.get();
	if (!account || domain <= Terrain3DMemoryAccount::GENERAL || domain >= Terrain3DMemoryAccount::DOMAIN_COUNT) { return false; }
	const uint64_t bytes = uint64_t(CLAMP(budget_mb, 0, 4096)) * T3D_MEMORY_MIB;
	m_memoryBudgetChangeError = String();
	if (!account->setDomainLimit(domain, bytes)) {
		const uint64_t current = account->stats().domains[domain].committed;
		m_memoryBudgetChangeError = "Resource cap was not changed: live allocations require at least "
		        + String::num_uint64((current + T3D_MEMORY_MIB - 1u) / T3D_MEMORY_MIB)
		        + " MiB. Evict/rebuild resources or choose a higher cap.";
		LOG(INFO, m_memoryBudgetChangeError);
		return false;
	}
	notify_property_list_changed();
	return true;
}

int Terrain3D::getRenderMemoryBudgetMb(Terrain3DMemoryAccount::Domain domain) const {
	return m_memoryAccount.get() && domain > Terrain3DMemoryAccount::GENERAL && domain < Terrain3DMemoryAccount::DOMAIN_COUNT
	        ? int(m_memoryAccount.get()->stats().domains[domain].limit / T3D_MEMORY_MIB) : 0;
}

void Terrain3D::setCookingMemoryBudgetMb(int value) {
	if (m_memoryAccount.get()) { m_memoryAccount.get()->setCookingLimit(uint64_t(CLAMP(value, 0, 4096)) * T3D_MEMORY_MIB); }
}

int Terrain3D::getCookingMemoryBudgetMb() const {
	return m_memoryAccount.get() ? int(m_memoryAccount.get()->stats().cooking_limit / T3D_MEMORY_MIB) : 0;
}

Dictionary Terrain3D::getMemoryTrackerStats() const {
	Dictionary result;
	if (!m_memoryAccount.get()) { return result; }
	// Mutex-protected CPU accounting only: no RD query, readback, forceSync or
	// scene traversal. The editor can sample this without stalling the render thread.
	const auto stats = m_memoryAccount.get()->stats();
	result["limit_bytes"] = int64_t(stats.limit);
	result["current_bytes"] = int64_t(stats.committed);
	result["peak_bytes"] = int64_t(stats.peak);
	result["headroom_bytes"] = int64_t(stats.limit > stats.committed ? stats.limit - stats.committed : 0);
	result["denied_allocations"] = int64_t(stats.denied);
	result["cpu_bytes"] = int64_t(stats.bytes[Terrain3DMemoryAccount::CPU]);
	result["gpu_bytes"] = int64_t(stats.bytes[Terrain3DMemoryAccount::GPU]);
	result["external_payload_bytes"] = int64_t(stats.bytes[Terrain3DMemoryAccount::EXTERNAL]);
	result["cpu_peak_bytes"] = int64_t(stats.kind_peaks[Terrain3DMemoryAccount::CPU]);
	result["gpu_peak_bytes"] = int64_t(stats.kind_peaks[Terrain3DMemoryAccount::GPU]);
	result["external_payload_peak_bytes"] = int64_t(stats.kind_peaks[Terrain3DMemoryAccount::EXTERNAL]);
	result["last_budget_change_error"] = m_memoryBudgetChangeError;
	result["cooking_working_limit_bytes"] = int64_t(t3dMemoryCookingLimit(m_memoryAccount.get()));
	result["composition_working_limit_bytes"] = int64_t(t3dMemoryCompositionLimit(m_memoryAccount.get()));
	result["arena_reserved_bytes"] = int64_t(m_runtimeMemoryArena.capacity());
	const auto zone = m_runtimeMemoryZone.getStats();
	result["zone_used_bytes"] = int64_t(zone.m_nLivePhysicalBytes);
	result["zone_reserved_bytes"] = int64_t(zone.m_nReservedBytes);
	result["scope"] = "This Terrain3D account: native backing, retained payload reservations and RD buffer/texture capacity. Arena suballocations are already included in backing. Godot objects, imported assets, shader pipelines, driver padding and other nodes are outside this account.";
	static const char *const names[Terrain3DMemoryAccount::DOMAIN_COUNT] = {
		"general", "terrain_render", "instance_render", "virtual_texture", "world_render", "hlod_render", "ocean_render", "forward_lighting"
	};
	Array resources;
	for (int index = 1; index < Terrain3DMemoryAccount::DOMAIN_COUNT; ++index) {
		const auto &domain = stats.domains[index];
		Dictionary row;
		row["name"] = names[index];
		row["current_bytes"] = int64_t(domain.committed);
		row["peak_bytes"] = int64_t(domain.peak);
		row["requested_limit_bytes"] = int64_t(domain.limit);
		row["effective_limit_bytes"] = int64_t(domain.limit && domain.limit < stats.limit ? domain.limit : stats.limit);
		row["denied_allocations"] = int64_t(domain.denied);
		row["inherits_total"] = domain.limit == 0;
		resources.push_back(row);
	}
	result["resources"] = resources;
	Array residency;
	const uint64_t current[5] = {
		m_pData ? uint64_t(m_pData->getGpuRegionCacheReservedBytes()) : 0,
		m_pData ? uint64_t(m_pData->getCpuRegionCacheReservedBytes()) : 0,
		m_pData ? uint64_t(m_pData->getInstanceRegionCacheReservedBytes()) : 0,
		m_pLightmapStreamer ? uint64_t(m_pLightmapStreamer->getResidentEstimatedBytes()) : 0,
		m_pHlodStreamer ? uint64_t(m_pHlodStreamer->getResidentEstimatedBytes()) : 0
	};
	for (int index = 0; index < 5; ++index) {
		Dictionary row;
		row["name"] = s_pszMemorySubsystemNames[index];
		row["current_bytes"] = int64_t(current[index]);
		row["effective_budget_bytes"] = int64_t(m_nMemoryBudgetResult.m_szSubsystems[index].m_nEffectiveBytes);
		row["accuracy"] = index >= 3 ? "estimated payload" : "logical cache capacity";
		residency.push_back(row);
	}
	result["residency"] = residency;
	return result;
}

void Terrain3D::setMemoryTransientReserveMb( const int nReserveMb ) {
	const int reserve_mb = CLAMP( nReserveMb, 4, 1024 );
	if ( m_nMemoryTransientReserveMb == reserve_mb ) {
		return;
	}
	m_nMemoryTransientReserveMb = reserve_mb;
	markMemoryBudgetDirty();
}

void Terrain3D::setGpuRegionCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bGpuRegionCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bGpuRegionCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void Terrain3D::setCpuRegionCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bCpuRegionCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bCpuRegionCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void Terrain3D::setInstanceRegionCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bInstanceRegionCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bInstanceRegionCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void Terrain3D::setLightmapResidencyBudgetAuto( const bool bEnabled ) {
	if ( m_bLightmapResidencyBudgetAuto == bEnabled ) {
		return;
	}
	m_bLightmapResidencyBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void Terrain3D::setLightmapResidencyBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 1024 );
	if ( m_nLightmapResidencyBudgetMb == budget_mb ) {
		return;
	}
	m_nLightmapResidencyBudgetMb = budget_mb;
	markMemoryBudgetDirty();
	if ( !m_bLightmapResidencyBudgetAuto && m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void Terrain3D::setHlodMemoryBudgetAuto( const bool bEnabled ) {
	if ( m_bHlodMemoryBudgetAuto == bEnabled ) {
		return;
	}
	m_bHlodMemoryBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void Terrain3D::setShaderCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bShaderCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bShaderCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

Dictionary Terrain3D::getMemoryBudgetStats( void ) const {
	Dictionary stats;
	stats["total_budget_mb"] = m_nTotalMemoryBudgetMb;
	if (m_memoryAccount.get()) {
		const auto account = m_memoryAccount.get()->stats();
		stats["enforced_limit_bytes"] = int64_t(account.limit);
		stats["enforced_committed_bytes"] = int64_t(account.committed);
		stats["enforced_peak_bytes"] = int64_t(account.peak);
		stats["allocation_denials"] = int64_t(account.denied);
		stats["enforced_cpu_bytes"] = int64_t(account.bytes[Terrain3DMemoryAccount::CPU]);
		stats["enforced_gpu_bytes"] = int64_t(account.bytes[Terrain3DMemoryAccount::GPU]);
		stats["enforced_available_bytes"] = int64_t(account.limit - account.committed);
		stats["enforcement_scope"] = "terrain_owned_native_and_reserved_payloads";
	}
	stats["total_budget_bytes"] = ( int64_t )( ( uint64_t )( m_nTotalMemoryBudgetMb )*T3D_MEMORY_MIB );
	stats["transient_reserve_mb"] = m_nMemoryTransientReserveMb;
	stats["fixed_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nFixedBytes );
	stats["allocator_contained_fixed_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nAllocatorContainedFixedBytes );
	stats["fixed_logical_bytes"] = ( int64_t )calculateFixedMemoryBytes();
	stats["allocator_reserve_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nAllocatorReserveBytes );
	stats["allocator_target_bytes"] = ( int64_t )calculateRuntimeAllocatorZoneBytes();
	stats["allocator_overflow_policy"] =
	        getRuntimeAllocatorOverflowPolicyInternal() == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ? "heap" : "strict";
	stats["managed_budget_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nManagedBudgetBytes );
	stats["managed_effective_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nManagedEffectiveBytes );
	stats["manual_reserved_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nManualReservedBytes );
	stats["auto_minimum_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nAutoMinimumBytes );
	stats["headroom_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nHeadroomBytes );
	stats["hard_overcommit"] = m_nMemoryBudgetResult.m_bHardOvercommit;
	stats["policy_generation"] = ( int64_t )( m_nMemoryBudgetPolicyGeneration );
	stats["policy_interval_frames"] = T3D_MEMORY_POLICY_INTERVAL_FRAMES;

	Array subsystems;
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		Dictionary subsystem;
		const t3dMemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.m_szSubsystems[( size_t )( index )];
		subsystem["name"] = s_pszMemorySubsystemNames[index];
		subsystem["requested_bytes"] = ( int64_t )( allocation.m_nRequestedBytes );
		subsystem["effective_bytes"] = ( int64_t )( allocation.m_nEffectiveBytes );
		subsystem["limiting_constraint"] = memoryLimitReasonName( allocation.m_limitingReason );
		subsystem["below_minimum"] = allocation.m_bBelowMinimum;
		subsystems.push_back( subsystem );
	}
	stats["subsystems"] = subsystems;
	stats["gpu_auto"] = m_bGpuRegionCacheBudgetAuto;
	stats["cpu_auto"] = m_bCpuRegionCacheBudgetAuto;
	stats["instance_auto"] = m_bInstanceRegionCacheBudgetAuto;
	stats["lightmap_auto"] = m_bLightmapResidencyBudgetAuto;
	stats["hlod_auto"] = m_bHlodMemoryBudgetAuto;
	stats["shader_auto"] = m_bShaderCacheBudgetAuto;
	stats["gpu_requested_mb"] = m_nGpuRegionCacheBudgetMb;
	stats["gpu_effective_bytes"] = ( int64_t )( m_nEffectiveGpuRegionCacheBudgetBytes );
	stats["gpu_effective_mb"] = m_nEffectiveGpuRegionCacheBudgetMb;
	stats["cpu_requested_mb"] = m_nCpuRegionCacheBudgetMb;
	stats["cpu_effective_mb"] = m_nEffectiveCpuRegionCacheBudgetMb;
	stats["instance_requested_mb"] = m_nInstanceRegionCacheBudgetMb;
	stats["instance_effective_mb"] = m_nEffectiveInstanceRegionCacheBudgetMb;
	stats["lightmap_requested_mb"] = m_nLightmapResidencyBudgetMb;
	stats["lightmap_effective_mb"] = m_nEffectiveLightmapResidencyBudgetMb;
	stats["hlod_requested_mb"] = m_nHlodMemoryBudgetMb;
	stats["hlod_effective_mb"] = m_nEffectiveHlodMemoryBudgetMb;
	stats["shader_requested_mb"] = 0;
	stats["shader_effective_mb"] = m_nEffectiveShaderCacheBudgetMb;
	return stats;
}

Dictionary Terrain3D::getTrackedCurrentResidency( void ) const {
	Dictionary memory;
	const uint64_t gpu_page_bytes = m_pData ? ( uint64_t )( m_pData->getGpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t middle_height_bytes = m_pData ? ( uint64_t )( m_pData->getMiddleHeightCacheReservedBytes() ) : 0u;
	const uint64_t cpu_page_bytes = m_pData ? ( uint64_t )( m_pData->getCpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t instance_page_bytes = m_pData ? ( uint64_t )( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u;
	const uint64_t lightmap_bytes = m_pLightmapStreamer ? ( uint64_t )( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t )( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes = 0u;
	const uint64_t rd_cpu_bytes = m_pRenderCoordinator ? ( uint64_t )( m_pRenderCoordinator->getRetainedCpuBytes() ) : 0u;
	const uint64_t world_cpu_bytes = m_pRenderCoordinator ? ( uint64_t )( m_pRenderCoordinator->getWorldRetainedCpuBytes() ) : 0u;
	const uint64_t world_gpu_buffer_bytes = m_pRenderCoordinator ? m_pRenderCoordinator->getWorldGpuBufferBytes() : 0u;
	const uint64_t world_hiz_gpu_bytes = m_pRenderCoordinator ? m_pRenderCoordinator->getWorldHiZGpuBytes() : 0u;
	const uint64_t vt_cpu_bytes = m_pRenderCoordinator ? m_pRenderCoordinator->getVirtualTextureCpuBytes() : 0u;
	const Terrain3DRdContext &rd_context = Terrain3DRdContext::getShared();
	const uint64_t rd_vram_buffer_bytes = rd_context.getLiveGpuBufferBytes();
	const uint64_t rd_vram_texture_bytes = rd_context.getLiveGpuTextureBytes();
	const uint64_t compute_local_vram_buffer_bytes = m_pData ? m_pData->getMemoryComputeLocalGpuBytes() : 0u;
	const uint64_t managed_bytes =
	        middle_height_bytes + cpu_page_bytes + instance_page_bytes + lightmap_bytes + hlod_bytes + shader_source_bytes + rd_cpu_bytes;
	const uint64_t fixed_bytes = calculateFixedMemoryBytes();
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_cpu_bytes = fixed_bytes + managed_bytes + transient_bytes;
	const uint64_t tracked_vram_bytes = rd_vram_buffer_bytes + rd_vram_texture_bytes + compute_local_vram_buffer_bytes;
	const uint64_t tracked_bytes = tracked_cpu_bytes + tracked_vram_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	Dictionary fixed;
	fixed["bytes"] = ( int64_t )( fixed_bytes );
	fixed["budget_external_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nFixedBytes );
	fixed["allocator_contained_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nAllocatorContainedFixedBytes );
	fixed["accuracy"] = "estimated";
	fixed["material_texture_arrays_bytes"] = ( int64_t )( m_assets.is_valid() ? m_assets->getGeneratedTexturePayloadBytes() : 0u );
	fixed["far_atlas_bytes"] = ( int64_t )( getGpuFarAtlasBytes() );
	fixed["clipmap_geometry_bytes"] = ( int64_t )( m_pTerrainMesher ? m_pTerrainMesher->getVisibleGeometryPayloadBytes() : 0u );
	fixed["shadow_geometry_bytes"] = ( int64_t )( m_pTerrainMesher ? m_pTerrainMesher->getShadowGeometryPayloadBytes() : 0u );
	fixed["repository_and_authoritative_storage_bytes"] = ( int64_t )( m_pData ? m_pData->getMemoryFixedRepositoryBytes() : 0u );
	fixed["node_streamer_retained_bytes"] = ( int64_t )( m_pNodeStreamer ? m_pNodeStreamer->getRetainedCapacityBytes() : 0u );
	fixed["instancer_retained_bytes"] = ( int64_t )( m_pInstancer ? m_pInstancer->getRetainedContainerBytes() : 0u );
	fixed["active_multimesh_bytes"] = ( int64_t )( m_pInstancer ? m_pInstancer->getActiveMultimeshBytes() : 0u );
	fixed["profiler_history_bytes"] = ( int64_t )( m_pRuntimeProfileHistory ? sizeof( t3dRuntimeProfileHistory_t ) : 0u );
	memory["fixed_unavoidable"] = fixed;

	Dictionary managed;
	managed["bytes"] = ( int64_t )( managed_bytes );
	managed["accuracy"] = "estimated";
	managed["gpu_terrain_page_pool_estimated_bytes"] = ( int64_t )( gpu_page_bytes );
	managed["rd_coordinator_retained_cpu_bytes"] = ( int64_t )( rd_cpu_bytes );
	managed["middle_height_page_pool_bytes"] = ( int64_t )( middle_height_bytes );
	managed["cpu_terrain_page_cache_bytes"] = ( int64_t )( cpu_page_bytes );
	managed["instance_region_page_bytes"] = ( int64_t )( instance_page_bytes );
	managed["baked_lighting_estimated_bytes"] = ( int64_t )( lightmap_bytes );
	managed["hlod_proxy_estimated_bytes"] = ( int64_t )( hlod_bytes );
	managed["shader_source_cache_bytes"] = ( int64_t )( shader_source_bytes );
	managed["rendering_device_cpu_bytes"] = ( int64_t )( rd_cpu_bytes );
	memory["managed_resident"] = managed;

	Dictionary unified_world;
	unified_world["cpu_retained_bytes"] = ( int64_t )world_cpu_bytes;
	unified_world["gpu_buffer_bytes"] = ( int64_t )world_gpu_buffer_bytes;
	unified_world["hiz_gpu_texture_bytes"] = ( int64_t )world_hiz_gpu_bytes;
	unified_world["virtual_texture_cpu_bytes"] = ( int64_t )vt_cpu_bytes;
	unified_world["included_in_rd_totals"] = true;
	unified_world["accuracy"] = "exact owner accounting; RD texture total remains authoritative for aggregate VRAM";
	memory["unified_world_ownership"] = unified_world;

	Dictionary transient;
	transient["bytes"] = ( int64_t )( transient_bytes );
	transient["accuracy"] = "estimated";
	transient["data_page_and_far_staging_bytes"] = ( int64_t )( m_pData ? m_pData->getMemoryTransientStagingBytes() : 0u );
	transient["clipmap_streaming_buffer_bytes"] = ( int64_t )( m_pTerrainMesher ? m_pTerrainMesher->getStreamingBufferBytes() : 0u );
	transient["collision_retained_scratch_bytes"] = ( int64_t )( m_pCollision ? m_pCollision->getRetainedBufferBytes() : 0u );
	memory["transient_staging"] = transient;

	const t3dEastlAllocationStats_t allocator = Terrain3DEastlAllocator::getAllocationStats();
	Dictionary allocator_stats;
	allocator_stats["live_bytes"] = ( int64_t )( allocator.m_nLiveBytes );
	allocator_stats["peak_bytes"] = ( int64_t )( allocator.m_nPeakBytes );
	allocator_stats["live_allocations"] = ( int64_t )( allocator.m_nLiveAllocations );
	allocator_stats["total_allocations"] = ( int64_t )( allocator.m_nTotalAllocations );
	allocator_stats["failed_allocations"] = ( int64_t )( allocator.m_nFailedAllocations );
	allocator_stats["overflow_allocations"] = ( int64_t )( allocator.m_nOverflowAllocations );
	allocator_stats["overflow_live_bytes"] = ( int64_t )( allocator.m_nOverflowLiveBytes );
	allocator_stats["accuracy"] = "exact";
	allocator_stats["source"] = "release_safe_allocator_telemetry";
	allocator_stats["included_in_total"] = false;
	allocator_stats["note"] = "Cross-cutting EASTL telemetry overlaps subsystem container accounting and is not summed twice.";
	memory["eastl_allocator"] = allocator_stats;

	const t3dRuntimeAllocationPhaseStats_t phase = t3dGetRuntimeAllocationPhaseStats();
	Dictionary phase_stats;
	static const char *const s_szPhaseNames[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {
		"initialization", "cold_rebuild", "hot_frame", "hot_render", "streaming_worker", "shutdown"
	};
	Dictionary eastl_events, eastl_bytes, zone_events, zone_bytes, raw_events, raw_bytes;
	for ( uint32_t index = 0u; index < T3D_RUNTIME_ALLOCATION_PHASE_COUNT; ++index ) {
		const StringName key( s_szPhaseNames[index] );
		eastl_events[key] = ( int64_t )( phase.m_szEastlEvents[index] );
		eastl_bytes[key] = ( int64_t )( phase.m_szEastlBytes[index] );
		zone_events[key] = ( int64_t )( phase.m_szZoneEvents[index] );
		zone_bytes[key] = ( int64_t )( phase.m_szZoneBytes[index] );
		raw_events[key] = ( int64_t )( phase.m_szRawEvents[index] );
		raw_bytes[key] = ( int64_t )( phase.m_szRawBytes[index] );
	}
	phase_stats["eastl_allocation_events_by_phase"] = eastl_events;
	phase_stats["eastl_allocation_bytes_by_phase"] = eastl_bytes;
	phase_stats["zone_allocation_events_by_phase"] = zone_events;
	phase_stats["zone_allocation_bytes_by_phase"] = zone_bytes;
	phase_stats["raw_allocation_events_by_phase"] = raw_events;
	phase_stats["raw_allocation_bytes_by_phase"] = raw_bytes;
	phase_stats["current_phase"] = ( int )( t3dGetRuntimeAllocationPhase() );
	phase_stats["accuracy"] = "exact successful allocation events by allocator layer and diagnostic runtime phase";
	phase_stats["note"] =
	        "Runtime phase is telemetry only; backing-growth legality is enforced by each owning subsystem and bound allocator domain.";
	memory["allocation_phases"] = phase_stats;

	const t3dMemoryZoneStats_t zone = m_runtimeMemoryZone.getStats();
	Dictionary zone_stats;
	zone_stats["state"] = ( int )m_runtimeMemoryZone.state();
	zone_stats["reserved_bytes"] = ( int64_t )zone.m_nReservedBytes;
	zone_stats["live_requested_bytes"] = ( int64_t )zone.m_nLiveRequestedBytes;
	zone_stats["live_physical_bytes"] = ( int64_t )zone.m_nLivePhysicalBytes;
	zone_stats["live_allocations"] = ( int64_t )zone.m_nLiveAllocationCount;
	zone_stats["extent_free_bytes"] = ( int64_t )zone.m_nExtentFreeBytes;
	zone_stats["small_page_reserved_bytes"] = ( int64_t )zone.m_nSmallPageReservedBytes;
	zone_stats["small_page_reusable_payload_bytes"] = ( int64_t )zone.m_nSmallPageReusablePayloadBytes;
	zone_stats["small_page_reusable_physical_bytes"] = ( int64_t )zone.m_nSmallPageReusablePhysicalBytes;
	zone_stats["large_allocation_physical_bytes"] = ( int64_t )zone.m_nLargeAllocationPhysicalBytes;
	zone_stats["metadata_bytes"] = ( int64_t )zone.m_nMetadataBytes;
	zone_stats["alignment_waste_bytes"] = ( int64_t )zone.m_nAlignmentWasteBytes;
	zone_stats["size_class_waste_bytes"] = ( int64_t )zone.m_nSizeClassWasteBytes;
	zone_stats["internal_fragmentation_bytes"] = ( int64_t )zone.m_nInternalFragmentationBytes;
	zone_stats["failed_allocations"] = ( int64_t )zone.m_nFailedAllocations;
	zone_stats["active_small_pages"] = ( int64_t )zone.m_nActiveSmallPages;
	zone_stats["peak_active_small_pages"] = ( int64_t )zone.m_nPeakActiveSmallPages;
	zone_stats["free_extent_count"] = ( int64_t )zone.m_nFreeExtentCount;
	zone_stats["peak_free_extent_count"] = ( int64_t )zone.m_nPeakFreeExtentCount;
	zone_stats["overflow_policy"] = getRuntimeAllocatorOverflowPolicyInternal() == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ? "heap" : "strict";
	zone_stats["arena_reserved_bytes"] = ( int64_t )m_runtimeMemoryArena.capacity();
	zone_stats["arena_committed"] = m_runtimeMemoryArena.committed();
	zone_stats["arena_sealed"] = m_runtimeMemoryArena.sealed();
	memory["memory_zone"] = zone_stats;

	Dictionary external;
	external["accuracy"] = "unavailable";
	external["godot_object_container_overhead_bytes"] = -1;
	external["renderer_driver_padding_bytes"] = -1;
	external["shader_pipeline_driver_bytes"] = -1;
	external["baked_lighting_cpu_package_overhead_bytes"] = -1;
	external["editor_profiler_tool_overhead_bytes"] = -1;
	external["note"] = "Unavailable values are excluded from tracked totals rather than estimated as zero.";
	memory["external_untracked"] = external;

	memory["tracked_cpu_ram_bytes"] = ( int64_t )tracked_cpu_bytes;
	memory["tracked_gpu_vram_bytes"] = ( int64_t )tracked_vram_bytes;
	memory["rd_gpu_buffer_bytes"] = ( int64_t )rd_vram_buffer_bytes;
	memory["rd_gpu_texture_bytes"] = ( int64_t )rd_vram_texture_bytes;
	memory["compute_local_gpu_buffer_bytes"] = ( int64_t )compute_local_vram_buffer_bytes;
	memory["compute_local_telemetry"] = m_pData ? m_pData->getComputeLocalGpuTelemetry() : Dictionary();
	memory["tracked_current_residency_bytes"] = ( int64_t )tracked_bytes;
	memory["tracked_logical_payload_bytes"] = ( int64_t )tracked_bytes;
	memory["native_runtime_arena_reserved_bytes"] = ( int64_t )m_runtimeMemoryArena.capacity();
	memory["allocator_zone_reserved_bytes"] = ( int64_t )zone.m_nReservedBytes;
	memory["tracked_peak_residency_bytes"] = ( int64_t )m_nTrackedMemoryPeakBytes;
	memory["budget"] = getMemoryBudgetStats();
	memory["performance_target"] = ( int )( m_performanceTarget );
	memory["performance_profile_status"] = ( int )( m_performanceProfileStatus );
	memory["performance_profile_version"] = ( int )( T3D_PROFILE_VERSION );
	memory["performance_profile_signature"] = getPerformanceProfileSignature();
	memory["scope"] = "Terrain3D-owned tracked memory; external Godot/renderer/driver allocations are explicitly unavailable.";
	return memory;
}

Dictionary Terrain3D::getMemoryEstimate( void ) const {
	return getTrackedCurrentResidency();
}

Dictionary Terrain3D::getRuntimeMemoryStatsFast( void ) const {
	Dictionary memory;
	const uint64_t gpu_page_bytes = m_pData ? ( uint64_t )( m_pData->getGpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t middle_height_bytes = m_pData ? ( uint64_t )( m_pData->getMiddleHeightCacheReservedBytes() ) : 0u;
	const uint64_t cpu_page_bytes = m_pData ? ( uint64_t )( m_pData->getCpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t instance_page_bytes = m_pData ? ( uint64_t )( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u;
	const uint64_t lightmap_bytes = m_pLightmapStreamer ? ( uint64_t )( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t )( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes = 0u;
	const uint64_t managed_bytes =
	        gpu_page_bytes + middle_height_bytes + cpu_page_bytes + instance_page_bytes + lightmap_bytes + hlod_bytes + shader_source_bytes;
	const uint64_t fixed_bytes = m_nMemoryBudgetResult.m_nFixedBytes;
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_bytes = fixed_bytes + managed_bytes + transient_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	memory["configured_budget_bytes"] = ( int64_t )( ( uint64_t )( m_nTotalMemoryBudgetMb )*T3D_MEMORY_MIB );
	memory["current_known_bytes"] = ( int64_t )tracked_bytes;
	memory["logical_tracked_bytes"] = ( int64_t )tracked_bytes;
	memory["allocator_reserve_bytes"] = ( int64_t )m_nMemoryBudgetResult.m_nAllocatorReserveBytes;
	memory["allocator_contained_fixed_bytes"] = ( int64_t )m_nMemoryBudgetResult.m_nAllocatorContainedFixedBytes;
	memory["budget_committed_bytes"] =
	        ( int64_t )( fixed_bytes + m_nMemoryBudgetResult.m_nAllocatorReserveBytes + managed_bytes + transient_bytes );
	memory["peak_known_bytes"] = ( int64_t )( m_nTrackedMemoryPeakBytes );
	memory["fixed_bytes"] = ( int64_t )( fixed_bytes );
	memory["managed_resident_bytes"] = ( int64_t )( managed_bytes );
	memory["middle_height_page_pool_bytes"] = ( int64_t )( middle_height_bytes );
	memory["transient_bytes"] = ( int64_t )( transient_bytes );
	memory["known_headroom_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nHeadroomBytes );
	memory["overcommitted"] = m_nMemoryBudgetResult.m_bHardOvercommit;
	const bool bPressure =
	        m_nMemoryBudgetResult.m_bHardOvercommit || m_nMemoryBudgetResult.m_nHeadroomBytes < T3D_MEMORY_POLICY_HYSTERESIS_BYTES;
	memory["pressure_state"] = m_nMemoryBudgetResult.m_bHardOvercommit ? "overcommit" : ( bPressure ? "pressure" : "normal" );
	memory["external_untracked_accuracy"] = "unavailable";
	memory["external_untracked"] = "Godot object/container overhead, renderer/driver padding, and shader pipeline allocations";
	memory["budget"] = getMemoryBudgetStats();

	const bool bAutomatic[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = { m_bGpuRegionCacheBudgetAuto,
		m_bCpuRegionCacheBudgetAuto,
		m_bInstanceRegionCacheBudgetAuto,
		m_bLightmapResidencyBudgetAuto,
		m_bHlodMemoryBudgetAuto,
		m_bShaderCacheBudgetAuto };
	Array reallocations;
	Array constrained;
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		const t3dMemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.m_szSubsystems[( size_t )( index )];
		if ( bAutomatic[index] ) {
			Dictionary entry;
			entry["subsystem"] = s_pszMemorySubsystemNames[index];
			entry["effective_bytes"] = ( int64_t )( allocation.m_nEffectiveBytes );
			entry["limiting_constraint"] = memoryLimitReasonName( allocation.m_limitingReason );
			reallocations.push_back( entry );
		}
		if ( allocation.m_limitingReason != TERRAIN3D_MEMORY_LIMIT_NONE
		        && allocation.m_limitingReason != TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE ) {
			constrained.push_back( s_pszMemorySubsystemNames[index] );
		}
	}
	memory["automatic_allocations"] = reallocations;
	memory["constrained_subsystems"] = constrained;
	return memory;
}
