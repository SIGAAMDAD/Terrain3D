// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_BINARY_IO_H
#define TERRAIN3D_BINARY_IO_H

#include "core/utility/terrain_3d_checked_math.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

// Explicit little-endian access. The caller validates fixed record spans first;
// use the checked cursor below for sequential or variable-length records.
template< typename T >
EA_FORCE_INLINE T t3dReadLittleUnsigned(
	const uint8_t *pBytes ) {
	static_assert( ( T ) -1 > ( T ) 0 && ( T ) 2 != ( T ) true, "Unsigned integer required" );
	T value = 0;
	for ( size_t i = 0; i < sizeof( T ); ++i ) {
		value |= ( T ) ( ( T ) pBytes[i] << ( i * 8u ) );
	}
	return value;
}

template< typename T >
EA_FORCE_INLINE void t3dWriteLittleUnsigned(
	uint8_t *pBytes,
	const T	 nValue ) {
	static_assert( ( T ) -1 > ( T ) 0 && ( T ) 2 != ( T ) true, "Unsigned integer required" );
	for ( size_t i = 0; i < sizeof( T ); ++i ) {
		pBytes[i] = ( uint8_t ) ( nValue >> ( i * 8u ) );
	}
}

EA_FORCE_INLINE float t3dReadLittleF32(
	const uint8_t *pBytes ) {
	const uint32_t bits = t3dReadLittleUnsigned< uint32_t >( pBytes );
	float		   value;
	memcpy( &value, &bits, sizeof( value ) );
	return value;
}

EA_FORCE_INLINE void t3dWriteLittleF32(
	uint8_t	   *pBytes,
	const float fValue ) {
	uint32_t bits;
	memcpy( &bits, &fValue, sizeof( bits ) );
	t3dWriteLittleUnsigned( pBytes, bits );
}

// Failed reads/writes latch invalid state and never advance the cursor. Formats
// retain their own schema, limits, allocation and transactional publication.
typedef struct t3dByteReader_s
{
	const uint8_t *m_pData = NULL;
	size_t		   m_nCapacity = 0u;
	size_t		   m_iOffset = 0u;
	bool		   m_bValid = true;

	t3dByteReader_s(
		const uint8_t *pBytes,
		const size_t   nBytes ) :
		m_pData( pBytes ),
		m_nCapacity( nBytes ),
		m_bValid( pBytes != NULL || nBytes == 0u ) {
	}
	bool canRead(
		const size_t nBytes ) const {
		return m_bValid && t3dByteRangeFits( m_iOffset, nBytes, m_nCapacity );
	}
	const uint8_t *take(
		const size_t nBytes ) {
		if ( !canRead( nBytes ) ) {
			m_bValid = false;
			return NULL;
		}
		const uint8_t *pBytes = m_pData ? m_pData + m_iOffset : NULL;
		m_iOffset += nBytes;
		return pBytes;
	}
	bool complete() const {
		return m_bValid && m_iOffset == m_nCapacity;
	}
	template< typename T > T readUnsigned() {
		const uint8_t *pBytes = take( sizeof( T ) );
		return pBytes ? t3dReadLittleUnsigned< T >( pBytes ) : 0;
	}
	uint8_t readU8() {
		return readUnsigned< uint8_t >();
	}
	uint16_t readU16() {
		return readUnsigned< uint16_t >();
	}
	uint32_t readU32() {
		return readUnsigned< uint32_t >();
	}
	uint64_t readU64() {
		return readUnsigned< uint64_t >();
	}
	int32_t readI32() {
		return ( int32_t ) readU32();
	}
	int64_t readI64() {
		return ( int64_t ) readU64();
	}
	float readF32() {
		const uint8_t *pBytes = take( sizeof( float ) );
		return pBytes ? t3dReadLittleF32( pBytes ) : 0.0f;
	}
} t3dByteReader_t;

typedef struct t3dByteWriter_s
{
	uint8_t *m_pData = NULL;
	size_t	 m_nCapacity = 0u;
	size_t	 m_iOffset = 0u;
	bool	 m_bValid = true;

	t3dByteWriter_s() = default;
	t3dByteWriter_s(
		uint8_t		*pBytes,
		const size_t nBytes ) :
		m_pData( pBytes ),
		m_nCapacity( nBytes ),
		m_bValid( pBytes != NULL || nBytes == 0u ) {
	}
	bool canWrite(
		const size_t nBytes ) const {
		return m_bValid && t3dByteRangeFits( m_iOffset, nBytes, m_nCapacity );
	}
	size_t offset() const {
		return m_iOffset;
	}
	size_t remaining() const {
		return m_iOffset <= m_nCapacity ? m_nCapacity - m_iOffset : 0u;
	}
	bool complete() const {
		return m_bValid && m_iOffset == m_nCapacity;
	}
	template< typename T >
	bool writeUnsigned(
		const T nValue ) {
		if ( !canWrite( sizeof( T ) ) ) {
			return fail();
		}
		t3dWriteLittleUnsigned( m_pData + m_iOffset, nValue );
		m_iOffset += sizeof( T );
		return true;
	}
	bool writeU8(
		const uint8_t nValue ) {
		return writeUnsigned( nValue );
	}
	bool writeU16(
		const uint16_t nValue ) {
		return writeUnsigned( nValue );
	}
	bool writeU32(
		const uint32_t nValue ) {
		return writeUnsigned( nValue );
	}
	bool writeU64(
		const uint64_t nValue ) {
		return writeUnsigned( nValue );
	}
	bool writeI32(
		const int32_t nValue ) {
		return writeU32( ( uint32_t ) nValue );
	}
	bool writeI64(
		const int64_t nValue ) {
		return writeU64( ( uint64_t ) nValue );
	}
	bool writeF32(
		const float fValue ) {
		uint32_t bits;
		memcpy( &bits, &fValue, sizeof( bits ) );
		return writeU32( bits );
	}
	bool writeBytes(
		const void	*pSource,
		const size_t nBytes ) {
		if ( !canWrite( nBytes ) || ( !pSource && nBytes != 0u ) ) {
			return fail();
		}
		if ( nBytes ) {
			memcpy( m_pData + m_iOffset, pSource, nBytes );
		}
		m_iOffset += nBytes;
		return true;
	}
	bool skipZeroed(
		const size_t nBytes ) {
		if ( !canWrite( nBytes ) ) {
			return fail();
		}
		if ( nBytes ) {
			memset( m_pData + m_iOffset, 0, nBytes );
		}
		m_iOffset += nBytes;
		return true;
	}

private:
	bool fail() {
		m_bValid = false;
		return false;
	}
} t3dByteWriter_t;

// Caller supplies at least sizeof(T) readable bytes. This is a native-order
// load, independent of alignment; little-endian codecs must use the explicit little-endian operations above.
template< typename T >
EA_FORCE_INLINE T t3dReadNativeUnaligned(
	const void *pBytes ) {
	static_assert( std::is_trivially_copyable< T >::value, "Byte loads require a trivially copyable value" );
	T value;
	memcpy( &value, pBytes, sizeof( value ) );
	return value;
}

#endif // TERRAIN3D_BINARY_IO_H
