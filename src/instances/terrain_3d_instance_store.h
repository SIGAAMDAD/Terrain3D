// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_STORE_H
#define TERRAIN3D_INSTANCE_STORE_H

#include "Terrain3DConstants.h"
#include "data/terrain_3d_packed_transform.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "core/terrain_3d_slab_array.h"
#include "instances/terrain_3d_instance_layout.h"

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef struct t3dInstanceCell_s {
	Vector2i m_location;
	Terrain3DTransformArray m_transforms;
	Terrain3DSlabArray<uint32_t> m_colors; // RGBA8. Empty means all white.
	Terrain3DSlabArray<uint32_t> m_owners; // Sparse semantic owner IDs. Empty means owner 0.
	uint32_t m_nRevision = 1;
	bool m_bModified = true;
	AABB m_cachedAabb;
	AABB m_cachedSourceMeshAabb;
	uint32_t m_nCachedAabbRevision = 0;
	uint32_t m_nCachedAabbGeneration = 0;
	bool m_bCachedAabbValid = false;

	t3dInstanceCell_s( void ) = default;
	EA_FORCE_INLINE void bindStorage( Terrain3DHighWaterSlab<t3dPackedTransform_t> *pTransformSlab,
	        Terrain3DHighWaterSlab<uint32_t> *pColorSlab,
	        Terrain3DHighWaterSlab<uint32_t> *pOwnerSlab ) {
		m_transforms.bind( pTransformSlab );
		m_colors.bind( pColorSlab );
		m_owners.bind( pOwnerSlab );
	}
	EA_FORCE_INLINE void rebindStorage( Terrain3DHighWaterSlab<t3dPackedTransform_t> *pTransformSlab,
	        Terrain3DHighWaterSlab<uint32_t> *pColorSlab,
	        Terrain3DHighWaterSlab<uint32_t> *pOwnerSlab ) {
		m_transforms.rebind( pTransformSlab );
		m_colors.rebind( pColorSlab );
		m_owners.rebind( pOwnerSlab );
	}
	t3dInstanceCell_s( const t3dInstanceCell_s & ) = delete;
	t3dInstanceCell_s &operator=( const t3dInstanceCell_s & ) = delete;
	t3dInstanceCell_s( t3dInstanceCell_s && ) noexcept = default;
	t3dInstanceCell_s &operator=( t3dInstanceCell_s && ) noexcept = default;

	EA_FORCE_INLINE bool usesColors( void ) const {
		return !m_colors.empty();
	}

	EA_FORCE_INLINE bool usesOwners( void ) const {
		return !m_owners.empty();
	}

	EA_FORCE_INLINE uint32_t getOwner( const size_t iIndex ) const {
		return m_owners.empty() || iIndex >= m_owners.size() ? 0u : m_owners[iIndex];
	}

	EA_FORCE_INLINE static uint32_t packColor( const Color &clrColor ) {
		return clrColor.to_rgba32();
	}

	EA_FORCE_INLINE static Color unpackColor( const uint32_t nColor ) {
		return Color::hex( nColor );
	}

	EA_FORCE_INLINE Color getColor( const size_t iIndex ) const {
		return m_colors.empty() || iIndex >= m_colors.size() ? Color( 1, 1, 1, 1 ) : unpackColor( m_colors[iIndex] );
	}

	EA_FORCE_INLINE void setColor( const size_t iIndex, const Color &clrColor ) {
		m_colors[iIndex] = packColor( clrColor );
	}

	EA_FORCE_INLINE bool pushColor( const Color &clrColor ) {
		return m_colors.pushBack( packColor( clrColor ) );
	}

	bool reserve( size_t nCount, bool bColorData, bool bOwnerData = false ) {
		if ( !m_transforms.reserve( nCount ) ) {
			return false;
		}
		if ( bColorData && !m_colors.reserve( nCount ) ) {
			return false;
		}
		return !bOwnerData || m_owners.reserve( nCount );
	}

	EA_FORCE_INLINE bool reserveExact( const size_t nCount, const bool bColorData, const bool bOwnerData = false ) {
		return reserve( nCount, bColorData, bOwnerData );
	}

	bool prepareAppend( size_t nAdditional, bool bColorData, bool bOwnerData = false ) {
		const size_t needed = m_transforms.size() + nAdditional;
		if ( needed > m_transforms.capacity()
		        && !m_transforms.reserve( MAX( needed, m_transforms.capacity() + MAX( 32u, m_transforms.capacity() / 2u ) ) ) ) {
			return false;
		}
		if ( bColorData && m_colors.empty() && !m_colors.resize( m_transforms.size(), s_nInstanceWhiteRgba8 ) ) {
			return false;
		}
		if ( !m_colors.empty() && needed > m_colors.capacity()
		        && !m_colors.reserve( MAX( needed, m_colors.capacity() + MAX( 32u, m_colors.capacity() / 2u ) ) ) ) {
			return false;
		}
		if ( bOwnerData && m_owners.empty() && !m_owners.resize( m_transforms.size(), 0u ) ) {
			return false;
		}
		if ( !m_owners.empty() && needed > m_owners.capacity()
		        && !m_owners.reserve( MAX( needed, m_owners.capacity() + MAX( 32u, m_owners.capacity() / 2u ) ) ) ) {
			return false;
		}
		return true;
	}

	EA_FORCE_INLINE Transform3D getTransform( const size_t iIndex ) const {
		return m_transforms[iIndex];
	}
	EA_FORCE_INLINE void setTransform( const size_t iIndex, const Transform3D &rTransform ) {
		m_transforms[iIndex] = rTransform;
	}
	EA_FORCE_INLINE bool pushTransform( const Transform3D &rTransform ) {
		return m_transforms.pushBack( rTransform );
	}
	EA_FORCE_INLINE Vector3 getTransformOrigin( const size_t iIndex ) const {
		return m_transforms.getOrigin( iIndex );
	}
	EA_FORCE_INLINE Vector3 getTransformUpAxis( const size_t iIndex ) const {
		return getTransform( iIndex ).basis.get_column( 1 );
	}
	EA_FORCE_INLINE void rescaleTransformOriginXz( const size_t iIndex, const real_t fOldSpacing, const real_t fNewSpacing ) {
		if ( fOldSpacing == 0.0 ) {
			return;
		}
		m_transforms.rescaleOriginXz( iIndex, fOldSpacing, fNewSpacing );
	}
	EA_FORCE_INLINE void setTransformOriginY( const size_t iIndex, const real_t fY ) {
		m_transforms.setOriginY( iIndex, fY );
	}

	void compactOwnerSidecar( void ) {
		for ( size_t iOwner = 0u; iOwner < ( size_t )( ( m_owners ).size() ); iOwner++ ) {
			const uint32_t owner = ( m_owners )[iOwner];
			if ( owner != 0u ) {
				return;
			}
		}
		m_owners.clear();
	}

	EA_FORCE_INLINE void compactStorage( void ) {
	}

	void touch( void ) {
		m_bModified = true;
		m_bCachedAabbValid = false;
		++m_nRevision;
		if ( m_nRevision == 0 ) {
			m_nRevision = 1;
		}
	}
} t3dInstanceCell_t;

typedef struct t3dInstanceMesh_s {
	int m_nId = -1;
	eastl::vector<t3dInstanceCell_t, Terrain3DEastlAllocator> m_cells;

	t3dInstanceMesh_s( void ) = default;
	t3dInstanceMesh_s( const t3dInstanceMesh_s & ) = delete;
	t3dInstanceMesh_s &operator=( const t3dInstanceMesh_s & ) = delete;
	t3dInstanceMesh_s( t3dInstanceMesh_s && ) noexcept = default;
	t3dInstanceMesh_s &operator=( t3dInstanceMesh_s && ) noexcept = default;
} t3dInstanceMesh_t;

class Terrain3DInstanceMeshRange {
	t3dInstanceMesh_t *m_pMeshes = NULL;
	uint16_t *m_pIds = NULL;
	size_t m_nCount = 0u;

public:
	class Iterator {
		t3dInstanceMesh_t *m_pMeshes = NULL;
		uint16_t *m_pIds = NULL;
		size_t m_iIndex = 0u;

	public:
		Iterator( t3dInstanceMesh_t *pMeshes, uint16_t *pIds, size_t iIndex ) : m_pMeshes( pMeshes ), m_pIds( pIds ), m_iIndex( iIndex ) {
		}
		EA_FORCE_INLINE bool operator!=( const Iterator &rOther ) const {
			return m_iIndex != rOther.m_iIndex;
		}
		EA_FORCE_INLINE Iterator &operator++( void ) {
			++m_iIndex;
			return *this;
		}
		EA_FORCE_INLINE t3dInstanceMesh_t &operator*( void ) const {
			return m_pMeshes[m_pIds[m_iIndex]];
		}
	};
	Terrain3DInstanceMeshRange( t3dInstanceMesh_t *pMeshes, uint16_t *pIds, size_t nCount ) :
	        m_pMeshes( pMeshes ), m_pIds( pIds ), m_nCount( nCount ) {
	}
	EA_FORCE_INLINE Iterator begin( void ) {
		return Iterator( m_pMeshes, m_pIds, 0u );
	}
	EA_FORCE_INLINE Iterator end( void ) {
		return Iterator( m_pMeshes, m_pIds, m_nCount );
	}
	EA_FORCE_INLINE size_t size( void ) const {
		return m_nCount;
	}
	EA_FORCE_INLINE bool empty( void ) const {
		return m_nCount == 0u;
	}
	EA_FORCE_INLINE t3dInstanceMesh_t &operator[]( const size_t iIndex ) {
		return m_pMeshes[m_pIds[iIndex]];
	}
};

class Terrain3DConstInstanceMeshRange {
	const t3dInstanceMesh_t *m_pMeshes = NULL;
	const uint16_t *m_pIds = NULL;
	size_t m_nCount = 0u;

public:
	class Iterator {
		const t3dInstanceMesh_t *m_pMeshes = NULL;
		const uint16_t *m_pIds = NULL;
		size_t m_iIndex = 0u;

	public:
		Iterator( const t3dInstanceMesh_t *pMeshes, const uint16_t *pIds, size_t iIndex ) :
		        m_pMeshes( pMeshes ), m_pIds( pIds ), m_iIndex( iIndex ) {
		}
		EA_FORCE_INLINE bool operator!=( const Iterator &rOther ) const {
			return m_iIndex != rOther.m_iIndex;
		}
		EA_FORCE_INLINE Iterator &operator++( void ) {
			++m_iIndex;
			return *this;
		}
		EA_FORCE_INLINE const t3dInstanceMesh_t &operator*( void ) const {
			return m_pMeshes[m_pIds[m_iIndex]];
		}
	};
	Terrain3DConstInstanceMeshRange( const t3dInstanceMesh_t *pMeshes, const uint16_t *pIds, size_t nCount ) :
	        m_pMeshes( pMeshes ), m_pIds( pIds ), m_nCount( nCount ) {
	}
	EA_FORCE_INLINE Iterator begin( void ) const {
		return Iterator( m_pMeshes, m_pIds, 0u );
	}
	EA_FORCE_INLINE Iterator end( void ) const {
		return Iterator( m_pMeshes, m_pIds, m_nCount );
	}
	EA_FORCE_INLINE size_t size( void ) const {
		return m_nCount;
	}
	EA_FORCE_INLINE bool empty( void ) const {
		return m_nCount == 0u;
	}
	EA_FORCE_INLINE const t3dInstanceMesh_t &operator[]( const size_t iIndex ) const {
		return m_pMeshes[m_pIds[iIndex]];
	}
};


typedef enum t3dInstanceStoreAllocationClass_e : uint8_t {
	TERRAIN3D_INSTANCE_STORE_PERSISTENT_RESOURCE = 0u,
	TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED,
} t3dInstanceStoreAllocationClass_t;

class Terrain3DInstanceStore {
	eastl::array<t3dInstanceMesh_t, s_nMaxMeshAssets> m_szMeshes;
	uint16_t m_szActiveMeshIds[s_nMaxMeshAssets];
	size_t m_nActiveMeshCount = 0u;
	Terrain3DHighWaterSlab<t3dPackedTransform_t> m_transformSlab;
	Terrain3DHighWaterSlab<uint32_t> m_colorSlab;
	Terrain3DHighWaterSlab<uint32_t> m_ownerSlab;
	t3dInstanceLayout_t m_layout;
	t3dInstanceStoreAllocationClass_t m_eAllocationClass = TERRAIN3D_INSTANCE_STORE_PERSISTENT_RESOURCE;
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;

	static bool cellLess( const t3dInstanceCell_t &a, const Vector2i &b );
	static size_t cellLowerBound( const eastl::vector<t3dInstanceCell_t, Terrain3DEastlAllocator> &cells, const Vector2i &location );
	static size_t findCellIndex( const t3dInstanceMesh_t &mesh, const Vector2i &location );
	void rebuildActiveMeshIds( void );
	void rebindCellStorage( void );
	bool copyFrom( const Terrain3DInstanceStore &rOther );

public:
	explicit Terrain3DInstanceStore( t3dInstanceStoreAllocationClass_t eAllocationClass = TERRAIN3D_INSTANCE_STORE_PERSISTENT_RESOURCE );
	~Terrain3DInstanceStore( void ) {
		shutdown();
	}
	Terrain3DInstanceStore( const Terrain3DInstanceStore &rOther );
	Terrain3DInstanceStore( Terrain3DInstanceStore &&rOther );
	Terrain3DInstanceStore &operator=( const Terrain3DInstanceStore &rOther );
	Terrain3DInstanceStore &operator=( Terrain3DInstanceStore &&rOther );

	EA_FORCE_INLINE void clear( void ) {
		reset();
	}
	void reset( void );
	EA_FORCE_INLINE void clearRetainCapacity( void ) {
		reset();
	}
	void shutdown( void );
	EA_FORCE_INLINE void releaseStorage( void ) {
		shutdown();
	}
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT );
	EA_FORCE_INLINE bool runtimeAllocatorReady( void ) const {
		return m_eAllocationClass != TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED || m_pRuntimeAllocatorZone != NULL;
	}
	EA_FORCE_INLINE t3dInstanceStoreAllocationClass_t allocationClass( void ) const {
		return m_eAllocationClass;
	}
	void compactStorage( void );
	EA_FORCE_INLINE void reserveMeshCapacity( const size_t ) {
	}

	EA_FORCE_INLINE void setLayout( const t3dInstanceLayout_t &rLayout ) {
		m_layout = rLayout;
	}
	EA_FORCE_INLINE const t3dInstanceLayout_t &getLayout( void ) const {
		return m_layout;
	}
	EA_FORCE_INLINE bool isValidCell( const Vector2i &rLocation ) const {
		return m_layout.contains( rLocation.x, rLocation.y );
	}
	EA_FORCE_INLINE bool empty( void ) const {
		return m_nActiveMeshCount == 0u;
	}
	EA_FORCE_INLINE size_t meshCount( void ) const {
		return m_nActiveMeshCount;
	}
	EA_FORCE_INLINE Terrain3DConstInstanceMeshRange meshes( void ) const {
		return Terrain3DConstInstanceMeshRange( m_szMeshes.data(), m_szActiveMeshIds, m_nActiveMeshCount );
	}
	EA_FORCE_INLINE Terrain3DInstanceMeshRange meshesMut( void ) {
		return Terrain3DInstanceMeshRange( m_szMeshes.data(), m_szActiveMeshIds, m_nActiveMeshCount );
	}

	const t3dInstanceMesh_t *findMesh( int nMeshId ) const;
	t3dInstanceMesh_t *findMesh( int nMeshId );
	t3dInstanceMesh_t &getOrCreateMesh( int nMeshId, size_t nReserveCells = 0 );
	bool eraseMesh( int nMeshId );
	void swapMeshIds( int nSrcId, int nDstId );
	EA_FORCE_INLINE bool hasMesh( const int nMeshId ) const {
		return findMesh( nMeshId ) != NULL;
	}

	const t3dInstanceCell_t *findCell( int nMeshId, const Vector2i &cell ) const;
	t3dInstanceCell_t *findCell( int nMeshId, const Vector2i &cell );
	t3dInstanceCell_t &getOrCreateCell(
	        int nMeshId, const Vector2i &cell, size_t nReserveInstances = 0, bool bColorData = false, bool bOwnerData = false );
	t3dInstanceCell_t &getOrCreateCellExact(
	        int nMeshId, const Vector2i &cell, size_t nReserveInstances, bool bColorData, bool bOwnerData );
	bool eraseCell( int nMeshId, const Vector2i &cell );

	void importDictionary( const Dictionary &instances );
	bool equalsDictionary( const Dictionary &instances ) const;
	Dictionary exportDictionary( void ) const;
	size_t instanceCount( int nMeshId = -1 ) const;
	size_t residentBytes( void ) const;
};

#endif // TERRAIN3D_INSTANCE_STORE_H
