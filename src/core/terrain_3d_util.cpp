// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>

#include <algorithm>
#include <cstring>
#include "eastl_config.h"
#include <EASTL/vector.h>

#include "core/logger.h"
#include "core/terrain_3d_util.h"
#include "simd/terrain_3d_parallel.h"

namespace {

typedef struct t3dPackedPBRPixel_s {
	Vector3 normal = Vector3( 0.0f, 0.0f, 1.0f );
	real_t ao = 1.0f;
	real_t metallic = 0.0f;
	real_t fRoughness = 1.0f;
	real_t displacement = 0.5f;
} t3dPackedPBRPixel_t;

Ref<Image> prepareSourceImage( const Ref<Image> &rImage ) {
	if ( rImage.is_null() ) {
		return Ref<Image>();
	}
	Ref<Image> pImage = rImage->duplicate();
	if ( pImage->is_compressed() ) {
		const Error err = pImage->decompress();
		if ( err != OK ) {
			return Ref<Image>();
		}
	}
	return pImage;
}

Ref<Image> prepareRgbaFloatSource( const Ref<Image> &rImage ) {
	Ref<Image> pImage = prepareSourceImage( rImage );
	if ( pImage.is_valid() && !pImage->is_empty() && pImage->get_format() != Image::FORMAT_RGBAF ) {
		pImage->convert( Image::FORMAT_RGBAF );
	}
	return pImage;
}

EA_FORCE_INLINE float rgbaFloatChannel( const uint8_t *pBytes, const size_t nPixel, const int nChannel ) { float fValue = 0.0f; std::memcpy( &fValue, pBytes + ( nPixel * 4u + ( size_t ) ( nChannel ) ) * sizeof( float ), sizeof( float ) ); return fValue; }

T3DParallelExecutor authoringExecutor( const size_t nItemCount ) {
	const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	const size_t useful_tasks = MAX<size_t>( 1u, nItemCount / 16384u );
	return T3DParallelExecutor( ( int32_t ) ( MIN<size_t>( ( size_t ) ( hardware_threads ), useful_tasks ) ) );
}

EA_FORCE_INLINE uint16_t quantizeU16( const real_t fValue ) { return ( uint16_t ) ( CLAMP( Math::round( CLAMP( fValue, ( real_t ) ( 0.0 ), ( real_t ) ( 1.0 ) ) * 65535.0 ), 0.0, 65535.0 ) ); }

EA_FORCE_INLINE uint8_t quantizeU8( const real_t fValue ) { return ( uint8_t ) ( CLAMP( Math::round( CLAMP( fValue, ( real_t ) ( 0.0 ), ( real_t ) ( 1.0 ) ) * 255.0 ), 0.0, 255.0 ) ); }

EA_FORCE_INLINE int clampSourceCoord( const int nValue, const int nLimit ) { return CLAMP( nValue, 0, MAX( nLimit - 1, 0 ) ); }

t3dPackedPBRPixel_t averagePackedPbrPixels(
	const eastl::vector<t3dPackedPBRPixel_t> &rSrc, const int nWidth,
	const int nHeight, const int iX, const int iY
) {
	t3dPackedPBRPixel_t result;
	result.normal = Vector3();
	result.ao = 0.0f;
	result.metallic = 0.0f;
	result.fRoughness = 0.0f;
	result.displacement = 0.0f;
	int sample_count = 0;
	for ( int oy = 0; oy < 2; oy++ ) {
		for ( int ox = 0; ox < 2; ox++ ) {
			const int sx = clampSourceCoord( iX * 2 + ox, nWidth );
			const int sy = clampSourceCoord( iY * 2 + oy, nHeight );
			const t3dPackedPBRPixel_t &sample = rSrc[( size_t ) ( sy ) * ( size_t ) ( nWidth ) + ( size_t ) ( sx )];
			result.normal += sample.normal;
			result.ao += sample.ao;
			result.metallic += sample.metallic;
			result.fRoughness += sample.fRoughness;
			result.displacement += sample.displacement;
			sample_count++;
		}
	}
	const real_t fInvCount = 1.0f / ( real_t ) ( MAX( sample_count, 1 ) );
	result.normal *= fInvCount;
	if ( result.normal.length_squared() > 1e-12f ) {
		result.normal.normalize();
	} else {
		result.normal = Vector3( 0.0f, 0.0f, 1.0f );
	}
	result.ao *= fInvCount;
	result.metallic *= fInvCount;
	result.fRoughness *= fInvCount;
	result.displacement *= fInvCount;
	return result;
}

void appendPackedPbrLevel( PackedByteArray &rData, int64_t &nOffset, const eastl::vector<t3dPackedPBRPixel_t> &rPixels ) {
	for ( const t3dPackedPBRPixel_t &pixel : rPixels ) {
		const uint8_t normal_x = quantizeU8( pixel.normal.x * 0.5f + 0.5f );
		const uint8_t normal_y = quantizeU8( pixel.normal.y * 0.5f + 0.5f );
		const uint8_t ao = quantizeU8( pixel.ao );
		const uint8_t metallic = quantizeU8( pixel.metallic );
		const uint16_t packed_normal_ao = ( ( uint16_t ) ( normal_x ) << 8u ) | ( uint16_t ) ( ao );
		const uint16_t packed_normal_metallic = ( ( uint16_t ) ( normal_y ) << 8u ) | ( uint16_t ) ( metallic );
		rData.encode_u16( nOffset, packed_normal_ao );
		rData.encode_u16( nOffset + 2, packed_normal_metallic );
		rData.encode_u16( nOffset + 4, quantizeU16( pixel.fRoughness ) );
		rData.encode_u16( nOffset + 6, quantizeU16( pixel.displacement ) );
		nOffset += 8;
	}
}

void appendSurfaceLevel( PackedByteArray &rData, int64_t &nOffset, const eastl::vector<t3dPackedPBRPixel_t> &rPixels ) {
	for ( const t3dPackedPBRPixel_t &pixel : rPixels ) {
		rData.set( nOffset++, quantizeU8( pixel.normal.x * 0.5f + 0.5f ) );
		rData.set( nOffset++, quantizeU8( pixel.normal.y * 0.5f + 0.5f ) );
		rData.set( nOffset++, quantizeU8( pixel.fRoughness ) );
		rData.set( nOffset++, quantizeU8( pixel.ao ) );
	}
}

static bool terrain3dValidateOptionalImage(
	const Ref<Image> &rImage, const Vector2i &rSize,
	const char *pszName
) {
	if ( rImage.is_null() ) {
		return true;
	}
	if ( rImage->is_empty() || rImage->get_size() != rSize ) {
		UtilityFunctions::push_error( "T3DUtil: ", pszName, " image size must match the normal image size: ", rSize );
		return false;
	}
	return true;
}

typedef struct t3dPackedPBRBuildContext_s {
	const uint8_t *normal = nullptr;
	const uint8_t *fRoughness = nullptr;
	const uint8_t *displacement = nullptr;
	const uint8_t *ao = nullptr;
	const uint8_t *metallic = nullptr;
	eastl::vector<t3dPackedPBRPixel_t> *output = nullptr;
	bool invert_green = false;
	int roughness_channel = 0;
	int displacement_channel = 0;
	int ao_channel = 0;
	int metallic_channel = 0;
} t3dPackedPBRBuildContext_t;

static void terrain3dBuildPackedPbrRange( void *pContext, const size_t nBegin, const size_t nEnd, int32_t ) {
	t3dPackedPBRBuildContext_t *context = ( t3dPackedPBRBuildContext_t * ) ( pContext );
	for ( size_t index = nBegin; index < nEnd; ++index ) {
		real_t fNx = rgbaFloatChannel( context->normal, index, 0 ) * 2.0f - 1.0f;
		const real_t fGreen = rgbaFloatChannel( context->normal, index, 1 );
		real_t fNy = ( context->invert_green ? 1.0f - fGreen : fGreen ) * 2.0f - 1.0f;
		const real_t fXyLengthSquared = fNx * fNx + fNy * fNy;
		if ( fXyLengthSquared > 1.0f ) {
			const real_t fInvLength = 1.0f / Math::sqrt( fXyLengthSquared );
			fNx *= fInvLength;
			fNy *= fInvLength;
		}
		const real_t fNz = Math::sqrt( MAX( 1.0f - fNx * fNx - fNy * fNy, 0.0f ) );
		t3dPackedPBRPixel_t &pixel = ( *context->output )[index];
		pixel.normal = Vector3( fNx, fNy, fNz ).normalized();
		pixel.fRoughness = CLAMP( rgbaFloatChannel( context->fRoughness, index, context->roughness_channel ), 0.0f, 1.0f );
		pixel.displacement = CLAMP( rgbaFloatChannel( context->displacement, index, context->displacement_channel ), 0.0f, 1.0f );
		pixel.ao = context->ao ? CLAMP( rgbaFloatChannel( context->ao, index, context->ao_channel ), 0.0f, 1.0f ) : 1.0f;
		pixel.metallic = context->metallic
			? CLAMP( rgbaFloatChannel( context->metallic, index, context->metallic_channel ), 0.0f, 1.0f )
			: 0.0f;
	}
}

typedef struct t3dSurfaceBuildContext_s {
	const uint8_t *normal = nullptr;
	const uint8_t *fRoughness = nullptr;
	const uint8_t *ao = nullptr;
	eastl::vector<t3dPackedPBRPixel_t> *output = nullptr;
	bool invert_green = false;
	int roughness_channel = 0;
	int ao_channel = 0;
} t3dSurfaceBuildContext_t;

static void terrain3dBuildSurfaceRange( void *pContext, const size_t nBegin, const size_t nEnd, int32_t ) {
	t3dSurfaceBuildContext_t *context = ( t3dSurfaceBuildContext_t * ) ( pContext );
	for ( size_t index = nBegin; index < nEnd; ++index ) {
		real_t fNx = rgbaFloatChannel( context->normal, index, 0 ) * 2.0f - 1.0f;
		const real_t fGreen = rgbaFloatChannel( context->normal, index, 1 );
		real_t fNy = ( context->invert_green ? 1.0f - fGreen : fGreen ) * 2.0f - 1.0f;
		const real_t fLengthSquared = fNx * fNx + fNy * fNy;
		if ( fLengthSquared > 1.0f ) {
			const real_t fInvLength = 1.0f / Math::sqrt( fLengthSquared );
			fNx *= fInvLength;
			fNy *= fInvLength;
		}
		const real_t fNz = Math::sqrt( MAX( 1.0f - fNx * fNx - fNy * fNy, 0.0f ) );
		t3dPackedPBRPixel_t &pixel = ( *context->output )[index];
		pixel.normal = Vector3( fNx, fNy, fNz ).normalized();
		pixel.fRoughness = CLAMP( rgbaFloatChannel( context->fRoughness, index, context->roughness_channel ), 0.0f, 1.0f );
		pixel.ao = context->ao ? CLAMP( rgbaFloatChannel( context->ao, index, context->ao_channel ), 0.0f, 1.0f ) : 1.0f;
	}
}

typedef struct t3dPackedMipContext_s {
	const eastl::vector<t3dPackedPBRPixel_t> *source = nullptr;
	eastl::vector<t3dPackedPBRPixel_t> *destination = nullptr;
	int source_width = 0;
	int source_height = 0;
	int destination_width = 0;
} t3dPackedMipContext_t;

static void terrain3dBuildPackedMipRows( void *pContext, const size_t nBegin, const size_t nEnd, int32_t ) {
	t3dPackedMipContext_t *context = ( t3dPackedMipContext_t * ) ( pContext );
	for ( size_t y = nBegin; y < nEnd; ++y ) {
		for ( int x = 0; x < context->destination_width; ++x ) {
			( *context->destination )[y * ( size_t ) ( context->destination_width ) + ( size_t ) ( x )] =
				averagePackedPbrPixels( *context->source, context->source_width, context->source_height, x, ( int ) ( y ) );
		}
	}
}

typedef struct t3dDisplacementBuildContext_s {
	const uint8_t *source = nullptr;
	uint8_t *destination = nullptr;
	int channel = 0;
} t3dDisplacementBuildContext_t;

static void terrain3dBuildDisplacementRange( void *pContext, const size_t nBegin, const size_t nEnd, int32_t ) {
	t3dDisplacementBuildContext_t *context = ( t3dDisplacementBuildContext_t * ) ( pContext );
	for ( size_t index = nBegin; index < nEnd; ++index ) {
		const float fValue = CLAMP( rgbaFloatChannel( context->source, index, context->channel ), 0.0f, 1.0f );
		context->destination[index] = quantizeU8( fValue );
	}
}

} // namespace

///////////////////////////
// Public Functions
///////////////////////////

void T3DUtil::printArr( const String &strName, const Array &rArr, const int nLevel ) {
	LOG( nLevel, "Array[", rArr.size(), "]: ", strName );
	for ( int i = 0; i < rArr.size(); i++ ) {
		const Variant var = rArr[i];
		switch ( var.get_type() ) {
			case Variant::ARRAY: {
				printArr( strName + String::num_int64( i ), var, nLevel );
				break;
			}
			case Variant::DICTIONARY: {
				printDict( strName + String::num_int64( i ), var, nLevel );
				break;
			}
			case Variant::OBJECT: {
				Object *const pObj = cast_to<Object>( var );
				const String str = "Object#" + String::num_uint64( pObj->get_instance_id() ) + ", " + ptrToStr( pObj );
				LOG( nLevel, i, ": ", str );
				break;
			}
			default: {
				LOG( nLevel, i, ": ", rArr[i] );
				break;
			}
		}
	}
}

void T3DUtil::printDict( const String &strName, const Dictionary &rDict, const int nLevel ) {
	LOG( nLevel, "Dictionary: ", strName );
	const Array keys = rDict.keys();
	for ( const StringName key : keys ) {
		const Variant var = rDict[key];
		switch ( var.get_type() ) {
			case Variant::ARRAY: {
				printArr( String( key ), var, nLevel );
				break;
			}
			case Variant::DICTIONARY: {
				printDict( String( key ), var, nLevel );
				break;
			}
			case Variant::OBJECT: {
				Object *const pObj = cast_to<Object>( var );
				const String str = "Object#" + String::num_uint64( pObj->get_instance_id() ) + ", " + ptrToStr( pObj );
				LOG( nLevel, "\"", key, "\": ", str );
				break;
			}
			default: {
				LOG( nLevel, "\"", key, "\": Value: ", var );
				break;
			}
		}
	}
}

void T3DUtil::dumpGentex( const T3DGeneratedTexture &rGen, const String &strName ) {
	LOG( MESG, "Generated ", strName, " RID: ", rGen.getRid(), ", dirty: ", rGen.isDirty(),
		", image: ", ptrToStr( *rGen.getImage() ) );
}

void T3DUtil::dumpMaps( const TypedArray<Image> &rMaps, const String &strName ) {
	LOG( MESG, "Dumping ", strName, " array. Size: ", rMaps.size() );
	for ( const Ref<Image> pImg : rMaps ) {
		LOG( MESG, "Map size: ", pImg->get_size(), ", eFormat: ", pImg->get_format(), ", ", ptrToStr( *pImg ) );
	}
}

// Expects a filename in a String like: "terrain3d-01_02.res" which returns (-1, 2)
Vector2i T3DUtil::filenameToLocation( const String &rFilename ) {
	String location_string = rFilename.trim_prefix( "terrain3d" ).trim_suffix( ".res" );
	return stringToLocation( location_string );
}

// Expects a string formatted as: "±##±##" which returns (##,##)
Vector2i T3DUtil::stringToLocation( const String &rString ) {
	String x_str = rString.left( 3 ).replace( "_", "" );
	String y_str = rString.right( 3 ).replace( "_", "" );
	if ( !x_str.is_valid_int() || !y_str.is_valid_int() ) {
		LOG( ERROR, "Malformed string '", rString, "'. Result: ", x_str, ", ", y_str );
		return V2I_MAX;
	}
	return Vector2i( x_str.to_int(), y_str.to_int() );
}

// Expects a v2i(-1,2) and returns terrain3d-01_02.res
String T3DUtil::locationToFilename( const Vector2i &rRegionLoc ) {
	return "terrain3d" + locationToString( rRegionLoc ) + ".res";
}

// Expects a v2i(-1,2) and returns -01_02
String T3DUtil::locationToString( const Vector2i &rRegionLoc ) {
	const String POS_REGION_FORMAT = "_%02d";
	const String NEG_REGION_FORMAT = "%03d";
	String x_str, y_str;
	x_str = vformat( ( rRegionLoc.x >= 0 ) ? POS_REGION_FORMAT : NEG_REGION_FORMAT, rRegionLoc.x );
	y_str = vformat( ( rRegionLoc.y >= 0 ) ? POS_REGION_FORMAT : NEG_REGION_FORMAT, rRegionLoc.y );
	return x_str + y_str;
}

PackedStringArray T3DUtil::getFiles( const String &rDir, const String &rGlob ) {
	PackedStringArray files;
	Ref<DirAccess> pDa = DirAccess::open( rDir );
	if ( pDa.is_null() ) {
		LOG( ERROR, "Cannot open directory: ", rDir );
		return files;
	}
	const PackedStringArray dir_files = pDa->get_files();
	for ( const String &file_name : dir_files ) {
		const String fname = file_name.trim_suffix( ".remap" );
		if ( !fname.matchn( rGlob ) ) {
			continue;
		}
		LOG( DEBUG, "Found file: ", rDir + String( "/" ) + fname );
		files.push_back( fname );
	}
	return files;
}

Ref<Image> T3DUtil::blackToAlpha( const Ref<Image> &rImage ) {
	const Ref<Image> pSource = prepareSourceImage( rImage );
	if ( pSource.is_null() ) {
		return Ref<Image>();
	}
	Ref<Image> pImg = Image::create_empty( pSource->get_width(), pSource->get_height(), false, Image::FORMAT_RGBAF );
	for ( int y = 0; y < pImg->get_height(); y++ ) {
		for ( int x = 0; x < pImg->get_width(); x++ ) {
			Color pixel = pSource->get_pixel( x, y );
			pixel.a = pixel.get_luminance();
			pImg->set_pixel( x, y, pixel );
		}
	}
	if ( pSource->has_mipmaps() ) {
		pImg->generate_mipmaps();
	}
	return pImg;
}

/**
 * Returns the minimum and maximum values for a heightmap (red channel only)
 */
Vector2 T3DUtil::getMinMax( const Ref<Image> &rImage ) {
	if ( rImage.is_null() ) {
		LOG( ERROR, "Provided image is not valid. Nothing to analyze" );
		return V2( INFINITY );
	} else if ( rImage->is_empty() ) {
		LOG( ERROR, "Provided image is empty. Nothing to analyze" );
		return V2( INFINITY );
	}

	const Ref<Image> pSource = prepareSourceImage( rImage );
	if ( pSource.is_null() ) {
		LOG( ERROR, "Provided image could not be decompressed. Nothing to analyze" );
		return V2( INFINITY );
	}
	Vector2 min_max = Vector2( FLT_MAX, -FLT_MAX );

	for ( int y = 0; y < pSource->get_height(); y++ ) {
		for ( int x = 0; x < pSource->get_width(); x++ ) {
			const Color col = pSource->get_pixel( x, y );
			if ( col.r < min_max.x ) {
				min_max.x = col.r;
			}
			if ( col.r > min_max.y ) {
				min_max.y = col.r;
			}
		}
	}

	LOG( INFO, "Calculating minimum and maximum values of the image: ", min_max );
	return min_max;
}

/**
 * Returns a Image of a float heightmap normalized to RGB8 greyscale and scaled
 * Minimum of 8x8
 */
Ref<Image> T3DUtil::getThumbnail( const Ref<Image> &rImage, const Vector2i &rSize ) {
	if ( rImage.is_null() ) {
		LOG( ERROR, "Provided image is not valid. Nothing to process" );
		return Ref<Image>();
	} else if ( rImage->is_empty() ) {
		LOG( ERROR, "Provided image is empty. Nothing to process" );
		return Ref<Image>();
	}
	const Vector2i size = Vector2i( CLAMP( rSize.x, 8, 16384 ), CLAMP( rSize.y, 8, 16384 ) );

	LOG( INFO, "Drawing a thumbnail sized: ", size );
	// Create a temporary work image scaled to desired width
	Ref<Image> pImg = prepareSourceImage( rImage );
	if ( pImg.is_null() ) {
		LOG( ERROR, "Provided image could not be decompressed. Cannot create thumbnail" );
		return Ref<Image>();
	}
	pImg->resize( size.x, size.y, Image::INTERPOLATE_LANCZOS );

	// Get minimum and maximum height values on the scaled image
	const Vector2 minmax = getMinMax( pImg );
	real_t fHmin = minmax.x;
	real_t fHmax = minmax.y;
	// Define maximum range
	fHmin = std::abs( fHmin );
	fHmax = std::abs( fHmax ) + fHmin;
	// Avoid divide by zero
	fHmax = ( fHmax == 0 ) ? 0.001f : fHmax;

	// Create a new image w / normalized values
	Ref<Image> pThumb = Image::create_empty( size.x, size.y, false, Image::FORMAT_RGB8 );
	for ( int y = 0; y < pThumb->get_height(); y++ ) {
		for ( int x = 0; x < pThumb->get_width(); x++ ) {
			Color col = pImg->get_pixel( x, y );
			col.r = ( col.r + fHmin ) / fHmax;
			col.g = col.r;
			col.b = col.r;
			pThumb->set_pixel( x, y, col );
		}
	}
	return pThumb;
}

/* Get an Image filled with specified color and eFormat
 * If p_color.a < 0, fill with checkered pattern multiplied by p_color.rgb
 *
 * Behavior changes if a compressed eFormat is requested:
 * If the editor is running and eFormat is DXT1/5, BPTC_RGBA, it returns a filled image.
 * Otherwise, it returns a blank image in that eFormat.
 *
 * The reason is the Image compression library is available only in the editor. And it is
 * unreliable, offering little control over the output eFormat, choosing automatically and
 * often wrong. We have selected a few compressed formats it gets right.
 */
Ref<Image> T3DUtil::getFilledImage(
	const Vector2i &rSize, const Color &rColor, const bool bCreateMipmaps,
	const Image::Format eRequestedFormat
) {
	Image::Format eFormat = eRequestedFormat;
	if ( eFormat < 0 || eFormat >= Image::FORMAT_MAX ) {
		eFormat = Image::FORMAT_DXT5;
	}

	Image::CompressMode compression_format = Image::COMPRESS_MAX;
	Image::UsedChannels channels = Image::USED_CHANNELS_RGBA;
	bool bCompress = false;
	bool bFillImage = true;

	if ( eFormat >= Image::Format::FORMAT_DXT1 ) {
		switch ( eFormat ) {
			case Image::FORMAT_DXT1:
				eFormat = Image::FORMAT_RGB8;
				channels = Image::USED_CHANNELS_RGB;
				compression_format = Image::COMPRESS_S3TC;
				bCompress = true;
				break;
			case Image::FORMAT_DXT5:
				eFormat = Image::FORMAT_RGBA8;
				channels = Image::USED_CHANNELS_RGBA;
				compression_format = Image::COMPRESS_S3TC;
				bCompress = true;
				break;
			case Image::FORMAT_BPTC_RGBA:
				eFormat = Image::FORMAT_RGBA8;
				channels = Image::USED_CHANNELS_RGBA;
				compression_format = Image::COMPRESS_BPTC;
				bCompress = true;
				break;
			default:
				bCompress = false;
				bFillImage = false;
				break;
		}
	}

	Ref<Image> pImg = Image::create_empty( rSize.x, rSize.y, bCreateMipmaps, eFormat );

	Color color = rColor;
	if ( bFillImage ) {
		if ( color.a < 0.0f ) {
			color.a = 1.0f;
			const Color col_a = Color( 0.8f, 0.8f, 0.8f, 1.0 ) * color;
			const Color col_b = Color( 0.5f, 0.5f, 0.5f, 1.0 ) * color;
			pImg->fill_rect( Rect2i( V2I_ZERO, rSize / 2 ), col_a );
			pImg->fill_rect( Rect2i( rSize / 2, rSize / 2 ), col_a );
			pImg->fill_rect( Rect2i( Vector2( rSize.x, 0 ) / 2, rSize / 2 ), col_b );
			pImg->fill_rect( Rect2i( Vector2( 0, rSize.y ) / 2, rSize / 2 ), col_b );
		} else {
			pImg->fill( color );
		}
		if ( bCreateMipmaps ) {
			pImg->generate_mipmaps();
		}
	}
	if ( bCompress && IS_EDITOR ) {
		pImg->compress_from_channels( compression_format, channels );
	}
	return pImg;
}

/**
 * Loads a file from disk and returns an Image
 * Parameters:
 *	p_filename - file on disk to load. EXR, R16/RAW, PNG, or a ResourceLoader eFormat (jpg, res, tres, etc)
 *	p_cache_mode - Send this flag to the resource loader to force caching or not
 *	p_height_range - R16 eFormat: x=Min & y=Max value ranges. Required for R16 import
 *	p_size - R16 eFormat: Image dimensions. Default (0,0) auto detects f/ square images. Required f/ non-square R16
 */
Ref<Image> T3DUtil::loadImage(
	const String &rFileName, const int nCacheMode,
	const Vector2 &rR16HeightRange, const Vector2i &rR16Size
) {
	if ( rFileName.is_empty() ) {
		LOG( ERROR, "No file specified. Nothing imported" );
		return Ref<Image>();
	}
	if ( !FileAccess::file_exists( rFileName ) ) {
		LOG( ERROR, "File ", rFileName, " does not exist. Nothing to import" );
		return Ref<Image>();
	}

	// Load file based on extension
	Ref<Image> pImg;
	LOG( INFO, "Attempting to load: ", rFileName );
	String ext = rFileName.get_extension().to_lower();
	const PackedStringArray imgloader_extensions =
		PackedStringArray( Array::make( "bmp", "dds", "exr", "hdr", "jpg", "jpeg", "png", "tga", "svg", "webp" ) );

	// If R16 integer eFormat (read/writeable by Krita)
	if ( ext == "r16" || ext == "raw" ) {
		LOG( DEBUG, "Loading file as an r16" );
		Ref<FileAccess> pFile = FileAccess::open( rFileName, FileAccess::READ );
		// If p_size is zero, assume square and try to auto detect size
		Vector2i r16_size = rR16Size;
		if ( r16_size <= V2I_ZERO ) {
			pFile->seek_end();
			const int fsize = pFile->get_position();
			const int fwidth = sqrt( fsize / 2 );
			r16_size = V2I( fwidth );
			LOG( DEBUG, "Total file size is: ", fsize, " calculated width: ", fwidth, " dimensions: ", r16_size );
			pFile->seek( 0 );
		}
		pImg = Image::create_empty( r16_size.x, r16_size.y, false, Image::FORMAT_RF );
		for ( int y = 0; y < r16_size.y; y++ ) {
			for ( int x = 0; x < r16_size.x; x++ ) {
				real_t fH = ( real_t ) ( pFile->get_16() ) / 65535.0f;
				fH = fH * ( rR16HeightRange.y - rR16HeightRange.x ) + rR16HeightRange.x;
				pImg->set_pixel( x, y, Color( fH, 0.f, 0.f ) );
			}
		}

		// If an Image extension, use Image loader
	} else if ( imgloader_extensions.has( ext ) ) {
		LOG( DEBUG, "ImageFormatLoader loading recognized file type: ", ext );
		pImg = Image::load_from_file( rFileName );

		// Else, see if Godot's resource loader will read it as an image: RES, TRES, etc
	} else {
		LOG( DEBUG, "Loading file as a resource" );
		pImg = ResourceLoader::get_singleton()->load( rFileName, "",
													( ResourceLoader::CacheMode ) ( nCacheMode ) );
	}

	if ( !pImg.is_valid() ) {
		LOG( ERROR, "File", rFileName, " cannot be loaded" );
		return Ref<Image>();
	}
	if ( pImg->is_empty() ) {
		LOG( ERROR, "File", rFileName, " is empty" );
		return Ref<Image>();
	}
	LOG( DEBUG, "Loaded Image size: ", pImg->get_size(), " eFormat: ", pImg->get_format() );
	return pImg;
}

/* From source RGB and selected source for Alpha channel, create a new RGBA image.
 * If p_invert_green is true, the destination green channel will be 1.0 - input green channel.
 * If p_invert_alpha is true, the destination alpha channel will be 1.0 - input source channel.
 */
Ref<Image> T3DUtil::packImage(
	const Ref<Image> &rSrcRgb, const Ref<Image> &rSrcA, const Ref<Image> &rSrcAo,
	const bool bInvertGreen, const bool bInvertAlpha, const bool bNormalizeAlpha,
	const int nAlphaChannel, const int nAoChannel
) {
	if ( !rSrcRgb.is_valid() || !rSrcA.is_valid() ) {
		LOG( ERROR, "Provided images are not valid. Cannot pack" );
		return Ref<Image>();
	}
	const Ref<Image> pSrcRgb = prepareSourceImage( rSrcRgb );
	const Ref<Image> pSrcA = prepareSourceImage( rSrcA );
	const Ref<Image> pSrcAo = rSrcAo.is_valid() ? prepareSourceImage( rSrcAo ) : Ref<Image>();
	if ( pSrcRgb.is_null() || pSrcA.is_null() || ( rSrcAo.is_valid() && pSrcAo.is_null() ) ) {
		LOG( ERROR, "A source image could not be decompressed. Cannot pack" );
		return Ref<Image>();
	}
	if ( rSrcRgb->get_size() != rSrcA->get_size() ) {
		LOG( ERROR, "Provided images are not the same size. Cannot pack" );
		return Ref<Image>();
	}
	if ( rSrcRgb->is_empty() || rSrcA->is_empty() ) {
		LOG( ERROR, "Provided images are empty. Cannot pack" );
		return Ref<Image>();
	}
	if ( nAlphaChannel < 0 || nAlphaChannel > 3 ) {
		LOG( ERROR, "Source t3dChannel_t of Height/Roughness invalid. Cannot Pack" );
		return Ref<Image>();
	}

	const bool bPackAo = pSrcAo.is_valid();
	if ( bPackAo ) {
		if ( rSrcRgb->get_size() != rSrcAo->get_size() ) {
			LOG( ERROR, "Provided AO and normal images are not the same size. Cannot pack" );
			return Ref<Image>();
		}
	}

	real_t fAMax = 0.0f;
	real_t fAMin = 0.0f;
	real_t fContrast = 1.0f;
	if ( bNormalizeAlpha ) {
		fAMin = 1.0f;
		// Calculate contrast and offset so that we can make full use of the height channel range.
		for ( int y = 0; y < rSrcRgb->get_height(); y++ ) {
			for ( int x = 0; x < rSrcRgb->get_width(); x++ ) {
				const real_t fH = pSrcA->get_pixel( x, y )[nAlphaChannel];
				fAMax = MAX( fH, fAMax );
				fAMin = MIN( fH, fAMin );
			}
		}
		fContrast /= MAX( fAMax - fAMin, 1e-6f );
	}

	Ref<Image> pDst = Image::create_empty( rSrcRgb->get_width(), rSrcRgb->get_height(), false, Image::FORMAT_RGBA8 );
	LOG( INFO, "Creating image from source RGB + source channel images" );
	for ( int y = 0; y < rSrcRgb->get_height(); y++ ) {
		for ( int x = 0; x < rSrcRgb->get_width(); x++ ) {
			Color col = pSrcRgb->get_pixel( x, y );
			col.a = pSrcA->get_pixel( x, y )[nAlphaChannel];
			if ( bNormalizeAlpha ) {
				col.a = CLAMP( ( col.a - fAMin ) * fContrast, 0.0f, 1.0f );
			}
			if ( bInvertGreen ) {
				col.g = 1.0f - col.g;
			}
			if ( bInvertAlpha ) {
				col.a = 1.0f - col.a;
			}
			if ( bPackAo ) {
				// Compress range to avoid low AO values completely destroying normal vector precision - recovered in
				// shader.
				const real_t fAo = sqrt( pSrcAo->get_pixel( x, y )[nAoChannel] ) * 0.5 + 0.5;
				col.r = col.r * fAo + ( 1.0f - fAo ) * 0.5f;
				col.g = col.g * fAo + ( 1.0f - fAo ) * 0.5f;
				col.b = col.b * fAo + ( 1.0f - fAo ) * 0.5f;
			}
			pDst->set_pixel( x, y, col );
		}
	}
	return pDst;
}

Ref<Image> T3DUtil::packPbrRgba16ui(
	const Ref<Image> &rNormal, const Ref<Image> &rRoughness,
	const Ref<Image> &rDisplacement, const Ref<Image> &rAo,
	const Ref<Image> &rMetallic, const bool bInvertNormalGreen,
	const int nRoughnessChannel, const int nDisplacementChannel,
	const int nAoChannel, const int nMetallicChannel,
	const bool bGenerateMipmaps
) {
	if ( rNormal.is_null() || rRoughness.is_null() || rDisplacement.is_null() ) {
		LOG( ERROR, "Normal, roughness, and displacement images are required for packed PBR generation." );
		return Ref<Image>();
	}
	const int channels[] = {nRoughnessChannel, nDisplacementChannel, nAoChannel, nMetallicChannel};
	for ( const int channel : channels ) {
		if ( channel < 0 || channel > 3 ) {
			LOG( ERROR, "Packed PBR source channel must be between 0 and 3." );
			return Ref<Image>();
		}
	}

	const Ref<Image> pNormal = prepareRgbaFloatSource( rNormal );
	const Ref<Image> pRoughness = prepareRgbaFloatSource( rRoughness );
	const Ref<Image> pDisplacement = prepareRgbaFloatSource( rDisplacement );
	const Ref<Image> pAo = prepareRgbaFloatSource( rAo );
	const Ref<Image> pMetallic = prepareRgbaFloatSource( rMetallic );
	if ( pNormal.is_null() || pRoughness.is_null() || pDisplacement.is_null() ) {
		LOG( ERROR, "A packed PBR source image could not be decompressed." );
		return Ref<Image>();
	}
	if ( pNormal->is_empty() || pRoughness->is_empty() || pDisplacement->is_empty() ) {
		LOG( ERROR, "Packed PBR source images cannot be empty." );
		return Ref<Image>();
	}

	const Vector2i size = pNormal->get_size();
	if ( pRoughness->get_size() != size || pDisplacement->get_size() != size || !terrain3dValidateOptionalImage( pAo, size, "AO" ) ||
		!terrain3dValidateOptionalImage( pMetallic, size, "Metallic" ) ) {
		LOG( ERROR, "All packed PBR source images must have identical dimensions." );
		return Ref<Image>();
	}
	if ( !is_power_of_2( size.x ) || !is_power_of_2( size.y ) ) {
		LOG( WARN, "Packed PBR image dimensions are not powers of two: ", size,
			". Mipmaps remain valid but may be less efficient." );
	}

	const size_t pixel_count = ( size_t ) ( size.x ) * ( size_t ) ( size.y );
	eastl::vector<t3dPackedPBRPixel_t> levelPixels( pixel_count );
	eastl::vector<t3dPackedPBRPixel_t> mip_scratch;
	mip_scratch.reserve( ( size_t ) ( MAX( size.x / 2, 1 ) ) * ( size_t ) ( MAX( size.y / 2, 1 ) ) );
	const PackedByteArray normal_data = pNormal->get_data();
	const PackedByteArray roughness_data = pRoughness->get_data();
	const PackedByteArray displacement_data = pDisplacement->get_data();
	const PackedByteArray ao_data = pAo.is_valid() ? pAo->get_data() : PackedByteArray();
	const PackedByteArray metallic_data = pMetallic.is_valid() ? pMetallic->get_data() : PackedByteArray();
	const uint8_t *pNormalBytes = normal_data.ptr();
	const uint8_t *pRoughnessBytes = roughness_data.ptr();
	const uint8_t *pDisplacementBytes = displacement_data.ptr();
	const uint8_t *pAoBytes = ao_data.is_empty() ? nullptr : ao_data.ptr();
	const uint8_t *pMetallicBytes = metallic_data.is_empty() ? nullptr : metallic_data.ptr();
	const T3DParallelExecutor executor = authoringExecutor( pixel_count );
	t3dPackedPBRBuildContext_t build_context;
	build_context.normal = pNormalBytes;
	build_context.fRoughness = pRoughnessBytes;
	build_context.displacement = pDisplacementBytes;
	build_context.ao = pAoBytes;
	build_context.metallic = pMetallicBytes;
	build_context.output = &levelPixels;
	build_context.invert_green = bInvertNormalGreen;
	build_context.roughness_channel = nRoughnessChannel;
	build_context.displacement_channel = nDisplacementChannel;
	build_context.ao_channel = nAoChannel;
	build_context.metallic_channel = nMetallicChannel;
	executor.runStatic( pixel_count, 256u, terrain3dBuildPackedPbrRange, &build_context );

	int mip_count = 1;
	int mip_width = size.x;
	int mip_height = size.y;
	int64_t total_bytes = ( int64_t ) ( mip_width ) * ( int64_t ) ( mip_height ) * 8;
	if ( bGenerateMipmaps ) {
		while ( mip_width > 1 || mip_height > 1 ) {
			mip_width = MAX( mip_width / 2, 1 );
			mip_height = MAX( mip_height / 2, 1 );
			total_bytes += ( int64_t ) ( mip_width ) * ( int64_t ) ( mip_height ) * 8;
			mip_count++;
		}
	}

	PackedByteArray data;
	data.resize( total_bytes );
	int64_t byte_offset = 0;
	int current_width = size.x;
	int current_height = size.y;
	appendPackedPbrLevel( data, byte_offset, levelPixels );

	for ( int mip = 1; mip < mip_count; mip++ ) {
		const int next_width = MAX( current_width / 2, 1 );
		const int next_height = MAX( current_height / 2, 1 );
		mip_scratch.resize( ( size_t ) ( next_width ) * ( size_t ) ( next_height ) );
		t3dPackedMipContext_t mip_context;
		mip_context.source = &levelPixels;
		mip_context.destination = &mip_scratch;
		mip_context.source_width = current_width;
		mip_context.source_height = current_height;
		mip_context.destination_width = next_width;
		executor.runStatic( ( size_t ) ( next_height ), 1u, terrain3dBuildPackedMipRows, &mip_context );
		appendPackedPbrLevel( data, byte_offset, mip_scratch );
		levelPixels.swap( mip_scratch );
		current_width = next_width;
		current_height = next_height;
	}

	if ( byte_offset != total_bytes ) {
		LOG( ERROR, "Packed PBR byte count mismatch. Expected ", total_bytes, ", wrote ", byte_offset );
		return Ref<Image>();
	}
	return Image::create_from_data( size.x, size.y, mip_count > 1, Image::FORMAT_RGBA16I, data );
}

Ref<Image> T3DUtil::getDefaultPackedPbrImage( const Vector2i &rSize, const bool bCreateMipmaps ) {
	if ( rSize.x <= 0 || rSize.y <= 0 ) {
		return Ref<Image>();
	}
	Ref<Image> pNormal = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_RGB8 );
	Ref<Image> pRoughness = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_L8 );
	Ref<Image> pDisplacement = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_L8 );
	pNormal->fill( Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
	pRoughness->fill( Color( 1.0f, 1.0f, 1.0f, 1.0f ) );
	pDisplacement->fill( Color( 0.5f, 0.5f, 0.5f, 1.0f ) );
	return packPbrRgba16ui( pNormal, pRoughness, pDisplacement, Ref<Image>(), Ref<Image>(), false, 0, 0, 0, 0,
							 bCreateMipmaps );
}

Ref<Image> T3DUtil::packSurfaceRgba8(
	const Ref<Image> &rNormal, const Ref<Image> &rRoughness,
	const Ref<Image> &rAo, const bool bInvertNormalGreen,
	const int nRoughnessChannel, const int nAoChannel,
	const bool bGenerateMipmaps
) {
	if ( rNormal.is_null() || rRoughness.is_null() ) {
		LOG( ERROR, "Normal and roughness images are required for surface texture generation." );
		return Ref<Image>();
	}
	if ( nRoughnessChannel < 0 || nRoughnessChannel > 3 || nAoChannel < 0 || nAoChannel > 3 ) {
		LOG( ERROR, "Surface source channels must be between 0 and 3." );
		return Ref<Image>();
	}
	const Ref<Image> pNormal = prepareRgbaFloatSource( rNormal );
	const Ref<Image> pRoughness = prepareRgbaFloatSource( rRoughness );
	const Ref<Image> pAo = prepareRgbaFloatSource( rAo );
	if ( pNormal.is_null() || pRoughness.is_null() || pNormal->is_empty() || pRoughness->is_empty() ) {
		LOG( ERROR, "A surface source image could not be prepared." );
		return Ref<Image>();
	}
	const Vector2i size = pNormal->get_size();
	if ( pRoughness->get_size() != size || ( pAo.is_valid() && ( pAo->is_empty() || pAo->get_size() != size ) ) ) {
		LOG( ERROR, "All surface source images must have identical dimensions." );
		return Ref<Image>();
	}

	const size_t pixel_count = ( size_t ) ( size.x ) * ( size_t ) ( size.y );
	eastl::vector<t3dPackedPBRPixel_t> levelPixels( pixel_count );
	eastl::vector<t3dPackedPBRPixel_t> mip_scratch;
	mip_scratch.reserve( ( size_t ) ( MAX( size.x / 2, 1 ) ) * ( size_t ) ( MAX( size.y / 2, 1 ) ) );
	const PackedByteArray normal_data = pNormal->get_data();
	const PackedByteArray roughness_data = pRoughness->get_data();
	const PackedByteArray ao_data = pAo.is_valid() ? pAo->get_data() : PackedByteArray();
	const uint8_t *pNormalBytes = normal_data.ptr();
	const uint8_t *pRoughnessBytes = roughness_data.ptr();
	const uint8_t *pAoBytes = ao_data.is_empty() ? nullptr : ao_data.ptr();
	const T3DParallelExecutor executor = authoringExecutor( pixel_count );
	t3dSurfaceBuildContext_t surface_context;
	surface_context.normal = pNormalBytes;
	surface_context.fRoughness = pRoughnessBytes;
	surface_context.ao = pAoBytes;
	surface_context.output = &levelPixels;
	surface_context.invert_green = bInvertNormalGreen;
	surface_context.roughness_channel = nRoughnessChannel;
	surface_context.ao_channel = nAoChannel;
	executor.runStatic( pixel_count, 256u, terrain3dBuildSurfaceRange, &surface_context );

	int mip_count = 1;
	int mip_width = size.x;
	int mip_height = size.y;
	int64_t total_bytes = ( int64_t ) ( mip_width ) * ( int64_t ) ( mip_height ) * 4;
	if ( bGenerateMipmaps ) {
		while ( mip_width > 1 || mip_height > 1 ) {
			mip_width = MAX( mip_width / 2, 1 );
			mip_height = MAX( mip_height / 2, 1 );
			total_bytes += ( int64_t ) ( mip_width ) * ( int64_t ) ( mip_height ) * 4;
			mip_count++;
		}
	}
	PackedByteArray data;
	data.resize( total_bytes );
	int64_t byte_offset = 0;
	int current_width = size.x;
	int current_height = size.y;
	appendSurfaceLevel( data, byte_offset, levelPixels );
	for ( int mip = 1; mip < mip_count; mip++ ) {
		const int next_width = MAX( current_width / 2, 1 );
		const int next_height = MAX( current_height / 2, 1 );
		mip_scratch.resize( ( size_t ) ( next_width ) * ( size_t ) ( next_height ) );
		t3dPackedMipContext_t mip_context;
		mip_context.source = &levelPixels;
		mip_context.destination = &mip_scratch;
		mip_context.source_width = current_width;
		mip_context.source_height = current_height;
		mip_context.destination_width = next_width;
		executor.runStatic( ( size_t ) ( next_height ), 1u, terrain3dBuildPackedMipRows, &mip_context );
		appendSurfaceLevel( data, byte_offset, mip_scratch );
		levelPixels.swap( mip_scratch );
		current_width = next_width;
		current_height = next_height;
	}
	return Image::create_from_data( size.x, size.y, mip_count > 1, Image::FORMAT_RGBA8, data );
}

Ref<Image> T3DUtil::packDisplacementR8(
	const Ref<Image> &rDisplacement, const int nDisplacementChannel,
	const bool bGenerateMipmaps
) {
	if ( rDisplacement.is_null() || nDisplacementChannel < 0 || nDisplacementChannel > 3 ) {
		LOG( ERROR, "A displacement image and source channel 0-3 are required." );
		return Ref<Image>();
	}
	const Ref<Image> pSource = prepareRgbaFloatSource( rDisplacement );
	if ( pSource.is_null() || pSource->is_empty() ) {
		return Ref<Image>();
	}
	const size_t pixel_count = ( size_t ) ( pSource->get_width() ) * ( size_t ) ( pSource->get_height() );
	const PackedByteArray source_data = pSource->get_data();
	const uint8_t *pSourceBytes = source_data.ptr();
	PackedByteArray output;
	output.resize( ( int64_t ) ( pixel_count ) );
	uint8_t *const pDestination = output.ptrw();
	const T3DParallelExecutor executor = authoringExecutor( pixel_count );
	t3dDisplacementBuildContext_t displacement_context;
	displacement_context.source = pSourceBytes;
	displacement_context.destination = pDestination;
	displacement_context.channel = nDisplacementChannel;
	executor.runStatic( pixel_count, 512u, terrain3dBuildDisplacementRange, &displacement_context );
	Ref<Image> pResult = Image::create_from_data(
		pSource->get_width(), pSource->get_height(), false, Image::FORMAT_R8, output );
	if ( bGenerateMipmaps ) {
		pResult->generate_mipmaps();
	}
	return pResult;
}

Ref<Image> T3DUtil::getDefaultSurfaceImage( const Vector2i &rSize, const bool bCreateMipmaps ) {
	if ( rSize.x <= 0 || rSize.y <= 0 ) {
		return Ref<Image>();
	}
	Ref<Image> pResult = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_RGBA8 );
	pResult->fill( Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
	if ( bCreateMipmaps ) {
		pResult->generate_mipmaps( true );
	}
	return pResult;
}

Ref<Image> T3DUtil::getDefaultDisplacementImage( const Vector2i &rSize, const bool bCreateMipmaps ) {
	if ( rSize.x <= 0 || rSize.y <= 0 ) {
		return Ref<Image>();
	}
	Ref<Image> pResult = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_R8 );
	pResult->fill( Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
	if ( bCreateMipmaps ) {
		pResult->generate_mipmaps();
	}
	return pResult;
}

Ref<Image> T3DUtil::convertPackedPbrRgba16uiToSurface(
	const Ref<Image> &rPacked,
	const bool bGenerateMipmaps
) {
	if ( rPacked.is_null() || rPacked->is_empty() || rPacked->get_format() != Image::FORMAT_RGBA16I ) {
		return Ref<Image>();
	}
	const int width = rPacked->get_width();
	const int height = rPacked->get_height();
	const PackedByteArray data = rPacked->get_data();
	if ( data.size() < ( int64_t ) ( width ) * ( int64_t ) ( height ) * 8 ) {
		return Ref<Image>();
	}
	PackedByteArray out;
	out.resize( ( int64_t ) ( width ) * ( int64_t ) ( height ) * 4 );
	int64_t in_offset = 0;
	int64_t out_offset = 0;
	for ( int i = 0; i < width * height; i++ ) {
		const uint16_t r = data.decode_u16( in_offset );
		const uint16_t g = data.decode_u16( in_offset + 2 );
		const uint16_t rough = data.decode_u16( in_offset + 4 );
		out.set( out_offset++, ( uint8_t ) ( r >> 8u ) );
		out.set( out_offset++, ( uint8_t ) ( g >> 8u ) );
		out.set( out_offset++, ( uint8_t ) ( rough >> 8u ) );
		out.set( out_offset++, ( uint8_t ) ( r & 0xffu ) );
		in_offset += 8;
	}
	Ref<Image> pResult = Image::create_from_data( width, height, false, Image::FORMAT_RGBA8, out );
	if ( bGenerateMipmaps ) {
		// Repack through the surface mip generator so normal XY is renormalized.
		Ref<Image> pNormal = Image::create_empty( width, height, false, Image::FORMAT_RGB8 );
		Ref<Image> pRoughness = Image::create_empty( width, height, false, Image::FORMAT_L8 );
		Ref<Image> pAo = Image::create_empty( width, height, false, Image::FORMAT_L8 );
		for ( int y = 0; y < height; y++ ) {
			for ( int x = 0; x < width; x++ ) {
				const Color c = pResult->get_pixel( x, y );
				pNormal->set_pixel( x, y, Color( c.r, c.g, 1.0f, 1.0f ) );
				pRoughness->set_pixel( x, y, Color( c.b, c.b, c.b, 1.0f ) );
				pAo->set_pixel( x, y, Color( c.a, c.a, c.a, 1.0f ) );
			}
		}
		return packSurfaceRgba8( pNormal, pRoughness, pAo, false, 0, 0, true );
	}
	return pResult;
}

Ref<Image> T3DUtil::convertPackedPbrRgba16uiToDisplacement(
	const Ref<Image> &rPacked,
	const bool bGenerateMipmaps
) {
	if ( rPacked.is_null() || rPacked->is_empty() || rPacked->get_format() != Image::FORMAT_RGBA16I ) {
		return Ref<Image>();
	}
	const int width = rPacked->get_width();
	const int height = rPacked->get_height();
	const PackedByteArray data = rPacked->get_data();
	if ( data.size() < ( int64_t ) ( width ) * ( int64_t ) ( height ) * 8 ) {
		return Ref<Image>();
	}
	PackedByteArray out;
	out.resize( ( int64_t ) ( width ) * ( int64_t ) ( height ) );
	int64_t in_offset = 6;
	for ( int i = 0; i < width * height; i++ ) {
		out.set( i, ( uint8_t ) ( data.decode_u16( in_offset ) >> 8u ) );
		in_offset += 8;
	}
	Ref<Image> pResult = Image::create_from_data( width, height, false, Image::FORMAT_R8, out );
	if ( bGenerateMipmaps ) {
		pResult->generate_mipmaps();
	}
	return pResult;
}

Error T3DUtil::saveDisplacementR8Dds( const Ref<Image> &rDisplacement, const String &rPath ) {
	if ( rDisplacement.is_null() || rDisplacement->is_empty() || rDisplacement->get_format() != Image::FORMAT_R8 ) {
		LOG( ERROR, "DDS displacement export requires Image::FORMAT_R8." );
		return ERR_INVALID_DATA;
	}
	return rDisplacement->save_dds( rPath );
}

// From source RGB, create a new L image that is scaled to use full 0 - 1 range.
Ref<Image> T3DUtil::luminanceToHeight( const Ref<Image> &rSrcRgb ) {
	if ( !rSrcRgb.is_valid() ) {
		LOG( ERROR, "Provided images are not valid. Cannot pack" );
		return Ref<Image>();
	}
	if ( rSrcRgb->is_empty() ) {
		LOG( ERROR, "Provided images are empty. Cannot pack" );
		return Ref<Image>();
	}
	const Ref<Image> pSource = prepareSourceImage( rSrcRgb );
	if ( pSource.is_null() ) {
		LOG( ERROR, "Provided image could not be decompressed. Cannot pack" );
		return Ref<Image>();
	}
	real_t fLumContrast;
	real_t fLMax = 0.0f;
	real_t fLMin = 1.0f;
	// Calculate contrast and offset so that we can make the most use of the height channel range.
	for ( int y = 0; y < pSource->get_height(); y++ ) {
		for ( int x = 0; x < pSource->get_width(); x++ ) {
			const Color col = pSource->get_pixel( x, y );
			const real_t fL = 0.299f * col.r + 0.587f * col.g + 0.114f * col.b;
			fLMax = MAX( fL, fLMax );
			fLMin = MIN( fL, fLMin );
		}
	}
	fLumContrast = 1.0f / MAX( fLMax - fLMin, 1e-6 );
	Ref<Image> pDst = Image::create_empty( pSource->get_width(), pSource->get_height(), false, Image::FORMAT_RGB8 );
	for ( int y = 0; y < pSource->get_height(); y++ ) {
		for ( int x = 0; x < pSource->get_width(); x++ ) {
			Color col = pSource->get_pixel( x, y );
			real_t fLum = 0.299f * col.r + 0.587f * col.g + 0.114f * col.b;
			fLum = CLAMP( ( fLum * fLumContrast - fLMin ), 0.0f, 1.0f );
			// some shaping
			col.r = 0.5f - sin( asin( 1.0f - 2.0f * fLum ) / 3.0f );
			col.g = col.r;
			col.b = col.r;
			col.a = col.r;
			pDst->set_pixel( x, y, col );
		}
	}
	return pDst;
}

void T3DUtil::benchmark( T3DTerrain *const pTerrain ) {
	if ( !pTerrain ) {
		return;
	}
	const T3DData *pData = pTerrain->getData();
	if ( !pData ) {
		return;
	}
	uint64_t start_time;
	Vector3 vec;
	Color col;
	for ( int i = 0; i < 3; i++ ) {
		start_time = Time::get_singleton()->get_ticks_msec();
		for ( int j = 0; j < 10000000; j++ ) {
			col = pData->getPixel( TYPE_HEIGHT, vec );
		}
		LOG( MESG, "getPixel() 10M: ", Time::get_singleton()->get_ticks_msec() - start_time, "ms" );
	}

	vec = Vector3( 0.5f, 0.f, 0.5f );
	for ( int i = 0; i < 3; i++ ) {
		start_time = Time::get_singleton()->get_ticks_msec();
		for ( int j = 0; j < 1000000; j++ ) {
			pData->getHeight( vec );
		}
		LOG( MESG, "getHeight() 1M interpolated: ", Time::get_singleton()->get_ticks_msec() - start_time, "ms" );
	}

	for ( int i = 0; i < 2; i++ ) {
		start_time = Time::get_singleton()->get_ticks_msec();
		pTerrain->bakeMesh( 0 );
		LOG( MESG, "Bake ArrayMesh: ", Time::get_singleton()->get_ticks_msec() - start_time, "ms" );
	}
}

///////////////////////////
// Protected Functions
///////////////////////////

void T3DUtil::_bind_methods( void ) {
	// Control map converters
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "as_float", "value" ), &asFloat );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "as_uint", "value" ), &asUint );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "get_base", "pixel" ), &gdGetBase );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_base", "base" ), &gdEncBase );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "get_overlay", "pixel" ), &gdGetOverlay );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_overlay", "overlay" ), &gdEncOverlay );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "get_blend", "pixel" ), &gdGetBlend );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_blend", "blend" ), &gdEncBlend );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "get_uv_rotation", "pixel" ), &gdGetUvRotation );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_uv_rotation", "rotation" ), &gdEncUvRotation );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "get_uv_scale", "pixel" ), &gdGetUvScale );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_uv_scale", "scale" ), &gdEncUvScale );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "is_hole", "pixel" ), &gdIsHole );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_hole", "pixel" ), &encHole );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "is_nav", "pixel" ), &gdIsNav );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_nav", "pixel" ), &encNav );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "is_auto", "pixel" ), &gdIsAuto );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "enc_auto", "pixel" ), &encAuto );

	// String functions
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "filename_to_location", "filename" ),
								&T3DUtil::filenameToLocation );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "location_to_filename", "region_location" ),
								&T3DUtil::locationToFilename );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "location_to_string", "region_location" ),
								&T3DUtil::locationToString );

	// Image handling
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "black_to_alpha", "image" ), &T3DUtil::blackToAlpha );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "get_min_max", "image" ), &T3DUtil::getMinMax );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "get_thumbnail", "image", "size" ),
								&T3DUtil::getThumbnail, DEFVAL( V2I( 256 ) ) );
	ClassDB::bind_static_method( "T3DUtil",
								D_METHOD( "get_filled_image", "size", "color", "create_mipmaps", "eFormat" ),
								&T3DUtil::getFilledImage );
	ClassDB::bind_static_method( "T3DUtil",
								D_METHOD( "load_image", "file_name", "cache_mode", "r16_height_range", "r16_size" ),
								&T3DUtil::loadImage, DEFVAL( ResourceLoader::CACHE_MODE_IGNORE ),
								DEFVAL( Vector2( 0.f, 255.f ) ), DEFVAL( V2I_ZERO ) );
	ClassDB::bind_static_method( "T3DUtil",
								D_METHOD( "pack_image", "src_rgb", "src_a", "src_ao", "invert_green", "invert_alpha",
										 "normalize_alpha", "alpha_channel", "ao_channel" ),
								&T3DUtil::packImage, DEFVAL( false ), DEFVAL( false ), DEFVAL( false ), DEFVAL( 0 ),
								DEFVAL( 0 ) );
	ClassDB::bind_static_method( "T3DUtil",
								D_METHOD( "pack_pbr_rgba16ui", "normal", "roughness", "displacement", "ao", "metallic",
										 "invert_normal_green", "roughness_channel", "displacement_channel",
										 "ao_channel", "metallic_channel", "generate_mipmaps" ),
								&T3DUtil::packPbrRgba16ui, DEFVAL( Ref<Image>() ), DEFVAL( Ref<Image>() ),
								DEFVAL( false ), DEFVAL( 0 ), DEFVAL( 0 ), DEFVAL( 0 ), DEFVAL( 0 ), DEFVAL( true ) );
	ClassDB::bind_static_method( "T3DUtil",
								D_METHOD( "pack_surface_rgba8", "normal", "roughness", "ao", "invert_normal_green",
										 "roughness_channel", "ao_channel", "generate_mipmaps" ),
								&T3DUtil::packSurfaceRgba8, DEFVAL( Ref<Image>() ), DEFVAL( false ), DEFVAL( 0 ),
								DEFVAL( 0 ), DEFVAL( true ) );
	ClassDB::bind_static_method( "T3DUtil",
								D_METHOD( "pack_displacement_r8", "displacement", "channel", "generate_mipmaps" ),
								&T3DUtil::packDisplacementR8, DEFVAL( 0 ), DEFVAL( true ) );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "save_displacement_r8_dds", "displacement", "path" ),
								&T3DUtil::saveDisplacementR8Dds );
	ClassDB::bind_static_method( "T3DUtil",
								D_METHOD( "pack_displacement_r16", "displacement", "channel", "generate_mipmaps" ),
								&T3DUtil::packDisplacementR16, DEFVAL( 0 ), DEFVAL( true ) );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "save_displacement_r16_dds", "displacement", "path" ),
								&T3DUtil::saveDisplacementR16Dds );
	ClassDB::bind_static_method( "T3DUtil", D_METHOD( "luminance_to_height", "src_rgb" ),
								&T3DUtil::luminanceToHeight );
}
