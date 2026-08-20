// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_region_repository.h"

#include <algorithm>
#include <cstring>

T3DRegionRepository::T3DRegionRepository( void ) {
	std::fill_n( m_szRegionLocations, TERRAIN3D_MAX_REGION_SLOTS, V2I_MAX );
}

void T3DRegionRepository::clear( void ) {
	m_regions.clear();
	std::fill_n( m_szRegionPtrMap, TERRAIN3D_MAX_REGION_SLOTS, nullptr );
	std::fill_n( m_szRegionLocations, TERRAIN3D_MAX_REGION_SLOTS, V2I_MAX );
	std::fill_n( m_szFreeRegionSlots, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	std::fill_n( m_szRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	m_nRegionLocationCount = 0;
	m_nFreeRegionSlotCount = 0;
	m_nActiveRegionCount = 0;
	m_bRegionMapDirty = true;
	bumpTopologyGeneration();
}

void T3DRegionRepository::configureGeometry( const int nRegionSize, const real_t fVertexSpacing ) {
	const int regionMapSize = nRegionSize > 0 ? TERRAIN3D_ACTIVE_WORLD_TEXELS / nRegionSize : 0;
	const bool bLayoutChanged = m_nRegionSize != nRegionSize || m_nRegionMapSize != regionMapSize;
	const bool bSpacingChanged = m_fVertexSpacing != fVertexSpacing;
	m_nRegionSize = nRegionSize;
	m_nRegionMapSize = regionMapSize;
	m_regionSizev = V2I( nRegionSize );
	m_fVertexSpacing = fVertexSpacing;
	if ( bLayoutChanged ) {
		m_bRegionMapDirty = true;
	}
	if ( bLayoutChanged || bSpacingChanged ) {
		bumpTopologyGeneration();
	}
}

void T3DRegionRepository::setVertexSpacing( const real_t fVertexSpacing ) {
	if ( m_fVertexSpacing == fVertexSpacing ) {
		return;
	}
	m_fVertexSpacing = fVertexSpacing;
	bumpTopologyGeneration();
}

int T3DRegionRepository::getRegionMapIndex( const Vector2i &rRegionLoc ) {
	const Vector2i loc = rRegionLoc + ( TERRAIN3D_REGION_MAP_VSIZE / 2 );
	if ( ( uint32_t ) ( loc.x ) >= ( uint32_t ) ( TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) ||
		( uint32_t ) ( loc.y ) >= ( uint32_t ) ( TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) ) {
		return -1;
	}
	return loc.y * TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE + loc.x;
}

bool T3DRegionRepository::isLocationValid( const Vector2i &vLocation ) const {
	if ( m_nRegionMapSize <= 0 ) {
		return false;
	}
	const int half = m_nRegionMapSize / 2;
	return vLocation.x >= -half && vLocation.x < half && vLocation.y >= -half && vLocation.y < half;
}

Vector2i T3DRegionRepository::getRegionLocation( const Vector3 &rGlobalPosition ) const {
	const Vector2 descaled = v3v2( rGlobalPosition ) / m_fVertexSpacing;
	return Vector2i( ( descaled / ( real_t ) ( m_nRegionSize ) ).floor() );
}

int T3DRegionRepository::getRegionId( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return -1;
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	if ( mapIndex < 0 ) {
		return -1;
	}
	const int regionId = ( int ) ( m_szRegionMap[mapIndex] ) - 1;
	if ( regionId >= 0 && regionId < m_nRegionLocationCount && m_szRegionLocations[regionId] == rRegionLoc ) {
		return regionId;
	}
	return -1;
}

int T3DRegionRepository::findSlot( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return -1;
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	if ( mapIndex >= 0 ) {
		const int mappedSlot = ( int ) ( m_szRegionMap[mapIndex] ) - 1;
		if ( mappedSlot >= 0 && mappedSlot < m_nRegionLocationCount && m_szRegionLocations[mappedSlot] == rRegionLoc ) {
			return mappedSlot;
		}
	}
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( m_szRegionLocations[slot] == rRegionLoc ) {
			return slot;
		}
	}
	return -1;
}

Terrain3DRegion *T3DRegionRepository::getRegionPtr( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return nullptr;
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	return mapIndex >= 0 ? m_szRegionPtrMap[mapIndex] : nullptr;
}

Ref<Terrain3DRegion> T3DRegionRepository::getRegion( const Vector2i &rRegionLoc ) const {
	return m_regions.get( rRegionLoc, Ref<Terrain3DRegion>() );
}

TypedArray<Vector2i> T3DRegionRepository::getActiveLocations( void ) const {
	TypedArray<Vector2i> locations;
	locations.resize( m_nActiveRegionCount );
	int writeIndex = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		Terrain3DRegion *const pRegion = getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() ) {
			locations[writeIndex++] = location;
		}
	}
	if ( writeIndex != locations.size() ) {
		locations.resize( writeIndex );
	}
	return locations;
}

TypedArray<Vector2i> T3DRegionRepository::getSlots( void ) const {
	TypedArray<Vector2i> slots;
	slots.resize( m_nRegionLocationCount );
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		slots[slot] = m_szRegionLocations[slot];
	}
	return slots;
}

PackedInt32Array T3DRegionRepository::getLogicalRegionMapPacked( void ) const {
	PackedInt32Array regionMap;
	if ( m_nRegionMapSize <= 0 ) {
		return regionMap;
	}
	regionMap.resize( m_nRegionMapSize * m_nRegionMapSize );
	int32_t *const pWrite = regionMap.ptrw();
	const int half = m_nRegionMapSize / 2;
	for ( int y = 0; y < m_nRegionMapSize; ++y ) {
		for ( int x = 0; x < m_nRegionMapSize; ++x ) {
			const int sourceIndex = getRegionMapIndex( Vector2i( x - half, y - half ) );
			pWrite[y * m_nRegionMapSize + x] = sourceIndex >= 0 && m_szRegionMap[sourceIndex] != 0u ? 1 : 0;
		}
	}
	return regionMap;
}

T3DRegionRepositoryView T3DRegionRepository::getView( void ) const {
	T3DRegionRepositoryView view;
	view.pSlots = m_szRegionLocations;
	view.ppRegionMap = m_szRegionPtrMap;
	view.pLogicalRegionMap = m_szRegionMap;
	view.nSlotCount = m_nRegionLocationCount;
	view.nRegionSize = m_nRegionSize;
	view.nRegionMapSize = m_nRegionMapSize;
	view.fVertexSpacing = m_fVertexSpacing;
	view.nTopologyGeneration = m_nTopologyGeneration;
	return view;
}

bool T3DRegionRepository::storeRegion( const Vector2i &rLocation, const Ref<Terrain3DRegion> &rRegion ) {
	if ( !isLocationValid( rLocation ) || rRegion.is_null() || rRegion->getLocation() != rLocation ) {
		return false;
	}
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( mapIndex < 0 ) {
		return false;
	}
	m_regions[rLocation] = rRegion;
	m_szRegionPtrMap[mapIndex] = rRegion.ptr();
	return true;
}

void T3DRegionRepository::eraseRegionResource( const Vector2i &rLocation ) {
	m_regions.erase( rLocation );
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( mapIndex >= 0 ) {
		m_szRegionPtrMap[mapIndex] = nullptr;
	}
}

int T3DRegionRepository::activateLocation( const Vector2i &rLocation, const bool bPublishRegionMap ) {
	return activateLocationInternal( rLocation, bPublishRegionMap, true );
}

int T3DRegionRepository::activateLocationInternal(
	const Vector2i &rLocation, const bool bPublishRegionMap, const bool bBumpGeneration ) {
	if ( !isLocationValid( rLocation ) ) {
		return -1;
	}
	const int existingSlot = findSlot( rLocation );
	if ( existingSlot >= 0 ) {
		return existingSlot;
	}

	int slot = -1;
	while ( m_nFreeRegionSlotCount > 0 ) {
		const int candidate = m_szFreeRegionSlots[--m_nFreeRegionSlotCount];
		if ( candidate >= 0 && candidate < m_nRegionLocationCount && m_szRegionLocations[candidate] == V2I_MAX ) {
			slot = candidate;
			break;
		}
	}
	if ( slot < 0 ) {
		if ( m_nRegionLocationCount >= TERRAIN3D_MAX_REGION_SLOTS ||
			m_nRegionLocationCount >= m_nRegionMapSize * m_nRegionMapSize ) {
			return -1;
		}
		slot = m_nRegionLocationCount++;
	}

	m_szRegionLocations[slot] = rLocation;
	++m_nActiveRegionCount;
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( bPublishRegionMap && mapIndex >= 0 && getRegionPtr( rLocation ) != nullptr ) {
		m_szRegionMap[mapIndex] = ( uint16_t ) ( slot + 1 );
	} else {
		m_bRegionMapDirty = true;
	}
	if ( bBumpGeneration ) {
		bumpTopologyGeneration();
	}
	return slot;
}

int T3DRegionRepository::deactivateLocation( const Vector2i &rLocation ) {
	const int slot = findSlot( rLocation );
	if ( slot < 0 ) {
		return -1;
	}

	m_szRegionLocations[slot] = V2I_MAX;
	if ( m_nFreeRegionSlotCount < TERRAIN3D_MAX_REGION_SLOTS ) {
		m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
	}
	m_nActiveRegionCount = MAX( 0, m_nActiveRegionCount - 1 );
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( mapIndex >= 0 ) {
		m_szRegionMap[mapIndex] = 0u;
	} else {
		m_bRegionMapDirty = true;
	}
	bumpTopologyGeneration();
	return slot;
}

T3DRegionLocationReconcileResult T3DRegionRepository::reconcileLocations( const TypedArray<Vector2i> &rLocations ) {
	T3DRegionLocationReconcileResult result;
	result.nOldSlotCount = m_nRegionLocationCount;
	result.removedLocations.reserve( ( size_t ) ( m_nActiveRegionCount ) );
	result.addedSlots.reserve( ( size_t ) ( rLocations.size() ) );

	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> desiredLocations;
	eastl::vector<Vector2i> uniqueLocations;
	uniqueLocations.reserve( ( size_t ) ( rLocations.size() ) );
	for ( const Vector2i location : rLocations ) {
		if ( location == V2I_MAX || !isLocationValid( location ) ) {
			continue;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || desiredLocations.test( ( size_t ) ( mapIndex ) ) ) {
			continue;
		}
		desiredLocations.set( ( size_t ) ( mapIndex ) );
		uniqueLocations.push_back( location );
	}

	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> existingLocations;
	int retainedCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || !desiredLocations.test( ( size_t ) ( mapIndex ) ) ) {
			result.removedLocations.push_back( location );
			m_szRegionLocations[slot] = V2I_MAX;
			result.bSlotsChanged = true;
		} else {
			existingLocations.set( ( size_t ) ( mapIndex ) );
			++retainedCount;
		}
	}

	m_nActiveRegionCount = retainedCount;
	rebuildFreeSlots();
	for ( const Vector2i location : uniqueLocations ) {
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex >= 0 && existingLocations.test( ( size_t ) ( mapIndex ) ) ) {
			continue;
		}
		const int slot = activateLocationInternal( location, false, false );
		if ( slot < 0 ) {
			result.bCapacityExceeded = true;
			continue;
		}
		result.addedSlots.push_back( slot );
		result.bSlotsChanged = true;
	}

	if ( result.bSlotsChanged ) {
		m_bRegionMapDirty = true;
		bumpTopologyGeneration();
	}
	return result;
}

void T3DRegionRepository::rebuildFreeSlots( void ) {
	m_nFreeRegionSlotCount = 0;
	for ( int slot = m_nRegionLocationCount - 1; slot >= 0; --slot ) {
		if ( m_szRegionLocations[slot] == V2I_MAX ) {
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
		}
	}
}

bool T3DRegionRepository::ensureSlotReserve( const int nDesiredFreeSlots ) {
	if ( m_nActiveRegionCount <= 0 || m_nRegionMapSize <= 0 ) {
		return false;
	}

	int freeSlots = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( m_szRegionLocations[slot] == V2I_MAX ) {
			++freeSlots;
		}
	}

	const int maximumSlots = m_nRegionMapSize * m_nRegionMapSize;
	const int desiredFree = MIN( MAX( 0, nDesiredFreeSlots ), maximumSlots - m_nActiveRegionCount );
	const int slotsToAdd = desiredFree - freeSlots;
	if ( slotsToAdd <= 0 ) {
		return false;
	}

	for ( int i = 0; i < slotsToAdd && m_nRegionLocationCount < maximumSlots; ++i ) {
		const int slot = m_nRegionLocationCount++;
		m_szRegionLocations[slot] = V2I_MAX;
		m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
	}
	bumpTopologyGeneration();
	return true;
}

void T3DRegionRepository::rebuildRegionMapFromSlots( void ) {
	std::fill_n( m_szRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	m_nActiveRegionCount = 0;
	m_nFreeRegionSlotCount = 0;
	bool bSanitized = false;
	eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> publishedLocations;

	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
			continue;
		}

		Terrain3DRegion *const pRegion = getRegionPtr( location );
		const int mapIndex = getRegionMapIndex( location );
		if ( !pRegion || pRegion->isDeleted() || mapIndex < 0 || !isLocationValid( location ) ||
			publishedLocations.test( ( size_t ) ( mapIndex ) ) ) {
			m_szRegionLocations[slot] = V2I_MAX;
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
			bSanitized = true;
			continue;
		}

		publishedLocations.set( ( size_t ) ( mapIndex ) );
		m_szRegionMap[mapIndex] = ( uint16_t ) ( slot + 1 );
		++m_nActiveRegionCount;
	}
	m_bRegionMapDirty = false;
	if ( bSanitized ) {
		bumpTopologyGeneration();
	}
}

void T3DRegionRepository::replaceAll( const Dictionary &rRegions, Terrain3DRegion *const *ppRegionMap,
	const Vector2i *pLocations, const int nLocationCount, const uint16_t *pLogicalRegionMap ) {
	m_regions = rRegions;
	std::memcpy( m_szRegionPtrMap, ppRegionMap, sizeof( m_szRegionPtrMap ) );
	std::fill_n( m_szRegionLocations, TERRAIN3D_MAX_REGION_SLOTS, V2I_MAX );
	m_nRegionLocationCount = CLAMP( nLocationCount, 0, TERRAIN3D_MAX_REGION_SLOTS );
	if ( m_nRegionLocationCount > 0 ) {
		std::memcpy( m_szRegionLocations, pLocations, ( size_t ) ( m_nRegionLocationCount ) * sizeof( Vector2i ) );
	}
	std::memcpy( m_szRegionMap, pLogicalRegionMap, sizeof( m_szRegionMap ) );
	m_bRegionMapDirty = false;
	m_nActiveRegionCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( m_szRegionLocations[slot] != V2I_MAX ) {
			++m_nActiveRegionCount;
		}
	}
	rebuildFreeSlots();
	bumpTopologyGeneration();
}

bool T3DRegionRepository::validateInvariants( void ) const {
	if ( m_nRegionLocationCount < 0 || m_nRegionLocationCount > TERRAIN3D_MAX_REGION_SLOTS ||
		m_nFreeRegionSlotCount < 0 || m_nFreeRegionSlotCount > m_nRegionLocationCount ||
		m_nActiveRegionCount < 0 || m_nActiveRegionCount > m_nRegionLocationCount ) {
		return false;
	}

	const Array regionKeys = m_regions.keys();
	for ( const Vector2i location : regionKeys ) {
		if ( !isLocationValid( location ) ) {
			return false;
		}
	}

	for ( int mapIndex = 0; mapIndex < TERRAIN3D_MAX_REGION_SLOTS; ++mapIndex ) {
		const int mapX = mapIndex % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
		const int mapY = mapIndex / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
		const Vector2i location( mapX - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
			mapY - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2 );
		const Ref<Terrain3DRegion> region = m_regions.get( location, Ref<Terrain3DRegion>() );
		if ( region.ptr() != m_szRegionPtrMap[mapIndex] ) {
			return false;
		}
	}

	bool freeSeen[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int i = 0; i < m_nFreeRegionSlotCount; ++i ) {
		const int slot = m_szFreeRegionSlots[i];
		if ( slot < 0 || slot >= m_nRegionLocationCount || freeSeen[slot] || m_szRegionLocations[slot] != V2I_MAX ) {
			return false;
		}
		freeSeen[slot] = true;
	}

	bool locationSeen[TERRAIN3D_MAX_REGION_SLOTS] = {};
	int activeCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			if ( !freeSeen[slot] ) {
				return false;
			}
			continue;
		}
		++activeCount;
		if ( !isLocationValid( location ) ) {
			return false;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || locationSeen[mapIndex] ) {
			return false;
		}
		locationSeen[mapIndex] = true;
		if ( !m_bRegionMapDirty &&
			( m_szRegionMap[mapIndex] != ( uint16_t ) ( slot + 1 ) || m_szRegionPtrMap[mapIndex] == nullptr ) ) {
			return false;
		}
	}
	if ( activeCount != m_nActiveRegionCount ) {
		return false;
	}

	if ( !m_bRegionMapDirty ) {
		for ( int mapIndex = 0; mapIndex < TERRAIN3D_MAX_REGION_SLOTS; ++mapIndex ) {
			const int slot = ( int ) ( m_szRegionMap[mapIndex] ) - 1;
			if ( slot < 0 ) {
				continue;
			}
			if ( slot >= m_nRegionLocationCount || m_szRegionLocations[slot] == V2I_MAX ) {
				return false;
			}
		}
	}
	return true;
}

void T3DRegionRepository::bumpTopologyGeneration( void ) {
	++m_nTopologyGeneration;
	if ( m_nTopologyGeneration == 0u ) {
		m_nTopologyGeneration = 1u;
	}
}
