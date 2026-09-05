// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_region_repository.h"

#include <string.h>

Terrain3DRegionRepository::Terrain3DRegionRepository( void ) {
	for ( int i = 0; i < s_nRegionSlots; ++i ) {
		m_szRegionLocations[i] = t3dInvalidRegionCoord();
	}
}

void Terrain3DRegionRepository::clear( void ) {
	for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_szRegionRefs ); iRegion++ ) {
		Ref<Terrain3DRegion> &region = ( m_szRegionRefs )[iRegion];
		region.unref();
	}
	m_nRegionResourceCount = 0;
	memset( m_pszRegionPtrMap, 0, sizeof( m_pszRegionPtrMap[0] ) * s_nRegionSlots );
	for ( int i = 0; i < s_nRegionSlots; ++i ) {
		m_szRegionLocations[i] = t3dInvalidRegionCoord();
	}
	memset( m_szRegionMap, ( uint16_t )( 0 ), sizeof( *m_szRegionMap ) * s_nRegionSlots );
	m_nRegionLocationCount = 0;
	m_freeRegionSlots.clear();
	m_nActiveRegionCount = 0;
	m_bRegionMapDirty = true;
	bumpTopologyGeneration();
}

bool Terrain3DRegionRepository::releaseRuntimeAllocatorStorage( void ) {
	bool released = true;
	for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_szRegionRefs ); iRegion++ ) {
		Ref<Terrain3DRegion> &region = ( m_szRegionRefs )[iRegion];
		if ( region.is_valid() && !region->releaseRuntimeAllocatorStorageInternal() ) {
			released = false;
		}
	}
	return released;
}

int Terrain3DRegionRepository::getRegionMapIndex( const Vector2i &rRegionLoc ) {
	const Vector2i loc = rRegionLoc + ( s_regionMapVSize / 2 );
	if ( ( uint32_t )( loc.x ) >= ( uint32_t )( s_nRegionMapSize )
	        || ( uint32_t )( loc.y ) >= ( uint32_t )( s_nRegionMapSize ) ) {
		return -1;
	}
	return loc.y * s_nRegionMapSize + loc.x;
}

bool Terrain3DRegionRepository::isLocationValid( const Vector2i &vLocation ) const {
	return t3dRegionLocationValid( vLocation, s_nRegionMapSize );
}

Vector2i Terrain3DRegionRepository::getRegionLocation( const Vector3 &rGlobalPosition ) const {
	return Vector2i( ( t3dVector3ToVector2( rGlobalPosition ) / ( real_t )( s_nRegionWorldSizeMeters ) ).floor() );
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
	if ( regionId >= 0 && regionId < m_nRegionLocationCount && t3dRegionCoordEquals( m_szRegionLocations[regionId], rRegionLoc ) ) {
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
		if ( mappedSlot >= 0 && mappedSlot < m_nRegionLocationCount && t3dRegionCoordEquals( m_szRegionLocations[mappedSlot], rRegionLoc ) ) {
			return mappedSlot;
		}
	}
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( t3dRegionCoordEquals( m_szRegionLocations[slot], rRegionLoc ) ) {
			return slot;
		}
	}
	return -1;
}

Terrain3DRegion *Terrain3DRegionRepository::getRegionPtr( const Vector2i &rRegionLoc ) const {
	if ( !isLocationValid( rRegionLoc ) ) {
		return NULL;
	}
	const int mapIndex = getRegionMapIndex( rRegionLoc );
	return mapIndex >= 0 ? m_pszRegionPtrMap[mapIndex] : NULL;
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
	for ( int mapIndex = 0; mapIndex < s_nRegionSlots; ++mapIndex ) {
		if ( m_szRegionRefs[mapIndex].is_null() ) {
			continue;
		}
		const int x = mapIndex % s_nRegionMapSize;
		const int y = mapIndex / s_nRegionMapSize;
		regions[Vector2i( x - s_nRegionMapSize / 2, y - s_nRegionMapSize / 2 )] = m_szRegionRefs[mapIndex];
	}
	return regions;
}

Array Terrain3DRegionRepository::getRegionKeys( void ) const {
	Array keys;
	keys.resize( m_nRegionResourceCount );
	int write = 0;
	for ( int mapIndex = 0; mapIndex < s_nRegionSlots; ++mapIndex ) {
		if ( m_szRegionRefs[mapIndex].is_null() ) {
			continue;
		}
		const int x = mapIndex % s_nRegionMapSize;
		const int y = mapIndex / s_nRegionMapSize;
		keys[write++] = Vector2i( x - s_nRegionMapSize / 2, y - s_nRegionMapSize / 2 );
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
		const Vector2i location = t3dUnpackRegionCoord( m_szRegionLocations[slot] );
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
		slots[slot] = t3dUnpackRegionCoord( m_szRegionLocations[slot] );
	}
	return slots;
}

PackedInt32Array Terrain3DRegionRepository::getLogicalRegionMapPacked( void ) const {
	PackedInt32Array regionMap;
	if ( s_nRegionMapSize <= 0 ) {
		return regionMap;
	}
	regionMap.resize( s_nRegionMapSize * s_nRegionMapSize );
	int32_t *const pWrite = regionMap.ptrw();
	const int half = s_nRegionMapSize / 2;
	for ( int y = 0; y < s_nRegionMapSize; ++y ) {
		for ( int x = 0; x < s_nRegionMapSize; ++x ) {
			const int sourceIndex = getRegionMapIndex( Vector2i( x - half, y - half ) );
			pWrite[y * s_nRegionMapSize + x] = sourceIndex >= 0 && m_szRegionMap[sourceIndex] != 0u ? 1 : 0;
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
	m_pszRegionPtrMap[mapIndex] = NULL;
}

int Terrain3DRegionRepository::activateLocation( const Vector2i &rLocation, const bool bPublishRegionMap ) {
	return activateLocationInternal( rLocation, bPublishRegionMap, true );
}

int Terrain3DRegionRepository::activateLocationInternal(
        const Vector2i &rLocation, const bool bPublishRegionMap, const bool bBumpGeneration ) {
	if ( !isLocationValid( rLocation ) ) {
		return -1;
	}
	const int existingSlot = findSlot( rLocation );
	if ( existingSlot >= 0 ) {
		return existingSlot;
	}

	int slot = -1;
	while ( !m_freeRegionSlots.empty() ) {
		const int candidate = m_freeRegionSlots.back();
		m_freeRegionSlots.pop_back();
		if ( candidate >= 0 && candidate < m_nRegionLocationCount && !t3dRegionCoordValid( m_szRegionLocations[candidate] ) ) {
			slot = candidate;
			break;
		}
	}
	if ( slot < 0 ) {
		if ( m_nRegionLocationCount >= s_nRegionSlots || m_nRegionLocationCount >= s_nRegionMapSize * s_nRegionMapSize ) {
			return -1;
		}
		slot = m_nRegionLocationCount++;
	}

	m_szRegionLocations[slot] = t3dPackRegionCoord( rLocation );
	++m_nActiveRegionCount;
	const int mapIndex = getRegionMapIndex( rLocation );
	if ( bPublishRegionMap && mapIndex >= 0 && getRegionPtr( rLocation ) != NULL ) {
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

	m_szRegionLocations[slot] = t3dInvalidRegionCoord();
	if ( !m_freeRegionSlots.full() ) {
		m_freeRegionSlots.push_back( ( uint16_t )( slot ) );
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

	eastl::bitset<s_nRegionSlots> desiredLocations;
	eastl::fixed_vector<Vector2i, s_nRegionSlots, false> uniqueLocations;
	for ( size_t iLocation = 0u; iLocation < ( size_t )( ( rLocations ).size() ); iLocation++ ) {
		const Vector2i location = ( rLocations )[iLocation];
		if ( location == s_v2iMax || !isLocationValid( location ) ) {
			continue;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || desiredLocations.test( ( size_t )( mapIndex ) ) ) {
			continue;
		}
		desiredLocations.set( ( size_t )mapIndex );
		uniqueLocations.push_back( location );
	}

	eastl::bitset<s_nRegionSlots> existingLocations;
	int retainedCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_szRegionLocations[slot] );
		if ( location == s_v2iMax ) {
			continue;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || !desiredLocations.test( ( size_t )( mapIndex ) ) ) {
			result.m_removedLocations.push_back( location );
			m_szRegionLocations[slot] = t3dInvalidRegionCoord();
			result.m_bSlotsChanged = true;
		} else {
			existingLocations.set( ( size_t )( mapIndex ) );
			++retainedCount;
		}
	}

	m_nActiveRegionCount = retainedCount;
	rebuildFreeSlots();
	for ( size_t iLocation = 0u; iLocation < ( size_t )( ( uniqueLocations ).size() ); iLocation++ ) {
		const Vector2i location = ( uniqueLocations )[iLocation];
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
	m_freeRegionSlots.clear();
	for ( int slot = m_nRegionLocationCount - 1; slot >= 0; --slot ) {
		if ( !t3dRegionCoordValid( m_szRegionLocations[slot] ) ) {
			m_freeRegionSlots.push_back( ( uint16_t )( slot ) );
		}
	}
}

bool Terrain3DRegionRepository::ensureSlotReserve( const int nDesiredFreeSlots ) {
	if ( m_nActiveRegionCount <= 0 || s_nRegionMapSize <= 0 ) {
		return false;
	}

	int freeSlots = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( !t3dRegionCoordValid( m_szRegionLocations[slot] ) ) {
			++freeSlots;
		}
	}

	const int maximumSlots = s_nRegionMapSize * s_nRegionMapSize;
	const int desiredFree = MIN( MAX( 0, nDesiredFreeSlots ), maximumSlots - m_nActiveRegionCount );
	const int slotsToAdd = desiredFree - freeSlots;
	if ( slotsToAdd <= 0 ) {
		return false;
	}

	for ( int i = 0; i < slotsToAdd && m_nRegionLocationCount < maximumSlots; ++i ) {
		const int slot = m_nRegionLocationCount++;
		m_szRegionLocations[slot] = t3dInvalidRegionCoord();
		m_freeRegionSlots.push_back( ( uint16_t )( slot ) );
	}
	bumpTopologyGeneration();
	return true;
}

void Terrain3DRegionRepository::rebuildRegionMapFromSlots( void ) {
	memset( m_szRegionMap, ( uint16_t )0, sizeof( *m_szRegionMap ) * s_nRegionSlots );
	m_nActiveRegionCount = 0;
	m_freeRegionSlots.clear();
	bool bSanitized = false;
	eastl::bitset<s_nRegionSlots> publishedLocations;

	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_szRegionLocations[slot] );
		if ( location == s_v2iMax ) {
			m_freeRegionSlots.push_back( ( uint16_t )( slot ) );
			continue;
		}

		Terrain3DRegion *const pRegion = getRegionPtr( location );
		const int mapIndex = getRegionMapIndex( location );
		if ( !pRegion || pRegion->isDeleted() || mapIndex < 0 || !isLocationValid( location )
		        || publishedLocations.test( ( size_t )( mapIndex ) ) ) {
			m_szRegionLocations[slot] = t3dInvalidRegionCoord();
			m_freeRegionSlots.push_back( ( uint16_t )( slot ) );
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
        const t3dRegionCoord_t *pLocations,
        const int nLocationCount,
        const uint16_t *pLogicalRegionMap ) {
	for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_szRegionRefs ); iRegion++ ) {
		Ref<Terrain3DRegion> &region = ( m_szRegionRefs )[iRegion];
		region.unref();
	}
	m_nRegionResourceCount = 0;
	memset( m_pszRegionPtrMap, 0, sizeof( m_pszRegionPtrMap[0] ) * s_nRegionSlots );
	const Array keys = rRegions.keys();
	for ( size_t iKey = 0u; iKey < ( size_t )( ( keys ).size() ); iKey++ ) {
		const Vector2i location = ( keys )[iKey];
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
	for ( int i = 0; i < s_nRegionSlots; ++i ) {
		m_szRegionLocations[i] = t3dInvalidRegionCoord();
	}
	m_nRegionLocationCount = CLAMP( nLocationCount, 0, s_nRegionSlots );
	if ( m_nRegionLocationCount > 0 ) {
		memcpy( m_szRegionLocations, pLocations, ( size_t )( m_nRegionLocationCount ) * sizeof( t3dRegionCoord_t ) );
	}
	memcpy( m_szRegionMap, pLogicalRegionMap, sizeof( m_szRegionMap ) );
	m_bRegionMapDirty = false;
	m_nActiveRegionCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		if ( t3dRegionCoordValid( m_szRegionLocations[slot] ) ) {
			++m_nActiveRegionCount;
		}
	}
	rebuildFreeSlots();
	bumpTopologyGeneration();
}

bool Terrain3DRegionRepository::validateInvariants( void ) const {
	if ( m_nRegionLocationCount < 0 || m_nRegionLocationCount > s_nRegionSlots
	        || m_freeRegionSlots.size() > ( size_t )( m_nRegionLocationCount ) || m_nActiveRegionCount < 0
	        || m_nActiveRegionCount > m_nRegionLocationCount ) {
		return false;
	}

	int resourceCount = 0;
	for ( int mapIndex = 0; mapIndex < s_nRegionSlots; ++mapIndex ) {
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

	bool szFreeSeen[s_nRegionSlots];
	memset( szFreeSeen, 0, sizeof( szFreeSeen ) );
	for ( size_t iRegion = 0u; iRegion < ( size_t )( ( m_freeRegionSlots ).size() ); iRegion++ ) {
		const uint16_t free_slot = ( m_freeRegionSlots )[iRegion];
		const int slot = ( int )( free_slot );
		if ( slot < 0 || slot >= m_nRegionLocationCount || szFreeSeen[( size_t )( slot )] || t3dRegionCoordValid( m_szRegionLocations[slot] ) ) {
			return false;
		}
		szFreeSeen[( size_t )( slot )] = true;
	}

	bool szLocationSeen[s_nRegionSlots];
	memset( szLocationSeen, 0, sizeof( szLocationSeen ) );
	int activeCount = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_szRegionLocations[slot] );
		if ( location == s_v2iMax ) {
			if ( !szFreeSeen[( size_t )( slot )] ) {
				return false;
			}
			continue;
		}
		++activeCount;
		if ( !isLocationValid( location ) ) {
			return false;
		}
		const int mapIndex = getRegionMapIndex( location );
		if ( mapIndex < 0 || szLocationSeen[( size_t )( mapIndex )] ) {
			return false;
		}
		szLocationSeen[( size_t )( mapIndex )] = true;
		if ( !m_bRegionMapDirty && ( m_szRegionMap[mapIndex] != ( uint16_t )( slot + 1 ) || m_pszRegionPtrMap[mapIndex] == NULL ) ) {
			return false;
		}
	}
	if ( activeCount != m_nActiveRegionCount ) {
		return false;
	}

	if ( !m_bRegionMapDirty ) {
		for ( int mapIndex = 0; mapIndex < s_nRegionSlots; ++mapIndex ) {
			const int slot = ( int )( m_szRegionMap[mapIndex] ) - 1;
			if ( slot < 0 ) {
				continue;
			}
			if ( slot >= m_nRegionLocationCount || !t3dRegionCoordValid( m_szRegionLocations[slot] ) ) {
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
