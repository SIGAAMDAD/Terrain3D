// Fixed-residency pool backed by one contiguous map slab and fixed slot metadata.
#pragma once

#ifndef TERRAIN3D_RESIDENT_POOL_H
#define TERRAIN3D_RESIDENT_POOL_H

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>

#include <godot_cpp/variant/vector2i.hpp>

#include "core/terrain_3d_limits.h"
#include "data/terrain_3d_region.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef struct t3dResidentMapView_s {
	const uint16_t *pHeight = nullptr;
	const uint32_t *pControl = nullptr;
	const uint32_t *pColor = nullptr;
	const uint8_t *pDerived = nullptr;
	uint32_t nGeneration = 0;
	EA_FORCE_INLINE bool valid( void ) const { return pHeight != nullptr; }
} t3dResidentMapView_t;

class T3DResidentRegionPool;

class T3DResidentMapLease {
public:
	T3DResidentMapLease( void ) = default;
	~T3DResidentMapLease( void );
	T3DResidentMapLease( const T3DResidentMapLease & ) = delete;
	T3DResidentMapLease &operator=( const T3DResidentMapLease & ) = delete;
	T3DResidentMapLease( T3DResidentMapLease &&rOther ) noexcept;
	T3DResidentMapLease &operator=( T3DResidentMapLease &&rOther ) noexcept;

	EA_FORCE_INLINE bool valid( void ) const { return m_view.valid(); }
	EA_FORCE_INLINE const t3dResidentMapView_t &getView( void ) const { return m_view; }
	void reset( void );

private:
	friend class T3DResidentRegionPool;
	T3DResidentRegionPool *m_pPool = nullptr;
	int16_t m_iSlot = -1;
	uint32_t m_nGeneration = 0u;
	t3dResidentMapView_t m_view;

	T3DResidentMapLease(
		T3DResidentRegionPool *pPool, int iSlot, uint32_t nGeneration,
		const t3dResidentMapView_t &rView
	);
};


class T3DResidentRegionPool {
	friend class T3DResidentMapLease;

public:

	typedef enum t3dChannel_e : uint32_t {
		CHANNEL_HEIGHT = 1u << 0u,
		CHANNEL_CONTROL = 1u << 1u,
		CHANNEL_COLOR = 1u << 2u,
		CHANNEL_DERIVED = 1u << 3u,
	} t3dChannel_t;

	T3DResidentRegionPool( void ) = default;
	~T3DResidentRegionPool( void );
	T3DResidentRegionPool( const T3DResidentRegionPool & ) = delete;
	T3DResidentRegionPool &operator=( const T3DResidentRegionPool & ) = delete;

	static size_t estimateSlotBytes( int nRegionSize, uint32_t nChannels );
	bool configure( int nRegionSize, int nCapacity, uint32_t nChannels );
	void clear( void );
	void resetResidency( void );
	T3DResidentMapLease acquire( const Vector2i &rLocation, const T3DRegion *pRegion );
	bool ensureResident( const Vector2i &rLocation, const T3DRegion *pRegion );
	void syncMap(
		const Vector2i &rLocation, const T3DRegion *pRegion, t3dMapType_t type,
		const Rect2i &rDirtyRect = Rect2i()
	);
	void syncDerived(
		const Vector2i &rLocation, const T3DRegion *pRegion,
		const Rect2i &rDirtyRect = Rect2i()
	);
	void invalidate( const Vector2i &rLocation );
	EA_FORCE_INLINE int getCapacity( void ) const { return m_nCapacity; }
	EA_FORCE_INLINE int getRegionSize( void ) const { return m_nRegionSize; }
	EA_FORCE_INLINE uint32_t getChannels( void ) const { return m_channels; }
	EA_FORCE_INLINE size_t getReservedBytes( void ) const { return m_nBackingBytes + m_nMetadataBytes; }
	EA_FORCE_INLINE size_t getBackingBytes( void ) const { return m_nBackingBytes; }
	EA_FORCE_INLINE size_t getMetadataBytes( void ) const { return m_nMetadataBytes; }

private:
typedef struct t3dAlignedAllocation_s {
		void *pRaw = nullptr;
		uint8_t *pAligned = nullptr;
		size_t nBytes = 0u;
		bool bOwned = false;
		bool allocate( size_t nRequestedBytes, size_t nAlignment );
		void release( void );
	} t3dAlignedAllocation_t;

	typedef struct t3dSlotLayout_s {
		size_t nHeightSlotBytes = 0u;
		size_t nControlSlotBytes = 0u;
		size_t nColorSlotBytes = 0u;
		size_t nDerivedSlotBytes = 0u;
		size_t stride = 0u;
	} t3dSlotLayout_t;

	// Replacement and lookup touch only the prefix below. Keep it in the first
	// cacheline and prevent adjacent slot metadata from sharing that line; sparse
	// allocation ownership and plane pointers are cold outside acquire/sync.
	typedef struct alignas( TERRAIN3D_RESIDENT_CACHE_ALIGNMENT ) t3dSlot_s {
		Vector2i location = Vector2i( INT32_MAX, INT32_MAX );
		std::atomic<uint32_t> nGeneration{1u};
		std::atomic<uint32_t> nPinCount{0u};
		std::atomic<uint8_t> nState{0u};
		std::atomic<uint8_t> nValidChannels{0u};
		std::atomic_bool bReferenced{false};
		uint16_t *pHeight = nullptr;
		uint32_t *pControl = nullptr;
		uint32_t *pColor = nullptr;
		uint8_t *pDerived = nullptr;
	} t3dSlot_t;

	int m_nRegionSize = 0;
	int m_nCapacity = 0;
	uint32_t m_channels = 0u;
	size_t m_nTexelCount = 0u;
	size_t m_nBackingBytes = 0u;
	size_t m_nMetadataBytes = 0u;
	t3dSlotLayout_t m_iSlotLayout;
	t3dAlignedAllocation_t m_mapMemory;
	t3dSlot_t *m_pSlots = nullptr;
	bool m_bSlotsOwned = false;
	std::atomic<int16_t> m_szLocationToSlot[TERRAIN3D_RESIDENT_WORLD_SLOT_COUNT] = {};
	mutable std::mutex m_mutex;
	int m_iClockHand = 0;

	static bool alignUp( size_t nValue, size_t nAlignment, size_t &nAligned );
	static bool buildSlotLayout(
		int nRegionSize, uint32_t nChannels, t3dSlotLayout_t &rLayout,
		size_t &nTexelCount
	);
	static bool addEstimatedOptionalBytes(
		uint32_t nChannels, uint32_t nChannel, size_t nTexelCount,
		size_t nBytesPerTexel, size_t &nTotal
	);
	static int locationIndex( const Vector2i &rLocation );
	bool allocateSlotsUnlocked( int nCapacity );
	bool assignMapBankPointersUnlocked( void );
	bool hasPinnedSlotsUnlocked( void ) const;
	void releaseSlotsUnlocked( void );
	void clearUnlocked( void );
	int chooseReplacementSlotUnlocked( void );
	int findSlotUnlocked( const Vector2i &rLocation ) const;
	bool resetSlotStorage( int iSlot, bool bZero );
	void assignSlot( int iSlot, const Vector2i &rLocation );
	void publishSlot( int iSlot );
	void retireSlotUnlocked( int iSlot );
	void releaseLease( int iSlot, uint32_t nGeneration );
	bool copyMap( int iSlot, const T3DRegion *pRegion, t3dMapType_t type );
	bool copyMapRect( int iSlot, const T3DRegion *pRegion, t3dMapType_t type, const Rect2i &rDirtyRect );
	bool copyDerived( int iSlot, const T3DRegion *pRegion );
	bool copyDerivedRect( int iSlot, const T3DRegion *pRegion, const Rect2i &rDirtyRect );
	t3dResidentMapView_t getView( int iSlot ) const;
};

#endif // TERRAIN3D_RESIDENT_POOL_H
