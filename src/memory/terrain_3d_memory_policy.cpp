// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
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

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

#include "eastl_config.h"
#include "core/logger.h"
#include "terrain_3d.h"
#include "core/terrain_3d_integration_helpers.h"
#include "rendering/terrain_3d_compute.h"
#include "authoring/terrain_3d_editor.h"
#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_zone_allocator.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_lighting_policy.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "simd/terrain_3d_parallel.h"
#include "streaming/terrain_3d_performance_profile.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "streaming/terrain_3d_streaming_policy.h"
#include "streaming/terrain_3d_upload_bridge.h"
#include "core/terrain_3d_util.h"


bool T3DTerrain::initializeRuntimeMemory( void ) {
	if ( m_bRuntimeMemoryReady ) {
		if ( m_pData && m_pData->isActiveStorageOwnerInternal() ) return true;
		const t3dMemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
		return m_pData && active_span.valid() && m_pData->bindActiveStorageInternal(
			( int ) ( m_nRegionSize ), active_span.data, active_span.size );
	}
	// Arbitrary-lifetime worker/container storage is isolated in the M2 zone.
	// Active/resident fixed page banks retain separate binding work until their
	// P09 immutable-publication topology is enabled; they must never use this lock.
	const size_t active_bytes = terrain3dActiveStorageRequiredBytes( ( int ) ( m_nRegionSize ) );
	const size_t zone_bytes = calculateRuntimeAllocatorZoneBytes();
	if ( active_bytes == 0u || zone_bytes < T3DMemoryZone::SMALL_PAGE_BYTES || active_bytes > std::numeric_limits<size_t>::max() - zone_bytes -
		2u * TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT ) return false;
	T3DMemoryArenaLayout layout( active_bytes + zone_bytes + 2u * TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT );
	if ( !layout.reserveLow( active_bytes, TERRAIN3D_ACTIVE_STORAGE_ALIGNMENT, m_runtimeActiveHeightSlice ) ||
		!layout.reserveLow( zone_bytes, TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT, m_runtimeZoneSlice ) ||
		!layout.seal() || !m_runtimeMemoryArena.initialize( layout ) || !m_runtimeMemoryArena.commit() ) {
		return false;
	}
	const t3dMemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
	const t3dMemoryArenaSpan_t zone_span = m_runtimeMemoryArena.resolve( m_runtimeZoneSlice );
	if ( !m_pData || !active_span.valid() || !m_pData->bindActiveStorageInternal( ( int ) ( m_nRegionSize ), active_span.data, active_span.size ) ||
		!zone_span.valid() || !m_runtimeMemoryZone.initialize( zone_span.data, zone_span.size ) ||
		!m_runtimeMemoryArena.seal() ) {
		m_runtimeMemoryArena.release();
		return false;
	}
	T3DZoneAllocatorRouter::setOverflowPolicy( Engine::get_singleton()->is_editor_hint() ?
		TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP : TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT );
	T3DZoneAllocatorRouter::install( &m_runtimeMemoryZone );
	m_bRuntimeMemoryReady = true;
	return true;
}

void T3DTerrain::markMemoryBudgetDirty( void ) {
	m_bMemoryBudgetPolicyDirty = true;
}

const char *T3DTerrain::memoryLimitReasonName( const t3dMemoryLimitReason_t reason ) {
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

size_t T3DTerrain::calculateRuntimeAllocatorZoneBytes( void ) const {
	const uint64_t total_bytes = ( uint64_t ) m_nTotalMemoryBudgetMb * TERRAIN3D_MEMORY_MIB;
	const uint64_t transient_bytes = ( uint64_t ) m_nMemoryTransientReserveMb * TERRAIN3D_MEMORY_MIB;
	const size_t active_bytes = terrain3dActiveStorageRequiredBytes( ( int ) m_nRegionSize );
	uint64_t target = total_bytes / TERRAIN3D_RUNTIME_ALLOCATOR_BUDGET_DIVISOR;
	target = MAX( target, ( uint64_t ) TERRAIN3D_RUNTIME_ALLOCATOR_MIN_BYTES );
	target = MIN( target, ( uint64_t ) TERRAIN3D_RUNTIME_ALLOCATOR_MAX_BYTES );
	const uint64_t fixed_floor = ( uint64_t ) active_bytes + transient_bytes;
	if ( total_bytes <= fixed_floor + T3DMemoryZone::SMALL_PAGE_BYTES ) return 0u;
	target = MIN( target, total_bytes - fixed_floor );
	target &= ~( ( uint64_t ) T3DMemoryZone::SMALL_PAGE_BYTES - 1u );
	return target >= T3DMemoryZone::SMALL_PAGE_BYTES && target <= SIZE_MAX ? ( size_t ) target : 0u;
}

uint64_t T3DTerrain::calculateFixedMemoryBytes( void ) const {
	uint64_t bytes = sizeof( *this );
	if ( m_pData ) {
		bytes += ( uint64_t ) ( m_pData->getMemoryFixedRepositoryBytes() );
		bytes += getGpuFarAtlasBytes();
	}
	if ( m_pAssets.is_valid() ) {
		bytes += ( uint64_t ) ( m_pAssets->getGeneratedTexturePayloadBytes() );
	}
	if ( m_pTerrainMesher ) {
		bytes += ( uint64_t ) ( m_pTerrainMesher->getVisibleGeometryPayloadBytes() );
		bytes += ( uint64_t ) ( m_pTerrainMesher->getShadowGeometryPayloadBytes() );
	}
	if ( m_pInstancer ) {
		bytes += ( uint64_t ) ( sizeof( T3DInstancer ) );
		bytes += ( uint64_t ) ( m_pInstancer->getActiveMultimeshBytes() );
		bytes += ( uint64_t ) ( m_pInstancer->getRetainedContainerBytes() );
	}
	if ( m_pCollision ) {
		bytes += ( uint64_t ) ( sizeof( T3DCollision ) );
	}
	if ( m_pNodeStreamer ) {
		bytes += ( uint64_t ) ( sizeof( T3DNodeStreamer ) );
		bytes += ( uint64_t ) ( m_pNodeStreamer->getRetainedCapacityBytes() );
	}
	if ( m_pLightmapStreamer ) {
		bytes += ( uint64_t ) ( sizeof( T3DLightmapStreamer ) );
	}
	if ( m_pRuntimeProfileHistory ) {
		bytes += ( uint64_t ) ( sizeof( t3dRuntimeProfileHistory_t ) );
	}
	return bytes;
}

uint64_t T3DTerrain::calculateTransientMemoryBytes( void ) const {
	uint64_t bytes = 0u;
	if ( m_pData ) {
		bytes += ( uint64_t ) ( m_pData->getMemoryTransientStagingBytes() );
	}
	if ( m_pTerrainMesher ) {
		bytes += ( uint64_t ) ( m_pTerrainMesher->getStreamingBufferBytes() );
	}
	if ( m_pCollision ) {
		bytes += ( uint64_t ) ( m_pCollision->getRetainedBufferBytes() );
	}
	return bytes;
}

t3dMemoryBudgetInput_t T3DTerrain::buildMemoryBudgetInput( void ) const {
	t3dMemoryBudgetInput_t input;
	input.nTotalBudgetBytes = ( uint64_t ) ( m_nTotalMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	input.nFixedBytes = calculateFixedMemoryBytes();
	input.nAllocatorReserveBytes = m_runtimeMemoryZone.capacity() > 0u ? ( uint64_t ) m_runtimeMemoryZone.capacity() : ( uint64_t ) calculateRuntimeAllocatorZoneBytes();
	input.nTransientReserveBytes = ( uint64_t ) ( m_nMemoryTransientReserveMb ) * TERRAIN3D_MEMORY_MIB;

	t3dMemorySubsystemInput_t &gpu = input.subsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.bEnabled = effectiveGpuRegionCacheEnabled();
	gpu.bAutomatic = m_bGpuRegionCacheBudgetAuto;
	gpu.nManualBytes = ( uint64_t ) ( m_nGpuRegionCacheBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	gpu.nMinimumBytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheMinimumBytes() ) : TERRAIN3D_MEMORY_MIB;
	gpu.nHardCapBytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheHardCapBytes() ) : 128u * TERRAIN3D_MEMORY_MIB;
	gpu.weight = 5u;

	t3dMemorySubsystemInput_t &cpu = input.subsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES];
	cpu.bEnabled = effectiveCpuRegionCacheEnabled();
	cpu.bAutomatic = m_bCpuRegionCacheBudgetAuto;
	cpu.nManualBytes = ( uint64_t ) ( m_nCpuRegionCacheBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	cpu.nMinimumBytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheMinimumBytes() ) : 4u * TERRAIN3D_MEMORY_MIB;
	cpu.nHardCapBytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheHardCapBytes() ) : 128u * TERRAIN3D_MEMORY_MIB;
	cpu.weight = 4u;

	t3dMemorySubsystemInput_t &instances = input.subsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES];
	instances.bEnabled = effectiveInstanceRegionCacheEnabled();
	instances.bAutomatic = m_bInstanceRegionCacheBudgetAuto;
	instances.nManualBytes = ( uint64_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	instances.nMinimumBytes = TERRAIN3D_MEMORY_MIB;
	instances.nHardCapBytes = 128u * TERRAIN3D_MEMORY_MIB;
	instances.weight = 2u;

	t3dMemorySubsystemInput_t &lighting = input.subsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING];
	lighting.bEnabled = m_bLightmapStreamingEnabled;
	lighting.bAutomatic = m_bLightmapResidencyBudgetAuto;
	lighting.nManualBytes = ( uint64_t ) ( m_nLightmapResidencyBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	lighting.nMinimumBytes = TERRAIN3D_MEMORY_MIB;
	lighting.nHardCapBytes = 1024u * TERRAIN3D_MEMORY_MIB;
	lighting.weight = 2u;

	t3dMemorySubsystemInput_t &hlod = input.subsystems[TERRAIN3D_MEMORY_HLOD_PROXIES];
	hlod.bEnabled = m_bHlodEnabled;
	hlod.bAutomatic = m_bHlodMemoryBudgetAuto;
	hlod.nManualBytes = ( uint64_t ) ( m_nHlodMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	hlod.nMinimumBytes = 4u * TERRAIN3D_MEMORY_MIB;
	hlod.nHardCapBytes = 4096u * TERRAIN3D_MEMORY_MIB;
	hlod.weight = 2u;

	t3dMemorySubsystemInput_t &shader = input.subsystems[TERRAIN3D_MEMORY_SHADER_CACHE];
	shader.bEnabled = m_pMaterial.is_valid();
	shader.bAutomatic = m_bShaderCacheBudgetAuto;
	shader.nManualBytes = ( uint64_t ) ( m_pMaterial.is_valid() ? m_pMaterial->getShaderVariantSourceBudgetMb() : 8 ) * TERRAIN3D_MEMORY_MIB;
	shader.nMinimumBytes = ( uint64_t ) ( TERRAIN3D_MIN_SHADER_VARIANT_SOURCE_BUDGET_MB ) * TERRAIN3D_MEMORY_MIB;
	shader.nHardCapBytes = ( uint64_t ) ( TERRAIN3D_MAX_SHADER_VARIANT_SOURCE_BUDGET_MB ) * TERRAIN3D_MEMORY_MIB;
	shader.weight = 1u;
	return input;
}

void T3DTerrain::applyMemoryBudgetResult( const t3dMemoryBudgetResult_t &rResult, const bool bForce ) {
	const int previous_effective_mb[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_nEffectiveGpuRegionCacheBudgetMb,
		m_nEffectiveCpuRegionCacheBudgetMb,
		m_nEffectiveInstanceRegionCacheBudgetMb,
		m_nEffectiveLightmapResidencyBudgetMb,
		m_nEffectiveHlodMemoryBudgetMb,
		m_nEffectiveShaderCacheBudgetMb,
	};
	const uint64_t fixed_far_bytes = getGpuFarAtlasBytes();
	const uint64_t gpu_bytes = rResult.subsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].nEffectiveBytes + fixed_far_bytes;
	const int gpu_mb = MAX( 1, ( int ) ( ( gpu_bytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int cpu_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int instance_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int lightmap_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int hlod_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_HLOD_PROXIES].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int shader_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_SHADER_CACHE].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );

	const int hysteresis_mb = ( int ) ( TERRAIN3D_MEMORY_POLICY_HYSTERESIS_BYTES / TERRAIN3D_MEMORY_MIB );
	const bool bGpuChanged = bForce || std::abs( gpu_mb - m_nEffectiveGpuRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bCpuChanged = bForce || std::abs( cpu_mb - m_nEffectiveCpuRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bInstanceChanged = bForce || std::abs( instance_mb - m_nEffectiveInstanceRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bLightmapChanged = bForce || std::abs( lightmap_mb - m_nEffectiveLightmapResidencyBudgetMb ) >= hysteresis_mb;
	const bool bHlodChanged = bForce || std::abs( hlod_mb - m_nEffectiveHlodMemoryBudgetMb ) >= hysteresis_mb;
	const bool bShaderChanged = bForce || std::abs( shader_mb - m_nEffectiveShaderCacheBudgetMb ) >= hysteresis_mb;

	if ( bGpuChanged ) {
		m_nEffectiveGpuRegionCacheBudgetMb = gpu_mb;
		if ( m_pData ) {
			m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
				m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb, m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
		}
	}
	if ( bCpuChanged ) {
		m_nEffectiveCpuRegionCacheBudgetMb = cpu_mb;
		if ( m_pData ) {
			m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(), m_nEffectiveCpuRegionCacheBudgetMb,
				m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
		}
	}
	if ( bInstanceChanged ) {
		m_nEffectiveInstanceRegionCacheBudgetMb = instance_mb;
		if ( m_pData ) {
			m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
				m_nEffectiveInstanceRegionCacheBudgetMb, m_nInstanceRegionLoadBudgetPages,
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
		if ( m_pHlodStreamer ) { m_pHlodStreamer->setMemoryBudgetMb( m_nEffectiveHlodMemoryBudgetMb ); }
	}
	if ( bShaderChanged ) {
		m_nEffectiveShaderCacheBudgetMb = shader_mb;
		if ( m_pMaterial.is_valid() ) {
			m_pMaterial->setEffectiveShaderVariantSourceBudgetMb( m_nEffectiveShaderCacheBudgetMb );
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
		emit_signal( "cache_budget_changed", StringName( TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index] ),
			( int64_t ) ( rResult.subsystems[( size_t ) ( index )].nEffectiveBytes ),
			StringName( memoryLimitReasonName( rResult.subsystems[( size_t ) ( index )].limitingReason ) ) );
	}
}

void T3DTerrain::serviceMemoryBudgetPolicy( const bool bForce ) {
	if ( !bForce ) {
		++m_nMemoryBudgetPolicyFrame;
		if ( ( m_nMemoryBudgetPolicyFrame % ( uint64_t ) ( TERRAIN3D_MEMORY_POLICY_INTERVAL_FRAMES ) ) != 0u ) {
			return;
		}
	}
	const t3dMemoryBudgetInput_t input = buildMemoryBudgetInput();
	const t3dMemoryBudgetResult_t result = T3DMemoryBudgetCoordinator::calculate( input );
	applyMemoryBudgetResult( result, bForce || m_nMemoryBudgetPolicyGeneration == 0u );
	const bool bPressure = result.bHardOvercommit || result.nHeadroomBytes < TERRAIN3D_MEMORY_POLICY_HYSTERESIS_BYTES;
	if ( bPressure != m_bMemoryPressureEventState || result.bHardOvercommit != m_bMemoryOvercommitEventState ) {
		m_bMemoryPressureEventState = bPressure;
		m_bMemoryOvercommitEventState = result.bHardOvercommit;
		emit_signal( "memory_pressure_changed", bPressure, result.bHardOvercommit, ( int64_t ) ( result.nHeadroomBytes ) );
	}
	m_nMemoryBudgetResult = result;
	m_bMemoryBudgetPolicyDirty = false;
	++m_nMemoryBudgetPolicyGeneration;
}

void T3DTerrain::forceMemoryBudgetRebalance( void ) {
	serviceMemoryBudgetPolicy( true );
	notify_property_list_changed();
}

void T3DTerrain::setTotalMemoryBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 32, 4096 );
	if ( m_nTotalMemoryBudgetMb == budget_mb ) {
		return;
	}
	m_nTotalMemoryBudgetMb = budget_mb;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
}

void T3DTerrain::setMemoryTransientReserveMb( const int nReserveMb ) {
	const int reserve_mb = CLAMP( nReserveMb, 4, 1024 );
	if ( m_nMemoryTransientReserveMb == reserve_mb ) {
		return;
	}
	m_nMemoryTransientReserveMb = reserve_mb;
	markMemoryBudgetDirty();
}

void T3DTerrain::setGpuRegionCacheBudgetAuto( const bool bEnabled ) {
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

void T3DTerrain::setCpuRegionCacheBudgetAuto( const bool bEnabled ) {
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

void T3DTerrain::setInstanceRegionCacheBudgetAuto( const bool bEnabled ) {
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

void T3DTerrain::setLightmapResidencyBudgetAuto( const bool bEnabled ) {
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

void T3DTerrain::setLightmapResidencyBudgetMb( const int nBudgetMb ) {
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

void T3DTerrain::setHlodMemoryBudgetAuto( const bool bEnabled ) {
	if ( m_bHlodMemoryBudgetAuto == bEnabled ) { return; }
	m_bHlodMemoryBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) { serviceMemoryBudgetPolicy( true ); }
}

void T3DTerrain::setShaderCacheBudgetAuto( const bool bEnabled ) {
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

Dictionary T3DTerrain::getMemoryBudgetStats( void ) const {
	Dictionary stats;
	stats["total_budget_mb"] = m_nTotalMemoryBudgetMb;
	stats["total_budget_bytes"] = ( int64_t ) ( ( uint64_t ) ( m_nTotalMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB );
	stats["transient_reserve_mb"] = m_nMemoryTransientReserveMb;
	stats["fixed_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nFixedBytes );
	stats["allocator_reserve_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nAllocatorReserveBytes );
	stats["allocator_target_bytes"] = ( int64_t ) calculateRuntimeAllocatorZoneBytes();
	stats["allocator_overflow_policy"] = T3DZoneAllocatorRouter::overflowPolicy() == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ? "heap" : "strict";
	stats["managed_budget_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nManagedBudgetBytes );
	stats["managed_effective_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nManagedEffectiveBytes );
	stats["manual_reserved_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nManualReservedBytes );
	stats["auto_minimum_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nAutoMinimumBytes );
	stats["headroom_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nHeadroomBytes );
	stats["hard_overcommit"] = m_nMemoryBudgetResult.bHardOvercommit;
	stats["policy_generation"] = ( int64_t ) ( m_nMemoryBudgetPolicyGeneration );
	stats["policy_interval_frames"] = TERRAIN3D_MEMORY_POLICY_INTERVAL_FRAMES;

	Array subsystems;
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		Dictionary subsystem;
		const t3dMemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.subsystems[( size_t ) ( index )];
		subsystem["name"] = TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index];
		subsystem["requested_bytes"] = ( int64_t ) ( allocation.nRequestedBytes );
		subsystem["effective_bytes"] = ( int64_t ) ( allocation.nEffectiveBytes );
		subsystem["limiting_constraint"] = memoryLimitReasonName( allocation.limitingReason );
		subsystem["below_minimum"] = allocation.bBelowMinimum;
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
	stats["shader_requested_mb"] = m_pMaterial.is_valid() ? m_pMaterial->getShaderVariantSourceBudgetMb() : 0;
	stats["shader_effective_mb"] = m_nEffectiveShaderCacheBudgetMb;
	return stats;
}

Dictionary T3DTerrain::getTrackedCurrentResidency( void ) const {
	Dictionary memory;
	const uint64_t gpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t middle_height_bytes = m_pData ? ( uint64_t ) ( m_pData->getMiddleHeightCacheReservedBytes() ) : 0u;
	const uint64_t cpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t instance_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u;
	const uint64_t lightmap_bytes = m_pLightmapStreamer ? ( uint64_t ) ( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t ) ( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes = m_pMaterial.is_valid() ? ( uint64_t ) ( m_pMaterial->getShaderVariantSourceBytesNative() ) : 0u;
	const uint64_t managed_bytes = gpu_page_bytes + middle_height_bytes + cpu_page_bytes + instance_page_bytes + lightmap_bytes + hlod_bytes + shader_source_bytes;
	const uint64_t fixed_bytes = calculateFixedMemoryBytes();
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_bytes = fixed_bytes + managed_bytes + transient_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	Dictionary fixed;
	fixed["bytes"] = ( int64_t ) ( fixed_bytes );
	fixed["accuracy"] = "estimated";
	fixed["material_texture_arrays_bytes"] = ( int64_t ) ( m_pAssets.is_valid() ? m_pAssets->getGeneratedTexturePayloadBytes() : 0u );
	fixed["far_atlas_bytes"] = ( int64_t ) ( getGpuFarAtlasBytes() );
	fixed["clipmap_geometry_bytes"] = ( int64_t ) ( m_pTerrainMesher ? m_pTerrainMesher->getVisibleGeometryPayloadBytes() : 0u );
	fixed["shadow_geometry_bytes"] = ( int64_t ) ( m_pTerrainMesher ? m_pTerrainMesher->getShadowGeometryPayloadBytes() : 0u );
	fixed["repository_and_authoritative_storage_bytes"] = ( int64_t ) ( m_pData ? m_pData->getMemoryFixedRepositoryBytes() : 0u );
	fixed["node_streamer_retained_bytes"] = ( int64_t ) ( m_pNodeStreamer ? m_pNodeStreamer->getRetainedCapacityBytes() : 0u );
	fixed["instancer_retained_bytes"] = ( int64_t ) ( m_pInstancer ? m_pInstancer->getRetainedContainerBytes() : 0u );
	fixed["active_multimesh_bytes"] = ( int64_t ) ( m_pInstancer ? m_pInstancer->getActiveMultimeshBytes() : 0u );
	fixed["profiler_history_bytes"] = ( int64_t ) ( m_pRuntimeProfileHistory ? sizeof( t3dRuntimeProfileHistory_t ) : 0u );
	memory["fixed_unavoidable"] = fixed;

	Dictionary managed;
	managed["bytes"] = ( int64_t ) ( managed_bytes );
	managed["accuracy"] = "estimated";
	managed["gpu_terrain_page_pool_bytes"] = ( int64_t ) ( gpu_page_bytes );
	managed["middle_height_page_pool_bytes"] = ( int64_t ) ( middle_height_bytes );
	managed["cpu_terrain_page_cache_bytes"] = ( int64_t ) ( cpu_page_bytes );
	managed["instance_region_page_bytes"] = ( int64_t ) ( instance_page_bytes );
	managed["baked_lighting_estimated_bytes"] = ( int64_t ) ( lightmap_bytes );
	managed["hlod_proxy_estimated_bytes"] = ( int64_t ) ( hlod_bytes );
	managed["shader_source_cache_bytes"] = ( int64_t ) ( shader_source_bytes );
	memory["managed_resident"] = managed;

	Dictionary transient;
	transient["bytes"] = ( int64_t ) ( transient_bytes );
	transient["accuracy"] = "estimated";
	transient["data_page_and_far_staging_bytes"] = ( int64_t ) ( m_pData ? m_pData->getMemoryTransientStagingBytes() : 0u );
	transient["clipmap_streaming_buffer_bytes"] = ( int64_t ) ( m_pTerrainMesher ? m_pTerrainMesher->getStreamingBufferBytes() : 0u );
	transient["collision_retained_scratch_bytes"] = ( int64_t ) ( m_pCollision ? m_pCollision->getRetainedBufferBytes() : 0u );
	memory["transient_staging"] = transient;

	const t3dEastlAllocationStats_t allocator = T3DEastlAllocator::getAllocationStats();
	Dictionary allocator_stats;
	allocator_stats["live_bytes"] = ( int64_t ) ( allocator.nLiveBytes );
	allocator_stats["peak_bytes"] = ( int64_t ) ( allocator.nPeakBytes );
	allocator_stats["live_allocations"] = ( int64_t ) ( allocator.nLiveAllocations );
	allocator_stats["total_allocations"] = ( int64_t ) ( allocator.nTotalAllocations );
	allocator_stats["failed_allocations"] = ( int64_t ) ( allocator.nFailedAllocations );
	allocator_stats["overflow_allocations"] = ( int64_t ) ( allocator.nOverflowAllocations );
	allocator_stats["overflow_live_bytes"] = ( int64_t ) ( allocator.nOverflowLiveBytes );
	allocator_stats["accuracy"] = "exact";
	allocator_stats["source"] = "release_safe_allocator_telemetry";
	allocator_stats["included_in_total"] = false;
	allocator_stats["note"] = "Cross-cutting EASTL telemetry overlaps subsystem container accounting and is not summed twice.";
	memory["eastl_allocator"] = allocator_stats;
	const t3dMemoryZoneStats_t zone = m_runtimeMemoryZone.getStats();
	Dictionary zone_stats;
	zone_stats["state"] = ( int ) m_runtimeMemoryZone.state();
	zone_stats["reserved_bytes"] = ( int64_t ) zone.nReservedBytes;
	zone_stats["live_requested_bytes"] = ( int64_t ) zone.nLiveRequestedBytes;
	zone_stats["live_physical_bytes"] = ( int64_t ) zone.nLivePhysicalBytes;
	zone_stats["live_allocations"] = ( int64_t ) zone.nLiveAllocationCount;
	zone_stats["extent_free_bytes"] = ( int64_t ) zone.nExtentFreeBytes;
	zone_stats["small_page_reserved_bytes"] = ( int64_t ) zone.nSmallPageReservedBytes;
	zone_stats["small_page_reusable_payload_bytes"] = ( int64_t ) zone.nSmallPageReusablePayloadBytes;
	zone_stats["small_page_reusable_physical_bytes"] = ( int64_t ) zone.nSmallPageReusablePhysicalBytes;
	zone_stats["large_allocation_physical_bytes"] = ( int64_t ) zone.nLargeAllocationPhysicalBytes;
	zone_stats["metadata_bytes"] = ( int64_t ) zone.nMetadataBytes;
	zone_stats["alignment_waste_bytes"] = ( int64_t ) zone.nAlignmentWasteBytes;
	zone_stats["size_class_waste_bytes"] = ( int64_t ) zone.nSizeClassWasteBytes;
	zone_stats["internal_fragmentation_bytes"] = ( int64_t ) zone.nInternalFragmentationBytes;
	zone_stats["failed_allocations"] = ( int64_t ) zone.nFailedAllocations;
	zone_stats["active_small_pages"] = ( int64_t ) zone.nActiveSmallPages;
	zone_stats["peak_active_small_pages"] = ( int64_t ) zone.nPeakActiveSmallPages;
	zone_stats["free_extent_count"] = ( int64_t ) zone.nFreeExtentCount;
	zone_stats["peak_free_extent_count"] = ( int64_t ) zone.nPeakFreeExtentCount;
	zone_stats["overflow_policy"] = T3DZoneAllocatorRouter::overflowPolicy() == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ? "heap" : "strict";
	zone_stats["arena_reserved_bytes"] = ( int64_t ) m_runtimeMemoryArena.capacity();
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

	memory["tracked_current_residency_bytes"] = ( int64_t ) tracked_bytes;
	memory["tracked_logical_payload_bytes"] = ( int64_t ) tracked_bytes;
	memory["native_runtime_arena_reserved_bytes"] = ( int64_t ) m_runtimeMemoryArena.capacity();
	memory["allocator_zone_reserved_bytes"] = ( int64_t ) zone.nReservedBytes;
	memory["tracked_peak_residency_bytes"] = ( int64_t ) m_nTrackedMemoryPeakBytes;
	memory["budget"] = getMemoryBudgetStats();
	memory["performance_target"] = ( int ) ( m_performanceTarget );
	memory["performance_profile_status"] = ( int ) ( m_performanceProfileStatus );
	memory["performance_profile_version"] = ( int ) ( TERRAIN3D_PROFILE_VERSION );
	memory["performance_profile_signature"] = getPerformanceProfileSignature();
	memory["scope"] = "Terrain3D-owned tracked memory; external Godot/renderer/driver allocations are explicitly unavailable.";
	return memory;
}

Dictionary T3DTerrain::getMemoryEstimate( void ) const {
	return getTrackedCurrentResidency();
}

Dictionary T3DTerrain::getRuntimeMemoryStatsFast( void ) const {
	Dictionary memory;
	const uint64_t gpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t middle_height_bytes = m_pData ? ( uint64_t ) ( m_pData->getMiddleHeightCacheReservedBytes() ) : 0u;
	const uint64_t cpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t instance_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u;
	const uint64_t lightmap_bytes = m_pLightmapStreamer ? ( uint64_t ) ( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t ) ( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes = m_pMaterial.is_valid() ? ( uint64_t ) ( m_pMaterial->getShaderVariantSourceBytesNative() ) : 0u;
	const uint64_t managed_bytes = gpu_page_bytes + middle_height_bytes + cpu_page_bytes + instance_page_bytes + lightmap_bytes + hlod_bytes + shader_source_bytes;
	const uint64_t fixed_bytes = m_nMemoryBudgetResult.nFixedBytes;
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_bytes = fixed_bytes + managed_bytes + transient_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	memory["configured_budget_bytes"] = ( int64_t ) ( ( uint64_t ) ( m_nTotalMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB );
	memory["current_known_bytes"] = ( int64_t ) tracked_bytes;
	memory["logical_tracked_bytes"] = ( int64_t ) tracked_bytes;
	memory["allocator_reserve_bytes"] = ( int64_t ) m_nMemoryBudgetResult.nAllocatorReserveBytes;
	memory["budget_committed_bytes"] = ( int64_t ) ( fixed_bytes + m_nMemoryBudgetResult.nAllocatorReserveBytes + managed_bytes + transient_bytes );
	memory["peak_known_bytes"] = ( int64_t ) ( m_nTrackedMemoryPeakBytes );
	memory["fixed_bytes"] = ( int64_t ) ( fixed_bytes );
	memory["managed_resident_bytes"] = ( int64_t ) ( managed_bytes );
	memory["middle_height_page_pool_bytes"] = ( int64_t ) ( middle_height_bytes );
	memory["transient_bytes"] = ( int64_t ) ( transient_bytes );
	memory["known_headroom_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nHeadroomBytes );
	memory["overcommitted"] = m_nMemoryBudgetResult.bHardOvercommit;
	const bool bPressure = m_nMemoryBudgetResult.bHardOvercommit ||
		m_nMemoryBudgetResult.nHeadroomBytes < TERRAIN3D_MEMORY_POLICY_HYSTERESIS_BYTES;
	memory["pressure_state"] = m_nMemoryBudgetResult.bHardOvercommit ? "overcommit" : ( bPressure ? "pressure" : "normal" );
	memory["external_untracked_accuracy"] = "unavailable";
	memory["external_untracked"] = "Godot object/container overhead, renderer/driver padding, and shader pipeline allocations";
	memory["budget"] = getMemoryBudgetStats();

	const bool bAutomatic[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_bGpuRegionCacheBudgetAuto, m_bCpuRegionCacheBudgetAuto, m_bInstanceRegionCacheBudgetAuto,
		m_bLightmapResidencyBudgetAuto, m_bHlodMemoryBudgetAuto, m_bShaderCacheBudgetAuto
	};
	Array reallocations;
	Array constrained;
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		const t3dMemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.subsystems[( size_t ) ( index )];
		if ( bAutomatic[index] ) {
			Dictionary entry;
			entry["subsystem"] = TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index];
			entry["effective_bytes"] = ( int64_t ) ( allocation.nEffectiveBytes );
			entry["limiting_constraint"] = memoryLimitReasonName( allocation.limitingReason );
			reallocations.push_back( entry );
		}
		if ( allocation.limitingReason != TERRAIN3D_MEMORY_LIMIT_NONE &&
			allocation.limitingReason != TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE ) {
			constrained.push_back( TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index] );
		}
	}
	memory["automatic_allocations"] = reallocations;
	memory["constrained_subsystems"] = constrained;
	return memory;
}

