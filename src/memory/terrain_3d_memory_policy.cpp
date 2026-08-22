// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "authoring/terrain_3d_editor.h"
#include "core/logger.h"
#include "core/terrain_3d_integration_helpers.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_lighting_policy.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_zone_allocator.h"
#include "rendering/terrain_3d_compute.h"
#include "simd/terrain_3d_parallel.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "streaming/terrain_3d_performance_profile.h"
#include "streaming/terrain_3d_streaming_policy.h"
#include "streaming/terrain_3d_upload_bridge.h"
#include "terrain_3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <limits>
#include <numeric>
#include <utility>

bool Terrain3D::initializeRuntimeMemory( void ) {
	if ( m_bRuntimeMemoryReady ) {
		if ( m_pData && m_pData->isActiveStorageOwnerInternal() ) {
			return true;
		}
		const t3dMemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
		return m_pData && active_span.valid() &&
				m_pData->bindActiveStorageInternal(
						( int )( m_nRegionSize ), active_span.m_pData, active_span.m_nSize );
	}
	// Worker and container allocations use the runtime zone. Active and resident
	// page banks retain independent storage and must never route through this allocator.
	const size_t active_bytes = t3dActiveStorageRequiredBytes( ( int )( m_nRegionSize ) );
	const size_t zone_bytes = calculateRuntimeAllocatorZoneBytes();
	if ( active_bytes == 0u || zone_bytes < Terrain3DMemoryZone::s_nSmallPageBytes ||
			active_bytes > std::numeric_limits<size_t>::max() - zone_bytes - 2u * s_nArenaDefaultBaseAlignment ) {
		return false;
	}
	Terrain3DMemoryArenaLayout layout( active_bytes + zone_bytes + 2u * s_nArenaDefaultBaseAlignment );
	if ( !layout.reserveLow( active_bytes, s_nActiveStorageAlignment, m_runtimeActiveHeightSlice ) ||
			!layout.reserveLow( zone_bytes, s_nArenaDefaultBaseAlignment, m_runtimeZoneSlice ) || !layout.seal() ||
			!m_runtimeMemoryArena.initialize( layout ) || !m_runtimeMemoryArena.commit() ) {
		return false;
	}
	const t3dMemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
	const t3dMemoryArenaSpan_t zone_span = m_runtimeMemoryArena.resolve( m_runtimeZoneSlice );
	if ( !m_pData || !active_span.valid() ||
			!m_pData->bindActiveStorageInternal( ( int )( m_nRegionSize ), active_span.m_pData, active_span.m_nSize ) ||
			!zone_span.valid() || !m_runtimeMemoryZone.initialize( zone_span.m_pData, zone_span.m_nSize ) ||
			!m_runtimeMemoryArena.seal() ) {
		m_runtimeMemoryArena.release();
		return false;
	}
	Terrain3DZoneAllocatorRouter::setOverflowPolicy( Engine::get_singleton()->is_editor_hint()
					? TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP
					: TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT );
	Terrain3DZoneAllocatorRouter::install( &m_runtimeMemoryZone );
	m_bRuntimeMemoryReady = true;
	return true;
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
	const uint64_t total_bytes = ( uint64_t )m_nTotalMemoryBudgetMb * s_nMemoryMib;
	const uint64_t transient_bytes = ( uint64_t )m_nMemoryTransientReserveMb * s_nMemoryMib;
	const size_t active_bytes = t3dActiveStorageRequiredBytes( ( int )m_nRegionSize );
	uint64_t target = total_bytes / s_nRuntimeAllocatorBudgetDivisor;
	target = MAX( target, ( uint64_t )s_nRuntimeAllocatorMinBytes );
	target = MIN( target, ( uint64_t )s_nRuntimeAllocatorMaxBytes );
	const uint64_t fixed_floor = ( uint64_t )active_bytes + transient_bytes;
	if ( total_bytes <= fixed_floor + Terrain3DMemoryZone::s_nSmallPageBytes ) {
		return 0u;
	}
	target = MIN( target, total_bytes - fixed_floor );
	target &= ~( ( uint64_t )Terrain3DMemoryZone::s_nSmallPageBytes - 1u );
	return target >= Terrain3DMemoryZone::s_nSmallPageBytes && target <= SIZE_MAX ? ( size_t )target : 0u;
}

uint64_t Terrain3D::calculateFixedMemoryBytes( void ) const {
	uint64_t bytes = sizeof( *this );
	if ( m_pData ) {
		bytes += ( uint64_t )( m_pData->getMemoryFixedRepositoryBytes() );
		bytes += getGpuFarAtlasBytes();
	}
	if ( m_assets.is_valid() ) {
		bytes += ( uint64_t )( m_assets->getGeneratedTexturePayloadBytes() );
	}
	if ( m_pTerrainMesher ) {
		bytes += ( uint64_t )( m_pTerrainMesher->getVisibleGeometryPayloadBytes() );
		bytes += ( uint64_t )( m_pTerrainMesher->getShadowGeometryPayloadBytes() );
	}
	if ( m_pInstancer ) {
		bytes += ( uint64_t )( sizeof( Terrain3DInstancer ) );
		bytes += ( uint64_t )( m_pInstancer->getActiveMultimeshBytes() );
		bytes += ( uint64_t )( m_pInstancer->getRetainedContainerBytes() );
	}
	if ( m_pCollision ) {
		bytes += ( uint64_t )( sizeof( Terrain3DCollision ) );
	}
	if ( m_pNodeStreamer ) {
		bytes += ( uint64_t )( sizeof( Terrain3DNodeStreamer ) );
		bytes += ( uint64_t )( m_pNodeStreamer->getRetainedCapacityBytes() );
	}
	if ( m_pLightmapStreamer ) {
		bytes += ( uint64_t )( sizeof( Terrain3DLightmapStreamer ) );
	}
	if ( m_pRuntimeProfileHistory ) {
		bytes += ( uint64_t )( sizeof( t3dRuntimeProfileHistory_t ) );
	}
	return bytes;
}

uint64_t Terrain3D::calculateTransientMemoryBytes( void ) const {
	uint64_t bytes = 0u;
	if ( m_pData ) {
		bytes += ( uint64_t )( m_pData->getMemoryTransientStagingBytes() );
	}
	if ( m_pTerrainMesher ) {
		bytes += ( uint64_t )( m_pTerrainMesher->getStreamingBufferBytes() );
	}
	if ( m_pCollision ) {
		bytes += ( uint64_t )( m_pCollision->getRetainedBufferBytes() );
	}
	return bytes;
}

t3dMemoryBudgetInput_t Terrain3D::buildMemoryBudgetInput( void ) const {
	t3dMemoryBudgetInput_t input;
	input.m_nTotalBudgetBytes = ( uint64_t )( m_nTotalMemoryBudgetMb )*s_nMemoryMib;
	input.m_nFixedBytes = calculateFixedMemoryBytes();
	input.m_nAllocatorReserveBytes = m_runtimeMemoryZone.capacity() > 0u
			? ( uint64_t )m_runtimeMemoryZone.capacity()
			: ( uint64_t )calculateRuntimeAllocatorZoneBytes();
	input.m_nTransientReserveBytes = ( uint64_t )( m_nMemoryTransientReserveMb )*s_nMemoryMib;

	t3dMemorySubsystemInput_t &gpu = input.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.m_bEnabled = effectiveGpuRegionCacheEnabled();
	gpu.m_bAutomatic = m_bGpuRegionCacheBudgetAuto;
	gpu.m_nManualBytes = ( uint64_t )( m_nGpuRegionCacheBudgetMb )*s_nMemoryMib;
	gpu.m_nMinimumBytes = m_pData ? ( uint64_t )( m_pData->getGpuRegionCacheMinimumBytes() ) : s_nMemoryMib;
	gpu.m_nHardCapBytes = m_pData ? ( uint64_t )( m_pData->getGpuRegionCacheHardCapBytes() ) : 128u * s_nMemoryMib;
	gpu.m_nWeight = 5u;

	t3dMemorySubsystemInput_t &cpu = input.m_szSubsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES];
	cpu.m_bEnabled = effectiveCpuRegionCacheEnabled();
	cpu.m_bAutomatic = m_bCpuRegionCacheBudgetAuto;
	cpu.m_nManualBytes = ( uint64_t )( m_nCpuRegionCacheBudgetMb )*s_nMemoryMib;
	cpu.m_nMinimumBytes = m_pData ? ( uint64_t )( m_pData->getCpuRegionCacheMinimumBytes() ) : 4u * s_nMemoryMib;
	cpu.m_nHardCapBytes = m_pData ? ( uint64_t )( m_pData->getCpuRegionCacheHardCapBytes() ) : 128u * s_nMemoryMib;
	cpu.m_nWeight = 4u;

	t3dMemorySubsystemInput_t &instances = input.m_szSubsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES];
	instances.m_bEnabled = effectiveInstanceRegionCacheEnabled();
	instances.m_bAutomatic = m_bInstanceRegionCacheBudgetAuto;
	instances.m_nManualBytes = ( uint64_t )( m_nInstanceRegionCacheBudgetMb )*s_nMemoryMib;
	instances.m_nMinimumBytes = s_nMemoryMib;
	instances.m_nHardCapBytes = 128u * s_nMemoryMib;
	instances.m_nWeight = 2u;

	t3dMemorySubsystemInput_t &lighting = input.m_szSubsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING];
	lighting.m_bEnabled = m_bLightmapStreamingEnabled;
	lighting.m_bAutomatic = m_bLightmapResidencyBudgetAuto;
	lighting.m_nManualBytes = ( uint64_t )( m_nLightmapResidencyBudgetMb )*s_nMemoryMib;
	lighting.m_nMinimumBytes = s_nMemoryMib;
	lighting.m_nHardCapBytes = 1024u * s_nMemoryMib;
	lighting.m_nWeight = 2u;

	t3dMemorySubsystemInput_t &hlod = input.m_szSubsystems[TERRAIN3D_MEMORY_HLOD_PROXIES];
	hlod.m_bEnabled = m_bHlodEnabled;
	hlod.m_bAutomatic = m_bHlodMemoryBudgetAuto;
	hlod.m_nManualBytes = ( uint64_t )( m_nHlodMemoryBudgetMb )*s_nMemoryMib;
	hlod.m_nMinimumBytes = 4u * s_nMemoryMib;
	hlod.m_nHardCapBytes = 4096u * s_nMemoryMib;
	hlod.m_nWeight = 2u;

	t3dMemorySubsystemInput_t &shader = input.m_szSubsystems[TERRAIN3D_MEMORY_SHADER_CACHE];
	shader.m_bEnabled = m_material.is_valid();
	shader.m_bAutomatic = m_bShaderCacheBudgetAuto;
	shader.m_nManualBytes =
			( uint64_t )( m_material.is_valid() ? m_material->getShaderVariantSourceBudgetMb() : 8 ) * s_nMemoryMib;
	shader.m_nMinimumBytes = ( uint64_t )( s_nMinShaderVariantSourceBudgetMb )*s_nMemoryMib;
	shader.m_nHardCapBytes = ( uint64_t )( s_nMaxShaderVariantSourceBudgetMb )*s_nMemoryMib;
	shader.m_nWeight = 1u;
	return input;
}

void Terrain3D::applyMemoryBudgetResult( const t3dMemoryBudgetResult_t &rResult, const bool bForce ) {
	const int previous_effective_mb[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_nEffectiveGpuRegionCacheBudgetMb,
		m_nEffectiveCpuRegionCacheBudgetMb,
		m_nEffectiveInstanceRegionCacheBudgetMb,
		m_nEffectiveLightmapResidencyBudgetMb,
		m_nEffectiveHlodMemoryBudgetMb,
		m_nEffectiveShaderCacheBudgetMb,
	};
	const uint64_t fixed_far_bytes = getGpuFarAtlasBytes();
	const uint64_t gpu_bytes =
			rResult.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_nEffectiveBytes + fixed_far_bytes;
	const int gpu_mb = MAX( 1, ( int )( ( gpu_bytes + s_nMemoryMib - 1u ) / s_nMemoryMib ) );
	const int cpu_mb = MAX( 1,
			( int )( ( rResult.m_szSubsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES].m_nEffectiveBytes + s_nMemoryMib -
							 1u ) /
					s_nMemoryMib ) );
	const int instance_mb = MAX( 1,
			( int )( ( rResult.m_szSubsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES].m_nEffectiveBytes + s_nMemoryMib - 1u ) /
					s_nMemoryMib ) );
	const int lightmap_mb = MAX( 1,
			( int )( ( rResult.m_szSubsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING].m_nEffectiveBytes + s_nMemoryMib - 1u ) /
					s_nMemoryMib ) );
	const int hlod_mb = MAX( 1,
			( int )( ( rResult.m_szSubsystems[TERRAIN3D_MEMORY_HLOD_PROXIES].m_nEffectiveBytes + s_nMemoryMib - 1u ) /
					s_nMemoryMib ) );
	const int shader_mb = MAX( 1,
			( int )( ( rResult.m_szSubsystems[TERRAIN3D_MEMORY_SHADER_CACHE].m_nEffectiveBytes + s_nMemoryMib - 1u ) /
					s_nMemoryMib ) );

	const int hysteresis_mb = ( int )( s_nMemoryPolicyHysteresisBytes / s_nMemoryMib );
	const bool bGpuChanged = bForce || std::abs( gpu_mb - m_nEffectiveGpuRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bCpuChanged = bForce || std::abs( cpu_mb - m_nEffectiveCpuRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bInstanceChanged =
			bForce || std::abs( instance_mb - m_nEffectiveInstanceRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bLightmapChanged =
			bForce || std::abs( lightmap_mb - m_nEffectiveLightmapResidencyBudgetMb ) >= hysteresis_mb;
	const bool bHlodChanged = bForce || std::abs( hlod_mb - m_nEffectiveHlodMemoryBudgetMb ) >= hysteresis_mb;
	const bool bShaderChanged = bForce || std::abs( shader_mb - m_nEffectiveShaderCacheBudgetMb ) >= hysteresis_mb;

	if ( bGpuChanged ) {
		m_nEffectiveGpuRegionCacheBudgetMb = gpu_mb;
		if ( m_pData ) {
			m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(),
					m_nEffectiveGpuRegionCacheBudgetMb,
					m_nGpuRegionUploadBudgetLayers,
					m_nGpuRegionUploadBudgetMb,
					m_nGpuRegionSparePages,
					m_nGpuRegionRetireFrames );
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
		m_nEffectiveShaderCacheBudgetMb = shader_mb;
		if ( m_material.is_valid() ) {
			m_material->setEffectiveShaderVariantSourceBudgetMb( m_nEffectiveShaderCacheBudgetMb );
		}
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
		if ( current_effective_mb[index] == previous_effective_mb[index] ) {
			continue;
		}
		emit_signal( "cache_budget_changed",
				StringName( s_pszMemorySubsystemNames[index] ),
				( int64_t )( rResult.m_szSubsystems[( size_t )( index )].m_nEffectiveBytes ),
				StringName( memoryLimitReasonName( rResult.m_szSubsystems[( size_t )( index )].m_limitingReason ) ) );
	}
}

void Terrain3D::serviceMemoryBudgetPolicy( const bool bForce ) {
	if ( !bForce ) {
		++m_nMemoryBudgetPolicyFrame;
		if ( ( m_nMemoryBudgetPolicyFrame % ( uint64_t )( s_nMemoryPolicyIntervalFrames ) ) != 0u ) {
			return;
		}
	}
	const t3dMemoryBudgetInput_t input = buildMemoryBudgetInput();
	const t3dMemoryBudgetResult_t result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	applyMemoryBudgetResult( result, bForce || m_nMemoryBudgetPolicyGeneration == 0u );
	const bool bPressure = result.m_bHardOvercommit || result.m_nHeadroomBytes < s_nMemoryPolicyHysteresisBytes;
	if ( bPressure != m_bMemoryPressureEventState || result.m_bHardOvercommit != m_bMemoryOvercommitEventState ) {
		m_bMemoryPressureEventState = bPressure;
		m_bMemoryOvercommitEventState = result.m_bHardOvercommit;
		emit_signal( "memory_pressure_changed",
				bPressure,
				result.m_bHardOvercommit,
				( int64_t )( result.m_nHeadroomBytes ) );
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
	const int budget_mb = CLAMP( nBudgetMb, 32, 4096 );
	if ( m_nTotalMemoryBudgetMb == budget_mb ) {
		return;
	}
	m_nTotalMemoryBudgetMb = budget_mb;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
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
	stats["total_budget_bytes"] = ( int64_t )( ( uint64_t )( m_nTotalMemoryBudgetMb )*s_nMemoryMib );
	stats["transient_reserve_mb"] = m_nMemoryTransientReserveMb;
	stats["fixed_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nFixedBytes );
	stats["allocator_reserve_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nAllocatorReserveBytes );
	stats["allocator_target_bytes"] = ( int64_t )calculateRuntimeAllocatorZoneBytes();
	stats["allocator_overflow_policy"] =
			Terrain3DZoneAllocatorRouter::overflowPolicy() == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ? "heap" : "strict";
	stats["managed_budget_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nManagedBudgetBytes );
	stats["managed_effective_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nManagedEffectiveBytes );
	stats["manual_reserved_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nManualReservedBytes );
	stats["auto_minimum_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nAutoMinimumBytes );
	stats["headroom_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nHeadroomBytes );
	stats["hard_overcommit"] = m_nMemoryBudgetResult.m_bHardOvercommit;
	stats["policy_generation"] = ( int64_t )( m_nMemoryBudgetPolicyGeneration );
	stats["policy_interval_frames"] = s_nMemoryPolicyIntervalFrames;

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
	stats["gpu_effective_mb"] = m_nEffectiveGpuRegionCacheBudgetMb;
	stats["cpu_requested_mb"] = m_nCpuRegionCacheBudgetMb;
	stats["cpu_effective_mb"] = m_nEffectiveCpuRegionCacheBudgetMb;
	stats["instance_requested_mb"] = m_nInstanceRegionCacheBudgetMb;
	stats["instance_effective_mb"] = m_nEffectiveInstanceRegionCacheBudgetMb;
	stats["lightmap_requested_mb"] = m_nLightmapResidencyBudgetMb;
	stats["lightmap_effective_mb"] = m_nEffectiveLightmapResidencyBudgetMb;
	stats["hlod_requested_mb"] = m_nHlodMemoryBudgetMb;
	stats["hlod_effective_mb"] = m_nEffectiveHlodMemoryBudgetMb;
	stats["shader_requested_mb"] = m_material.is_valid() ? m_material->getShaderVariantSourceBudgetMb() : 0;
	stats["shader_effective_mb"] = m_nEffectiveShaderCacheBudgetMb;
	return stats;
}

Dictionary Terrain3D::getTrackedCurrentResidency( void ) const {
	Dictionary memory;
	const uint64_t gpu_page_bytes = m_pData ? ( uint64_t )( m_pData->getGpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t middle_height_bytes = m_pData ? ( uint64_t )( m_pData->getMiddleHeightCacheReservedBytes() ) : 0u;
	const uint64_t cpu_page_bytes = m_pData ? ( uint64_t )( m_pData->getCpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t instance_page_bytes = m_pData ? ( uint64_t )( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u;
	const uint64_t lightmap_bytes =
			m_pLightmapStreamer ? ( uint64_t )( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t )( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes =
			m_material.is_valid() ? ( uint64_t )( m_material->getShaderVariantSourceBytesNative() ) : 0u;
	const uint64_t managed_bytes = gpu_page_bytes + middle_height_bytes + cpu_page_bytes + instance_page_bytes +
			lightmap_bytes + hlod_bytes + shader_source_bytes;
	const uint64_t fixed_bytes = calculateFixedMemoryBytes();
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_bytes = fixed_bytes + managed_bytes + transient_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	Dictionary fixed;
	fixed["bytes"] = ( int64_t )( fixed_bytes );
	fixed["accuracy"] = "estimated";
	fixed["material_texture_arrays_bytes"] =
			( int64_t )( m_assets.is_valid() ? m_assets->getGeneratedTexturePayloadBytes() : 0u );
	fixed["far_atlas_bytes"] = ( int64_t )( getGpuFarAtlasBytes() );
	fixed["clipmap_geometry_bytes"] =
			( int64_t )( m_pTerrainMesher ? m_pTerrainMesher->getVisibleGeometryPayloadBytes() : 0u );
	fixed["shadow_geometry_bytes"] =
			( int64_t )( m_pTerrainMesher ? m_pTerrainMesher->getShadowGeometryPayloadBytes() : 0u );
	fixed["repository_and_authoritative_storage_bytes"] =
			( int64_t )( m_pData ? m_pData->getMemoryFixedRepositoryBytes() : 0u );
	fixed["node_streamer_retained_bytes"] =
			( int64_t )( m_pNodeStreamer ? m_pNodeStreamer->getRetainedCapacityBytes() : 0u );
	fixed["instancer_retained_bytes"] = ( int64_t )( m_pInstancer ? m_pInstancer->getRetainedContainerBytes() : 0u );
	fixed["active_multimesh_bytes"] = ( int64_t )( m_pInstancer ? m_pInstancer->getActiveMultimeshBytes() : 0u );
	fixed["profiler_history_bytes"] =
			( int64_t )( m_pRuntimeProfileHistory ? sizeof( t3dRuntimeProfileHistory_t ) : 0u );
	memory["fixed_unavoidable"] = fixed;

	Dictionary managed;
	managed["bytes"] = ( int64_t )( managed_bytes );
	managed["accuracy"] = "estimated";
	managed["gpu_terrain_page_pool_bytes"] = ( int64_t )( gpu_page_bytes );
	managed["middle_height_page_pool_bytes"] = ( int64_t )( middle_height_bytes );
	managed["cpu_terrain_page_cache_bytes"] = ( int64_t )( cpu_page_bytes );
	managed["instance_region_page_bytes"] = ( int64_t )( instance_page_bytes );
	managed["baked_lighting_estimated_bytes"] = ( int64_t )( lightmap_bytes );
	managed["hlod_proxy_estimated_bytes"] = ( int64_t )( hlod_bytes );
	managed["shader_source_cache_bytes"] = ( int64_t )( shader_source_bytes );
	memory["managed_resident"] = managed;

	Dictionary transient;
	transient["bytes"] = ( int64_t )( transient_bytes );
	transient["accuracy"] = "estimated";
	transient["data_page_and_far_staging_bytes"] =
			( int64_t )( m_pData ? m_pData->getMemoryTransientStagingBytes() : 0u );
	transient["clipmap_streaming_buffer_bytes"] =
			( int64_t )( m_pTerrainMesher ? m_pTerrainMesher->getStreamingBufferBytes() : 0u );
	transient["collision_retained_scratch_bytes"] =
			( int64_t )( m_pCollision ? m_pCollision->getRetainedBufferBytes() : 0u );
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
	allocator_stats["note"] =
			"Cross-cutting EASTL telemetry overlaps subsystem container accounting and is not summed twice.";
	memory["eastl_allocator"] = allocator_stats;
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
	zone_stats["overflow_policy"] =
			Terrain3DZoneAllocatorRouter::overflowPolicy() == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ? "heap" : "strict";
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

	memory["tracked_current_residency_bytes"] = ( int64_t )tracked_bytes;
	memory["tracked_logical_payload_bytes"] = ( int64_t )tracked_bytes;
	memory["native_runtime_arena_reserved_bytes"] = ( int64_t )m_runtimeMemoryArena.capacity();
	memory["allocator_zone_reserved_bytes"] = ( int64_t )zone.m_nReservedBytes;
	memory["tracked_peak_residency_bytes"] = ( int64_t )m_nTrackedMemoryPeakBytes;
	memory["budget"] = getMemoryBudgetStats();
	memory["performance_target"] = ( int )( m_performanceTarget );
	memory["performance_profile_status"] = ( int )( m_performanceProfileStatus );
	memory["performance_profile_version"] = ( int )( s_nProfileVersion );
	memory["performance_profile_signature"] = getPerformanceProfileSignature();
	memory["scope"] =
			"Terrain3D-owned tracked memory; external Godot/renderer/driver allocations are explicitly unavailable.";
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
	const uint64_t lightmap_bytes =
			m_pLightmapStreamer ? ( uint64_t )( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t )( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes =
			m_material.is_valid() ? ( uint64_t )( m_material->getShaderVariantSourceBytesNative() ) : 0u;
	const uint64_t managed_bytes = gpu_page_bytes + middle_height_bytes + cpu_page_bytes + instance_page_bytes +
			lightmap_bytes + hlod_bytes + shader_source_bytes;
	const uint64_t fixed_bytes = m_nMemoryBudgetResult.m_nFixedBytes;
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_bytes = fixed_bytes + managed_bytes + transient_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	memory["configured_budget_bytes"] = ( int64_t )( ( uint64_t )( m_nTotalMemoryBudgetMb )*s_nMemoryMib );
	memory["current_known_bytes"] = ( int64_t )tracked_bytes;
	memory["logical_tracked_bytes"] = ( int64_t )tracked_bytes;
	memory["allocator_reserve_bytes"] = ( int64_t )m_nMemoryBudgetResult.m_nAllocatorReserveBytes;
	memory["budget_committed_bytes"] = ( int64_t )( fixed_bytes + m_nMemoryBudgetResult.m_nAllocatorReserveBytes +
			managed_bytes + transient_bytes );
	memory["peak_known_bytes"] = ( int64_t )( m_nTrackedMemoryPeakBytes );
	memory["fixed_bytes"] = ( int64_t )( fixed_bytes );
	memory["managed_resident_bytes"] = ( int64_t )( managed_bytes );
	memory["middle_height_page_pool_bytes"] = ( int64_t )( middle_height_bytes );
	memory["transient_bytes"] = ( int64_t )( transient_bytes );
	memory["known_headroom_bytes"] = ( int64_t )( m_nMemoryBudgetResult.m_nHeadroomBytes );
	memory["overcommitted"] = m_nMemoryBudgetResult.m_bHardOvercommit;
	const bool bPressure = m_nMemoryBudgetResult.m_bHardOvercommit ||
			m_nMemoryBudgetResult.m_nHeadroomBytes < s_nMemoryPolicyHysteresisBytes;
	memory["pressure_state"] =
			m_nMemoryBudgetResult.m_bHardOvercommit ? "overcommit" : ( bPressure ? "pressure" : "normal" );
	memory["external_untracked_accuracy"] = "unavailable";
	memory["external_untracked"] =
			"Godot object/container overhead, renderer/driver padding, and shader pipeline allocations";
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
		if ( allocation.m_limitingReason != TERRAIN3D_MEMORY_LIMIT_NONE &&
				allocation.m_limitingReason != TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE ) {
			constrained.push_back( s_pszMemorySubsystemNames[index] );
		}
	}
	memory["automatic_allocations"] = reallocations;
	memory["constrained_subsystems"] = constrained;
	return memory;
}
