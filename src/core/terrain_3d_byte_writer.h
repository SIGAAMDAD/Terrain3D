// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_BYTE_WRITER_H
#define TERRAIN3D_BYTE_WRITER_H

#include "core/terrain_3d_compiler.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

struct T3DByteWriter {
	uint8_t *pData = nullptr;
	size_t nCapacity = 0u;
	size_t nOffset = 0u;
	bool bValid = true;

	T3DByteWriter() = default;
	EA_FORCE_INLINE T3DByteWriter( uint8_t *pBytes, const size_t nBytes ) : pData( pBytes ), nCapacity( nBytes ) {}

	EA_FORCE_INLINE bool canWrite( const size_t nBytes ) const { return bValid && nOffset <= nCapacity && nBytes <= nCapacity - nOffset; }
	EA_FORCE_INLINE size_t offset() const { return nOffset; }
	EA_FORCE_INLINE size_t remaining() const { return nOffset <= nCapacity ? nCapacity - nOffset : 0u; }
	EA_FORCE_INLINE bool complete() const { return bValid && nOffset == nCapacity; }

	EA_FORCE_INLINE bool writeU8( const uint8_t nValue ) { if ( !canWrite( 1u ) ) return fail(); pData[nOffset++] = nValue; return true; }
	EA_FORCE_INLINE bool writeU16( const uint16_t nValue ) { if ( !canWrite( 2u ) ) return fail(); pData[nOffset] = ( uint8_t ) ( nValue & 0xffu ); pData[nOffset + 1u] = ( uint8_t ) ( nValue >> 8u ); nOffset += 2u; return true; }
	EA_FORCE_INLINE bool writeU32( const uint32_t nValue ) { if ( !canWrite( 4u ) ) return fail(); pData[nOffset] = ( uint8_t ) ( nValue & 0xffu ); pData[nOffset + 1u] = ( uint8_t ) ( nValue >> 8u ); pData[nOffset + 2u] = ( uint8_t ) ( nValue >> 16u ); pData[nOffset + 3u] = ( uint8_t ) ( nValue >> 24u ); nOffset += 4u; return true; }
	EA_FORCE_INLINE bool writeI32( const int32_t nValue ) { return writeU32( ( uint32_t ) nValue ); }
	EA_FORCE_INLINE bool writeU64( const uint64_t nValue ) { return writeU32( ( uint32_t ) nValue ) && writeU32( ( uint32_t ) ( nValue >> 32u ) ); }
	EA_FORCE_INLINE bool writeI64( const int64_t nValue ) { return writeU64( ( uint64_t ) nValue ); }
	EA_FORCE_INLINE bool writeF32( const float fValue ) { uint32_t bits = 0u; std::memcpy( &bits, &fValue, sizeof( bits ) ); return writeU32( bits ); }
	EA_FORCE_INLINE bool writeBytes( const void *pSource, const size_t nBytes ) { if ( nBytes == 0u ) return true; if ( pSource == nullptr || !canWrite( nBytes ) ) return fail(); std::memcpy( pData + nOffset, pSource, nBytes ); nOffset += nBytes; return true; }
	EA_FORCE_INLINE bool skipZeroed( const size_t nBytes ) { if ( !canWrite( nBytes ) ) return fail(); std::memset( pData + nOffset, 0, nBytes ); nOffset += nBytes; return true; }

private:
	EA_FORCE_INLINE bool fail() { bValid = false; return false; }
};

#endif // TERRAIN3D_BYTE_WRITER_H
