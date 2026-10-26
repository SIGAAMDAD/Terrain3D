// Fixed-residency pool backed by one contiguous map slab and fixed slot metadata.
#ifndef TERRAIN3D_RESIDENT_POOL_H
#define TERRAIN3D_RESIDENT_POOL_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_compiler.h"
#include "memory/storage/terrain_3d_callback_lease.h"
#include "core/config/terrain_3d_limits.h"
#include "data/model/terrain_3d_region.h"
#include "memory/arena/terrain_3d_memory_zone.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/vector2i.hpp>
#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef struct t3dResidentMapView_s
{
	const uint16_t		*m_pHeight = NULL;
	const uint32_t		*m_pControl = NULL;
	const uint32_t		*m_pColor = NULL;
	const uint8_t		*m_pDerived = NULL;
	uint32_t			 m_nGeneration = 0;
	EA_FORCE_INLINE bool valid(
		void ) const {
		return m_pHeight != NULL;
	}
} t3dResidentMapView_t;

typedef void ( *t3dResidentLeaseReleaseFn_t )( void *pContext, int iSlot, uint32_t nGeneration );

struct t3dResidentLeaseReleaseState_t
{
	void					   *m_pReleaseContext = NULL;
	t3dResidentLeaseReleaseFn_t m_pRelease = NULL;
	int16_t						m_iSlot = -1;
	uint32_t					m_nGeneration = 0u;
	void						release() {
		   if ( m_pRelease && m_pReleaseContext && m_iSlot >= 0 ) {
			   m_pRelease( m_pReleaseContext, m_iSlot, m_nGeneration );
		   }
	}
};

class Terrain3DResidentMapLease
{
public:
	Terrain3DResidentMapLease( void ) = default;
	~Terrain3DResidentMapLease( void );
	Terrain3DResidentMapLease( const Terrain3DResidentMapLease & ) = delete;
	Terrain3DResidentMapLease &operator=( const Terrain3DResidentMapLease & ) = delete;
	Terrain3DResidentMapLease( Terrain3DResidentMapLease &&rOther ) noexcept;
	Terrain3DResidentMapLease &operator=( Terrain3DResidentMapLease &&rOther ) noexcept;

	EA_FORCE_INLINE bool valid(
		void ) const {
		return m_lease.valid();
	}
	EA_FORCE_INLINE const t3dResidentMapView_t &getView(
		void ) const {
		return m_lease.view();
	}
	void reset( void );

private:
	Terrain3DCallbackLease< t3dResidentMapView_t, t3dResidentLeaseReleaseState_t > m_lease;

public:
	Terrain3DResidentMapLease( void						  *pReleaseContext,
							   t3dResidentLeaseReleaseFn_t pRelease,
							   int						   iSlot,
							   uint32_t					   nGeneration,
							   const t3dResidentMapView_t &rView );
};

typedef struct t3dAlignedAllocation_s
{
	Terrain3DMemoryZone *m_pZone = NULL;
	void				*m_pRaw = NULL;
	uint8_t				*m_pAligned = NULL;
	size_t				 m_nBytes = 0u;
	bool				 m_bOwned = false;
	bool				 allocate( Terrain3DMemoryZone *pZone, size_t nRequestedBytes, size_t nAlignment );
	void				 release( void );
} t3dAlignedAllocation_t;

typedef struct alignas(
	T3D_RESIDENT_CACHE_ALIGNMENT ) t3dSlot_s
{
	Vector2i					m_location = Vector2i( INT32_MAX, INT32_MAX );
	Terrain3DAtomic< uint32_t > m_nGeneration{ 1u };
	Terrain3DAtomic< uint32_t > m_nPinCount{ 0u };
	Terrain3DAtomic< uint8_t >	m_nState{ 0u };
	Terrain3DAtomic< uint8_t >	m_nValidChannels{ 0u };
	t3dAtomicBool_t				m_bReferenced{ false };
	uint16_t				   *m_pHeight = NULL;
	uint32_t				   *m_pControl = NULL;
	uint32_t				   *m_pColor = NULL;
	uint8_t					   *m_pDerived = NULL;
} t3dSlot_t;

class Terrain3DResidentRegionPool
{
	friend class Terrain3DResidentMapLease;

public:
	Terrain3DResidentRegionPool( void ) = default;
	~Terrain3DResidentRegionPool( void );
	Terrain3DResidentRegionPool( const Terrain3DResidentRegionPool & ) = delete;
	Terrain3DResidentRegionPool &operator=( const Terrain3DResidentRegionPool & ) = delete;

	static size_t			  estimateSlotBytes( uint32_t nChannels );
	bool					  bindRuntimeAllocator( Terrain3DMemoryZone *pZone );
	bool					  configure( int nCapacity, uint32_t nChannels );
	void					  clear( void );
	void					  resetResidency( void );
	Terrain3DResidentMapLease acquire( const Vector2i &rLocation, const Terrain3DRegion *pRegion );
	bool					  ensureResident( const Vector2i &rLocation, const Terrain3DRegion *pRegion );
	void					  syncMap( const Vector2i		 &rLocation,
									   const Terrain3DRegion *pRegion,
									   t3dMapType_t			  type,
									   const Rect2i			 &rDirtyRect = Rect2i() );
	void syncDerived( const Vector2i &rLocation, const Terrain3DRegion *pRegion, const Rect2i &rDirtyRect = Rect2i() );
	void invalidate( const Vector2i &rLocation );
	void releaseLeaseCallback(
		int		 iSlot,
		uint32_t nGeneration ) {
		releaseLease( iSlot, nGeneration );
	}
	EA_FORCE_INLINE int getCapacity(
		void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE Terrain3DMemoryZone *getRuntimeAllocatorZone(
		void ) const {
		return m_pRuntimeAllocatorZone;
	}
	EA_FORCE_INLINE int getRegionSize(
		void ) const {
		return T3D_REGION_SIZE;
	}
	EA_FORCE_INLINE uint32_t getChannels(
		void ) const {
		return m_nChannels;
	}
	EA_FORCE_INLINE size_t getReservedBytes(
		void ) const {
		return m_nBackingBytes + m_nMetadataBytes;
	}
	EA_FORCE_INLINE size_t getBackingBytes(
		void ) const {
		return m_nBackingBytes;
	}
	EA_FORCE_INLINE size_t getMetadataBytes(
		void ) const {
		return m_nMetadataBytes;
	}

private:
	// Replacement and lookup touch only the prefix below. Keep it in the first
	// cacheline and prevent adjacent slot metadata from sharing that line; sparse
	// allocation ownership and plane pointers are cold outside acquire/sync.

	Terrain3DMemoryZone		   *m_pRuntimeAllocatorZone = NULL;
	int							m_nCapacity = 0;
	uint32_t					m_nChannels = 0u;
	size_t						m_nBackingBytes = 0u;
	size_t						m_nMetadataBytes = 0u;
	t3dSlotLayout_t				m_iSlotLayout;
	t3dAlignedAllocation_t		m_mapMemory;
	t3dSlot_t				   *m_pSlots = NULL;
	bool						m_bSlotsOwned = false;
	Terrain3DAtomic< uint8_t >	m_nLifecycle{ T3D_RESIDENT_POOL_INACTIVE };
	Terrain3DAtomic< uint32_t > m_nAcquireGate{ UINT32_C( 0x80000000 ) };
	Terrain3DAtomic< int16_t >	m_szLocationToSlot[T3D_RESIDENT_WORLD_SLOT_COUNT] = {};
	mutable Terrain3DMutex		m_mutex;
	int							m_iClockHand = 0;

	static bool buildSlotLayout( uint32_t nChannels, t3dSlotLayout_t &rLayout, size_t &nTexelCount );
	static bool addEstimatedOptionalBytes( uint32_t nChannels,
										   uint32_t nChannel,
										   size_t	nTexelCount,
										   size_t	nBytesPerTexel,
										   size_t  &nTotal );
	static int	locationIndex( const Vector2i &rLocation );
	bool		allocateSlotsUnlocked( int nCapacity );
	bool		assignMapBankPointersUnlocked( void );
	bool		hasPinnedSlotsUnlocked( void ) const;
	bool		beginAcquireOperation( void );
	void		endAcquireOperation( void );
	void		beginDrainUnlocked( void );
	void		waitForReadersUnlocked( void ) const;
	void		activateUnlocked( void );
	void		deactivateUnlocked( void );
	void		releaseSlotsUnlocked( void );
	void		clearUnlocked( void );
	int			chooseReplacementSlotUnlocked( void );
	int			findSlotUnlocked( const Vector2i &rLocation ) const;
	bool		resetSlotStorage( int iSlot, bool bZero );
	void		assignSlot( int iSlot, const Vector2i &rLocation );
	void		publishSlot( int iSlot );
	void		retireSlotUnlocked( int iSlot );
	void		releaseLease( int iSlot, uint32_t nGeneration );
	bool		copyMap( int iSlot, const Terrain3DRegion *pRegion, t3dMapType_t type );
	bool		copyMapRect( int iSlot, const Terrain3DRegion *pRegion, t3dMapType_t type, const Rect2i &rDirtyRect );
	bool		copyDerived( int iSlot, const Terrain3DRegion *pRegion );
	bool		copyDerivedRect( int iSlot, const Terrain3DRegion *pRegion, const Rect2i &rDirtyRect );
	t3dResidentMapView_t getView( int iSlot ) const;
};

#endif // TERRAIN3D_RESIDENT_POOL_H
