// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_middle_height_region_cache.h"

#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_sampling_storage.h"
#include "streaming/terrain_3d_runtime_page_store.h"

#include <algorithm>
#include <cstring>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

template <typename Callback>
static bool
t3dMiddleHeightRegionCacheForEachSquareRing( const Vector2i &rCenter, const int nMaxRadius, Callback &&rCallback ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !rCallback( rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !rCallback( rCenter + Vector2i( x, -radius ) ) || !rCallback( rCenter + Vector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !rCallback( rCenter + Vector2i( -radius, y ) ) || !rCallback( rCenter + Vector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

T3DMiddleHeightRegionCache::T3DMiddleHeightRegionCache( T3DRegionRepository *pRepository,
		T3DSamplingStorage *pSamplingStorage,
		T3DRuntimePageStore *pRuntimePageStore ) :
		m_pRepository( pRepository ), m_pSamplingStorage( pSamplingStorage ), m_pRuntimePageStore( pRuntimePageStore ) {
	reset();
}

void T3DMiddleHeightRegionCache::reset( void ) {
	m_generatedHeightMaps.clear();
	m_generatedRegionMap.clear();
	std::fill_n( m_szRegionMap, s_nMaxRegionSlots, ( uint16_t )( 0 ) );
	std::fill_n( m_szLocations, s_nGpuHeightCacheMaxLayers, s_v2iMax );
	std::fill_n( m_szLastUseTick, s_nGpuHeightCacheMaxLayers, ( uint64_t )( 0 ) );
	std::fill_n( m_szRetireAfterFrame, s_nGpuHeightCacheMaxLayers, ( uint64_t )( 0 ) );
	std::fill_n( m_szSlotStates, s_nGpuHeightCacheMaxLayers, ( uint8_t )( TERRAIN3D_GPU_SLOT_FREE ) );
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

bool T3DMiddleHeightRegionCache::configure( const t3dMiddleHeightRegionCacheConfig_t &rConfig ) {
	m_config = rConfig;
	if ( !m_pRepository || m_pRepository->getRegionSize() <= 0 || m_pRepository->getRegionMapSize() <= 0 ||
			m_pRepository->getActiveRegionCount() <= 0 ) {
		return false;
	}

	const int hot_radius = rConfig.m_bAuthoring ? m_pRepository->getRegionMapSize() : MAX( 0, rConfig.m_nHotRadius );
	const int diameter = hot_radius * 2 + 1;
	const int required_ceiling = rConfig.m_bAuthoring
			? m_pRepository->getActiveRegionCount()
			: MIN( m_pRepository->getActiveRegionCount(), diameter * diameter );
	const int boundary_spares = rConfig.m_bAuthoring ? 0 : MAX( s_nGpuHeightCacheMinSpareLayers, 4 * hot_radius + 4 );
	m_nMappedCapacity = MIN( required_ceiling, s_nGpuHeightCacheMaxLayers );
	m_nPhysicalLayerCount = MIN( m_pRepository->getActiveRegionCount(),
			MIN( s_nGpuHeightCacheMaxLayers, m_nMappedCapacity + boundary_spares ) );
	if ( m_nPhysicalLayerCount <= 0 || m_nMappedCapacity < required_ceiling ) {
		UtilityFunctions::push_error(
				"Terrain3D: Middle height cache cannot satisfy the configured Active + Hot footprint." );
		return false;
	}

	const int divisor = rConfig.m_bAuthoring ? 1 : MAX( 1, rConfig.m_nResolutionDivisor );
	const int height_side = MAX( 1, ( m_pRepository->getRegionSize() + divisor - 1 ) / divisor );
	PackedByteArray default_height_bytes;
	default_height_bytes.resize( ( int64_t )( height_side ) * ( int64_t )( height_side ) * 2 );
	const uint16_t default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
	for ( int index = 0; index < height_side * height_side; ++index ) {
		std::memcpy(
				default_height_bytes.ptrw() + ( size_t )( index ) * 2u, &default_height, sizeof( default_height ) );
	}
	const Ref<Image> pMiddleDefault =
			Image::create_from_data( height_side, height_side, false, Image::FORMAT_R16, default_height_bytes );
	if ( pMiddleDefault.is_null() ) {
		return false;
	}
	TypedArray<Image> layers;
	layers.resize( m_nPhysicalLayerCount );
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		layers[layer] = pMiddleDefault;
	}
	if ( !m_generatedHeightMaps.create( layers ).is_valid() ) {
		UtilityFunctions::push_error( "Terrain3D: Failed to allocate the Middle height-only texture array." );
		reset();
		return false;
	}
	std::fill_n( m_szRegionMap, s_nMaxRegionSlots, ( uint16_t )( 0 ) );
	std::fill_n( m_szLocations, s_nGpuHeightCacheMaxLayers, s_v2iMax );
	std::fill_n( m_szSlotStates, s_nGpuHeightCacheMaxLayers, ( uint8_t )( TERRAIN3D_GPU_SLOT_FREE ) );
	m_bTexturesReady = true;
	m_bPageTableDirty = true;
	return updatePageTableTexture();
}

int T3DMiddleHeightRegionCache::findSlot( const Vector2i &vLocation ) const {
	if ( !m_pRepository ) {
		return -1;
	}
	const int map_index = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( !m_pRepository->isLocationValid( vLocation ) || map_index < 0 ) {
		return -1;
	}
	const int layer = ( int )( m_szRegionMap[map_index] ) - 1;
	if ( layer < 0 || layer >= m_nPhysicalLayerCount || m_szSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ||
			m_szLocations[layer] != vLocation ) {
		return -1;
	}
	return layer;
}

Ref<Image> T3DMiddleHeightRegionCache::makeHeightImage( const Vector2i &vLocation,
		const bool bRuntimeResolution ) const {
	if ( !m_pRepository || !m_pSamplingStorage ) {
		return Ref<Image>();
	}
	T3DActiveMapLease lease = m_pSamplingStorage->acquireActive( vLocation );
	const uint16_t *pActiveHeight = nullptr;
	PackedByteArray source_bytes;
	const uint8_t *pSourceBytes = nullptr;
	if ( lease.valid() && lease.view().m_nRegionSize == m_pRepository->getRegionSize() && lease.view().m_pHeight ) {
		pActiveHeight = lease.view().m_pHeight;
	} else {
		const T3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
		const Ref<Image> pHeight = pRegion && !pRegion->isDeleted() ? pRegion->getHeightMap() : Ref<Image>();
		if ( pHeight.is_null() || pHeight->get_width() != m_pRepository->getRegionSize() ||
				pHeight->get_height() != m_pRepository->getRegionSize() || pHeight->get_format() != Image::FORMAT_R16 ||
				pHeight->has_mipmaps() ) {
			return Ref<Image>();
		}
		source_bytes = pHeight->get_data();
		const size_t expected_source_bytes = ( size_t )( m_pRepository->getRegionSize() ) *
				( size_t )( m_pRepository->getRegionSize() ) * sizeof( uint16_t );
		if ( ( size_t )( source_bytes.size() ) != expected_source_bytes ) {
			return Ref<Image>();
		}
		pSourceBytes = source_bytes.ptr();
	}
	const int divisor = ( bRuntimeResolution || !m_config.m_bAuthoring ) ? MAX( 1, m_config.m_nResolutionDivisor ) : 1;
	const int side = MAX( 1, ( m_pRepository->getRegionSize() + divisor - 1 ) / divisor );
	const size_t byte_count = ( size_t )( side ) * ( size_t )( side ) * sizeof( uint16_t );
	PackedByteArray bytes;
	bytes.resize( ( int64_t )( byte_count ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return Ref<Image>();
	}
	if ( divisor == 1 ) {
		std::memcpy( pWrite,
				pActiveHeight ? ( const void * )( pActiveHeight ) : ( const void * )( pSourceBytes ),
				byte_count );
	} else {
		for ( int y = 0; y < side; ++y ) {
			const int source_y = MIN( y * divisor, m_pRepository->getRegionSize() - 1 );
			for ( int x = 0; x < side; ++x ) {
				const int source_x = MIN( x * divisor, m_pRepository->getRegionSize() - 1 );
				uint16_t value = 0u;
				if ( pActiveHeight ) {
					value = pActiveHeight[source_y * m_pRepository->getRegionSize() + source_x];
				} else {
					const size_t source_offset =
							( size_t )( source_y * m_pRepository->getRegionSize() + source_x ) * sizeof( uint16_t );
					std::memcpy( &value, pSourceBytes + source_offset, sizeof( value ) );
				}
				std::memcpy( pWrite + ( size_t )( y * side + x ) * sizeof( uint16_t ), &value, sizeof( value ) );
			}
		}
	}
	return Image::create_from_data( side, side, false, Image::FORMAT_R16, bytes );
}

bool T3DMiddleHeightRegionCache::uploadLayer( const int nLayer,
		const Vector2i &vLocation,
		const Ref<Image> &rPreparedHeight ) {
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

int T3DMiddleHeightRegionCache::collectDesired( const t3dResidencyFrameState_t &rFrame,
		const Vector2i &rCenter,
		Vector2i *pLocations,
		const int nCapacity,
		int &rRequiredCount ) const {
	rRequiredCount = 0;
	if ( !m_pRepository || !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, s_nGpuHeightCacheMaxLayers );
	int output_count = 0;
	uint8_t selected_map[s_nMaxRegionSlots] = {};
	for ( int state_value = ( int )( TERRAIN3D_REGION_STREAMING_ACTIVE );
			state_value >= ( int )( TERRAIN3D_REGION_STREAMING_HOT ) && output_count < capacity;
			--state_value ) {
		const t3dRegionStreamingState_t wanted = ( t3dRegionStreamingState_t )( state_value );
		t3dMiddleHeightRegionCacheForEachSquareRing(
				rCenter, m_pRepository->getRegionMapSize(), [&]( const Vector2i &location ) {
					if ( output_count >= capacity ) {
						return false;
					}
					if ( !m_pRepository->isLocationValid( location ) || !m_pRepository->getRegionPtr( location ) ||
							rFrame.streamingState( location ) != wanted ) {
						return true;
					}
					const int map_index = T3DRegionRepository::getRegionMapIndex( location );
					if ( map_index < 0 || selected_map[map_index] != 0u ) {
						return true;
					}
					pLocations[output_count++] = location;
					selected_map[map_index] = 1u;
					return output_count < capacity;
				} );
	}
	rRequiredCount = output_count;
	for ( int slot = 0; slot < m_pRepository->getSlotCount() && output_count < capacity; ++slot ) {
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		const int map_index = location == s_v2iMax ? -1 : T3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u || !m_pRepository->getRegionPtr( location ) ||
				!rFrame.isPrefetch( location ) ) {
			continue;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
	}
	return output_count;
}

bool T3DMiddleHeightRegionCache::updatePageTableTexture( void ) {
	if ( !m_pRepository || m_pRepository->getRegionMapSize() <= 0 ) {
		return false;
	}
	PackedByteArray bytes;
	bytes.resize(
			m_pRepository->getRegionMapSize() * m_pRepository->getRegionMapSize() * ( int )( sizeof( uint16_t ) ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	std::memset( pWrite, 0, ( size_t )( bytes.size() ) );
	const int half = m_pRepository->getRegionMapSize() / 2;
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		if ( m_szSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = m_szLocations[layer];
		const int x = location.m_nX + half;
		const int y = location.m_nY + half;
		if ( ( uint32_t )( x ) >= ( uint32_t )( m_pRepository->getRegionMapSize() ) ||
				( uint32_t )( y ) >= ( uint32_t )( m_pRepository->getRegionMapSize() ) ) {
			continue;
		}
		const uint16_t value = ( uint16_t )( layer + 1 );
		std::memcpy( pWrite + ( size_t )( y * m_pRepository->getRegionMapSize() + x ) * sizeof( uint16_t ),
				&value,
				sizeof( uint16_t ) );
	}
	const Ref<Image> pImage = Image::create_from_data(
			m_pRepository->getRegionMapSize(), m_pRepository->getRegionMapSize(), false, Image::FORMAT_R16, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bExisting = m_generatedRegionMap.getRid().is_valid();
	bool ok = bExisting ? m_generatedRegionMap.update2d( pImage ) : m_generatedRegionMap.create( pImage ).is_valid();
	if ( !ok && bExisting ) {
		m_generatedRegionMap.clear();
		ok = m_generatedRegionMap.create( pImage ).is_valid();
	}
	m_bPageTableDirty = !ok;
	return ok;
}

void T3DMiddleHeightRegionCache::invalidate( const Vector2i &vLocation ) {
	const int layer = findSlot( vLocation );
	if ( layer >= 0 ) {
		const int map_index = T3DRegionRepository::getRegionMapIndex( vLocation );
		if ( map_index >= 0 ) {
			m_szRegionMap[map_index] = 0u;
		}
		m_szSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
		m_szLocations[layer] = s_v2iMax;
		m_szRetireAfterFrame[layer] = 0u;
		m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
		m_bPageTableDirty = true;
	}
	m_bSelectionDirty = true;
}

void T3DMiddleHeightRegionCache::update( const t3dResidencyFrameState_t &rFrame,
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
	if ( !bForce && m_bTexturesReady && rCenter == m_center && streaming_generation == m_nStreamingGenerationLast &&
			!m_bSelectionDirty && !m_bPageTableDirty && !bRetirementPending ) {
		return;
	}
	const bool large_jump =
			m_center != s_v2iMax && MAX( Math::abs( rCenter.x - m_center.x ), Math::abs( rCenter.y - m_center.y ) ) > 1;
	if ( !m_bTexturesReady ) {
		if ( !configure( rConfig ) ) {
			return;
		}
	} else if ( large_jump ) {
		std::fill_n( m_szRegionMap, s_nMaxRegionSlots, ( uint16_t )( 0 ) );
		std::fill_n( m_szLocations, s_nGpuHeightCacheMaxLayers, s_v2iMax );
		std::fill_n( m_szSlotStates, s_nGpuHeightCacheMaxLayers, ( uint8_t )( TERRAIN3D_GPU_SLOT_FREE ) );
		std::fill_n( m_szRetireAfterFrame, s_nGpuHeightCacheMaxLayers, 0u );
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
				m_szLocations[layer] = s_v2iMax;
				m_szRetireAfterFrame[layer] = 0u;
			} else {
				bRetirementPending = true;
			}
		}
	}

	Vector2i desired[s_nGpuHeightCacheMaxLayers];
	int required_count = 0;
	const int desired_count = collectDesired( rFrame, rCenter, desired, m_nMappedCapacity, required_count );
	m_nDesiredLastTick = desired_count;
	uint8_t desired_map[s_nMaxRegionSlots] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = T3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}
	for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
		if ( m_szSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = m_szLocations[layer];
		const int map_index = T3DRegionRepository::getRegionMapIndex( location );
		if ( map_index >= 0 && desired_map[map_index] != 0u ) {
			m_szLastUseTick[layer] = m_nTick;
			continue;
		}
		if ( map_index >= 0 ) {
			m_szRegionMap[map_index] = 0u;
		}
		m_szSlotStates[layer] = TERRAIN3D_GPU_SLOT_RETIRED;
		m_szRetireAfterFrame[layer] = frame + ( uint64_t )( MAX( 1, rConfig.m_nRetireFrames ) );
		m_nResidentCount = MAX( 0, m_nResidentCount - 1 );
		++m_nEvictionsLastTick;
		m_bPageTableDirty = true;
		bRetirementPending = true;
	}

	const int upload_budget = bForce ? s_nGpuHeightCacheMaxLayers : MAX( 1, nEffectiveUploadLayers );
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
		int free_layer = -1;
		for ( int layer = 0; layer < m_nPhysicalLayerCount; ++layer ) {
			if ( m_szSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE ) {
				free_layer = layer;
				break;
			}
		}
		if ( free_layer < 0 ) {
			if ( required ) {
				++m_nMissingRequiredLastTick;
			}
			continue;
		}
		Ref<Image> pPreparedHeight;
		if ( cooked_runtime &&
				( !m_pRuntimePageStore ||
						!m_pRuntimePageStore->takeHeight(
								location, RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT, pPreparedHeight, true ) ) ) {
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
		m_szLocations[free_layer] = location;
		m_szLastUseTick[free_layer] = m_nTick;
		m_szSlotStates[free_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int map_index = T3DRegionRepository::getRegionMapIndex( location );
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

size_t T3DMiddleHeightRegionCache::getReservedBytes( void ) const {
	if ( !m_pRepository || m_nPhysicalLayerCount <= 0 || m_pRepository->getRegionSize() <= 0 ) {
		return 0u;
	}
	const int divisor = m_config.m_bAuthoring ? 1 : MAX( 1, m_config.m_nResolutionDivisor );
	const size_t side = ( size_t )( MAX( 1, ( m_pRepository->getRegionSize() + divisor - 1 ) / divisor ) );
	const size_t page_table = ( size_t )( MAX( 0, m_pRepository->getRegionMapSize() ) ) *
			( size_t )( MAX( 0, m_pRepository->getRegionMapSize() ) ) * sizeof( uint16_t );
	return side * side * sizeof( uint16_t ) * ( size_t )( m_nPhysicalLayerCount ) + page_table;
}

t3dMiddleHeightRegionCacheStats_t T3DMiddleHeightRegionCache::getStats( void ) const {
	t3dMiddleHeightRegionCacheStats_t stats;
	stats.m_nResident = m_nResidentCount;
	stats.m_nMappedCapacity = m_nMappedCapacity;
	stats.m_nPhysicalLayers = m_nPhysicalLayerCount;
	stats.m_nDesiredLastTick = m_nDesiredLastTick;
	stats.m_nMissingRequiredLastTick = m_nMissingRequiredLastTick;
	stats.m_nUploadsLastTick = m_nUploadsLastTick;
	stats.m_nEvictionsLastTick = m_nEvictionsLastTick;
	stats.m_nResolutionDivisor = m_config.m_bAuthoring ? 1 : MAX( 1, m_config.m_nResolutionDivisor );
	stats.m_nReservedBytes = getReservedBytes();
	stats.m_bPageTableValid = m_generatedRegionMap.getRid().is_valid();
	stats.m_bHeightArrayValid = m_generatedHeightMaps.getRid().is_valid();
	return stats;
}

Dictionary T3DMiddleHeightRegionCache::getStatsDictionary( void ) const {
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
