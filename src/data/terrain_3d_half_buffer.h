// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Compact IEEE-754 binary16 storage for immutable per-cell coefficients.
#pragma once

#ifndef TERRAIN3D_HALF_BUFFER_H
#define TERRAIN3D_HALF_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "memory/terrain_3d_linear_arena.h"

namespace terrain3d_half {

inline uint16_t encode( const float fValue ) {
	uint32_t bits = 0u;
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	const uint16_t sign = ( uint16_t ) ( ( bits >> 16u ) & 0x8000u );
	uint32_t mantissa = bits & 0x007fffffu;
	const uint32_t source_exponent = ( bits >> 23u ) & 0xffu;

	if ( source_exponent == 0xffu ) {
		if ( mantissa == 0u ) {
			return ( uint16_t ) ( sign | 0x7c00u );
		}
		return ( uint16_t ) ( sign | 0x7e00u | ( uint16_t ) ( mantissa >> 13u ) );
	}

	int32_t exponent = ( int32_t ) ( source_exponent ) - 127 + 15;
	if ( exponent >= 31 ) {
		return ( uint16_t ) ( sign | 0x7c00u );
	}
	if ( exponent <= 0 ) {
		if ( exponent < -10 ) {
			return sign;
		}
		mantissa |= 0x00800000u;
		const uint32_t shift = ( uint32_t ) ( 14 - exponent );
		const uint32_t rounding = ( ( uint32_t ) ( 1 ) << ( shift - 1u ) ) - 1u + ( ( mantissa >> shift ) & 1u );
		return ( uint16_t ) ( sign | ( uint16_t ) ( ( mantissa + rounding ) >> shift ) );
	}

	mantissa += 0x00000fffu + ( ( mantissa >> 13u ) & 1u );
	if ( ( mantissa & 0x00800000u ) != 0u ) {
		mantissa = 0u;
		++exponent;
		if ( exponent >= 31 ) {
			return ( uint16_t ) ( sign | 0x7c00u );
		}
	}
	return ( uint16_t ) ( sign | ( uint16_t ) ( ( uint32_t ) ( exponent ) << 10u ) | ( uint16_t ) ( mantissa >> 13u ) );
}

inline float decode( const uint16_t nValue ) {
	const uint32_t sign = ( uint32_t ) ( nValue & 0x8000u ) << 16u;
	uint32_t exponent = ( nValue >> 10u ) & 0x1fu;
	uint32_t mantissa = nValue & 0x03ffu;
	uint32_t bits = 0u;

	if ( exponent == 0u ) {
		if ( mantissa == 0u ) {
			bits = sign;
		} else {
			exponent = 1u;
			while ( ( mantissa & 0x0400u ) == 0u ) {
				mantissa <<= 1u;
				--exponent;
			}
			mantissa &= 0x03ffu;
			const uint32_t float_exponent = exponent + ( uint32_t ) ( 127 - 15 );
			bits = sign | ( float_exponent << 23u ) | ( mantissa << 13u );
		}
	} else if ( exponent == 0x1fu ) {
		bits = sign | 0x7f800000u | ( mantissa << 13u );
	} else {
		const uint32_t float_exponent = exponent + ( uint32_t ) ( 127 - 15 );
		bits = sign | ( float_exponent << 23u ) | ( mantissa << 13u );
	}

	float fResult = 0.0f;
	std::memcpy( &fResult, &bits, sizeof( fResult ) );
	return fResult;
}

} // namespace terrain3d_half

class T3DHalfBuffer {
public:
	using value_type = uint16_t;

	class T3DReference {
	public:
		explicit T3DReference( uint16_t *const pStorage ) : m_pStorage( pStorage ) {}

		T3DReference &operator=( const float fValue ) {
			*m_pStorage = terrain3d_half::encode( fValue );
			return *this;
		}

		T3DReference &operator=( const T3DReference &rOther ) {
			return operator=( ( float ) ( rOther ) );
		}

		operator float( void ) const {
			return terrain3d_half::decode( *m_pStorage );
		}

	private:
		uint16_t *m_pStorage = nullptr;
	};

	bool assign( T3DLinearArena &rArena, const size_t nCount, const float fValue ) {
		clear();
		if ( nCount == 0u ) {
			return true;
		}
		m_pData = rArena.allocateArray<uint16_t>( nCount );
		if ( !m_pData ) {
			return false;
		}
		m_nSize = nCount;
		const uint16_t encoded = terrain3d_half::encode( fValue );
		std::fill_n( m_pData, m_nSize, encoded );
		return true;
	}

	void clear( void ) {
		m_pData = nullptr;
		m_nSize = 0u;
	}

	T3DReference operator[]( const size_t iIndex ) {
		return T3DReference( m_pData + iIndex );
	}

	float operator[]( const size_t iIndex ) const {
		return terrain3d_half::decode( m_pData[iIndex] );
	}

	size_t size( void ) const {
		return m_nSize;
	}

	bool empty( void ) const {
		return m_nSize == 0u;
	}

	uint16_t *data( void ) {
		return m_pData;
	}

	const uint16_t *data( void ) const {
		return m_pData;
	}

private:
	uint16_t *m_pData = nullptr;
	size_t m_nSize = 0u;
};

#endif // TERRAIN3D_HALF_BUFFER_H
