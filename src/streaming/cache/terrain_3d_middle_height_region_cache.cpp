// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypeSupport.h"
#include "streaming/residency/terrain_3d_region_residency_primitives.h"
#include "streaming/cache/terrain_3d_middle_height_region_cache.h"
#include "streaming/residency/terrain_3d_region_candidate_collector.h"

#include "data/persistence/terrain_3d_region_codec.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"

#include <string.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>


Terrain3DMiddleHeightRegionCache::Terrain3DMiddleHeightRegionCache(
        Terrain3DRegionRepository *pRepository, Terrain3DSamplingStorage *pSamplingStorage, Terrain3DRuntimePageStore *pRuntimePageStore ) :
        m_pRepository( pRepository ), m_pSamplingStorage( pSamplingStorage ), m_pRuntimePageStore( pRuntimePageStore ) {
	m_generatedHeightMaps.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	m_generatedRegionMap.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	reset();
}

void Terrain3DMiddleHeightRegionCache::reset( void ) {
	m_generatedHeightMaps.clear();
	m_generatedRegionMap.clear();
	memset( m_szRegionMap, 0, sizeof( m_szRegionMap ) );
	for ( size_t iLocation = 0u; iLocation < T3D_ARRAY_COUNT( m_szLocations ); iLocation++ ) {
		t3dRegionCoord_t &location = ( m_szLocations )[iLocation];
		location = t3dInvalidRegionCoord();
	}
	memset( m_szLastUseTick, 0, sizeof( m_szLastUseTick ) );
	memset( m_szRetireAfterFrame, 0, sizeof( m_szRetireAfterFrame ) );
	memset( m_szSlotStates, 0, sizeof( m_szSlotStates ) );
	m_nPhysicalLayerCount = 0;
	m_nMappedCapacity = 0;
	m_nResidentCount = 0;
	m_nDesiredLastTick = 0;
	m_nMissingRequiredLastTick = 0;
	m_nUploadsLastTick = 0;
	m_nEvictionsLastTick = 0;
	m_nTick = 0u;
	m_center = s_v2iMax;
	m_bTexturesReady = false;
	m_bPageTableDirty = true;
	m_bSelectionDirty = true;
	m_nStreamingGenerationLast = 0u;
}

bool Terrain3DMiddleHeightRegionCache::configure( const t3dMiddleHeightRegionCacheConfig_t &rConfig ) {
	m_config = rConfig;
	if ( !m_pRepository || m_pRepository->getActiveRegionCount() <= 0 ) {
		return false;
	}

	const int hot_radius = rConfig.m_bAuthoring ? T3D_REGION_MAP_SIZE : rConfig.m_nHotRadius;
	const int diameter = hot_radius * 2 + 1;
	const int required_ceiling = rConfig.m_bAuthoring ? m_pRepository->getActiveRegionCount()
	                                                  : MIN( m_pRepository->getActiveRegionCount(), diameter * diameter );
	const int boundary_spares = rConfig.m_bAuthoring ? 0 : MAX( T3D_GPU_HEIGHT_CACHE_MIN_SPARE_LAYERS, 4 * hot_radius + 4 );
	m_nMappedCapacity = MIN( required_ceiling, T3D_GPU_HEIGHT_CACHE_MAX_LAYERS );
	m_nPhysicalLayerCount =
	        MIN( m_pRepository->getActiveRegionCount(), MIN( T3D_GPU_HEIGHT_CACHE_MAX_LAYERS, m_nMappedCapacity + boundary_spares ) );
	if ( m_nPhysicalLayerCount <= 0 || m_nMappedCapacity < required_ceiling ) {
		UtilityFunctions::push_error( "Terrain3D: Middle height cache cannot satisfy the configured Active + Hot footprint." );
		return false;
	}

	const int divisor = rConfig.m_bAuthoring ? 1 : rConfig.m_nResolutionDivisor;
	const int height_side = MAX( 1, ( T3D_REGION_SIZE + divisor - 1 ) / divisor );
	PackedByteArray default_height_bytes;
	default_height_bytes.resize( ( int64_t )( height_side ) * ( int64_t )( height_side ) * 2 );
	const uint16_t default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
	for ( int index = 0; index < height_side * height_side; ++index ) {
		memcpy( default_height_bytes.ptrw() + ( size_t )( index ) * 2u, &default_height, sizeof( default_height ) );
	}
	const Ref<Image> pMiddleDefault = Image::create_from_data( height_side, height_side, false, Image::FORMAT_R16, default_height_bytes );
	if ( pMiddleDefault.is_null() ) {
		return false;
	}
	TypedArray<Image> layers;
	layers.resize( m_nPhysicalLayerCount );
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		layers[layer] = pMiddleDefault;
	}
	if ( !m_generatedHeightMaps.create( layers, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid() ) {
		UtilityFunctions::push_error( "Terrain3D: Failed to allocate the Middle height-only texture array." );
		reset();
		return false;
	}
	memset( m_szRegionMap, 0, sizeof( m_szRegionMap ) );
	for ( size_t iLocation = 0u; iLocation < T3D_ARRAY_COUNT( m_szLocations ); iLocation++ ) {
		t3dRegionCoord_t &location = ( m_szLocations )[iLocation];
		location = t3dInvalidRegionCoord();
	}
	memset( m_szSlotStates, 0, sizeof( m_szSlotStates ) );
	m_bTexturesReady = true;
	m_bPageTableDirty = true;
	return updatePageTableTexture();
}

int Terrain3DMiddleHeightRegionCache::findSlot( const Vector2i &vLocation ) const {
	if ( !m_pRepository || !m_pRepository->isLocationValid( vLocation ) ) {
		return -1;
	}
	return t3dRegionResidencyFindMappedSlot( vLocation, m_szRegionMap, m_szLocations, m_nPhysicalLayerCount,
	        [&]( const int layer ) { return m_szSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED; } );
}

Ref<Image> Terrain3DMiddleHeightRegionCache::makeHeightImage( const Vector2i &vLocation, const bool bRuntimeResolution ) const {
	const int divisor = ( bRuntimeResolution || !m_config.m_bAuthoring ) ? m_config.m_nResolutionDivisor : 1;
	return makeHeightImageForDivisor( vLocation, divisor );
}

Ref<Image> Terrain3DMiddleHeightRegionCache::makeHeightImageForDivisor( const Vector2i &vLocation, const int nResolutionDivisor ) const {
	if ( !m_pRepository || !m_pSamplingStorage ) {
		return Ref<Image>();
	}
	const int divisor = nResolutionDivisor <= 1 ? 1 : ( nResolutionDivisor <= 2 ? 2 : 4 );
	Terrain3DActiveMapLease lease = m_pSamplingStorage->acquireActive( vLocation );
	const uint16_t *pActiveHeight = NULL;
	PackedByteArray source_bytes;
	const uint8_t *pSourceBytes = NULL;
	if ( lease.valid() && lease.view().m_pHeight ) {
		pActiveHeight = lease.view().m_pHeight;
	} else {
		const Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
		const Ref<Image> pHeight = pRegion && !pRegion->isDeleted() ? pRegion->getHeightMap() : Ref<Image>();
		if ( pHeight.is_null() || pHeight->get_width() != T3D_REGION_SIZE || pHeight->get_height() != T3D_REGION_SIZE
		        || pHeight->get_format() != Image::FORMAT_R16 || pHeight->has_mipmaps() ) {
			return Ref<Image>();
		}
		source_bytes = pHeight->get_data();
		const size_t expected_source_bytes = ( size_t )( T3D_REGION_SIZE ) * ( size_t )( T3D_REGION_SIZE ) * sizeof( uint16_t );
		if ( ( size_t )( source_bytes.size() ) != expected_source_bytes ) {
			return Ref<Image>();
		}
		pSourceBytes = source_bytes.ptr();
	}
	const int side = MAX( 1, ( T3D_REGION_SIZE + divisor - 1 ) / divisor );
	const size_t byte_count = ( size_t )( side ) * ( size_t )( side ) * sizeof( uint16_t );
	PackedByteArray bytes;
	bytes.resize( ( int64_t )( byte_count ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return Ref<Image>();
	}
	if ( divisor == 1 ) {
		memcpy( pWrite, pActiveHeight ? ( const void * )( pActiveHeight ) : ( const void * )( pSourceBytes ), byte_count );
	} else {
		for ( int y = 0; y < side; ++y ) {
			const int source_y = MIN( y * divisor, T3D_REGION_SIZE - 1 );
			for ( int x = 0; x < side; ++x ) {
				const int source_x = MIN( x * divisor, T3D_REGION_SIZE - 1 );
				uint16_t value = 0u;
				if ( pActiveHeight ) {
					value = pActiveHeight[source_y * T3D_REGION_SIZE + source_x];
				} else {
					const size_t source_offset = ( size_t )( source_y * T3D_REGION_SIZE + source_x ) * sizeof( uint16_t );
					memcpy( &value, pSourceBytes + source_offset, sizeof( value ) );
				}
				memcpy( pWrite + ( size_t )( y * side + x ) * sizeof( uint16_t ), &value, sizeof( value ) );
			}
		}
	}
	return Image::create_from_data( side, side, false, Image::FORMAT_R16, bytes );
}

bool Terrain3DMiddleHeightRegionCache::uploadLayer( const int nLayer, const Vector2i &vLocation, const Ref<Image> &rPreparedHeight ) {
	if ( !m_bTexturesReady || nLayer < 0 || nLayer >= m_nPhysicalLayerCount ) {
		return false;
	}
	Ref<Image> pHeight = rPreparedHeight;
	if ( pHeight.is_null() ) {
		pHeight = makeHeightImage( vLocation, false );
	}
	if ( pHeight.is_null() || pHeight->is_empty() || !m_generatedHeightMaps.update( pHeight, nLayer ) ) {
		return false;
	}
	++m_nUploadsLastTick;
	return true;
}

int Terrain3DMiddleHeightRegionCache::collectDesired( const t3dResidencyFrameState_s &rFrame,
        const Vector2i &rCenter,
        Vector2i *pLocations,
        const int nCapacity,
        int &rRequiredCount ) const {
	rRequiredCount = 0;
	if ( !m_pRepository || !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, T3D_GPU_HEIGHT_CACHE_MAX_LAYERS );
	int output_count = 0;
	uint8_t selected_map[T3D_REGION_SLOTS];
	memset( selected_map, 0, sizeof( selected_map ) );
	for ( int state_value = ( int )( TERRAIN3D_REGION_STREAMING_ACTIVE );
	        state_value >= ( int )( TERRAIN3D_REGION_STREAMING_HOT ) && output_count < capacity;
	        --state_value ) {
		output_count = t3dCollectDesiredRegionsForState( *m_pRepository,
		        rFrame,
		        rCenter,
		        T3D_REGION_MAP_SIZE,
		        ( t3dRegionStreamingState_t )( state_value ),
		        pLocations,
		        capacity,
		        output_count,
		        selected_map );
	}
	rRequiredCount = output_count;
	return t3dAppendPrefetchRegionsFromRepositorySlots(
	        *m_pRepository, rFrame, pLocations, capacity, output_count, selected_map );
}

bool Terrain3DMiddleHeightRegionCache::updatePageTableTexture( void ) {
	if ( !m_pRepository || T3D_REGION_MAP_SIZE <= 0 ) {
		return false;
	}
	PackedByteArray bytes;
	bytes.resize( T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * ( int )( sizeof( uint16_t ) ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	memset( pWrite, 0, ( size_t )( bytes.size() ) );
	const int half = T3D_REGION_MAP_SIZE / 2;
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		if ( m_szSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = t3dUnpackRegionCoord( m_szLocations[layer] );
		const int x = location.x + half;
		const int y = location.y + half;
		if ( ( uint32_t )( x ) >= ( uint32_t )( T3D_REGION_MAP_SIZE ) || ( uint32_t )( y ) >= ( uint32_t )( T3D_REGION_MAP_SIZE ) ) {
			continue;
		}
		const uint16_t value = ( uint16_t )( layer + 1 );
		memcpy( pWrite + ( size_t )( y * T3D_REGION_MAP_SIZE + x ) * sizeof( uint16_t ), &value, sizeof( uint16_t ) );
	}
	const Ref<Image> pImage = Image::create_from_data( T3D_REGION_MAP_SIZE, T3D_REGION_MAP_SIZE, false, Image::FORMAT_R16, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bExisting = m_generatedRegionMap.getRid().is_valid();
	bool ok = bExisting ? m_generatedRegionMap.update2d( pImage )
	                    : m_generatedRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	if ( !ok && bExisting ) {
		m_generatedRegionMap.clear();
		ok = m_generatedRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	}
	m_bPageTableDirty = !ok;
	return ok;
}

void Terrain3DMiddleHeightRegionCache::invalidate( const Vector2i &vLocation ) {
	const int layer = findSlot( vLocation );
	if ( layer >= 0 ) {
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
		if ( map_index >= 0 ) {
			m_szRegionMap[map_index] = 0u;
		}
		m_szSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
		m_szLocations[layer] = t3dInvalidRegionCoord();
		m_szRetireAfterFrame[layer] = 0u;
		m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
		m_bPageTableDirty = true;
	}
	m_bSelectionDirty = true;
}

void Terrain3DMiddleHeightRegionCache::update( const t3dResidencyFrameState_s &rFrame,
        const bool bForce,
        const int nEffectiveUploadLayers,
        const t3dMiddleHeightRegionCacheConfig_t &rConfig ) {
	m_config = rConfig;
	const Vector2i &rCenter = rFrame.m_residencyCenter;
	m_nUploadsLastTick = 0;
	m_nEvictionsLastTick = 0;
	m_nMissingRequiredLastTick = 0;
	if ( !m_pRepository || !rFrame.m_bRuntimeTiering || m_pRepository->getActiveRegionCount() <= 0 ) {
		return;
	}
	++m_nTick;
	const uint64_t streaming_generation = rFrame.m_nStreamingGeneration;
	bool bRetirementPending = false;
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		if ( m_szSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED ) {
			bRetirementPending = true;
			break;
		}
	}
	if ( !bForce && m_bTexturesReady && rCenter == m_center && streaming_generation == m_nStreamingGenerationLast && !m_bSelectionDirty
	        && !m_bPageTableDirty && !bRetirementPending ) {
		return;
	}
	const bool large_jump = m_center != s_v2iMax && MAX( Math::abs( rCenter.x - m_center.x ), Math::abs( rCenter.y - m_center.y ) ) > 1;
	if ( !m_bTexturesReady ) {
		if ( !configure( rConfig ) ) {
			return;
		}
	} else if ( large_jump ) {
		memset( m_szRegionMap, 0, sizeof( m_szRegionMap ) );
		for ( size_t iLocation = 0u; iLocation < T3D_ARRAY_COUNT( m_szLocations ); iLocation++ ) {
			t3dRegionCoord_t &location = ( m_szLocations )[iLocation];
			location = t3dInvalidRegionCoord();
		}
		memset( m_szSlotStates, 0, sizeof( m_szSlotStates ) );
		memset( m_szRetireAfterFrame, 0, sizeof( m_szRetireAfterFrame ) );
		m_nResidentCount = 0;
		m_bPageTableDirty = true;
	}
	m_center = rCenter;
	m_nStreamingGenerationLast = streaming_generation;

	Engine *const pEngine = Engine::get_singleton();
	const uint64_t frame = pEngine ? ( uint64_t )( pEngine->get_frames_drawn() ) : m_nTick;
	bRetirementPending = false;
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		if ( m_szSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED ) {
			if ( frame >= m_szRetireAfterFrame[layer] ) {
				m_szSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
				m_szLocations[layer] = t3dInvalidRegionCoord();
				m_szRetireAfterFrame[layer] = 0u;
			} else {
				bRetirementPending = true;
			}
		}
	}

	Vector2i desired[T3D_GPU_HEIGHT_CACHE_MAX_LAYERS];
	int required_count = 0;
	const int desired_count = collectDesired( rFrame, rCenter, desired, m_nMappedCapacity, required_count );
	m_nDesiredLastTick = desired_count;
	uint8_t desired_map[T3D_REGION_SLOTS];
	memset( desired_map, 0, sizeof( desired_map ) );
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		if ( m_szSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = t3dUnpackRegionCoord( m_szLocations[layer] );
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index >= 0 && desired_map[map_index] != 0u ) {
			m_szLastUseTick[layer] = m_nTick;
			continue;
		}
		if ( map_index >= 0 ) {
			m_szRegionMap[map_index] = 0u;
		}
		m_szSlotStates[layer] = TERRAIN3D_GPU_SLOT_RETIRED;
		m_szRetireAfterFrame[layer] = frame + ( uint64_t )( rConfig.m_nRetireFrames );
		m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
		++m_nEvictionsLastTick;
		m_bPageTableDirty = true;
		bRetirementPending = true;
	}

	const int upload_budget = bForce ? T3D_GPU_HEIGHT_CACHE_MAX_LAYERS : MAX( 1, nEffectiveUploadLayers );
	const bool cooked_runtime = rFrame.m_bCookedPagesEnabled;
	bool bAllDesiredResident = true;
	for ( int index = 0; index < desired_count; ++index ) {
		const Vector2i location = desired[index];
		if ( findSlot( location ) >= 0 ) {
			continue;
		}
		bAllDesiredResident = false;
		const bool required = index < required_count;
		if ( cooked_runtime && m_pRuntimePageStore ) {
			m_pRuntimePageStore->request( location, RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT, required ? 2u : 3u, true );
		}
		if ( !bForce && m_nUploadsLastTick >= upload_budget ) {
			if ( required ) {
				++m_nMissingRequiredLastTick;
			}
			continue;
		}
		const int free_layer = t3dRegionResidencyFindFirstSlot( m_nPhysicalLayerCount,
		        [&]( const int layer ) { return m_szSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE; } );
		if ( free_layer < 0 ) {
			if ( required ) {
				++m_nMissingRequiredLastTick;
			}
			continue;
		}
		Ref<Image> pPreparedHeight;
		if ( cooked_runtime
		        && ( !m_pRuntimePageStore
		                || !m_pRuntimePageStore->takeHeight( location, RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT, pPreparedHeight, true ) ) ) {
			if ( required ) {
				++m_nMissingRequiredLastTick;
			}
			continue;
		}
		if ( !uploadLayer( free_layer, location, pPreparedHeight ) ) {
			if ( required ) {
				++m_nMissingRequiredLastTick;
			}
			continue;
		}
		m_szLocations[free_layer] = t3dPackRegionCoord( location );
		m_szLastUseTick[free_layer] = m_nTick;
		m_szSlotStates[free_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index >= 0 ) {
			m_szRegionMap[map_index] = ( uint16_t )( free_layer + 1 );
		}
		++m_nResidentCount;
		m_bPageTableDirty = true;
	}
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findSlot( desired[index] ) < 0 ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	if ( m_bPageTableDirty || bForce ) {
		updatePageTableTexture();
	}
	m_bSelectionDirty = !bAllDesiredResident || bRetirementPending || m_bPageTableDirty;
}

size_t Terrain3DMiddleHeightRegionCache::getReservedBytes( void ) const {
	if ( !m_pRepository || m_nPhysicalLayerCount <= 0 || T3D_REGION_SIZE <= 0 ) {
		return 0u;
	}
	const int divisor = m_config.m_bAuthoring ? 1 : m_config.m_nResolutionDivisor;
	const size_t side = ( size_t )( MAX( 1, ( T3D_REGION_SIZE + divisor - 1 ) / divisor ) );
	const size_t page_table = ( size_t )( MAX( 0, T3D_REGION_MAP_SIZE ) ) * ( size_t )( MAX( 0, T3D_REGION_MAP_SIZE ) ) * sizeof( uint16_t );
	return side * side * sizeof( uint16_t ) * ( size_t )( m_nPhysicalLayerCount ) + page_table;
}

t3dMiddleHeightRegionCacheStats_t Terrain3DMiddleHeightRegionCache::getStats( void ) const {
	t3dMiddleHeightRegionCacheStats_t stats;
	stats.m_nResident = m_nResidentCount;
	stats.m_nMappedCapacity = m_nMappedCapacity;
	stats.m_nPhysicalLayers = m_nPhysicalLayerCount;
	stats.m_nDesiredLastTick = m_nDesiredLastTick;
	stats.m_nMissingRequiredLastTick = m_nMissingRequiredLastTick;
	stats.m_nUploadsLastTick = m_nUploadsLastTick;
	stats.m_nEvictionsLastTick = m_nEvictionsLastTick;
	stats.m_nResolutionDivisor = m_config.m_bAuthoring ? 1 : m_config.m_nResolutionDivisor;
	stats.m_nReservedBytes = getReservedBytes();
	stats.m_bPageTableValid = m_generatedRegionMap.getRid().is_valid();
	stats.m_bHeightArrayValid = m_generatedHeightMaps.getRid().is_valid();
	return stats;
}

Dictionary Terrain3DMiddleHeightRegionCache::getStatsDictionary( void ) const {
	const t3dMiddleHeightRegionCacheStats_t values = getStats();
	Dictionary stats;
	stats["resident"] = values.m_nResident;
	stats["mapped_capacity"] = values.m_nMappedCapacity;
	stats["physical_layers"] = values.m_nPhysicalLayers;
	stats["desired_last_tick"] = values.m_nDesiredLastTick;
	stats["missing_required_last_tick"] = values.m_nMissingRequiredLastTick;
	stats["uploads_last_tick"] = values.m_nUploadsLastTick;
	stats["evictions_last_tick"] = values.m_nEvictionsLastTick;
	stats["page_table_valid"] = values.m_bPageTableValid;
	stats["height_array_valid"] = values.m_bHeightArrayValid;
	stats["resolution_divisor"] = values.m_nResolutionDivisor;
	stats["reserved_bytes"] = ( int64_t )( values.m_nReservedBytes );
	return stats;
}
