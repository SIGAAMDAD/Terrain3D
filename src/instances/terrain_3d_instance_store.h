// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_INSTANCE_STORE_H
#define TERRAIN3D_INSTANCE_STORE_H

#include "Terrain3DConstants.h"
#include "data/terrain_3d_packed_transform.h"
#include "eastl_config.h"
#include "instances/terrain_3d_instance_layout.h"

#include <EASTL/algorithm.h>
#include <EASTL/sort.h>
#include <EASTL/utility.h>
#include <EASTL/vector.h>
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
	t3dInstanceCell_s( const t3dInstanceCell_s & ) = delete;
	t3dInstanceCell_s &operator=( const t3dInstanceCell_s & ) = delete;
	t3dInstanceCell_s( t3dInstanceCell_s && ) noexcept = default;
	t3dInstanceCell_s &operator=( t3dInstanceCell_s && ) noexcept = default;

	EA_FORCE_INLINE void bindPayloadPools( Terrain3DHighWaterSlab<t3dPackedTransform_t> *const pTransforms,
			Terrain3DHighWaterSlab<uint32_t> *const pColors,
			Terrain3DHighWaterSlab<uint32_t> *const pOwners ) {
		m_transforms.bind( pTransforms );
		m_colors.bind( pColors );
		m_owners.bind( pOwners );
	}

	EA_FORCE_INLINE void rebindPayloadPools( Terrain3DHighWaterSlab<t3dPackedTransform_t> *const pTransforms,
			Terrain3DHighWaterSlab<uint32_t> *const pColors,
			Terrain3DHighWaterSlab<uint32_t> *const pOwners ) {
		m_transforms.rebind( pTransforms );
		m_colors.rebind( pColors );
		m_owners.rebind( pOwners );
	}

	EA_FORCE_INLINE void relocatePayloadsExact( Terrain3DHighWaterSlab<t3dPackedTransform_t> *const pTransforms,
			Terrain3DHighWaterSlab<uint32_t> *const pColors,
			Terrain3DHighWaterSlab<uint32_t> *const pOwners ) {
		m_transforms.relocateExact( pTransforms );
		m_colors.relocateExact( pColors );
		m_owners.relocateExact( pOwners );
	}

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

	EA_FORCE_INLINE void pushColor( const Color &clrColor ) {
		m_colors.pushBack( packColor( clrColor ) );
	}

	void reserve( size_t nCount, bool bColorData, bool bOwnerData = false ) {
		m_transforms.reserve( nCount );
		if ( bColorData ) {
			m_colors.reserve( nCount );
		}
		if ( bOwnerData ) {
			m_owners.reserve( nCount );
		}
	}

	EA_FORCE_INLINE void reserveExact( const size_t nCount, const bool bColorData, const bool bOwnerData = false ) {
		m_transforms.reserveExact( nCount );
		if ( bColorData ) {
			m_colors.reserveExact( nCount );
		}
		if ( bOwnerData ) {
			m_owners.reserveExact( nCount );
		}
	}

	void prepareAppend( size_t nAdditional, bool bColorData, bool bOwnerData = false ) {
		const size_t needed = m_transforms.size() + nAdditional;
		if ( needed > m_transforms.capacity() ) {
			m_transforms.reserve( eastl::max(
					needed, m_transforms.capacity() + eastl::max<size_t>( 32, m_transforms.capacity() / 2 ) ) );
		}
		if ( bColorData && m_colors.empty() ) {
			m_colors.resize( m_transforms.size(), s_nInstanceWhiteRgba8 );
		}
		if ( !m_colors.empty() && needed > m_colors.capacity() ) {
			m_colors.reserve(
					eastl::max( needed, m_colors.capacity() + eastl::max<size_t>( 32, m_colors.capacity() / 2 ) ) );
		}
		if ( bOwnerData && m_owners.empty() ) {
			m_owners.resize( m_transforms.size(), 0u );
		}
		if ( !m_owners.empty() && needed > m_owners.capacity() ) {
			m_owners.reserve(
					eastl::max( needed, m_owners.capacity() + eastl::max<size_t>( 32, m_owners.capacity() / 2 ) ) );
		}
	}

	void compactOwnerSidecar( void ) {
		for ( const uint32_t owner : m_owners ) {
			if ( owner != 0u ) {
				return;
			}
		}
		m_owners.clear();
	}

	EA_FORCE_INLINE void compactStorage( void ) {}

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
	eastl::vector<t3dInstanceCell_t> m_cells;

	t3dInstanceMesh_s( void ) = default;
	t3dInstanceMesh_s( const t3dInstanceMesh_s & ) = delete;
	t3dInstanceMesh_s &operator=( const t3dInstanceMesh_s & ) = delete;
	t3dInstanceMesh_s( t3dInstanceMesh_s && ) noexcept = default;
	t3dInstanceMesh_s &operator=( t3dInstanceMesh_s && ) noexcept = default;
} t3dInstanceMesh_t;

class Terrain3DInstanceStore {
	static constexpr uint16_t s_nInvalidMeshSlot = UINT16_MAX;
	eastl::vector<t3dInstanceMesh_t> m_meshes;
	Terrain3DHighWaterSlab<t3dPackedTransform_t> m_transformSlab;
	Terrain3DHighWaterSlab<uint32_t> m_colorSlab;
	Terrain3DHighWaterSlab<uint32_t> m_ownerSlab;
	t3dInstanceLayout_t m_layout;
	mutable uint16_t m_szMeshSlots[s_nMaxMeshAssets];
	mutable bool m_bMeshLookupDirty = true;

	static bool cellLess( const t3dInstanceCell_t &a, const Vector2i &b );
	static bool meshLess( const t3dInstanceMesh_t &a, int b );
	static size_t cellLowerBound( const eastl::vector<t3dInstanceCell_t> &cells, const Vector2i &location );
	static size_t findCellIndex( const t3dInstanceMesh_t &mesh, const Vector2i &location );
	size_t meshLowerBound( int nMeshId ) const;
	void rebuildMeshLookup( void ) const;
	void rebindPayloadPools( void );
	void copyFrom( const Terrain3DInstanceStore &rOther );
	EA_FORCE_INLINE void bindCellPayloads( t3dInstanceCell_t &rCell ) {
		rCell.bindPayloadPools( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
	}
	EA_FORCE_INLINE void invalidateMeshLookup( void ) {
		m_bMeshLookupDirty = true;
	}

public:
	EA_FORCE_INLINE Terrain3DInstanceStore( void ) {
		rebuildMeshLookup();
	}
	EA_FORCE_INLINE ~Terrain3DInstanceStore( void ) = default;
	EA_FORCE_INLINE Terrain3DInstanceStore( const Terrain3DInstanceStore &rOther ) {
		copyFrom( rOther );
	}
	Terrain3DInstanceStore( Terrain3DInstanceStore &&rOther );
	Terrain3DInstanceStore &operator=( const Terrain3DInstanceStore &rOther );
	Terrain3DInstanceStore &operator=( Terrain3DInstanceStore &&rOther );

	EA_FORCE_INLINE void clear( void ) {
		clearRetainCapacity();
	}
	void clearRetainCapacity( void );
	void releaseStorage( void );
	void compactStorage( void );
	EA_FORCE_INLINE void reserveMeshCapacity( const size_t nMeshes ) {
		if ( nMeshes > m_meshes.capacity() ) {
			m_meshes.reserve( nMeshes );
		}
	}
	EA_FORCE_INLINE void
	reservePayloadCapacity( const size_t nTransforms, const size_t nColors, const size_t nOwners ) {
		m_transformSlab.reserve( nTransforms );
		m_colorSlab.reserve( nColors );
		m_ownerSlab.reserve( nOwners );
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
		return m_meshes.empty();
	}
	EA_FORCE_INLINE size_t meshCount( void ) const {
		return m_meshes.size();
	}
	EA_FORCE_INLINE const eastl::vector<t3dInstanceMesh_t> &meshes( void ) const {
		return m_meshes;
	}
	EA_FORCE_INLINE eastl::vector<t3dInstanceMesh_t> &meshesMut( void ) {
		invalidateMeshLookup();
		return m_meshes;
	}

	const t3dInstanceMesh_t *findMesh( int nMeshId ) const;
	t3dInstanceMesh_t *findMesh( int nMeshId );
	t3dInstanceMesh_t &getOrCreateMesh( int nMeshId, size_t nReserveCells = 0 );
	bool eraseMesh( int nMeshId );
	void swapMeshIds( int nSrcId, int nDstId );
	EA_FORCE_INLINE bool hasMesh( const int nMeshId ) const {
		return findMesh( nMeshId ) != nullptr;
	}

	const t3dInstanceCell_t *findCell( int nMeshId, const Vector2i &cell ) const;
	t3dInstanceCell_t *findCell( int nMeshId, const Vector2i &cell );
	t3dInstanceCell_t &getOrCreateCell( int nMeshId,
			const Vector2i &cell,
			size_t nReserveInstances = 0,
			bool bColorData = false,
			bool bOwnerData = false );
	t3dInstanceCell_t &getOrCreateCellExact( int nMeshId,
			const Vector2i &cell,
			size_t nReserveInstances,
			bool bColorData,
			bool bOwnerData );
	bool eraseCell( int nMeshId, const Vector2i &cell );

	void importDictionary( const Dictionary &instances );
	bool equalsDictionary( const Dictionary &instances ) const;
	Dictionary exportDictionary( void ) const;
	size_t instanceCount( int nMeshId = -1 ) const;
	size_t residentBytes( void ) const;
	EA_FORCE_INLINE size_t transformPayloadCapacity( void ) const {
		return m_transformSlab.capacity();
	}
	EA_FORCE_INLINE size_t colorPayloadCapacity( void ) const {
		return m_colorSlab.capacity();
	}
	EA_FORCE_INLINE size_t ownerPayloadCapacity( void ) const {
		return m_ownerSlab.capacity();
	}
};

#endif
