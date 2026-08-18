// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Compact persistent instance transforms: float32 origins and binary16 bases.
#pragma once

#ifndef TERRAIN3D_PACKED_TRANSFORM_H
#define TERRAIN3D_PACKED_TRANSFORM_H

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstddef>
#include <cstdint>

#include "data/terrain_3d_half_buffer.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Two transforms fit exactly in one 64-byte cacheline. The 32-byte alignment
// prevents an individual transform from straddling cachelines in EASTL storage.
typedef struct alignas( 32 ) t3dpackedTransform_s {
	float origin[3] = {0.0f, 0.0f, 0.0f};
	uint16_t basis[9] = {};
	uint16_t reserved = 0u;

	t3dpackedTransform_s( void ) {
		set( Transform3D() );
	}

	explicit t3dpackedTransform_s( const Transform3D &rTransform ) {
		set( rTransform );
	}

	void set( const Transform3D &rTransform ) {
		origin[0] = ( float ) ( rTransform.origin.x );
		origin[1] = ( float ) ( rTransform.origin.y );
		origin[2] = ( float ) ( rTransform.origin.z );
		for ( int row = 0; row < 3; ++row ) {
			basis[row * 3 + 0] = terrain3d_half::encode( ( float ) ( rTransform.basis[row].x ) );
			basis[row * 3 + 1] = terrain3d_half::encode( ( float ) ( rTransform.basis[row].y ) );
			basis[row * 3 + 2] = terrain3d_half::encode( ( float ) ( rTransform.basis[row].z ) );
		}
		reserved = 0u;
	}

	Transform3D get( void ) const {
		Basis unpacked_basis;
		for ( int row = 0; row < 3; ++row ) {
			unpacked_basis[row] = Vector3(
				terrain3d_half::decode( basis[row * 3 + 0] ), terrain3d_half::decode( basis[row * 3 + 1] ),
				terrain3d_half::decode( basis[row * 3 + 2] ) );
		}
		return Transform3D( unpacked_basis, Vector3( origin[0], origin[1], origin[2] ) );
	}
} t3dpackedTransform_t;

static_assert( sizeof( t3dpackedTransform_t ) == 32u, "Packed instance transforms must remain 32 bytes." );
static_assert( alignof( t3dpackedTransform_t ) == 32u, "Packed instance transforms must remain 32-byte aligned." );

class T3DTransformArray {
public:
	class T3DReference {
	public:
		explicit T3DReference( t3dpackedTransform_t *const pTransform ) : m_pTransform( pTransform ) {}

		T3DReference &operator=( const Transform3D &rTransform ) {
			m_pTransform->set( rTransform );
			return *this;
		}

		T3DReference &operator=( const T3DReference &rOther ) {
			return operator=( Transform3D( rOther ) );
		}

		operator Transform3D( void ) const {
			return m_pTransform->get();
		}

	private:
		t3dpackedTransform_t *m_pTransform = nullptr;
	};

	void reserve( const size_t nCount ) {
		m_storage.reserve( nCount );
	}

	void resize( const size_t nCount ) {
		m_storage.resize( nCount );
	}

	void clear( void ) {
		m_storage.clear();
	}

	void shrinkToFit( void ) {
		m_storage.shrink_to_fit();
	}

	void pushBack( const Transform3D &rTransform ) {
		m_storage.emplace_back( rTransform );
	}

	size_t size( void ) const {
		return m_storage.size();
	}

	size_t capacity( void ) const {
		return m_storage.capacity();
	}

	bool empty( void ) const {
		return m_storage.empty();
	}

	T3DReference operator[]( const size_t iIndex ) {
		return T3DReference( &m_storage[iIndex] );
	}

	Transform3D operator[]( const size_t iIndex ) const {
		return m_storage[iIndex].get();
	}

	const t3dpackedTransform_t *data( void ) const {
		return m_storage.data();
	}

	t3dpackedTransform_t *data( void ) {
		return m_storage.data();
	}

	Vector3 getOrigin( const size_t iIndex ) const {
		const t3dpackedTransform_t &packed = m_storage[iIndex];
		return Vector3( packed.origin[0], packed.origin[1], packed.origin[2] );
	}

	Vector3 getUpAxis( const size_t iIndex ) const {
		const t3dpackedTransform_t &packed = m_storage[iIndex];
		return Vector3( terrain3d_half::decode( packed.basis[1] ), terrain3d_half::decode( packed.basis[4] ),
			terrain3d_half::decode( packed.basis[7] ) );
	}

	void setOriginY( const size_t iIndex, const real_t fY ) {
		m_storage[iIndex].origin[1] = ( float ) ( fY );
	}

	void rescaleOriginXz( const size_t iIndex, const real_t fOldSpacing, const real_t fNewSpacing ) {
		t3dpackedTransform_t &packed = m_storage[iIndex];
		packed.origin[0] = ( float ) ( ( real_t ) ( packed.origin[0] ) / fOldSpacing * fNewSpacing );
		packed.origin[2] = ( float ) ( ( real_t ) ( packed.origin[2] ) / fOldSpacing * fNewSpacing );
	}

private:
	eastl::vector<t3dpackedTransform_t> m_storage;
};

#endif // TERRAIN3D_PACKED_TRANSFORM_H
