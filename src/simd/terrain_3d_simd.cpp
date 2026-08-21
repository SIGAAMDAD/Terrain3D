// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "simd/terrain_3d_simd.h"

#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_half_buffer.h"
#include "data/terrain_3d_packed_transform.h"
#include "simd/terrain_3d_simd_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

#if defined( _MSC_VER ) && ( defined( _M_IX86 ) || defined( _M_X64 ) )
#include <intrin.h>
#elif ( defined( __i386__ ) || defined( __x86_64__ ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
#include <cpuid.h>
#endif

#if defined( _WIN32 ) && ( defined( _M_ARM ) || defined( _M_ARM64 ) )
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined( __linux__ ) && defined( __arm__ ) && !defined( __aarch64__ )
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

static t3dSimdCapabilities_t s_simdCapabilities;
static t3dSimdDispatch_t s_szDispatchTables[5];
static std::once_flag s_simdInitializeOnce;
static std::atomic<const t3dSimdDispatch_t *> s_pActiveDispatch{ nullptr };

static uint64_t t3dSimdXgetbv0( void ) {
#if defined( _MSC_VER ) && ( defined( _M_IX86 ) || defined( _M_X64 ) )
	return _xgetbv( 0 );
#elif ( defined( __i386__ ) || defined( __x86_64__ ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
	uint32_t eax = 0u;
	uint32_t edx = 0u;
	__asm__ volatile( "xgetbv" : "=a"( eax ), "=d"( edx ) : "c"( 0 ) );
	return ( ( uint64_t )( edx ) << 32u ) | ( uint64_t )( eax );
#else
	return 0u;
#endif
}

static void t3dSimdDetectX86( t3dSimdCapabilities_t &rCaps ) {
#if defined( _MSC_VER ) && ( defined( _M_IX86 ) || defined( _M_X64 ) )
	int registers[4] = {};
	__cpuidex( registers, 0, 0 );
	const int max_leaf = registers[0];
	if ( max_leaf < 1 ) {
		return;
	}
	__cpuidex( registers, 1, 0 );
	const uint32_t ecx = ( uint32_t )( registers[2] );
	const uint32_t edx = ( uint32_t )( registers[3] );
	rCaps.m_bSse2 = ( edx & ( 1u << 26u ) ) != 0u;
	const bool bOsxsave = ( ecx & ( 1u << 27u ) ) != 0u;
	const bool bCpuAvx = ( ecx & ( 1u << 28u ) ) != 0u;
	rCaps.m_bF16c = ( ecx & ( 1u << 29u ) ) != 0u;
	const bool bOsAvx = bOsxsave && bCpuAvx && ( t3dSimdXgetbv0() & 0x6u ) == 0x6u;
	rCaps.m_bAvx = bOsAvx;
	if ( max_leaf >= 7 ) {
		__cpuidex( registers, 7, 0 );
		rCaps.m_bAvx2 = bOsAvx && ( ( uint32_t )( registers[1] ) & ( 1u << 5u ) ) != 0u;
	}
#elif ( defined( __i386__ ) || defined( __x86_64__ ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
	uint32_t eax = 0u;
	uint32_t ebx = 0u;
	uint32_t ecx = 0u;
	uint32_t edx = 0u;
	if ( !__get_cpuid( 1u, &eax, &ebx, &ecx, &edx ) ) {
		return;
	}
	rCaps.m_bSse2 = ( edx & bit_SSE2 ) != 0u;
	const bool bOsxsave = ( ecx & bit_OSXSAVE ) != 0u;
	const bool bCpuAvx = ( ecx & bit_AVX ) != 0u;
	rCaps.m_bF16c = ( ecx & bit_F16C ) != 0u;
	const bool bOsAvx = bOsxsave && bCpuAvx && ( t3dSimdXgetbv0() & 0x6u ) == 0x6u;
	rCaps.m_bAvx = bOsAvx;
	if ( __get_cpuid_max( 0u, nullptr ) >= 7u ) {
		__cpuid_count( 7u, 0u, eax, ebx, ecx, edx );
		rCaps.m_bAvx2 = bOsAvx && ( ebx & bit_AVX2 ) != 0u;
	}
#else
	( void )rCaps;
#endif
}

static void t3dSimdDetectArm( t3dSimdCapabilities_t &rCaps ) {
#if defined( __aarch64__ ) || defined( _M_ARM64 )
	rCaps.m_bNeon = true;
#elif defined( __ARM_NEON ) || defined( __ARM_NEON__ )
#if defined( _WIN32 )
	rCaps.m_bNeon = IsProcessorFeaturePresent( PF_ARM_NEON_INSTRUCTIONS_AVAILABLE ) != 0;
#elif defined( __linux__ )
	rCaps.m_bNeon = ( getauxval( AT_HWCAP ) & HWCAP_NEON ) != 0u;
#else
	rCaps.m_bNeon = true;
#endif
#else
	( void )rCaps;
#endif
}

static t3dSimdDispatch_t t3dSimdMakeDispatch( const t3dSimdBackend_t backend ) {
	t3dSimdDispatch_t result;
	result.m_backend = backend;
	switch ( backend ) {
		case T3D_SIMD_AVX2_F16C:
			result.m_pPfnHalfToFloat = t3dSimdAvx2HalfToFloat;
			result.m_pPfnPackedTransformsToMultimesh = t3dSimdAvx2PackedTransforms;
			result.m_pPfnDerivedNormalsRg8 = t3dSimdAvx2DerivedNormals;
			result.m_pPfnUpdateCumulativeErosion = t3dSimdAvx2UpdateCumulativeErosion;
			break;
		case T3D_SIMD_AVX_F16C:
			result.m_pPfnHalfToFloat = t3dSimdAvxHalfToFloat;
			result.m_pPfnPackedTransformsToMultimesh = t3dSimdAvxPackedTransforms;
			result.m_pPfnDerivedNormalsRg8 = t3dSimdAvxDerivedNormals;
			result.m_pPfnUpdateCumulativeErosion = t3dSimdAvxUpdateCumulativeErosion;
			break;
		case T3D_SIMD_SSE2:
			result.m_pPfnHalfToFloat = t3dSimdSse2HalfToFloat;
			result.m_pPfnPackedTransformsToMultimesh = t3dSimdSse2PackedTransforms;
			result.m_pPfnDerivedNormalsRg8 = t3dSimdSse2DerivedNormals;
			result.m_pPfnUpdateCumulativeErosion = t3dSimdSse2UpdateCumulativeErosion;
			break;
		case T3D_SIMD_NEON:
			result.m_pPfnHalfToFloat = t3dSimdNeonHalfToFloat;
			result.m_pPfnPackedTransformsToMultimesh = t3dSimdNeonPackedTransforms;
			result.m_pPfnDerivedNormalsRg8 = t3dSimdNeonDerivedNormals;
			result.m_pPfnUpdateCumulativeErosion = t3dSimdNeonUpdateCumulativeErosion;
			break;
		case T3D_SIMD_SCALAR:
		default:
			result.m_backend = T3D_SIMD_SCALAR;
			result.m_pPfnHalfToFloat = t3dSimdScalarHalfToFloat;
			result.m_pPfnPackedTransformsToMultimesh = t3dSimdScalarPackedTransforms;
			result.m_pPfnDerivedNormalsRg8 = t3dSimdScalarDerivedNormals;
			result.m_pPfnUpdateCumulativeErosion = t3dSimdScalarUpdateCumulativeErosion;
			break;
	}
	return result;
}

static bool t3dSimdBackendSupported( const t3dSimdBackend_t backend ) {
	switch ( backend ) {
		case T3D_SIMD_AVX2_F16C:
			return s_simdCapabilities.m_bAvx2 && s_simdCapabilities.m_bF16c && t3dSimdAvx2BackendAvailableAtBuild();
		case T3D_SIMD_AVX_F16C:
			return s_simdCapabilities.m_bAvx && s_simdCapabilities.m_bF16c && t3dSimdAvxBackendAvailableAtBuild();
		case T3D_SIMD_SSE2:
			return s_simdCapabilities.m_bSse2 && t3dSimdSse2BackendAvailableAtBuild();
		case T3D_SIMD_NEON:
			return s_simdCapabilities.m_bNeon && t3dSimdNeonBackendAvailableAtBuild();
		case T3D_SIMD_SCALAR:
		default:
			return true;
	}
}

static bool t3dSimdParseEnvironmentOverride( t3dSimdBackend_t &rBackend ) {
	const char *value = std::getenv( "TERRAIN3D_SIMD" );
	if ( !value || value[0] == '\0' ) {
		return false;
	}
	if ( std::strcmp( value, "scalar" ) == 0 ) {
		rBackend = T3D_SIMD_SCALAR;
		return true;
	}
	if ( std::strcmp( value, "sse2" ) == 0 || std::strcmp( value, "sse" ) == 0 ) {
		rBackend = T3D_SIMD_SSE2;
		return true;
	}
	if ( std::strcmp( value, "avx" ) == 0 || std::strcmp( value, "avx_f16c" ) == 0 ) {
		rBackend = T3D_SIMD_AVX_F16C;
		return true;
	}
	if ( std::strcmp( value, "avx2" ) == 0 || std::strcmp( value, "avx2_f16c" ) == 0 ) {
		rBackend = T3D_SIMD_AVX2_F16C;
		return true;
	}
	if ( std::strcmp( value, "neon" ) == 0 ) {
		rBackend = T3D_SIMD_NEON;
		return true;
	}
	return false;
}

EA_FORCE_INLINE static const t3dSimdDispatch_t *t3dSimdDispatchTable( const t3dSimdBackend_t backend ) {
	return &s_szDispatchTables[( size_t )( backend )];
}

static t3dSimdBackend_t t3dSimdBestSupportedBackend( void ) {
	if ( t3dSimdBackendSupported( T3D_SIMD_AVX2_F16C ) ) {
		return T3D_SIMD_AVX2_F16C;
	}
	if ( t3dSimdBackendSupported( T3D_SIMD_AVX_F16C ) ) {
		return T3D_SIMD_AVX_F16C;
	}
	if ( t3dSimdBackendSupported( T3D_SIMD_SSE2 ) ) {
		return T3D_SIMD_SSE2;
	}
	if ( t3dSimdBackendSupported( T3D_SIMD_NEON ) ) {
		return T3D_SIMD_NEON;
	}
	return T3D_SIMD_SCALAR;
}

static void t3dSimdInitialize( void ) {
	t3dSimdDetectX86( s_simdCapabilities );
	t3dSimdDetectArm( s_simdCapabilities );
	for ( size_t index = 0u; index < 5u; ++index ) {
		s_szDispatchTables[index] = t3dSimdMakeDispatch( ( t3dSimdBackend_t )( index ) );
	}
	t3dSimdBackend_t selected = t3dSimdBestSupportedBackend();
	t3dSimdBackend_t requested = T3D_SIMD_SCALAR;
	if ( t3dSimdParseEnvironmentOverride( requested ) && t3dSimdBackendSupported( requested ) ) {
		selected = requested;
	}
	s_pActiveDispatch.store( t3dSimdDispatchTable( selected ), std::memory_order_release );
}

EA_FORCE_INLINE static void t3dSimdEnsureInitialized( void ) {
	std::call_once( s_simdInitializeOnce, t3dSimdInitialize );
}

EA_FORCE_INLINE static uint8_t t3dSimdEncodeNormalComponent( const float fValue ) {
	const float fEncoded = std::max( 0.0f, std::min( s_fByteMaxFloat, ( fValue * 0.5f + 0.5f ) * s_fByteMaxFloat ) );
	return ( uint8_t )( std::floor( fEncoded + 0.5f ) );
}

void t3dSimdScalarHalfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
	for ( size_t i = 0; i < nCount; ++i ) {
		pDestination[i] = t3dHalfDecode( pSource[i] );
	}
}

void t3dSimdScalarPackedTransforms( const t3dPackedTransform_t *pTransforms,
		const uint32_t *pColors,
		const size_t nCount,
		const int nStride,
		float *pDestination ) {
	for ( size_t i = 0; i < nCount; ++i ) {
		const t3dPackedTransform_t &t = pTransforms[i];
		float *pOut = pDestination + i * ( size_t )( nStride );
		pOut[0] = t3dHalfDecode( t.m_szBasis[0] );
		pOut[1] = t3dHalfDecode( t.m_szBasis[1] );
		pOut[2] = t3dHalfDecode( t.m_szBasis[2] );
		pOut[3] = t.m_szOrigin[0];
		pOut[4] = t3dHalfDecode( t.m_szBasis[3] );
		pOut[5] = t3dHalfDecode( t.m_szBasis[4] );
		pOut[6] = t3dHalfDecode( t.m_szBasis[5] );
		pOut[7] = t.m_szOrigin[1];
		pOut[8] = t3dHalfDecode( t.m_szBasis[6] );
		pOut[9] = t3dHalfDecode( t.m_szBasis[7] );
		pOut[10] = t3dHalfDecode( t.m_szBasis[8] );
		pOut[11] = t.m_szOrigin[2];
		if ( nStride == 16 ) {
			const uint32_t color = pColors ? pColors[i] : 0xffffffffu;
			pOut[12] = ( float )( ( color >> 24u ) & 0xffu ) * ( s_fByteInvMaxFloat );
			pOut[13] = ( float )( ( color >> 16u ) & 0xffu ) * ( s_fByteInvMaxFloat );
			pOut[14] = ( float )( ( color >> 8u ) & 0xffu ) * ( s_fByteInvMaxFloat );
			pOut[15] = ( float )( color & 0xffu ) * ( s_fByteInvMaxFloat );
		}
	}
}

void t3dSimdScalarDerivedNormals( const float *pHalo,
		const int nWidth,
		const int nHeight,
		const int nHaloStride,
		const float fSpacing,
		uint8_t *pDestination ) {
	for ( int y = 0; y < nHeight; ++y ) {
		for ( int x = 0; x < nWidth; ++x ) {
			const size_t center = ( size_t )( y + 1 ) * ( size_t )( nHaloStride ) + ( size_t )( x + 1 );
			const float fDx = pHalo[center] - pHalo[center + 1u];
			const float fDz = pHalo[center] - pHalo[center + ( size_t )( nHaloStride )];
			const float fLength = std::sqrt( fDx * fDx + fSpacing * fSpacing + fDz * fDz );
			const float fInverse = fLength > 0.0f ? 1.0f / fLength : 0.0f;
			const size_t pOut = ( ( size_t )( y ) * ( size_t )( nWidth ) + ( size_t )( x ) ) * 2u;
			pDestination[pOut] = t3dSimdEncodeNormalComponent( fDx * fInverse );
			pDestination[pOut + 1u] = t3dSimdEncodeNormalComponent( fDz * fInverse );
		}
	}
}

void t3dSimdScalarUpdateCumulativeErosion( const uint8_t *pActive,
		const float *pInitialHeight,
		const float *pHeight,
		float *pCumulativeErosion,
		const size_t nCount ) {
	for ( size_t index = 0u; index < nCount; ++index ) {
		if ( pActive[index] != 0u ) {
			pCumulativeErosion[index] =
					std::max( pCumulativeErosion[index], std::max( 0.0f, pInitialHeight[index] - pHeight[index] ) );
		}
	}
}

const t3dSimdCapabilities_t &t3dSimdGetCapabilities( void ) {
	t3dSimdEnsureInitialized();
	return s_simdCapabilities;
}

const t3dSimdDispatch_t &t3dSimdGetDispatch( void ) {
	t3dSimdEnsureInitialized();
	return *s_pActiveDispatch.load( std::memory_order_acquire );
}

t3dSimdBackend_t t3dSimdGetActiveBackend( void ) {
	return t3dSimdGetDispatch().m_backend;
}

const char *t3dSimdGetBackendName( const t3dSimdBackend_t backend ) {
	switch ( backend ) {
		case T3D_SIMD_SSE2:
			return "SSE2";
		case T3D_SIMD_AVX_F16C:
			return "AVX/F16C";
		case T3D_SIMD_AVX2_F16C:
			return "AVX2/F16C";
		case T3D_SIMD_NEON:
			return "NEON";
		case T3D_SIMD_SCALAR:
		default:
			return "Scalar";
	}
}

void t3dSimdHalfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
	if ( !pSource || !pDestination || nCount == 0u ) {
		return;
	}
	t3dSimdGetDispatch().m_pPfnHalfToFloat( pSource, pDestination, nCount );
}

void t3dSimdPackedTransformsToMultimesh( const t3dPackedTransform_t *pTransforms,
		const uint32_t *pColors,
		const size_t nCount,
		const int nStride,
		float *pDestination ) {
	if ( !pTransforms || !pDestination || nCount == 0u || ( nStride != 12 && nStride != 16 ) ) {
		return;
	}
	if ( nCount > std::numeric_limits<size_t>::max() / ( size_t )( nStride ) ) {
		return;
	}
	t3dSimdGetDispatch().m_pPfnPackedTransformsToMultimesh( pTransforms, pColors, nCount, nStride, pDestination );
}

void t3dSimdDerivedNormalsRg8( const float *pHalo,
		const int nWidth,
		const int nHeight,
		const int nHaloStride,
		const float fSpacing,
		uint8_t *pDestination ) {
	if ( !pHalo || !pDestination || nWidth <= 0 || nHeight <= 0 || nWidth > std::numeric_limits<int>::max() - 2 ||
			nHaloStride < nWidth + 2 || !std::isfinite( fSpacing ) || fSpacing <= 0.0f ) {
		return;
	}
	const size_t width = ( size_t )( nWidth );
	const size_t height = ( size_t )( nHeight );
	const size_t stride = ( size_t )( nHaloStride );
	const size_t maximum = std::numeric_limits<size_t>::max();
	if ( height + 2u > maximum / stride || height > maximum / width || width * height > maximum / 2u ) {
		return;
	}
	t3dSimdGetDispatch().m_pPfnDerivedNormalsRg8( pHalo, nWidth, nHeight, nHaloStride, fSpacing, pDestination );
}

void t3dSimdUpdateCumulativeErosion( const uint8_t *pActive,
		const float *pInitialHeight,
		const float *pHeight,
		float *pCumulativeErosion,
		const size_t nCount ) {
	if ( !pActive || !pInitialHeight || !pHeight || !pCumulativeErosion || nCount == 0u ) {
		return;
	}
	t3dSimdGetDispatch().m_pPfnUpdateCumulativeErosion( pActive, pInitialHeight, pHeight, pCumulativeErosion, nCount );
}

bool t3dSimdSetBackendOverrideForTests( const t3dSimdBackend_t backend ) {
	t3dSimdEnsureInitialized();
	if ( !t3dSimdBackendSupported( backend ) ) {
		return false;
	}
	s_pActiveDispatch.store( t3dSimdDispatchTable( backend ), std::memory_order_release );
	return true;
}

void t3dSimdClearBackendOverrideForTests( void ) {
	t3dSimdEnsureInitialized();
	s_pActiveDispatch.store( t3dSimdDispatchTable( t3dSimdBestSupportedBackend() ), std::memory_order_release );
}
