// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN_3D_PERFORMANCE_PROFILE_H
#define TERRAIN_3D_PERFORMANCE_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Terrain3DConstants.h"

namespace terrain_3d_profile {

inline constexpr size_t settingCount( void ) {
	return XBOX360_EXPERIMENTAL_SETTING_COUNT;
}

inline void signatureAppendByte( uint64_t &nHash, const uint8_t nByte ) {
	nHash ^= ( uint64_t ) ( nByte );
	nHash *= TERRAIN3D_PROFILE_FNV_PRIME;
}

inline void signatureAppendUint32( uint64_t &nHash, const uint32_t nValue ) {
	for ( uint32_t shift = 0u; shift < 32u; shift += 8u ) {
		signatureAppendByte( nHash, ( uint8_t ) ( ( nValue >> shift ) & 0xffu ) );
	}
}

inline void signatureAppendUint64( uint64_t &nHash, const uint64_t nValue ) {
	for ( uint32_t shift = 0u; shift < 64u; shift += 8u ) {
		signatureAppendByte( nHash, ( uint8_t ) ( ( nValue >> shift ) & 0xffu ) );
	}
}

inline void signatureAppendString( uint64_t &nHash, const char *pszString ) {
	for ( const char *pszCharacter = pszString; *pszCharacter != '\0'; ++pszCharacter ) {
		signatureAppendByte( nHash, ( uint8_t ) ( *pszCharacter ) );
	}
	signatureAppendByte( nHash, 0u );
}

inline uint64_t computeSignature( void ) {
	uint64_t nHash = TERRAIN3D_PROFILE_FNV_OFFSET;
	signatureAppendUint32( nHash, TERRAIN3D_PROFILE_VERSION );
	signatureAppendString( nHash, TERRAIN3D_PROFILE_KEY );
	for ( const t3dsetting_t &setting : XBOX360_EXPERIMENTAL_SETTINGS ) {
		signatureAppendByte( nHash, ( uint8_t ) ( setting.owner ) );
		signatureAppendByte( nHash, ( uint8_t ) ( setting.kind ) );
		signatureAppendString( nHash, setting.pszProperty );
		switch ( setting.kind ) {
			case VALUE_INT:
				signatureAppendUint64( nHash, ( uint64_t ) ( setting.intValue ) );
				break;
			case VALUE_FLOAT: {
				uint64_t bits = 0u;
				static_assert( sizeof( bits ) == sizeof( setting.fFloatValue ), "profile float signature size mismatch" );
				std::memcpy( &bits, &setting.fFloatValue, sizeof( bits ) );
				signatureAppendUint64( nHash, bits );
				break;
			}
			case VALUE_BOOL:
				signatureAppendByte( nHash, setting.bBoolValue ? 1u : 0u );
				break;
		}
	}
	return nHash;
}

inline uint64_t signature( void ) {
	static const uint64_t s_nCachedSignature = computeSignature();
	return s_nCachedSignature;
}

} // namespace terrain_3d_profile

#endif // TERRAIN_3D_PERFORMANCE_PROFILE_H
