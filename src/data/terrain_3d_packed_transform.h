// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Compact persistent instance transforms: float32 origins and binary16 bases.
#pragma once

#ifndef TERRAIN3D_PACKED_TRANSFORM_H
#define TERRAIN3D_PACKED_TRANSFORM_H

#include "core/terrain_3d_slab_array.h"
#include "eastl_config.h"
#include <godot_cpp/variant/transform3d.hpp>

#include <cstddef>
#include <cstdint>

#include "data/terrain_3d_half_buffer.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Two transforms fit exactly in one 64-byte cacheline. The 32-byte alignment
// prevents an individual transform from straddling cachelines in EASTL storage.
typedef struct alignas( 32 ) t3dPackedTransform_s {
	float origin[3] = {0.0f, 0.0f, 0.0f};
	uint16_t basis[9] = {};
	uint16_t reserved = 0u;

	EA_FORCE_INLINE t3dPackedTransform_s( void ) { set( Transform3D() ); }

	EA_FORCE_INLINE explicit t3dPackedTransform_s( const Transform3D &rTransform ) { set( rTransform ); }

	void set( const Transform3D &rTransform ) {
		origin[0] = ( float ) ( rTransform.origin.x );
		origin[1] = ( float ) ( rTransform.origin.y );
		origin[2] = ( float ) ( rTransform.origin.z );
		for ( int row = 0; row < 3; ++row ) {
			basis[row * 3 + 0] = t3dHalfEncode( ( float ) ( rTransform.basis[row].x ) );
			basis[row * 3 + 1] = t3dHalfEncode( ( float ) ( rTransform.basis[row].y ) );
			basis[row * 3 + 2] = t3dHalfEncode( ( float ) ( rTransform.basis[row].z ) );
		}
		reserved = 0u;
	}

	Transform3D get( void ) const {
		Basis unpacked_basis;
		for ( int row = 0; row < 3; ++row ) {
			unpacked_basis[row] = Vector3(
				t3dHalfDecode( basis[row * 3 + 0] ), t3dHalfDecode( basis[row * 3 + 1] ),
				t3dHalfDecode( basis[row * 3 + 2] ) );
		}
		return Transform3D( unpacked_basis, Vector3( origin[0], origin[1], origin[2] ) );
	}
} t3dPackedTransform_t;

static_assert( sizeof( t3dPackedTransform_t ) == 32u, "Packed instance transforms must remain 32 bytes." );
static_assert( alignof( t3dPackedTransform_t ) == 32u, "Packed instance transforms must remain 32-byte aligned." );

class T3DTransformArray {
public:
	class T3DReference {
	public:
		EA_FORCE_INLINE explicit T3DReference( t3dPackedTransform_t *const pTransform ) : m_pTransform( pTransform ) {}

		T3DReference &operator=( const Transform3D &rTransform ) {
			m_pTransform->set( rTransform );
			return *this;
		}

		T3DReference &operator=( const T3DReference &rOther ) {
			return operator=( Transform3D( rOther ) );
		}

		EA_FORCE_INLINE operator Transform3D( void ) const { return m_pTransform->get(); }

	private:
		t3dPackedTransform_t *m_pTransform = nullptr;
	};

	EA_FORCE_INLINE void bind( T3DHighWaterSlab<t3dPackedTransform_t> *const pSlab ) { m_storage.bind( pSlab ); }

	EA_FORCE_INLINE void rebind( T3DHighWaterSlab<t3dPackedTransform_t> *const pSlab ) { m_storage.rebind( pSlab ); }

	EA_FORCE_INLINE void relocateExact( T3DHighWaterSlab<t3dPackedTransform_t> *const pSlab ) { m_storage.relocateExact( pSlab ); }

	EA_FORCE_INLINE void reserve( const size_t nCount ) { m_storage.reserve( nCount ); }

	EA_FORCE_INLINE void reserveExact( const size_t nCount ) { m_storage.reserveExact( nCount ); }

	EA_FORCE_INLINE void resize( const size_t nCount ) { m_storage.resize( nCount ); }

	EA_FORCE_INLINE void clear( void ) { m_storage.clear(); }

	EA_FORCE_INLINE void shrinkToFit( void ) {}

	EA_FORCE_INLINE void pushBack( const Transform3D &rTransform ) { m_storage.push_back( t3dPackedTransform_t( rTransform ) ); }

	EA_FORCE_INLINE size_t size( void ) const { return m_storage.size(); }

	EA_FORCE_INLINE size_t capacity( void ) const { return m_storage.capacity(); }

	EA_FORCE_INLINE bool empty( void ) const { return m_storage.empty(); }

	T3DReference operator[]( const size_t iIndex ) {
		return T3DReference( &m_storage[iIndex] );
	}

	Transform3D operator[]( const size_t iIndex ) const {
		return m_storage[iIndex].get();
	}

	EA_FORCE_INLINE const t3dPackedTransform_t *data( void ) const { return m_storage.data(); }

	EA_FORCE_INLINE t3dPackedTransform_t *data( void ) { return m_storage.data(); }

	EA_FORCE_INLINE Vector3 getOrigin( const size_t iIndex ) const { const t3dPackedTransform_t &packed = m_storage[iIndex]; return Vector3( packed.origin[0], packed.origin[1], packed.origin[2] ); }

	EA_FORCE_INLINE Vector3 getUpAxis( const size_t iIndex ) const { const t3dPackedTransform_t &packed = m_storage[iIndex]; return Vector3( t3dHalfDecode( packed.basis[1] ), t3dHalfDecode( packed.basis[4] ), t3dHalfDecode( packed.basis[7] ) ); }

	EA_FORCE_INLINE void setOriginY( const size_t iIndex, const real_t fY ) { m_storage[iIndex].origin[1] = ( float ) ( fY ); }

	EA_FORCE_INLINE void rescaleOriginXz( const size_t iIndex, const real_t fOldSpacing, const real_t fNewSpacing ) { t3dPackedTransform_t &packed = m_storage[iIndex]; packed.origin[0] = ( float ) ( ( real_t ) ( packed.origin[0] ) / fOldSpacing * fNewSpacing ); packed.origin[2] = ( float ) ( ( real_t ) ( packed.origin[2] ) / fOldSpacing * fNewSpacing ); }

private:
	T3DSlabArray<t3dPackedTransform_t> m_storage;
};

#endif // TERRAIN3D_PACKED_TRANSFORM_H
