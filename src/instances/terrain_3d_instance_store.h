// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_INSTANCE_STORE_H
#define TERRAIN3D_INSTANCE_STORE_H

#include "Terrain3DConstants.h"

#include "eastl_config.h"
#include "instances/terrain_3d_instance_layout.h"
#include "data/terrain_3d_packed_transform.h"
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

typedef struct t3dinstanceCell_s {
Vector2i location;
	T3DTransformArray transforms;
	eastl::vector<uint32_t> colors; // RGBA8. Empty means all white.
	eastl::vector<uint32_t> owners; // Sparse semantic owner IDs. Empty means owner 0.
	uint32_t nRevision = 1;
	bool bModified = true;
	AABB cachedAabb;
	AABB cachedSourceMeshAabb;
	uint32_t nCachedAabbRevision = 0;
	uint32_t nCachedAabbGeneration = 0;
	bool bCachedAabbValid = false;

	bool usesColors( void ) const {
		return !colors.empty();
	}

	bool usesOwners( void ) const {
		return !owners.empty();
	}

	uint32_t getOwner( const size_t iIndex ) const {
		return owners.empty() || iIndex >= owners.size() ? 0u : owners[iIndex];
	}

	static uint32_t packColor( const Color &clrColor ) {
		return clrColor.to_rgba32();
	}

	static Color unpackColor( const uint32_t nColor ) {
		return Color::hex( nColor );
	}

	Color getColor( const size_t iIndex ) const {
		return colors.empty() || iIndex >= colors.size() ? Color( 1, 1, 1, 1 ) : unpackColor( colors[iIndex] );
	}

	void setColor( const size_t iIndex, const Color &clrColor ) {
		colors[iIndex] = packColor( clrColor );
	}

	void pushColor( const Color &clrColor ) {
		colors.push_back( packColor( clrColor ) );
	}

	void reserve( size_t nCount, bool bColorData, bool bOwnerData = false ) {
		transforms.reserve( nCount );
		if ( bColorData ) {
			colors.reserve( nCount );
		}
		if ( bOwnerData ) {
			owners.reserve( nCount );
		}
	}

	void prepareAppend( size_t nAdditional, bool bColorData, bool bOwnerData = false ) {
		const size_t needed = transforms.size() + nAdditional;
		if ( needed > transforms.capacity() ) {
			transforms.reserve(
				eastl::max( needed, transforms.capacity() + eastl::max<size_t>( 32, transforms.capacity() / 2 ) ) );
		}
		if ( bColorData && colors.empty() ) {
			colors.resize( transforms.size(), TERRAIN3D_INSTANCE_WHITE_RGBA8 );
		}
		if ( !colors.empty() && needed > colors.capacity() ) {
			colors.reserve( eastl::max( needed, colors.capacity() + eastl::max<size_t>( 32, colors.capacity() / 2 ) ) );
		}
		if ( bOwnerData && owners.empty() ) {
			owners.resize( transforms.size(), 0u );
		}
		if ( !owners.empty() && needed > owners.capacity() ) {
			owners.reserve( eastl::max( needed, owners.capacity() + eastl::max<size_t>( 32, owners.capacity() / 2 ) ) );
		}
	}

	void compactOwnerSidecar( void ) {
		for ( const uint32_t owner : owners ) {
			if ( owner != 0u ) {
				return;
			}
		}
		owners.clear();
		owners.shrink_to_fit();
	}

	void compactStorage( void ) {
		transforms.shrinkToFit();
		colors.shrink_to_fit();
		owners.shrink_to_fit();
	}

	void touch( void ) {
		bModified = true;
		bCachedAabbValid = false;
		++nRevision;
		if ( nRevision == 0 ) {
			nRevision = 1;
		}
	}
} t3dinstanceCell_t;

typedef struct t3dinstanceMesh_s {
	int id = -1;
	eastl::vector<t3dinstanceCell_t> cells;
} t3dinstanceMesh_t;

class T3DInstanceStore {
	eastl::vector<t3dinstanceMesh_t> m_meshes;
	t3dinstanceLayout_t m_layout;

	static bool cellLess( const t3dinstanceCell_t &a, const Vector2i &b );
	static bool meshLess( const t3dinstanceMesh_t &a, int b );
	static size_t cellLowerBound( const eastl::vector<t3dinstanceCell_t> &cells, const Vector2i &location );
	static size_t findCellIndex( const t3dinstanceMesh_t &mesh, const Vector2i &location );
	size_t meshLowerBound( int nMeshId ) const;

public:
	T3DInstanceStore( void );
	~T3DInstanceStore( void );
	T3DInstanceStore( const T3DInstanceStore &rOther );
	T3DInstanceStore( T3DInstanceStore &&rOther );
	T3DInstanceStore &operator=( const T3DInstanceStore &rOther );
	T3DInstanceStore &operator=( T3DInstanceStore &&rOther );

	void clear( void );
	void compactStorage( void );

	void setLayout( const t3dinstanceLayout_t &rLayout ) {
		m_layout = rLayout;
	}
	const t3dinstanceLayout_t &getLayout( void ) const {
		return m_layout;
	}
	bool isValidCell( const Vector2i &rLocation ) const {
		return m_layout.contains( rLocation.x, rLocation.y );
	}
	bool empty( void ) const {
		return m_meshes.empty();
	}
	size_t meshCount( void ) const {
		return m_meshes.size();
	}
	const eastl::vector<t3dinstanceMesh_t> &meshes( void ) const {
		return m_meshes;
	}
	eastl::vector<t3dinstanceMesh_t> &meshesMut( void ) {
		return m_meshes;
	}

	const t3dinstanceMesh_t *findMesh( int nMeshId ) const;
	t3dinstanceMesh_t *findMesh( int nMeshId );
	t3dinstanceMesh_t &getOrCreateMesh( int nMeshId, size_t nReserveCells = 0 );
	bool eraseMesh( int nMeshId );
	void swapMeshIds( int nSrcId, int nDstId );
	bool hasMesh( const int nMeshId ) const {
		return findMesh( nMeshId ) != nullptr;
	}

	const t3dinstanceCell_t *findCell( int nMeshId, const Vector2i &cell ) const;
	t3dinstanceCell_t *findCell( int nMeshId, const Vector2i &cell );
	t3dinstanceCell_t &getOrCreateCell(
		int nMeshId, const Vector2i &cell, size_t nReserveInstances = 0,
		bool bColorData = false, bool bOwnerData = false
	);
	bool eraseCell( int nMeshId, const Vector2i &cell );

	void importDictionary( const Dictionary &instances );
	bool equalsDictionary( const Dictionary &instances ) const;
	Dictionary exportDictionary( void ) const;
	size_t instanceCount( int nMeshId = -1 ) const;
	size_t residentBytes( void ) const;
};

#endif
