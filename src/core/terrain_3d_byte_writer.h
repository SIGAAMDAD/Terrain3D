// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_BYTE_WRITER_H
#define TERRAIN3D_BYTE_WRITER_H

#include "core/terrain_3d_compiler.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

typedef struct t3dByteWriter_s {
	uint8_t *m_pData = nullptr;
	size_t m_nCapacity = 0u;
	size_t m_iOffset = 0u;
	bool m_bValid = true;

	t3dByteWriter_s() = default;
	EA_FORCE_INLINE t3dByteWriter_s( uint8_t *pBytes, const size_t nBytes ) :
			m_pData( pBytes ), m_nCapacity( nBytes ) {}

	EA_FORCE_INLINE bool canWrite( const size_t nBytes ) const {
		return m_bValid && m_iOffset <= m_nCapacity && nBytes <= m_nCapacity - m_iOffset;
	}
	EA_FORCE_INLINE size_t offset() const {
		return m_iOffset;
	}
	EA_FORCE_INLINE size_t remaining() const {
		return m_iOffset <= m_nCapacity ? m_nCapacity - m_iOffset : 0u;
	}
	EA_FORCE_INLINE bool complete() const {
		return m_bValid && m_iOffset == m_nCapacity;
	}

	EA_FORCE_INLINE bool writeU8( const uint8_t nValue ) {
		if ( !canWrite( 1u ) ) {
			return fail();
		}
		m_pData[m_iOffset++] = nValue;
		return true;
	}
	EA_FORCE_INLINE bool writeU16( const uint16_t nValue ) {
		if ( !canWrite( 2u ) ) {
			return fail();
		}
		m_pData[m_iOffset] = ( uint8_t )( nValue & 0xffu );
		m_pData[m_iOffset + 1u] = ( uint8_t )( nValue >> 8u );
		m_iOffset += 2u;
		return true;
	}
	EA_FORCE_INLINE bool writeU32( const uint32_t nValue ) {
		if ( !canWrite( 4u ) ) {
			return fail();
		}
		m_pData[m_iOffset] = ( uint8_t )( nValue & 0xffu );
		m_pData[m_iOffset + 1u] = ( uint8_t )( nValue >> 8u );
		m_pData[m_iOffset + 2u] = ( uint8_t )( nValue >> 16u );
		m_pData[m_iOffset + 3u] = ( uint8_t )( nValue >> 24u );
		m_iOffset += 4u;
		return true;
	}
	EA_FORCE_INLINE bool writeI32( const int32_t nValue ) {
		return writeU32( ( uint32_t )nValue );
	}
	EA_FORCE_INLINE bool writeU64( const uint64_t nValue ) {
		return writeU32( ( uint32_t )nValue ) && writeU32( ( uint32_t )( nValue >> 32u ) );
	}
	EA_FORCE_INLINE bool writeI64( const int64_t nValue ) {
		return writeU64( ( uint64_t )nValue );
	}
	EA_FORCE_INLINE bool writeF32( const float fValue ) {
		uint32_t bits = 0u;
		std::memcpy( &bits, &fValue, sizeof( bits ) );
		return writeU32( bits );
	}
	EA_FORCE_INLINE bool writeBytes( const void *pSource, const size_t nBytes ) {
		if ( nBytes == 0u ) {
			return true;
		}
		if ( pSource == nullptr || !canWrite( nBytes ) ) {
			return fail();
		}
		std::memcpy( m_pData + m_iOffset, pSource, nBytes );
		m_iOffset += nBytes;
		return true;
	}
	EA_FORCE_INLINE bool skipZeroed( const size_t nBytes ) {
		if ( !canWrite( nBytes ) ) {
			return fail();
		}
		std::memset( m_pData + m_iOffset, 0, nBytes );
		m_iOffset += nBytes;
		return true;
	}

private:
	EA_FORCE_INLINE bool fail() {
		m_bValid = false;
		return false;
	}
} t3dByteWriter_t;

#endif // TERRAIN3D_BYTE_WRITER_H
