// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN_3D_PERFORMANCE_PROFILE_H
#define TERRAIN_3D_PERFORMANCE_PROFILE_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

EA_FORCE_INLINE constexpr size_t t3dPerformanceProfileSettingCount( void ) {
	return g_nXbox360ExperimentalSettingCount;
}

EA_FORCE_INLINE void t3dPerformanceProfileSignatureAppendByte( uint64_t &nHash, const uint8_t nByte ) {
	nHash ^= ( uint64_t )( nByte );
	nHash *= s_nProfileFnvPrime;
}

inline void t3dPerformanceProfileSignatureAppendUint32( uint64_t &nHash, const uint32_t nValue ) {
	for ( uint32_t shift = 0u; shift < 32u; shift += 8u ) {
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t )( ( nValue >> shift ) & 0xffu ) );
	}
}

inline void t3dPerformanceProfileSignatureAppendUint64( uint64_t &nHash, const uint64_t nValue ) {
	for ( uint32_t shift = 0u; shift < 64u; shift += 8u ) {
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t )( ( nValue >> shift ) & 0xffu ) );
	}
}

inline void t3dPerformanceProfileSignatureAppendString( uint64_t &nHash, const char *pszString ) {
	for ( const char *pszCharacter = pszString; *pszCharacter != '\0'; ++pszCharacter ) {
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t )( *pszCharacter ) );
	}
	t3dPerformanceProfileSignatureAppendByte( nHash, 0u );
}

inline uint64_t t3dPerformanceProfileComputeSignature( void ) {
	uint64_t nHash = s_nProfileFnvOffset;
	t3dPerformanceProfileSignatureAppendUint32( nHash, s_nProfileVersion );
	t3dPerformanceProfileSignatureAppendString( nHash, s_pszProfileKey );
	for ( const t3dPerformanceProfileSetting_t &setting : g_szXbox360ExperimentalSettings ) {
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t )( setting.m_owner ) );
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t )( setting.m_kind ) );
		t3dPerformanceProfileSignatureAppendString( nHash, setting.m_pszProperty );
		switch ( setting.m_kind ) {
			case T3D_PROFILE_VALUE_INT:
				t3dPerformanceProfileSignatureAppendUint64( nHash, ( uint64_t )( setting.m_nIntValue ) );
				break;
			case T3D_PROFILE_VALUE_FLOAT: {
				uint64_t bits = 0u;
				static_assert( sizeof( bits ) == sizeof( setting.m_fFloatValue ),
						"profile float t3dPerformanceProfileSignature size mismatch" );
				std::memcpy( &bits, &setting.m_fFloatValue, sizeof( bits ) );
				t3dPerformanceProfileSignatureAppendUint64( nHash, bits );
				break;
			}
			case T3D_PROFILE_VALUE_BOOL:
				t3dPerformanceProfileSignatureAppendByte( nHash, setting.m_bBoolValue ? 1u : 0u );
				break;
		}
	}
	return nHash;
}

EA_FORCE_INLINE uint64_t t3dPerformanceProfileSignature( void ) {
	static const uint64_t s_nCachedSignature = t3dPerformanceProfileComputeSignature();
	return s_nCachedSignature;
}

#endif // TERRAIN_3D_PERFORMANCE_PROFILE_H
