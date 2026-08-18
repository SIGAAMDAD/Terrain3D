// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_CODEC_H
#define TERRAIN3D_REGION_CODEC_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <godot_cpp/classes/image.hpp>

#include "Terrain3DConstants.h"
#include "core/terrain_3d_util.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Compact region payload codec used by the optimized 128-sample fork.
//
// Height is stored as R16_UNORM rather than IEEE-754 half float. Half float has
// only a 10-bit mantissa and steps of roughly 0.25-0.5 m at the game's expected
// absolute heights. R16_UNORM over this fixed range retains ~1.76 cm precision.
namespace Terrain3DRegionCodec {

inline float encodeHeightNormalized( const float fHeight ) {
	return CLAMP( ( fHeight - TERRAIN3D_REGION_HEIGHT_MIN ) * TERRAIN3D_REGION_HEIGHT_INV_RANGE, 0.0f, 1.0f );
}

inline float decodeHeightNormalized( const float fNormalized ) {
	return TERRAIN3D_REGION_HEIGHT_MIN + CLAMP( fNormalized, 0.0f, 1.0f ) * TERRAIN3D_REGION_HEIGHT_RANGE;
}

inline uint16_t encodeHeightU16( const float fHeight ) {
	return ( uint16_t ) ( CLAMP( Math::round( encodeHeightNormalized( fHeight ) * TERRAIN3D_REGION_HEIGHT_U16_SCALE ), 0.0f, TERRAIN3D_REGION_HEIGHT_U16_SCALE ) );
}

inline float decodeHeightU16( const uint16_t nValue ) {
	return TERRAIN3D_REGION_HEIGHT_MIN + ( ( float ) ( nValue ) / TERRAIN3D_REGION_HEIGHT_U16_SCALE ) * TERRAIN3D_REGION_HEIGHT_RANGE;
}

inline float readHeightPixel( const Image *pImage, const Vector2i &rPosition ) {
	if ( !pImage ) {
		return NAN;
	}
	const float fValue = pImage->get_pixelv( rPosition ).r;
	return pImage->get_format() == Image::FORMAT_R16 ? decodeHeightNormalized( fValue ) : fValue;
}

inline void writeHeightPixel( Image *pImage, const Vector2i &rPosition, const float fHeight ) {
	if ( !pImage ) {
		return;
	}
	const float fValue = pImage->get_format() == Image::FORMAT_R16 ? encodeHeightNormalized( fHeight ) : fHeight;
	pImage->set_pixelv( rPosition, Color( fValue, 0.0f, 0.0f, 1.0f ) );
}

inline float readHeightBytes( const uint8_t *pBytes, const int iIndex, const Image::Format format ) {
	if ( !pBytes || iIndex < 0 ) {
		return NAN;
	}
	if ( format == Image::FORMAT_R16 ) {
		uint16_t fValue = 0u;
		std::memcpy( &fValue, pBytes + ( size_t ) ( iIndex ) * sizeof( uint16_t ), sizeof( uint16_t ) );
		return decodeHeightU16( fValue );
	}
	float fValue = NAN;
	std::memcpy( &fValue, pBytes + ( size_t ) ( iIndex ) * sizeof( float ), sizeof( float ) );
	return fValue;
}

inline size_t expectedHeightByteCount( const int nSize, const Image::Format format ) {
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

inline bool hasValidHeightPayload( const Ref<Image> &rImage, const int nSize ) {
	if ( rImage.is_null() || nSize <= 0 || rImage->get_width() != nSize || rImage->get_height() != nSize ) {
		return false;
	}
	const size_t expected = expectedHeightByteCount( nSize, rImage->get_format() );
	return expected == 0u || ( size_t ) ( rImage->get_data().size() ) == expected;
}

// Converts legacy RF32 height storage directly from its byte plane into the
// authoritative R16_UNORM representation. The function never reads beyond the
// source payload and reports values that could not be represented exactly.
inline Ref<Image> convertHeightToR16(
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
		return hasValidHeightPayload( rSource, nSize ) ? rSource : Ref<Image>();
	}

	const size_t texel_count = ( size_t ) ( nSize ) * ( size_t ) ( nSize );
	PackedByteArray output;
	output.resize( ( int64_t ) ( texel_count * sizeof( uint16_t ) ) );
	uint8_t *pWrite = output.ptrw();

	const bool bDirectRf32 = rSource.is_valid() && rSource->get_width() == nSize &&
		rSource->get_height() == nSize && rSource->get_format() == Image::FORMAT_RF &&
		hasValidHeightPayload( rSource, nSize );
	PackedByteArray source_bytes;
	const uint8_t *pRead = nullptr;
	if ( bDirectRf32 ) {
		source_bytes = rSource->get_data();
		pRead = source_bytes.ptr();
	}

	for ( size_t index = 0; index < texel_count; ++index ) {
		float fHeight = 0.0f;
		if ( bDirectRf32 ) {
			std::memcpy( &fHeight, pRead + index * sizeof( float ), sizeof( float ) );
		} else if ( rSource.is_valid() ) {
			const int x = ( int ) ( index % ( size_t ) ( nSize ) );
			const int y = ( int ) ( index / ( size_t ) ( nSize ) );
			if ( x < rSource->get_width() && y < rSource->get_height() ) {
				fHeight = rSource->get_pixel( x, y ).r;
			}
		}

		if ( !std::isfinite( fHeight ) ) {
			fHeight = 0.0f;
			++nNonFiniteCount;
		} else if ( fHeight < TERRAIN3D_REGION_HEIGHT_MIN || fHeight > TERRAIN3D_REGION_HEIGHT_MAX ) {
			fHeight = CLAMP( fHeight, TERRAIN3D_REGION_HEIGHT_MIN, TERRAIN3D_REGION_HEIGHT_MAX );
			++nClampedCount;
		}

		const uint16_t encoded = encodeHeightU16( fHeight );
		std::memcpy( pWrite + index * sizeof( uint16_t ), &encoded, sizeof( uint16_t ) );
	}
	return Image::create_from_data( nSize, nSize, false, Image::FORMAT_R16, output );
}

inline Ref<Image> convertHeightToR16( const Ref<Image> &rSource, const int nSize ) {
	uint32_t clamped_count = 0u;
	uint32_t non_finite_count = 0u;
	return convertHeightToR16( rSource, nSize, clamped_count, non_finite_count );
}

inline Vector2 getHeightMinMax( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		return V2_ZERO;
	}
	float fMinimum = std::numeric_limits<float>::infinity();
	float fMaximum = -std::numeric_limits<float>::infinity();
	const int width = rImage->get_width();
	const int height = rImage->get_height();
	for ( int y = 0; y < height; ++y ) {
		for ( int x = 0; x < width; ++x ) {
			const float fValue = readHeightPixel( *rImage, Vector2i( x, y ) );
			if ( std::isfinite( fValue ) ) {
				fMinimum = MIN( fMinimum, fValue );
				fMaximum = MAX( fMaximum, fValue );
			}
		}
	}
	return std::isfinite( fMinimum ) && std::isfinite( fMaximum ) ? Vector2( fMinimum, fMaximum ) : V2_ZERO;
}

inline Color defaultHeightColor( void ) {
	return Color( encodeHeightNormalized( 0.0f ), 0.0f, 0.0f, 1.0f );
}

} // namespace Terrain3DRegionCodec

#endif // TERRAIN3D_REGION_CODEC_H
