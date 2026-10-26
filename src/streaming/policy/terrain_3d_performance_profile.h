// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_PERFORMANCE_PROFILE_H
#define TERRAIN3D_PERFORMANCE_PROFILE_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

EA_FORCE_INLINE size_t t3dPerformanceProfileSettingCount(
	void ) {
	return g_nXbox360ExperimentalSettingCount;
}

EA_FORCE_INLINE void t3dPerformanceProfileSignatureAppendByte(
	uint64_t	 &nHash,
	const uint8_t nByte ) {
	nHash ^= ( uint64_t ) ( nByte );
	nHash *= T3D_PROFILE_FNV_PRIME;
}

inline void t3dPerformanceProfileSignatureAppendUint32(
	uint64_t	  &nHash,
	const uint32_t nValue ) {
	for ( uint32_t shift = 0u; shift < 32u; shift += 8u ) {
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t ) ( ( nValue >> shift ) & 0xffu ) );
	}
}

inline void t3dPerformanceProfileSignatureAppendUint64(
	uint64_t	  &nHash,
	const uint64_t nValue ) {
	for ( uint32_t shift = 0u; shift < 64u; shift += 8u ) {
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t ) ( ( nValue >> shift ) & 0xffu ) );
	}
}

inline void t3dPerformanceProfileSignatureAppendString(
	uint64_t   &nHash,
	const char *pszString ) {
	for ( const char *pszCharacter = pszString; *pszCharacter != '\0'; ++pszCharacter ) {
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t ) ( *pszCharacter ) );
	}
	t3dPerformanceProfileSignatureAppendByte( nHash, 0u );
}

inline uint64_t t3dPerformanceProfileComputeSignature(
	void ) {
	uint64_t nHash = T3D_PROFILE_FNV_OFFSET;
	t3dPerformanceProfileSignatureAppendUint32( nHash, T3D_PROFILE_VERSION );
	t3dPerformanceProfileSignatureAppendString( nHash, s_pszProfileKey );
	for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
		const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t ) ( setting.m_owner ) );
		t3dPerformanceProfileSignatureAppendByte( nHash, ( uint8_t ) ( setting.m_kind ) );
		t3dPerformanceProfileSignatureAppendString( nHash, setting.m_pszProperty );
		switch ( setting.m_kind ) {
		case T3D_PROFILE_VALUE_INT:
			t3dPerformanceProfileSignatureAppendUint64( nHash, ( uint64_t ) ( setting.m_nIntValue ) );
			break;
		case T3D_PROFILE_VALUE_FLOAT: {
			uint64_t bits = 0u;
			static_assert( sizeof( bits ) == sizeof( setting.m_fFloatValue ),
						   "profile float t3dPerformanceProfileSignature size mismatch" );
			memcpy( &bits, &setting.m_fFloatValue, sizeof( bits ) );
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

EA_FORCE_INLINE uint64_t t3dPerformanceProfileSignature(
	void ) {
	return T3D_PERFORMANCE_PROFILE_SIGNATURE;
}

#endif // TERRAIN3D_PERFORMANCE_PROFILE_H
