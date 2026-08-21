// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "core/terrain_3d_compiler.h"
#include "simd/terrain_3d_simd.h"

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

namespace terrain3d_simd {
namespace {

t3dCapabilities_t s_capabilities;
t3dDispatch_t s_szDispatchTables[5];
std::once_flag s_initializeOnce;
std::atomic<const t3dDispatch_t *> s_pActiveDispatch{nullptr};

static uint64_t xgetbv0( void ) {
#if defined( _MSC_VER ) && ( defined( _M_IX86 ) || defined( _M_X64 ) )
	return _xgetbv( 0 );
#elif ( defined( __i386__ ) || defined( __x86_64__ ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
	uint32_t eax = 0u;
	uint32_t edx = 0u;
	__asm__ volatile( "xgetbv" : "=a"( eax ), "=d"( edx ) : "c"( 0 ) );
	return ( ( uint64_t ) ( edx ) << 32u ) | ( uint64_t ) ( eax );
#else
	return 0u;
#endif
}

static void detectX86( t3dCapabilities_t &rCaps ) {
#if defined( _MSC_VER ) && ( defined( _M_IX86 ) || defined( _M_X64 ) )
	int registers[4] = {};
	__cpuidex( registers, 0, 0 );
	const int max_leaf = registers[0];
	if ( max_leaf < 1 ) {
		return;
	}
	__cpuidex( registers, 1, 0 );
	const uint32_t ecx = ( uint32_t ) ( registers[2] );
	const uint32_t edx = ( uint32_t ) ( registers[3] );
	rCaps.bSse2 = ( edx & ( 1u << 26u ) ) != 0u;
	const bool bOsxsave = ( ecx & ( 1u << 27u ) ) != 0u;
	const bool bCpuAvx = ( ecx & ( 1u << 28u ) ) != 0u;
	rCaps.bF16c = ( ecx & ( 1u << 29u ) ) != 0u;
	const bool bOsAvx = bOsxsave && bCpuAvx && ( xgetbv0() & 0x6u ) == 0x6u;
	rCaps.bAvx = bOsAvx;
	if ( max_leaf >= 7 ) {
		__cpuidex( registers, 7, 0 );
		rCaps.bAvx2 = bOsAvx && ( ( uint32_t ) ( registers[1] ) & ( 1u << 5u ) ) != 0u;
	}
#elif ( defined( __i386__ ) || defined( __x86_64__ ) ) && ( defined( __GNUC__ ) || defined( __clang__ ) )
	uint32_t eax = 0u;
	uint32_t ebx = 0u;
	uint32_t ecx = 0u;
	uint32_t edx = 0u;
	if ( !__get_cpuid( 1u, &eax, &ebx, &ecx, &edx ) ) {
		return;
	}
	rCaps.bSse2 = ( edx & bit_SSE2 ) != 0u;
	const bool bOsxsave = ( ecx & bit_OSXSAVE ) != 0u;
	const bool bCpuAvx = ( ecx & bit_AVX ) != 0u;
	rCaps.bF16c = ( ecx & bit_F16C ) != 0u;
	const bool bOsAvx = bOsxsave && bCpuAvx && ( xgetbv0() & 0x6u ) == 0x6u;
	rCaps.bAvx = bOsAvx;
	if ( __get_cpuid_max( 0u, nullptr ) >= 7u ) {
		__cpuid_count( 7u, 0u, eax, ebx, ecx, edx );
		rCaps.bAvx2 = bOsAvx && ( ebx & bit_AVX2 ) != 0u;
	}
#else
	( void ) rCaps;
#endif
}

static void detectArm( t3dCapabilities_t &rCaps ) {
#if defined( __aarch64__ ) || defined( _M_ARM64 )
	rCaps.bNeon = true;
#elif defined( __ARM_NEON ) || defined( __ARM_NEON__ )
#if defined( _WIN32 )
	rCaps.bNeon = IsProcessorFeaturePresent( PF_ARM_NEON_INSTRUCTIONS_AVAILABLE ) != 0;
#elif defined( __linux__ )
	rCaps.bNeon = ( getauxval( AT_HWCAP ) & HWCAP_NEON ) != 0u;
#else
	rCaps.bNeon = true;
#endif
#else
	( void ) rCaps;
#endif
}

static t3dDispatch_t makeDispatch( const t3dBackend_t backend ) {
	t3dDispatch_t result;
	result.backend = backend;
	switch ( backend ) {
		case t3dBackend_t::AVX2_F16C:
			result.pfnHalfToFloat = detail::avx2HalfToFloat;
			result.pfnPackedTransformsToMultimesh = detail::avx2PackedTransforms;
			result.pfnDerivedNormalsRg8 = detail::avx2DerivedNormals;
			result.pfnUpdateCumulativeErosion = detail::avx2UpdateCumulativeErosion;
			break;
		case t3dBackend_t::AVX_F16C:
			result.pfnHalfToFloat = detail::avxHalfToFloat;
			result.pfnPackedTransformsToMultimesh = detail::avxPackedTransforms;
			result.pfnDerivedNormalsRg8 = detail::avxDerivedNormals;
			result.pfnUpdateCumulativeErosion = detail::avxUpdateCumulativeErosion;
			break;
		case t3dBackend_t::SSE2:
			result.pfnHalfToFloat = detail::sse2HalfToFloat;
			result.pfnPackedTransformsToMultimesh = detail::sse2PackedTransforms;
			result.pfnDerivedNormalsRg8 = detail::sse2DerivedNormals;
			result.pfnUpdateCumulativeErosion = detail::sse2UpdateCumulativeErosion;
			break;
		case t3dBackend_t::NEON:
			result.pfnHalfToFloat = detail::neonHalfToFloat;
			result.pfnPackedTransformsToMultimesh = detail::neonPackedTransforms;
			result.pfnDerivedNormalsRg8 = detail::neonDerivedNormals;
			result.pfnUpdateCumulativeErosion = detail::neonUpdateCumulativeErosion;
			break;
		case t3dBackend_t::SCALAR:
		default:
			result.backend = t3dBackend_t::SCALAR;
			result.pfnHalfToFloat = detail::scalarHalfToFloat;
			result.pfnPackedTransformsToMultimesh = detail::scalarPackedTransforms;
			result.pfnDerivedNormalsRg8 = detail::scalarDerivedNormals;
			result.pfnUpdateCumulativeErosion = detail::scalarUpdateCumulativeErosion;
			break;
	}
	return result;
}

static bool backendSupported( const t3dBackend_t backend ) {
	switch ( backend ) {
		case t3dBackend_t::AVX2_F16C:
			return s_capabilities.bAvx2 && s_capabilities.bF16c && detail::avx2BackendAvailableAtBuild();
		case t3dBackend_t::AVX_F16C:
			return s_capabilities.bAvx && s_capabilities.bF16c && detail::avxBackendAvailableAtBuild();
		case t3dBackend_t::SSE2:
			return s_capabilities.bSse2 && detail::sse2BackendAvailableAtBuild();
		case t3dBackend_t::NEON:
			return s_capabilities.bNeon && detail::neonBackendAvailableAtBuild();
		case t3dBackend_t::SCALAR:
		default:
			return true;
	}
}

static bool parseEnvironmentOverride( t3dBackend_t &rBackend ) {
	const char *value = std::getenv( "TERRAIN3D_SIMD" );
	if ( !value || value[0] == '\0' ) {
		return false;
	}
	if ( std::strcmp( value, "scalar" ) == 0 ) {
		rBackend = t3dBackend_t::SCALAR;
		return true;
	}
	if ( std::strcmp( value, "sse2" ) == 0 || std::strcmp( value, "sse" ) == 0 ) {
		rBackend = t3dBackend_t::SSE2;
		return true;
	}
	if ( std::strcmp( value, "avx" ) == 0 || std::strcmp( value, "avx_f16c" ) == 0 ) {
		rBackend = t3dBackend_t::AVX_F16C;
		return true;
	}
	if ( std::strcmp( value, "avx2" ) == 0 || std::strcmp( value, "avx2_f16c" ) == 0 ) {
		rBackend = t3dBackend_t::AVX2_F16C;
		return true;
	}
	if ( std::strcmp( value, "neon" ) == 0 ) {
		rBackend = t3dBackend_t::NEON;
		return true;
	}
	return false;
}

EA_FORCE_INLINE static const t3dDispatch_t *dispatchTable( const t3dBackend_t backend ) { return &s_szDispatchTables[( size_t ) ( backend )]; }

static t3dBackend_t bestSupportedBackend( void ) {
	if ( backendSupported( t3dBackend_t::AVX2_F16C ) ) {
		return t3dBackend_t::AVX2_F16C;
	}
	if ( backendSupported( t3dBackend_t::AVX_F16C ) ) {
		return t3dBackend_t::AVX_F16C;
	}
	if ( backendSupported( t3dBackend_t::SSE2 ) ) {
		return t3dBackend_t::SSE2;
	}
	if ( backendSupported( t3dBackend_t::NEON ) ) {
		return t3dBackend_t::NEON;
	}
	return t3dBackend_t::SCALAR;
}

static void initialize( void ) {
	detectX86( s_capabilities );
	detectArm( s_capabilities );
	for ( size_t index = 0u; index < 5u; ++index ) {
		s_szDispatchTables[index] = makeDispatch( ( t3dBackend_t ) ( index ) );
	}
	t3dBackend_t selected = bestSupportedBackend();
	t3dBackend_t requested = t3dBackend_t::SCALAR;
	if ( parseEnvironmentOverride( requested ) && backendSupported( requested ) ) {
		selected = requested;
	}
	s_pActiveDispatch.store( dispatchTable( selected ), std::memory_order_release );
}

EA_FORCE_INLINE static void ensureInitialized( void ) { std::call_once( s_initializeOnce, initialize ); }

EA_FORCE_INLINE static uint8_t encodeNormalComponent( const float fValue ) { const float fEncoded = std::max( 0.0f, std::min( TERRAIN3D_BYTE_MAX_FLOAT, ( fValue * 0.5f + 0.5f ) * TERRAIN3D_BYTE_MAX_FLOAT ) ); return ( uint8_t ) ( std::floor( fEncoded + 0.5f ) ); }

} // namespace

namespace detail {

void scalarHalfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
	for ( size_t i = 0; i < nCount; ++i ) {
		pDestination[i] = terrain3d_half::decode( pSource[i] );
	}
}

void scalarPackedTransforms(
	const t3dPackedTransform_t *pTransforms, const uint32_t *pColors,
	const size_t nCount, const int nStride, float *pDestination
) {
	for ( size_t i = 0; i < nCount; ++i ) {
		const t3dPackedTransform_t &t = pTransforms[i];
		float *pOut = pDestination + i * ( size_t ) ( nStride );
		pOut[0] = terrain3d_half::decode( t.basis[0] );
		pOut[1] = terrain3d_half::decode( t.basis[1] );
		pOut[2] = terrain3d_half::decode( t.basis[2] );
		pOut[3] = t.origin[0];
		pOut[4] = terrain3d_half::decode( t.basis[3] );
		pOut[5] = terrain3d_half::decode( t.basis[4] );
		pOut[6] = terrain3d_half::decode( t.basis[5] );
		pOut[7] = t.origin[1];
		pOut[8] = terrain3d_half::decode( t.basis[6] );
		pOut[9] = terrain3d_half::decode( t.basis[7] );
		pOut[10] = terrain3d_half::decode( t.basis[8] );
		pOut[11] = t.origin[2];
		if ( nStride == 16 ) {
			const uint32_t color = pColors ? pColors[i] : 0xffffffffu;
			pOut[12] = ( float ) ( ( color >> 24u ) & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
			pOut[13] = ( float ) ( ( color >> 16u ) & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
			pOut[14] = ( float ) ( ( color >> 8u ) & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
			pOut[15] = ( float ) ( color & 0xffu ) * ( TERRAIN3D_BYTE_INV_MAX_FLOAT );
		}
	}
}

void scalarDerivedNormals(
	const float *pHalo, const int nWidth, const int nHeight, const int nHaloStride,
	const float fSpacing, uint8_t *pDestination
) {
	for ( int y = 0; y < nHeight; ++y ) {
		for ( int x = 0; x < nWidth; ++x ) {
			const size_t center = ( size_t ) ( y + 1 ) * ( size_t ) ( nHaloStride ) + ( size_t ) ( x + 1 );
			const float fDx = pHalo[center] - pHalo[center + 1u];
			const float fDz = pHalo[center] - pHalo[center + ( size_t ) ( nHaloStride )];
			const float fLength = std::sqrt( fDx * fDx + fSpacing * fSpacing + fDz * fDz );
			const float fInverse = fLength > 0.0f ? 1.0f / fLength : 0.0f;
			const size_t pOut = ( ( size_t ) ( y ) * ( size_t ) ( nWidth ) + ( size_t ) ( x ) ) * 2u;
			pDestination[pOut] = encodeNormalComponent( fDx * fInverse );
			pDestination[pOut + 1u] = encodeNormalComponent( fDz * fInverse );
		}
	}
}

void scalarUpdateCumulativeErosion(
	const uint8_t *pActive, const float *pInitialHeight, const float *pHeight,
	float *pCumulativeErosion, const size_t nCount
) {
	for ( size_t index = 0u; index < nCount; ++index ) {
		if ( pActive[index] != 0u ) {
			pCumulativeErosion[index] = std::max(
				pCumulativeErosion[index], std::max( 0.0f, pInitialHeight[index] - pHeight[index] ) );
		}
	}
}

} // namespace detail

const t3dCapabilities_t &capabilities( void ) {
	ensureInitialized();
	return s_capabilities;
}

const t3dDispatch_t &dispatch( void ) {
	ensureInitialized();
	return *s_pActiveDispatch.load( std::memory_order_acquire );
}

t3dBackend_t activeBackend( void ) {
	return dispatch().backend;
}

const char *backendName( const t3dBackend_t backend ) {
	switch ( backend ) {
		case t3dBackend_t::SSE2:
			return "SSE2";
		case t3dBackend_t::AVX_F16C:
			return "AVX/F16C";
		case t3dBackend_t::AVX2_F16C:
			return "AVX2/F16C";
		case t3dBackend_t::NEON:
			return "NEON";
		case t3dBackend_t::SCALAR:
		default:
			return "Scalar";
	}
}

void halfToFloat( const uint16_t *pSource, float *pDestination, const size_t nCount ) {
	if ( !pSource || !pDestination || nCount == 0u ) {
		return;
	}
	dispatch().pfnHalfToFloat( pSource, pDestination, nCount );
}

void packedTransformsToMultimesh(
	const t3dPackedTransform_t *pTransforms, const uint32_t *pColors,
	const size_t nCount, const int nStride, float *pDestination
) {
	if ( !pTransforms || !pDestination || nCount == 0u || ( nStride != 12 && nStride != 16 ) ) {
		return;
	}
	if ( nCount > std::numeric_limits<size_t>::max() / ( size_t ) ( nStride ) ) {
		return;
	}
	dispatch().pfnPackedTransformsToMultimesh( pTransforms, pColors, nCount, nStride, pDestination );
}

void derivedNormalsRg8(
	const float *pHalo, const int nWidth, const int nHeight, const int nHaloStride,
	const float fSpacing, uint8_t *pDestination
) {
	if ( !pHalo || !pDestination || nWidth <= 0 || nHeight <= 0 ||
		nWidth > std::numeric_limits<int>::max() - 2 || nHaloStride < nWidth + 2 ||
		!std::isfinite( fSpacing ) || fSpacing <= 0.0f ) {
		return;
	}
	const size_t width = ( size_t ) ( nWidth );
	const size_t height = ( size_t ) ( nHeight );
	const size_t stride = ( size_t ) ( nHaloStride );
	const size_t maximum = std::numeric_limits<size_t>::max();
	if ( height + 2u > maximum / stride || height > maximum / width || width * height > maximum / 2u ) {
		return;
	}
	dispatch().pfnDerivedNormalsRg8( pHalo, nWidth, nHeight, nHaloStride, fSpacing, pDestination );
}

void updateCumulativeErosion(
	const uint8_t *pActive, const float *pInitialHeight, const float *pHeight,
	float *pCumulativeErosion, const size_t nCount
) {
	if ( !pActive || !pInitialHeight || !pHeight || !pCumulativeErosion || nCount == 0u ) {
		return;
	}
	dispatch().pfnUpdateCumulativeErosion( pActive, pInitialHeight, pHeight, pCumulativeErosion, nCount );
}

bool setBackendOverrideForTests( const t3dBackend_t backend ) {
	ensureInitialized();
	if ( !backendSupported( backend ) ) {
		return false;
	}
	s_pActiveDispatch.store( dispatchTable( backend ), std::memory_order_release );
	return true;
}

void clearBackendOverrideForTests( void ) {
	ensureInitialized();
	s_pActiveDispatch.store( dispatchTable( bestSupportedBackend() ), std::memory_order_release );
}

} // namespace terrain3d_simd
