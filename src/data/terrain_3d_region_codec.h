// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_CODEC_H
#define TERRAIN3D_REGION_CODEC_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <godot_cpp/classes/image.hpp>

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"
#include "core/terrain_3d_util.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Compact region payload codec used by the optimized 128-sample fork.
//
// Height is stored as R16_UNORM rather than IEEE-754 half float. Half float has
// only a 10-bit mantissa and steps of roughly 0.25-0.5 m at the game's expected
// absolute heights. R16_UNORM over this s_nFixed range retains ~1.76 cm precision.
EA_FORCE_INLINE float t3dRegionCodecEncodeHeightNormalized( const float fHeight ) { return CLAMP( ( fHeight - s_fRegionHeightMin ) * s_fRegionHeightInvRange, 0.0f, 1.0f ); }

EA_FORCE_INLINE float t3dRegionCodecDecodeHeightNormalized( const float fNormalized ) { return s_fRegionHeightMin + CLAMP( fNormalized, 0.0f, 1.0f ) * s_fRegionHeightRange; }

EA_FORCE_INLINE uint16_t t3dRegionCodecEncodeHeightU16( const float fHeight ) { return ( uint16_t ) ( CLAMP( Math::round( t3dRegionCodecEncodeHeightNormalized( fHeight ) * s_fRegionHeightU16Scale ), 0.0f, s_fRegionHeightU16Scale ) ); }

EA_FORCE_INLINE float t3dRegionCodecDecodeHeightU16( const uint16_t nValue ) { return s_fRegionHeightMin + ( ( float ) ( nValue ) / s_fRegionHeightU16Scale ) * s_fRegionHeightRange; }

inline float t3dRegionCodecReadHeightPixel( const Image *pImage, const Vector2i &rPosition ) {
	if ( !pImage ) {
		return NAN;
	}
	const float fValue = pImage->get_pixelv( rPosition ).r;
	return pImage->get_format() == Image::FORMAT_R16 ? t3dRegionCodecDecodeHeightNormalized( fValue ) : fValue;
}

inline void t3dRegionCodecWriteHeightPixel( Image *pImage, const Vector2i &rPosition, const float fHeight ) {
	if ( !pImage ) {
		return;
	}
	const float fValue = pImage->get_format() == Image::FORMAT_R16 ? t3dRegionCodecEncodeHeightNormalized( fHeight ) : fHeight;
	pImage->set_pixelv( rPosition, Color( fValue, 0.0f, 0.0f, 1.0f ) );
}

inline float t3dRegionCodecReadHeightBytes( const uint8_t *pBytes, const int iIndex, const Image::Format format ) {
	if ( !pBytes || iIndex < 0 ) {
		return NAN;
	}
	if ( format == Image::FORMAT_R16 ) {
		uint16_t fValue = 0u;
		std::memcpy( &fValue, pBytes + ( size_t ) ( iIndex ) * sizeof( uint16_t ), sizeof( uint16_t ) );
		return t3dRegionCodecDecodeHeightU16( fValue );
	}
	float fValue = NAN;
	std::memcpy( &fValue, pBytes + ( size_t ) ( iIndex ) * sizeof( float ), sizeof( float ) );
	return fValue;
}

inline size_t t3dRegionCodecExpectedHeightByteCount( const int nSize, const Image::Format format ) {
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

inline bool t3dRegionCodecHasValidHeightPayload( const Ref<Image> &rImage, const int nSize ) {
	if ( rImage.is_null() || nSize <= 0 || rImage->get_width() != nSize || rImage->get_height() != nSize ) {
		return false;
	}
	const size_t expected = t3dRegionCodecExpectedHeightByteCount( nSize, rImage->get_format() );
	return expected == 0u || ( size_t ) ( rImage->get_data().size() ) == expected;
}

// Converts legacy RF32 height storage directly from its byte plane into the
// authoritative R16_UNORM representation. The function never reads beyond the
// source payload and reports values that could not be represented exactly.
inline Ref<Image> t3dRegionCodecConvertHeightToR16(
	const Ref<Image> &rSource, const int nSize, uint32_t &nClampedCount,
	uint32_t &nNonFiniteCount
) {
	nClampedCount = 0u;
	nNonFiniteCount = 0u;
	if ( nSize <= 0 ) {
		return Ref<Image>();
	}
	if ( rSource.is_valid() && rSource->get_width() == nSize && rSource->get_height() == nSize &&
		rSource->get_format() == Image::FORMAT_R16 ) {
		return t3dRegionCodecHasValidHeightPayload( rSource, nSize ) ? rSource : Ref<Image>();
	}
	Ref<Image> pReadableSource = rSource;
	if ( pReadableSource.is_valid() && pReadableSource->is_compressed() ) {
		pReadableSource = pReadableSource->duplicate();
		if ( pReadableSource->decompress() != OK ) {
			pReadableSource.unref();
		}
	}

	const size_t texel_count = ( size_t ) ( nSize ) * ( size_t ) ( nSize );
	PackedByteArray output;
	output.resize( ( int64_t ) ( texel_count * sizeof( uint16_t ) ) );
	uint8_t *pWrite = output.ptrw();

	const bool bDirectRf32 = pReadableSource.is_valid() && pReadableSource->get_width() == nSize &&
		pReadableSource->get_height() == nSize && pReadableSource->get_format() == Image::FORMAT_RF &&
		t3dRegionCodecHasValidHeightPayload( pReadableSource, nSize );
	PackedByteArray source_bytes;
	const uint8_t *pRead = nullptr;
	if ( bDirectRf32 ) {
		source_bytes = pReadableSource->get_data();
		pRead = source_bytes.ptr();
	}

	for ( size_t index = 0; index < texel_count; ++index ) {
		float fHeight = 0.0f;
		if ( bDirectRf32 ) {
			std::memcpy( &fHeight, pRead + index * sizeof( float ), sizeof( float ) );
		} else if ( pReadableSource.is_valid() ) {
			const int x = ( int ) ( index % ( size_t ) ( nSize ) );
			const int y = ( int ) ( index / ( size_t ) ( nSize ) );
			if ( x < pReadableSource->get_width() && y < pReadableSource->get_height() ) {
				fHeight = pReadableSource->get_pixel( x, y ).r;
			}
		}

		if ( !std::isfinite( fHeight ) ) {
			fHeight = 0.0f;
			++nNonFiniteCount;
		} else if ( fHeight < s_fRegionHeightMin || fHeight > s_fRegionHeightMax ) {
			fHeight = CLAMP( fHeight, s_fRegionHeightMin, s_fRegionHeightMax );
			++nClampedCount;
		}

		const uint16_t encoded = t3dRegionCodecEncodeHeightU16( fHeight );
		std::memcpy( pWrite + index * sizeof( uint16_t ), &encoded, sizeof( uint16_t ) );
	}
	return Image::create_from_data( nSize, nSize, false, Image::FORMAT_R16, output );
}

EA_FORCE_INLINE Ref<Image> t3dRegionCodecConvertHeightToR16( const Ref<Image> &rSource, const int nSize ) { uint32_t clamped_count = 0u; uint32_t non_finite_count = 0u; return t3dRegionCodecConvertHeightToR16( rSource, nSize, clamped_count, non_finite_count ); }

inline Vector2 t3dRegionCodecGetHeightMinMax( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		return V2_ZERO;
	}
	float fMinimum = std::numeric_limits<float>::infinity();
	float fMaximum = -std::numeric_limits<float>::infinity();
	const int width = rImage->get_width();
	const int height = rImage->get_height();
	for ( int y = 0; y < height; ++y ) {
		for ( int x = 0; x < width; ++x ) {
			const float fValue = t3dRegionCodecReadHeightPixel( *rImage, Vector2i( x, y ) );
			if ( std::isfinite( fValue ) ) {
				fMinimum = MIN( fMinimum, fValue );
				fMaximum = MAX( fMaximum, fValue );
			}
		}
	}
	return std::isfinite( fMinimum ) && std::isfinite( fMaximum ) ? Vector2( fMinimum, fMaximum ) : V2_ZERO;
}

EA_FORCE_INLINE Color t3dRegionCodecDefaultHeightColor( void ) { return Color( t3dRegionCodecEncodeHeightNormalized( 0.0f ), 0.0f, 0.0f, 1.0f ); }


#endif // TERRAIN3D_REGION_CODEC_H
