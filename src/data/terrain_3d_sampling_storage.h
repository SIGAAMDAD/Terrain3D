// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_SAMPLING_STORAGE_H
#define TERRAIN3D_SAMPLING_STORAGE_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "eastl_config.h"
#include "memory/terrain_3d_active_storage.h"
#include "memory/terrain_3d_resident_pool.h"

#include <stddef.h>
#include <stdint.h>

class Terrain3DRegion;
class Terrain3DRegionRepository;

typedef struct t3dSamplingSource_s {
	const Terrain3DRegionRepository *m_pRepository = NULL;
	const class Terrain3DSamplingStorage *m_pStorage = NULL;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_pRepository != NULL && m_pStorage != NULL;
	}
} t3dSamplingSource_t;

// Owns the CPU publication mechanisms used by terrain sampling. Spatial policy
// (which regions should be active/resident) stays outside this class; this class
// only owns storage, leases, and the epochs that make sampler-local caches safe.
class Terrain3DSamplingStorage {
public:
	Terrain3DSamplingStorage( void );
	~Terrain3DSamplingStorage( void );
	Terrain3DSamplingStorage( const Terrain3DSamplingStorage & ) = delete;
	Terrain3DSamplingStorage &operator=( const Terrain3DSamplingStorage & ) = delete;

	bool claimOrReconfigureActive( void );
	bool bindActive( void *pBacking, size_t nBackingBytes );
	void releaseActive( void );
	void resetActive( void );
	EA_FORCE_INLINE bool isActiveOwner( void ) const {
		return t3dActiveStorageIsOwner( this );
	}
	uint32_t refreshActiveGeneration( void );
	EA_FORCE_INLINE uint32_t getActiveGeneration( void ) const {
		return m_nActiveStorageGeneration;
	}

	EA_FORCE_INLINE Terrain3DActiveMapLease acquireActive( const Vector2i &rLocation ) const {
		return isActiveOwner() ? t3dActiveStorageAcquire( this, rLocation ) : Terrain3DActiveMapLease();
	}
	bool containsActive( const Vector2i &rLocation ) const;
	bool syncActiveRegion( const Vector2i &rLocation, const Terrain3DRegion *pRegion );
	bool syncActiveHeightPage( const Vector2i &rLocation, const uint8_t *pHeightBytes, size_t nHeightBytes );
	bool replaceActiveHeightPages( const uint8_t *const *ppHeightPages, const int *pSlots, size_t nPageCount );
	bool syncActiveMap(
	        const Vector2i &rLocation, const Terrain3DRegion *pRegion, t3dMapType_t eType, const Rect2i &rDirtyRect = Rect2i() );
	bool syncActiveDerived( const Vector2i &rLocation, const Terrain3DRegion *pRegion, const Rect2i &rDirtyRect = Rect2i() );
	void invalidateActive( const Vector2i &rLocation );

	size_t getActiveReservedBytes( void ) const;
	int getActivePhysicalCapacity( void ) const;
	uint64_t getActiveReaderRetryCount( void ) const;
	uint64_t getActiveReaderMutexCount( void ) const;
	uint64_t getActivePublicationCount( void ) const;

	Terrain3DResidentMapLease acquireResident( const Vector2i &rLocation, const Terrain3DRegion *pRegion ) const;
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone );
	bool configureResidentPool( bool bControlMapsImplicit, int nRequestedCapacity );
	void clearResidentPool( void );
	void resetResidentPool( void );
	void invalidateResident( const Vector2i &rLocation );
	void syncResidentMap(
	        const Vector2i &rLocation, const Terrain3DRegion *pRegion, t3dMapType_t eType, const Rect2i &rDirtyRect = Rect2i() );
	void syncResidentDerived( const Vector2i &rLocation, const Terrain3DRegion *pRegion, const Rect2i &rDirtyRect = Rect2i() );
	size_t getResidentReservedBytes( void ) const;

	void invalidateAllSamplers( void );
	void publishRegionGeneration( const Vector2i &rLocation );
	EA_FORCE_INLINE uint64_t getSamplingGeneration( void ) const {
		return m_nSamplingGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint32_t getRegionGeneration( const Vector2i &rLocation ) const {
		const int index = locationIndex( rLocation );
		return index >= 0 ? m_szRegionSamplingGenerations[( size_t )( index )].load( T3D_MEMORY_ORDER_ACQUIRE ) : 0u;
	}

private:
	uint32_t m_nActiveStorageGeneration = 0u;
	mutable Terrain3DResidentRegionPool m_residentPool;
	t3dAtomic_t<uint64_t> m_nSamplingGeneration{ 1u };
	t3dAtomic_t<uint32_t> m_szRegionSamplingGenerations[s_nRegionSlots];

	static int locationIndex( const Vector2i &rLocation );
};

#endif // TERRAIN3D_SAMPLING_STORAGE_H
