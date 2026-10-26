// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGION_CODEC_H
#define TERRAIN3D_REGION_CODEC_H

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_util.h"
#include "core/utility/terrain_3d_binary_io.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <godot_cpp/classes/image.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Compact region payload codec used by the optimized 128-sample fork.
//
// Height is stored as R16_UNORM rather than IEEE-754 half float. Half float has
// only a 10-bit mantissa and steps of roughly 0.25-0.5 m at the game's expected
// absolute heights. R16_UNORM over this fixed range retains ~1.76 cm precision.
EA_FORCE_INLINE float t3dRegionCodecEncodeHeightNormalized(
	const float fHeight ) {
	return CLAMP( ( fHeight - s_fRegionHeightMin ) * s_fRegionHeightInvRange, 0.0f, 1.0f );
}

EA_FORCE_INLINE float t3dRegionCodecDecodeHeightNormalized(
	const float fNormalized ) {
	return s_fRegionHeightMin + CLAMP( fNormalized, 0.0f, 1.0f ) * s_fRegionHeightRange;
}

EA_FORCE_INLINE uint16_t t3dRegionCodecEncodeHeightU16(
	const float fHeight ) {
	return (
		uint16_t ) ( CLAMP( Math::round( t3dRegionCodecEncodeHeightNormalized( fHeight ) * s_fRegionHeightU16Scale ),
							0.0f,
							s_fRegionHeightU16Scale ) );
}

EA_FORCE_INLINE float t3dRegionCodecDecodeHeightU16(
	const uint16_t nValue ) {
	return s_fRegionHeightMin + ( ( float ) ( nValue ) / s_fRegionHeightU16Scale ) * s_fRegionHeightRange;
}

inline float t3dRegionCodecReadHeightPixel(
	const Image	   *pImage,
	const Vector2i &rPosition ) {
	if ( !pImage ) {
		return NAN;
	}
	const float fValue = pImage->get_pixelv( rPosition ).r;
	return pImage->get_format() == Image::FORMAT_R16 ? t3dRegionCodecDecodeHeightNormalized( fValue ) : fValue;
}

inline void t3dRegionCodecWriteHeightPixel(
	Image		   *pImage,
	const Vector2i &rPosition,
	const float		fHeight ) {
	if ( !pImage ) {
		return;
	}
	const float fValue =
		pImage->get_format() == Image::FORMAT_R16 ? t3dRegionCodecEncodeHeightNormalized( fHeight ) : fHeight;
	pImage->set_pixelv( rPosition, Color( fValue, 0.0f, 0.0f, 1.0f ) );
}

inline float t3dRegionCodecReadHeightBytes(
	const uint8_t	   *pBytes,
	const int			iIndex,
	const Image::Format format ) {
	if ( !pBytes || iIndex < 0 ) {
		return NAN;
	}
	if ( format == Image::FORMAT_R16 ) {
		const uint16_t fValue =
			t3dReadNativeUnaligned< uint16_t >( pBytes + ( size_t ) ( iIndex ) * sizeof( uint16_t ) );
		return t3dRegionCodecDecodeHeightU16( fValue );
	}
	return t3dReadNativeUnaligned< float >( pBytes + ( size_t ) ( iIndex ) * sizeof( float ) );
}

inline size_t t3dRegionCodecExpectedHeightByteCount(
	const int			nSize,
	const Image::Format format ) {
	if ( nSize <= 0 ) {
		return 0u;
	}
	const size_t texels = ( size_t ) ( nSize ) * ( size_t ) ( nSize );
	switch ( format ) {
	case Image::FORMAT_R16:
		return texels * sizeof( uint16_t );
	case Image::FORMAT_RF:
		return texels * sizeof( float );
	default:
		return 0u;
	}
}

inline bool t3dRegionCodecHasValidHeightPayload(
	const Ref< Image > &rImage,
	const int			nSize ) {
	if ( rImage.is_null() || nSize <= 0 || rImage->get_width() != nSize || rImage->get_height() != nSize ) {
		return false;
	}
	const size_t expected = t3dRegionCodecExpectedHeightByteCount( nSize, rImage->get_format() );
	return expected == 0u || ( size_t ) ( rImage->get_data().size() ) == expected;
}

// Converts legacy RF32 height storage directly from its byte plane into the
// authoritative R16_UNORM representation. The function never reads beyond the
// source payload and reports values that could not be represented exactly.
Ref< Image > t3dRegionCodecConvertHeightToR16( const Ref< Image > &rSource,
											   const int		   nSize,
											   uint32_t			  &nClampedCount,
											   uint32_t			  &nNonFiniteCount );

Ref< Image > t3dRegionCodecConvertHeightToR16( const Ref< Image > &rSource, const int nSize );

Vector2 t3dRegionCodecGetHeightMinMax( const Ref< Image > &rImage );

EA_FORCE_INLINE Color t3dRegionCodecDefaultHeightColor(
	void ) {
	return Color( t3dRegionCodecEncodeHeightNormalized( 0.0f ), 0.0f, 0.0f, 1.0f );
}

#endif // TERRAIN3D_REGION_CODEC_H
