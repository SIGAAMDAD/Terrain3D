// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_SAMPLING_STORAGE_H
#define TERRAIN3D_SAMPLING_STORAGE_H

#include "Terrain3DConstants.h"
#include "memory/terrain_3d_active_storage.h"
#include "memory/terrain_3d_resident_pool.h"

#include "eastl_config.h"
#include <EASTL/array.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

class T3DRegion;
class T3DRegionRepository;

typedef struct t3dSamplingSource_s {
	const T3DRegionRepository *pRepository = nullptr;
	const class T3DSamplingStorage *pStorage = nullptr;

	EA_FORCE_INLINE bool valid( void ) const { return pRepository != nullptr && pStorage != nullptr; }
} t3dSamplingSource_t;

// Owns the CPU publication mechanisms used by terrain sampling. Spatial policy
// (which regions should be active/resident) stays outside this class; this class
// only owns storage, leases, and the epochs that make sampler-local caches safe.
class T3DSamplingStorage {
public:
	T3DSamplingStorage( void );
	~T3DSamplingStorage( void );
	T3DSamplingStorage( const T3DSamplingStorage & ) = delete;
	T3DSamplingStorage &operator=( const T3DSamplingStorage & ) = delete;

	bool claimOrReconfigureActive( int nRegionSize );
	bool bindActive( int nRegionSize, void *pBacking, size_t nBackingBytes );
	void releaseActive( void );
	void resetActive( void );
	EA_FORCE_INLINE bool isActiveOwner( void ) const { return terrain3dActiveStorageIsOwner( this ); }
	uint32_t refreshActiveGeneration( void );
	EA_FORCE_INLINE uint32_t getActiveGeneration( void ) const { return m_nActiveStorageGeneration; }

	EA_FORCE_INLINE T3DActiveMapLease acquireActive( const Vector2i &rLocation ) const { return isActiveOwner() ? terrain3dActiveStorageAcquire( this, rLocation ) : T3DActiveMapLease(); }
	bool containsActive( const Vector2i &rLocation ) const;
	bool syncActiveRegion( const Vector2i &rLocation, const T3DRegion *pRegion );
	bool syncActiveHeightPage( const Vector2i &rLocation, const uint8_t *pHeightBytes, size_t nHeightBytes );
	bool replaceActiveHeightPages( int nRegionSize, const uint8_t *const *ppHeightPages, const int *pSlots, size_t nPageCount );
	bool syncActiveMap( const Vector2i &rLocation, const T3DRegion *pRegion, t3dMapType_t eType,
		const Rect2i &rDirtyRect = Rect2i() );
	bool syncActiveDerived( const Vector2i &rLocation, const T3DRegion *pRegion,
		const Rect2i &rDirtyRect = Rect2i() );
	void invalidateActive( const Vector2i &rLocation );

	size_t getActiveReservedBytes( void ) const;
	int getActivePhysicalCapacity( void ) const;
	uint64_t getActiveReaderRetryCount( void ) const;
	uint64_t getActiveReaderMutexCount( void ) const;
	uint64_t getActivePublicationCount( void ) const;

	T3DResidentMapLease acquireResident( const Vector2i &rLocation, const T3DRegion *pRegion ) const;
	bool configureResidentPool( int nRegionSize, int nRegionMapSize, bool bControlMapsImplicit,
		int nRequestedCapacity, size_t nBudgetBytes );
	void clearResidentPool( void );
	void resetResidentPool( void );
	void invalidateResident( const Vector2i &rLocation );
	void syncResidentMap( const Vector2i &rLocation, const T3DRegion *pRegion, t3dMapType_t eType,
		const Rect2i &rDirtyRect = Rect2i() );
	void syncResidentDerived( const Vector2i &rLocation, const T3DRegion *pRegion,
		const Rect2i &rDirtyRect = Rect2i() );
	size_t getResidentReservedBytes( void ) const;

	void invalidateAllSamplers( void );
	void publishRegionGeneration( const Vector2i &rLocation );
	EA_FORCE_INLINE uint64_t getSamplingGeneration( void ) const { return m_nSamplingGeneration.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint32_t getRegionGeneration( const Vector2i &rLocation ) const { const int index = locationIndex( rLocation ); return index >= 0 ? m_szRegionSamplingGenerations[( size_t ) ( index )].load( std::memory_order_acquire ) : 0u; }

private:
	uint32_t m_nActiveStorageGeneration = 0u;
	mutable T3DResidentRegionPool m_residentPool;
	std::atomic<uint64_t> m_nSamplingGeneration{1u};
	eastl::array<std::atomic<uint32_t>, TERRAIN3D_MAX_REGION_SLOTS> m_szRegionSamplingGenerations;

	static int locationIndex( const Vector2i &rLocation );
};

#endif // TERRAIN3D_SAMPLING_STORAGE_H
