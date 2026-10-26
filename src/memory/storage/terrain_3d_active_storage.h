// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_ACTIVE_STORAGE_H
#define TERRAIN3D_ACTIVE_STORAGE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/config/terrain_3d_limits.h"
#include "core/utility/terrain_3d_sync.h"
#include "data/model/terrain_3d_region.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Height hot storage is a fixed 32-page, cache-aligned R16 bank. Every
// Terrain3DSamplingStorage owns one independent bank, logical map, publication
// generation, and writer lock. There is no process-global active terrain owner.
typedef struct t3dActiveMapView_s
{

	const uint16_t *m_pHeight = NULL;
	uint32_t m_nOwnerGeneration = 0u;
	uint32_t m_nRegionGeneration = 0u;
	int m_iSlot = -1;

	EA_FORCE_INLINE bool valid( void ) const
	{
		return m_pHeight != NULL && m_iSlot >= 0;
	}
} t3dActiveMapView_t;


typedef void ( *t3dActiveLeaseReleaseFn_t )( void *pContext, int iPage );

class Terrain3DActiveMapLease
{
public:
	Terrain3DActiveMapLease( void ) = default;
	~Terrain3DActiveMapLease( void );
	Terrain3DActiveMapLease( const Terrain3DActiveMapLease& ) = delete;
	Terrain3DActiveMapLease& operator=( const Terrain3DActiveMapLease& ) = delete;
	Terrain3DActiveMapLease( Terrain3DActiveMapLease&& rOther ) noexcept;
	Terrain3DActiveMapLease& operator=( Terrain3DActiveMapLease&& rOther ) noexcept;

	EA_FORCE_INLINE bool valid( void ) const
	{
		return m_view.valid();
	}
	EA_FORCE_INLINE const t3dActiveMapView_t& view( void ) const
	{
		return m_view;
	}
	void reset( void );

private:
	void *m_pReleaseContext = NULL;
	t3dActiveLeaseReleaseFn_t m_pRelease = NULL;
	int m_iPage = -1;
	t3dActiveMapView_t m_view;

public:
	Terrain3DActiveMapLease( void *pReleaseContext, t3dActiveLeaseReleaseFn_t pRelease, int iPage, const t3dActiveMapView_t& rView );
};

class Terrain3DActiveStorage
{
public:
	Terrain3DActiveStorage( void );
	~Terrain3DActiveStorage( void );
	Terrain3DActiveStorage( const Terrain3DActiveStorage& ) = delete;
	Terrain3DActiveStorage& operator=( const Terrain3DActiveStorage& ) = delete;

	static size_t requiredBytes( void );

	bool claim( void );
	bool bind( void *pBacking, size_t nBackingBytes );
	bool reconfigure( void );
	void release( void );
	void reset( void );

	EA_FORCE_INLINE bool isActive( void ) const
	{
		return m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) == TERRAIN3D_ACTIVE_STORAGE_ACTIVE;
	}
	EA_FORCE_INLINE int getRegionMapSize( void ) const
	{
		return isActive() ? T3D_REGION_MAP_SIZE : 0;
	}
	EA_FORCE_INLINE uint32_t getGeneration( void ) const
	{
		return isActive() ? m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) : 0u;
	}
	EA_FORCE_INLINE size_t getReservedBytes( void ) const
	{
		return m_nHeightBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE int getPhysicalCapacity( void ) const
	{
		return T3D_ACTIVE_PHYSICAL_PAGE_COUNT;
	}
	EA_FORCE_INLINE uint64_t getReaderRetryCount( void ) const
	{
		return m_nReaderRetries.load( T3D_MEMORY_ORDER_RELAXED );
	}
	EA_FORCE_INLINE uint64_t getReaderMutexCount( void ) const
	{
		return 0u;
	}
	EA_FORCE_INLINE uint64_t getPublicationCount( void ) const
	{
		return m_nPublications.load( T3D_MEMORY_ORDER_RELAXED );
	}

	bool contains( const Vector2i& rLocation ) const;
	Terrain3DActiveMapLease acquire( const Vector2i& rLocation );
	bool syncRegion( const Vector2i& rLocation, const Terrain3DRegion *pRegion );
	bool syncHeightPage( const Vector2i& rLocation, const uint8_t *pHeightBytes, size_t nHeightBytes );
	bool replaceHeightPages( const uint8_t *const *pHeightPages, const int *pSlots, size_t nPageCount );
	bool syncMap( const Vector2i& rLocation, const Terrain3DRegion *pRegion, t3dMapType_t type, const Rect2i& rDirtyRect = Rect2i() );
	bool syncDerived( const Vector2i& rLocation, const Terrain3DRegion *pRegion, const Rect2i& rDirtyRect = Rect2i() );
	void invalidate( const Vector2i& rLocation );
	void releaseLease( int iPhysicalPage ) { releaseReader( iPhysicalPage ); }

private:
	friend class Terrain3DActiveMapLease;
typedef struct t3dActivePhysicalPage_s
	{

		uint16_t *m_pHeight = NULL;
		Terrain3DAtomic<uint32_t> m_nPins { 0u };
		Terrain3DAtomic<uint32_t> m_nGeneration { 1u };
		Terrain3DAtomic<int> m_iLogicalSlot { T3D_ACTIVE_INVALID_PAGE };
		Terrain3DAtomic<uint32_t> m_nState { TERRAIN3D_ACTIVE_PAGE_FREE };
	} t3dActivePhysicalPage_t;


	t3dActivePhysicalPage_t m_szPhysicalPages[ T3D_ACTIVE_PHYSICAL_PAGE_COUNT ];
	Terrain3DAtomic<int> m_szLogicalToPhysical[ T3D_REGION_SLOTS ];
	void *m_pHeightBackingRaw = NULL;
	uint8_t *m_pHeightBacking = NULL;
	bool m_bHeightBackingOwned = false;
	Terrain3DAtomic<size_t> m_nHeightBytes { 0u };
	Terrain3DAtomic<uint32_t> m_nState { TERRAIN3D_ACTIVE_STORAGE_FREE };
	Terrain3DAtomic<uint32_t> m_nGeneration { 1u };
	Terrain3DAtomic<uint64_t> m_nReaderRetries { 0u };
	Terrain3DAtomic<uint64_t> m_nPublications { 0u };
	uint32_t m_nClockHand = 0u;
	Terrain3DMutex m_writerMutex;

	EA_FORCE_INLINE static size_t activePageBytes( void )
	{
		return (size_t)( T3D_REGION_HEIGHT_BYTES );
	}
	static int locationToSlot( const Vector2i& rLocation );
	static void bumpGeneration( Terrain3DAtomic<uint32_t>& rGeneration );

	void clearMappingsUnsafe( void );
	void waitForReadersUnsafe( void );
	void freeBackingUnsafe( void );
	bool configureBackingUnsafe( void *pBacking, size_t nBackingBytes, bool bOwned );
	bool allocateBackingUnsafe( void );
	int chooseStagingPageUnsafe( int iPreservedPage );
	bool publishBytesUnsafe( int iLogicalSlot, const uint8_t *pHeightBytes, size_t nHeightBytes );
	bool copyRegionUnsafe( int iLogicalSlot, const Terrain3DRegion *pRegion );
	void releaseReader( int iPhysicalPage );
};

EA_FORCE_INLINE size_t t3dActiveStorageRequiredBytes( void )
{
	return Terrain3DActiveStorage::requiredBytes();
}

#endif // TERRAIN3D_ACTIVE_STORAGE_H
