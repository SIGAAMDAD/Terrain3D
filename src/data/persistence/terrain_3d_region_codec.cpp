#include "data/persistence/terrain_3d_region_codec.h"

Ref<Image> t3dRegionCodecConvertHeightToR16(
        const Ref<Image> &rSource, const int nSize, uint32_t &nClampedCount, uint32_t &nNonFiniteCount ) {
	nClampedCount = 0u;
	nNonFiniteCount = 0u;
	if ( nSize <= 0 ) {
		return Ref<Image>();
	}
	if ( rSource.is_valid() && rSource->get_width() == nSize && rSource->get_height() == nSize
	        && rSource->get_format() == Image::FORMAT_R16 ) {
		return t3dRegionCodecHasValidHeightPayload( rSource, nSize ) ? rSource : Ref<Image>();
	}
	Ref<Image> pReadableSource = rSource;
	if ( pReadableSource.is_valid() && pReadableSource->is_compressed() ) {
		pReadableSource = pReadableSource->duplicate();
		if ( pReadableSource->decompress() != OK ) {
			pReadableSource.unref();
		}
	}

	const size_t texel_count = ( size_t )( nSize ) * ( size_t )( nSize );
	PackedByteArray output;
	output.resize( ( int64_t )( texel_count * sizeof( uint16_t ) ) );
	uint8_t *pWrite = output.ptrw();

	const bool bDirectRf32 = pReadableSource.is_valid() && pReadableSource->get_width() == nSize && pReadableSource->get_height() == nSize
	        && pReadableSource->get_format() == Image::FORMAT_RF && t3dRegionCodecHasValidHeightPayload( pReadableSource, nSize );
	PackedByteArray source_bytes;
	const uint8_t *pRead = NULL;
	if ( bDirectRf32 ) {
		source_bytes = pReadableSource->get_data();
		pRead = source_bytes.ptr();
	}

	for ( size_t index = 0; index < texel_count; ++index ) {
		float fHeight = 0.0f;
		if ( bDirectRf32 ) {
			memcpy( &fHeight, pRead + index * sizeof( float ), sizeof( float ) );
		} else if ( pReadableSource.is_valid() ) {
			const int x = ( int )( index % ( size_t )( nSize ) );
			const int y = ( int )( index / ( size_t )( nSize ) );
			if ( x < pReadableSource->get_width() && y < pReadableSource->get_height() ) {
				fHeight = pReadableSource->get_pixel( x, y ).r;
			}
		}

		if ( !isfinite( fHeight ) ) {
			fHeight = 0.0f;
			++nNonFiniteCount;
		} else if ( fHeight < s_fRegionHeightMin || fHeight > s_fRegionHeightMax ) {
			fHeight = CLAMP( fHeight, s_fRegionHeightMin, s_fRegionHeightMax );
			++nClampedCount;
		}

		const uint16_t encoded = t3dRegionCodecEncodeHeightU16( fHeight );
		memcpy( pWrite + index * sizeof( uint16_t ), &encoded, sizeof( uint16_t ) );
	}
	return Image::create_from_data( nSize, nSize, false, Image::FORMAT_R16, output );
}

Vector2 t3dRegionCodecGetHeightMinMax( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		return s_v2Zero;
	}
	float fMinimum = FLT_MAX;
	float fMaximum = -FLT_MAX;
	const int width = rImage->get_width();
	const int height = rImage->get_height();
	for ( int y = 0; y < height; ++y ) {
		for ( int x = 0; x < width; ++x ) {
			const float fValue = t3dRegionCodecReadHeightPixel( *rImage, Vector2i( x, y ) );
			if ( isfinite( fValue ) ) {
				fMinimum = MIN( fMinimum, fValue );
				fMaximum = MAX( fMaximum, fValue );
			}
		}
	}
	return isfinite( fMinimum ) && isfinite( fMaximum ) ? Vector2( fMinimum, fMaximum ) : s_v2Zero;
}

Ref<Image> t3dRegionCodecConvertHeightToR16( const Ref<Image> &rSource, const int nSize ) {
	uint32_t clamped_count = 0u;
	uint32_t non_finite_count = 0u;
	return t3dRegionCodecConvertHeightToR16( rSource, nSize, clamped_count, non_finite_count );
}
