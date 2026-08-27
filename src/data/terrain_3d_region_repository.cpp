// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_region_repository.h"

#include <algorithm>
#include <cstring>

Terrain3DRegionRepository::Terrain3DRegionRepository( void ) {
	std::fill_n( m_szRegionLocations, s_nMaxRegionSlots, s_v2iMax );
}

void Terrain3DRegionRepository::clear( void ) {
	for ( Ref<Terrain3DRegion> &region : m_szRegionRefs ) {
		region.unref();
	}
	m_nRegionResourceCount = 0;
	std::fill_n( m_pszRegionPtrMap, s_nMaxRegionSlots, nullptr );
	std::fill_n( m_szRegionLocations, s_nMaxRegionSlots, s_v2iMax );
	std::fill_n( m_szFreeRegionSlots, s_nMaxRegionSlots, ( uint16_t )( 0 ) );
	std::fill_n( m_szRegionMap, s_nMaxRegionSlots, ( uint16_t )( 0 ) );
	m_nRegionLocationCount = 0;
	m_nFreeRegionSlotCount = 0;
	m_nActiveRegionCount = 0;
	m_bRegionMapDirty = true;
	bumpTopologyGeneration();
}

bool Terrain3DRegionRepository::releaseRuntimeAllocatorStorage( void ) {
	bool released = true;
	for ( Ref<Terrain3DRegion> &region : m_szRegionRefs ) {
		if ( region.is_valid() && !region->releaseRuntimeAllocatorStorageInternal() ) {
			released = false;
		}
	}
	return released;
}

void Terrain3DRegionRepository::configureGeometry( const int nRegionSize, const real_t fVertexSpacing ) {
	const int regionMapSize = nRegionSize > 0 ? s_nActiveWorldTexels / nRegionSize : 0;
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

void Terrain3DRegionRepository::setVertexSpacing( const real_t fVertexSpacing ) {
	if ( m_fVertexSpacing == fVertexSpacing ) {
		return;
	}
	m_fVertexSpacing = fVertexSpacing;
	bumpTopologyGeneration();
}

int Terrain3DRegionRepository::getRegionMapIndex( const Vector2i &rRegionLoc ) {
	const Vector2i loc = rRegionLoc + ( s_regionMapVSize / 2 );
	if ( ( uint32_t )( loc.x ) >= ( uint32_t )( s_nActiveMaxRegionMapSize ) ||
			( uint32_t )( loc.y ) >= ( uint32_t )( s_nActiveMaxRegionMapSize ) ) {
		return -1;
	}
	return loc.y * s_nActiveMaxRegionMapSize + loc.x;
}

bool Terrain3DRegionRepository::isLocationValid( const Vector2i &vLocation ) const {
	if ( m_nRegionMapSize <= 0 ) {
		return false;
	}
	const int half = m_nRegionMapSize / 2;
	return vLocation.x >= -half && vLocation.x < half && vLocation.y >= -half && vLocation.y < half;
}

Vector2i Terrain3DRegionRepository::getRegionLocation( const Vector3 &rGlobalPosition ) const {
	const Vector2 descaled = t3dVector3ToVector2( rGlobalPosition ) / m_fVertexSpacing;
	return Vector2i( ( descaled / ( real_t )( m_nRegionSize ) ).floor() );
}

int Terrain3DRegionRepository::getRegionId( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return -1;
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	if ( mapIndex < 0 ) {
		return -1;
	}
	const int regionId = ( int )( m_szRegionMap[mapIndex] ) - 1;
	if ( regionId >= 0 && regionId < m_nRegionLocationCount && m_szRegionLocations[regionId] == rRegionLoc ) {
		return regionId;
	}
	return -1;
}

int Terrain3DRegionRepository::findSlot( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return -1;
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	if ( mapIndex >= 0 ) {
		const int mappedSlot = ( int )( m_szRegionMap[mapIndex] ) - 1;
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

Terrain3DRegion *Terrain3DRegionRepository::getRegionPtr( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return nullptr;
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	return mapIndex >= 0 ? m_pszRegionPtrMap[mapIndex] : nullptr;
}

Ref<Terrain3DRegion> Terrain3DRegionRepository::getRegion( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return Ref<Terrain3DRegion>();
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	return mapIndex >= 0 ? m_szRegionRefs[mapIndex] : Ref<Terrain3DRegion>();
}

Dictionary Terrain3DRegionRepository::getRegions( void ) const {
	Dictionary regions;
	for ( int mapIndex = 0; mapIndex < s_nMaxRegionSlots; ++mapIndex ) {
		if ( m_szRegionRefs[mapIndex].is_null() ) {
			continue;
		}
		const int x = mapIndex % s_nActiveMaxRegionMapSize;
		const int y = mapIndex / s_nActiveMaxRegionMapSize;
		regions[Vector2i( x - s_nActiveMaxRegionMapSize / 2, y - s_nActiveMaxRegionMapSize / 2 )] =
				m_szRegionRefs[mapIndex];
	}
	return regions;
}

Array Terrain3DRegionRepository::getRegionKeys( void ) const {
	Array keys;
	keys.resize( m_nRegionResourceCount );
	int write = 0;
	for ( int mapIndex = 0; mapIndex < s_nMaxRegionSlots; ++mapIndex ) {
		if ( m_szRegionRefs[mapIndex].is_null() ) {
			continue;
		}
		const int x = mapIndex % s_nActiveMaxRegionMapSize;
		const int y = mapIndex / s_nActiveMaxRegionMapSize;
		keys[write++] = Vector2i( x - s_nActiveMaxRegionMapSize / 2, y - s_nActiveMaxRegionMapSize / 2 );
	}
	if ( write != keys.size() ) {
		keys.resize( write );
	}
	return keys;
}

TypedArray<Vector2i> Terrain3DRegionRepository::getActiveLocations( void ) const {
	TypedArray<Vector2i> locations;
	locations.resize( m_nActiveRegionCount );
	int writeIndex = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == s_v2iMax ) {
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

TypedArray<Vector2i> Terrain3DRegionRepository::getSlots( void ) const {
	TypedArray<Vector2i> slots;
	slots.resize( m_nRegionLocationCount );
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		slots[slot] = m_szRegionLocations[slot];
	}
	return slots;
}

PackedInt32Array Terrain3DRegionRepository::getLogicalRegionMapPacked( void ) const {
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

t3dRegionRepositoryView_t Terrain3DRegionRepository::getView( void ) const {
	t3dRegionRepositoryView_t view;
	view.m_pSlots = m_szRegionLocations;
	view.m_ppRegionMap = m_pszRegionPtrMap;
	view.m_pLogicalRegionMap = m_szRegionMap;
	view.m_nSlotCount = m_nRegionLocationCount;
	view.m_nRegionSize = m_nRegionSize;
	view.m_nRegionMapSize = m_nRegionMapSize;
	view.m_fVertexSpacing = m_fVertexSpacing;
	view.m_nTopologyGeneration = m_nTopologyGeneration;
	return view;
}

bool Terrain3DRegionRepository::storeRegion( const Vector2i &rLocation, const Ref<Terrain3DRegion> &rRegion ) {
	if ( !isLocationValid( rLocation ) || rRegion.is_null() || rRegion->getLocation() != rLocation ) {
		return false;
	}
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( mapIndex < 0 ) {
		return false;
	}
	if ( m_szRegionRefs[mapIndex].is_null() ) {
		++m_nRegionResourceCount;
	}
	m_szRegionRefs[mapIndex] = rRegion;
	m_pszRegionPtrMap[mapIndex] = rRegion.ptr();
	return true;
}

void Terrain3DRegionRepository::eraseRegionResource( const Vector2i &rLocation ) {
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( mapIndex < 0 ) {
		return;
	}
	if ( m_szRegionRefs[mapIndex].is_valid() ) {
		m_szRegionRefs[mapIndex].unref();
		m_nRegionResourceCount = MAX( 0, m_nRegionResourceCount - 1 );
	}
	m_pszRegionPtrMap[mapIndex] = nullptr;
}

int Terrain3DRegionRepository::activateLocation( const Vector2i &rLocation, const bool bPublishRegionMap ) {
	return activateLocationInternal( rLocation, bPublishRegionMap, true );
}

int Terrain3DRegionRepository::activateLocationInternal( const Vector2i &rLocation,
		const bool bPublishRegionMap,
		const bool bBumpGeneration ) {
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
		if ( candidate >= 0 && candidate < m_nRegionLocationCount && m_szRegionLocations[candidate] == s_v2iMax ) {
			slot = candidate;
			break;
		}
	}
	if ( slot < 0 ) {
		if ( m_nRegionLocationCount >= s_nMaxRegionSlots ||
				m_nRegionLocationCount >= m_nRegionMapSize * m_nRegionMapSize ) {
			return -1;
		}
		slot = m_nRegionLocationCount++;
	}

	m_szRegionLocations[slot] = rLocation;
	++m_nActiveRegionCount;
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( bPublishRegionMap && mapIndex >= 0 && getRegionPtr( rLocation ) != nullptr ) {
		m_szRegionMap[mapIndex] = ( uint16_t )( slot + 1 );
	} else {
		m_bRegionMapDirty = true;
	}
	if ( bBumpGeneration ) {
		bumpTopologyGeneration();
	}
	return slot;
}

int Terrain3DRegionRepository::deactivateLocation( const Vector2i &rLocation ) {
	const int slot = findSlot( rLocation );
	if ( slot < 0 ) {
		return -1;
	}

	m_szRegionLocations[slot] = s_v2iMax;
	if ( m_nFreeRegionSlotCount < s_nMaxRegionSlots ) {
		m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t )( slot );
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

t3dRegionLocationReconcileResult_t Terrain3DRegionRepository::reconcileLocations( const TypedArray<Vector2i> &rLocations ) {
	t3dRegionLocationReconcileResult_t result;
	result.m_nOldSlotCount = m_nRegionLocationCount;
	result.m_removedLocations.reserve( ( size_t )( m_nActiveRegionCount ) );
	result.m_addedSlots.reserve( ( size_t )( rLocations.size() ) );

	eastl::bitset<s_nMaxRegionSlots> desiredLocations;
	Vector2i uniqueLocations[s_nMaxRegionSlots];
	int uniqueLocationCount = 0;
	for ( const Vector2i location : rLocations ) {
		if ( location == s_v2iMax || !isLocationValid( location ) ) {
			continue;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || desiredLocations.test( ( size_t )( mapIndex ) ) ) {
			continue;
		}
		desiredLocations.set( ( size_t )mapIndex );
		if ( uniqueLocationCount < s_nMaxRegionSlots ) {
			uniqueLocations[uniqueLocationCount++] = location;
		}
	}

	eastl::bitset<s_nMaxRegionSlots> existingLocations;
	int retainedCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == s_v2iMax ) {
			continue;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || !desiredLocations.test( ( size_t )( mapIndex ) ) ) {
			result.m_removedLocations.push_back( location );
			m_szRegionLocations[slot] = s_v2iMax;
			result.m_bSlotsChanged = true;
		} else {
			existingLocations.set( ( size_t )( mapIndex ) );
			++retainedCount;
		}
	}

	m_nActiveRegionCount = retainedCount;
	rebuildFreeSlots();
	for ( int uniqueIndex = 0; uniqueIndex < uniqueLocationCount; ++uniqueIndex ) {
		const Vector2i location = uniqueLocations[uniqueIndex];
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex >= 0 && existingLocations.test( ( size_t )( mapIndex ) ) ) {
			continue;
		}
		const int slot = activateLocationInternal( location, false, false );
		if ( slot < 0 ) {
			result.m_bCapacityExceeded = true;
			continue;
		}
		result.m_addedSlots.push_back( slot );
		result.m_bSlotsChanged = true;
	}

	if ( result.m_bSlotsChanged ) {
		m_bRegionMapDirty = true;
		bumpTopologyGeneration();
	}
	return result;
}

void Terrain3DRegionRepository::rebuildFreeSlots( void ) {
	m_nFreeRegionSlotCount = 0;
	for ( int slot = m_nRegionLocationCount - 1; slot >= 0; --slot ) {
		if ( m_szRegionLocations[slot] == s_v2iMax ) {
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t )( slot );
		}
	}
}

bool Terrain3DRegionRepository::ensureSlotReserve( const int nDesiredFreeSlots ) {
	if ( m_nActiveRegionCount <= 0 || m_nRegionMapSize <= 0 ) {
		return false;
	}

	int freeSlots = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( m_szRegionLocations[slot] == s_v2iMax ) {
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
		m_szRegionLocations[slot] = s_v2iMax;
		m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t )( slot );
	}
	bumpTopologyGeneration();
	return true;
}

void Terrain3DRegionRepository::rebuildRegionMapFromSlots( void ) {
	std::fill_n( m_szRegionMap, s_nMaxRegionSlots, ( uint16_t )( 0 ) );
	m_nActiveRegionCount = 0;
	m_nFreeRegionSlotCount = 0;
	bool bSanitized = false;
	eastl::bitset<s_nMaxRegionSlots> publishedLocations;

	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == s_v2iMax ) {
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t )( slot );
			continue;
		}

		Terrain3DRegion *const pRegion = getRegionPtr( location );
		const int mapIndex = getRegionMapIndex( location );
		if ( !pRegion || pRegion->isDeleted() || mapIndex < 0 || !isLocationValid( location ) ||
				publishedLocations.test( ( size_t )( mapIndex ) ) ) {
			m_szRegionLocations[slot] = s_v2iMax;
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t )( slot );
			bSanitized = true;
			continue;
		}

		publishedLocations.set( ( size_t )( mapIndex ) );
		m_szRegionMap[mapIndex] = ( uint16_t )( slot + 1 );
		++m_nActiveRegionCount;
	}
	m_bRegionMapDirty = false;
	if ( bSanitized ) {
		bumpTopologyGeneration();
	}
}

void Terrain3DRegionRepository::replaceAll( const Dictionary &rRegions,
		Terrain3DRegion *const *ppRegionMap,
		const Vector2i *pLocations,
		const int nLocationCount,
		const uint16_t *pLogicalRegionMap ) {
	for ( Ref<Terrain3DRegion> &region : m_szRegionRefs ) {
		region.unref();
	}
	m_nRegionResourceCount = 0;
	std::fill_n( m_pszRegionPtrMap, s_nMaxRegionSlots, nullptr );
	const Array keys = rRegions.keys();
	for ( const Vector2i location : keys ) {
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 ) {
			continue;
		}
		const Ref<Terrain3DRegion> region = rRegions.get( location, Ref<Terrain3DRegion>() );
		if ( region.is_null() ) {
			continue;
		}
		m_szRegionRefs[mapIndex] = region;
		m_pszRegionPtrMap[mapIndex] = region.ptr();
		++m_nRegionResourceCount;
	}
	( void )ppRegionMap;
	std::fill_n( m_szRegionLocations, s_nMaxRegionSlots, s_v2iMax );
	m_nRegionLocationCount = CLAMP( nLocationCount, 0, s_nMaxRegionSlots );
	if ( m_nRegionLocationCount > 0 ) {
		std::memcpy( m_szRegionLocations, pLocations, ( size_t )( m_nRegionLocationCount ) * sizeof( Vector2i ) );
	}
	std::memcpy( m_szRegionMap, pLogicalRegionMap, sizeof( m_szRegionMap ) );
	m_bRegionMapDirty = false;
	m_nActiveRegionCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( m_szRegionLocations[slot] != s_v2iMax ) {
			++m_nActiveRegionCount;
		}
	}
	rebuildFreeSlots();
	bumpTopologyGeneration();
}

bool Terrain3DRegionRepository::validateInvariants( void ) const {
	if ( m_nRegionLocationCount < 0 || m_nRegionLocationCount > s_nMaxRegionSlots || m_nFreeRegionSlotCount < 0 ||
			m_nFreeRegionSlotCount > m_nRegionLocationCount || m_nActiveRegionCount < 0 ||
			m_nActiveRegionCount > m_nRegionLocationCount ) {
		return false;
	}

	int resourceCount = 0;
	for ( int mapIndex = 0; mapIndex < s_nMaxRegionSlots; ++mapIndex ) {
		const Ref<Terrain3DRegion> &region = m_szRegionRefs[mapIndex];
		if ( region.is_valid() ) {
			++resourceCount;
		}
		if ( region.ptr() != m_pszRegionPtrMap[mapIndex] ) {
			return false;
		}
	}
	if ( resourceCount != m_nRegionResourceCount ) {
		return false;
	}

	bool freeSeen[s_nMaxRegionSlots] = {};
	for ( int i = 0; i < m_nFreeRegionSlotCount; ++i ) {
		const int slot = m_szFreeRegionSlots[i];
		if ( slot < 0 || slot >= m_nRegionLocationCount || freeSeen[slot] || m_szRegionLocations[slot] != s_v2iMax ) {
			return false;
		}
		freeSeen[slot] = true;
	}

	bool locationSeen[s_nMaxRegionSlots] = {};
	int activeCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == s_v2iMax ) {
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
				( m_szRegionMap[mapIndex] != ( uint16_t )( slot + 1 ) || m_pszRegionPtrMap[mapIndex] == nullptr ) ) {
			return false;
		}
	}
	if ( activeCount != m_nActiveRegionCount ) {
		return false;
	}

	if ( !m_bRegionMapDirty ) {
		for ( int mapIndex = 0; mapIndex < s_nMaxRegionSlots; ++mapIndex ) {
			const int slot = ( int )( m_szRegionMap[mapIndex] ) - 1;
			if ( slot < 0 ) {
				continue;
			}
			if ( slot >= m_nRegionLocationCount || m_szRegionLocations[slot] == s_v2iMax ) {
				return false;
			}
		}
	}
	return true;
}

void Terrain3DRegionRepository::bumpTopologyGeneration( void ) {
	++m_nTopologyGeneration;
	if ( m_nTopologyGeneration == 0u ) {
		m_nTopologyGeneration = 1u;
	}
}
