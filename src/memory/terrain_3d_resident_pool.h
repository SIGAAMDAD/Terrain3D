// Fixed-residency pool backed by one contiguous map slab and fixed slot metadata.
#pragma once

#ifndef TERRAIN3D_RESIDENT_POOL_H
#define TERRAIN3D_RESIDENT_POOL_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_limits.h"
#include "data/terrain_3d_region.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <godot_cpp/variant/vector2i.hpp>
#include <mutex>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef struct t3dResidentMapView_s {
	const uint16_t *m_pHeight = nullptr;
	const uint32_t *m_pControl = nullptr;
	const uint32_t *m_pColor = nullptr;
	const uint8_t *m_pDerived = nullptr;
	uint32_t m_nGeneration = 0;
	EA_FORCE_INLINE bool valid( void ) const {
		return m_pHeight != nullptr;
	}
} t3dResidentMapView_t;

class Terrain3DResidentRegionPool;

class Terrain3DResidentMapLease {
public:
	Terrain3DResidentMapLease( void ) = default;
	~Terrain3DResidentMapLease( void );
	Terrain3DResidentMapLease( const Terrain3DResidentMapLease & ) = delete;
	Terrain3DResidentMapLease &operator=( const Terrain3DResidentMapLease & ) = delete;
	Terrain3DResidentMapLease( Terrain3DResidentMapLease &&rOther ) noexcept;
	Terrain3DResidentMapLease &operator=( Terrain3DResidentMapLease &&rOther ) noexcept;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_view.valid();
	}
	EA_FORCE_INLINE const t3dResidentMapView_t &getView( void ) const {
		return m_view;
	}
	void reset( void );

private:
	friend class Terrain3DResidentRegionPool;
	Terrain3DResidentRegionPool *m_pPool = nullptr;
	int16_t m_iSlot = -1;
	uint32_t m_nGeneration = 0u;
	t3dResidentMapView_t m_view;

	Terrain3DResidentMapLease( Terrain3DResidentRegionPool *pPool,
			int iSlot,
			uint32_t nGeneration,
			const t3dResidentMapView_t &rView );
};

typedef enum t3dChannel_e : uint32_t {
	RESIDENT_CHANNEL_HEIGHT = 1u << 0u,
	RESIDENT_CHANNEL_CONTROL = 1u << 1u,
	RESIDENT_CHANNEL_COLOR = 1u << 2u,
	RESIDENT_CHANNEL_DERIVED = 1u << 3u,
} t3dChannel_t;

typedef enum t3dResidentPoolState_e : uint8_t {
	T3D_RESIDENT_POOL_INACTIVE = 0u,
	T3D_RESIDENT_POOL_ACTIVE,
	T3D_RESIDENT_POOL_DRAINING,
} t3dResidentPoolState_t;

typedef struct t3dAlignedAllocation_s {
	void *m_pRaw = nullptr;
	uint8_t *m_pAligned = nullptr;
	size_t m_nBytes = 0u;
	bool m_bOwned = false;
	bool allocate( size_t nRequestedBytes, size_t nAlignment );
	void release( void );
} t3dAlignedAllocation_t;

typedef struct t3dSlotLayout_s {
	size_t m_nHeightSlotBytes = 0u;
	size_t m_nControlSlotBytes = 0u;
	size_t m_nColorSlotBytes = 0u;
	size_t m_nDerivedSlotBytes = 0u;
	size_t m_nStride = 0u;
} t3dSlotLayout_t;

typedef struct alignas( s_nResidentCacheAlignment ) t3dSlot_s {
	Vector2i m_location = Vector2i( INT32_MAX, INT32_MAX );
	std::atomic<uint32_t> m_nGeneration{ 1u };
	std::atomic<uint32_t> m_nPinCount{ 0u };
	std::atomic<uint8_t> m_nState{ 0u };
	std::atomic<uint8_t> m_nValidChannels{ 0u };
	std::atomic_bool m_bReferenced{ false };
	uint16_t *m_pHeight = nullptr;
	uint32_t *m_pControl = nullptr;
	uint32_t *m_pColor = nullptr;
	uint8_t *m_pDerived = nullptr;
} t3dSlot_t;

class Terrain3DResidentRegionPool {
	friend class Terrain3DResidentMapLease;

public:
	Terrain3DResidentRegionPool( void ) = default;
	~Terrain3DResidentRegionPool( void );
	Terrain3DResidentRegionPool( const Terrain3DResidentRegionPool & ) = delete;
	Terrain3DResidentRegionPool &operator=( const Terrain3DResidentRegionPool & ) = delete;

	static size_t estimateSlotBytes( int nRegionSize, uint32_t nChannels );
	bool configure( int nRegionSize, int nCapacity, uint32_t nChannels );
	void clear( void );
	void resetResidency( void );
	Terrain3DResidentMapLease acquire( const Vector2i &rLocation, const Terrain3DRegion *pRegion );
	bool ensureResident( const Vector2i &rLocation, const Terrain3DRegion *pRegion );
	void syncMap( const Vector2i &rLocation,
			const Terrain3DRegion *pRegion,
			t3dMapType_t type,
			const Rect2i &rDirtyRect = Rect2i() );
	void syncDerived( const Vector2i &rLocation, const Terrain3DRegion *pRegion, const Rect2i &rDirtyRect = Rect2i() );
	void invalidate( const Vector2i &rLocation );
	EA_FORCE_INLINE int getCapacity( void ) const {
		return m_nCapacity;
	}
	EA_FORCE_INLINE int getRegionSize( void ) const {
		return m_nRegionSize;
	}
	EA_FORCE_INLINE uint32_t getChannels( void ) const {
		return m_nChannels;
	}
	EA_FORCE_INLINE size_t getReservedBytes( void ) const {
		return m_nBackingBytes + m_nMetadataBytes;
	}
	EA_FORCE_INLINE size_t getBackingBytes( void ) const {
		return m_nBackingBytes;
	}
	EA_FORCE_INLINE size_t getMetadataBytes( void ) const {
		return m_nMetadataBytes;
	}

private:
	// Replacement and lookup touch only the prefix below. Keep it in the first
	// cacheline and prevent adjacent slot metadata from sharing that line; sparse
	// allocation ownership and plane pointers are cold outside acquire/sync.

	int m_nRegionSize = 0;
	int m_nCapacity = 0;
	uint32_t m_nChannels = 0u;
	size_t m_nTexelCount = 0u;
	size_t m_nBackingBytes = 0u;
	size_t m_nMetadataBytes = 0u;
	t3dSlotLayout_t m_iSlotLayout;
	t3dAlignedAllocation_t m_mapMemory;
	t3dSlot_t *m_pSlots = nullptr;
	bool m_bSlotsOwned = false;
	std::atomic<uint8_t> m_nLifecycle{ T3D_RESIDENT_POOL_INACTIVE };
	std::atomic<uint32_t> m_nAcquireGate{ UINT32_C( 0x80000000 ) };
	std::atomic<int16_t> m_szLocationToSlot[s_nResidentWorldSlotCount] = {};
	mutable std::mutex m_mutex;
	int m_iClockHand = 0;
	static constexpr uint32_t s_nAcquireGateClosed = UINT32_C( 0x80000000 );
	static constexpr uint32_t s_nAcquireReaderMask = ~s_nAcquireGateClosed;

	static bool alignUp( size_t nValue, size_t nAlignment, size_t &nAligned );
	static bool buildSlotLayout( int nRegionSize, uint32_t nChannels, t3dSlotLayout_t &rLayout, size_t &nTexelCount );
	static bool addEstimatedOptionalBytes( uint32_t nChannels,
			uint32_t nChannel,
			size_t nTexelCount,
			size_t nBytesPerTexel,
			size_t &nTotal );
	static int locationIndex( const Vector2i &rLocation );
	bool allocateSlotsUnlocked( int nCapacity );
	bool assignMapBankPointersUnlocked( void );
	bool hasPinnedSlotsUnlocked( void ) const;
	bool beginAcquireOperation( void );
	void endAcquireOperation( void );
	void beginDrainUnlocked( void );
	void waitForReadersUnlocked( void ) const;
	void activateUnlocked( void );
	void deactivateUnlocked( void );
	void releaseSlotsUnlocked( void );
	void clearUnlocked( void );
	int chooseReplacementSlotUnlocked( void );
	int findSlotUnlocked( const Vector2i &rLocation ) const;
	bool resetSlotStorage( int iSlot, bool bZero );
	void assignSlot( int iSlot, const Vector2i &rLocation );
	void publishSlot( int iSlot );
	void retireSlotUnlocked( int iSlot );
	void releaseLease( int iSlot, uint32_t nGeneration );
	bool copyMap( int iSlot, const Terrain3DRegion *pRegion, t3dMapType_t type );
	bool copyMapRect( int iSlot, const Terrain3DRegion *pRegion, t3dMapType_t type, const Rect2i &rDirtyRect );
	bool copyDerived( int iSlot, const Terrain3DRegion *pRegion );
	bool copyDerivedRect( int iSlot, const Terrain3DRegion *pRegion, const Rect2i &rDirtyRect );
	t3dResidentMapView_t getView( int iSlot ) const;
};

#endif // TERRAIN3D_RESIDENT_POOL_H
