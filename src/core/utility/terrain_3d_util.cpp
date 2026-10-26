// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"

#include "core/utility/terrain_3d_util.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "terrain_3d.h"

#include "core/utility/logger.h"
#include "eastl_config.h"
#include "simd/core/terrain_3d_parallel.h"

#include <EASTL/vector.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>

typedef struct t3dPackedPBRPixel_s
{
	Vector3 m_normal = Vector3( 0.0f, 0.0f, 1.0f );
	real_t	m_fAo = 1.0f;
	real_t	m_fMetallic = 0.0f;
	real_t	m_fRoughness = 1.0f;
	real_t	m_fDisplacement = 0.5f;
} t3dPackedPBRPixel_t;

static Ref< Image > t3dUtilPrepareSourceImage(
	const Ref< Image > &rImage ) {
	if ( rImage.is_null() ) {
		return Ref< Image >();
	}
	Ref< Image > pImage = rImage->duplicate();
	if ( pImage->is_compressed() ) {
		const Error err = pImage->decompress();
		if ( err != OK ) {
			return Ref< Image >();
		}
	}
	return pImage;
}

static Ref< Image > t3dUtilPrepareRgbaFloatSource(
	const Ref< Image > &rImage ) {
	Ref< Image > pImage = t3dUtilPrepareSourceImage( rImage );
	if ( pImage.is_valid() && !pImage->is_empty() && pImage->get_format() != Image::FORMAT_RGBAF ) {
		pImage->convert( Image::FORMAT_RGBAF );
	}
	return pImage;
}

EA_FORCE_INLINE static float t3dUtilRgbaFloatChannel(
	const uint8_t *pBytes,
	const size_t   nPixel,
	const int	   nChannel ) {
	float fValue = 0.0f;
	memcpy( &fValue, pBytes + ( nPixel * 4u + ( size_t ) ( nChannel ) ) * sizeof( float ), sizeof( float ) );
	return fValue;
}

static Terrain3DParallelExecutor t3dUtilAuthoringExecutor(
	const size_t nItemCount ) {
	const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	const size_t   useful_tasks = MAX< size_t >( 1u, nItemCount / 16384u );
	return Terrain3DParallelExecutor( ( int32_t ) ( MIN< size_t >( ( size_t ) ( hardware_threads ), useful_tasks ) ) );
}

EA_FORCE_INLINE static uint16_t t3dUtilQuantizeU16(
	const real_t fValue ) {
	return ( uint16_t ) ( CLAMP( Math::round( CLAMP( fValue, ( real_t ) ( 0.0 ), ( real_t ) ( 1.0 ) ) * 65535.0 ),
								 0.0,
								 65535.0 ) );
}

EA_FORCE_INLINE static uint8_t t3dUtilQuantizeU8(
	const real_t fValue ) {
	return ( uint8_t ) ( CLAMP( Math::round( CLAMP( fValue, ( real_t ) ( 0.0 ), ( real_t ) ( 1.0 ) ) * 255.0 ),
								0.0,
								255.0 ) );
}

EA_FORCE_INLINE static int t3dUtilClampSourceCoord(
	const int nValue,
	const int nLimit ) {
	return CLAMP( nValue, 0, MAX( nLimit - 1, 0 ) );
}

static t3dPackedPBRPixel_t t3dUtilAveragePackedPbrPixels(
	const eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > &rSrc,
	const int															 nWidth,
	const int															 nHeight,
	const int															 iX,
	const int															 iY ) {
	t3dPackedPBRPixel_t result;
	result.m_normal = Vector3();
	result.m_fAo = 0.0f;
	result.m_fMetallic = 0.0f;
	result.m_fRoughness = 0.0f;
	result.m_fDisplacement = 0.0f;
	int sample_count = 0;
	for ( int oy = 0; oy < 2; oy++ ) {
		for ( int ox = 0; ox < 2; ox++ ) {
			const int				   sx = t3dUtilClampSourceCoord( iX * 2 + ox, nWidth );
			const int				   sy = t3dUtilClampSourceCoord( iY * 2 + oy, nHeight );
			const t3dPackedPBRPixel_t &sample = rSrc[( size_t ) ( sy ) * ( size_t ) ( nWidth ) + ( size_t ) ( sx )];
			result.m_normal += sample.m_normal;
			result.m_fAo += sample.m_fAo;
			result.m_fMetallic += sample.m_fMetallic;
			result.m_fRoughness += sample.m_fRoughness;
			result.m_fDisplacement += sample.m_fDisplacement;
			sample_count++;
		}
	}
	const real_t fInvCount = 1.0f / ( real_t ) ( MAX( sample_count, 1 ) );
	result.m_normal *= fInvCount;
	if ( result.m_normal.length_squared() > 1e-12f ) {
		result.m_normal.normalize();
	} else {
		result.m_normal = Vector3( 0.0f, 0.0f, 1.0f );
	}
	result.m_fAo *= fInvCount;
	result.m_fMetallic *= fInvCount;
	result.m_fRoughness *= fInvCount;
	result.m_fDisplacement *= fInvCount;
	return result;
}

static void t3dUtilAppendPackedPbrLevel(
	PackedByteArray														&rData,
	int64_t																&nOffset,
	const eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > &rPixels ) {
	for ( size_t iPixel = 0u; iPixel < ( size_t ) ( ( rPixels ).size() ); iPixel++ ) {
		const t3dPackedPBRPixel_t &pixel = ( rPixels )[iPixel];
		const uint8_t			   normal_x = t3dUtilQuantizeU8( pixel.m_normal.x * 0.5f + 0.5f );
		const uint8_t			   normal_y = t3dUtilQuantizeU8( pixel.m_normal.y * 0.5f + 0.5f );
		const uint8_t			   ao = t3dUtilQuantizeU8( pixel.m_fAo );
		const uint8_t			   metallic = t3dUtilQuantizeU8( pixel.m_fMetallic );
		const uint16_t			   packed_normal_ao = ( ( uint16_t ) ( normal_x ) << 8u ) | ( uint16_t ) ( ao );
		const uint16_t packed_normal_metallic = ( ( uint16_t ) ( normal_y ) << 8u ) | ( uint16_t ) ( metallic );
		rData.encode_u16( nOffset, packed_normal_ao );
		rData.encode_u16( nOffset + 2, packed_normal_metallic );
		rData.encode_u16( nOffset + 4, t3dUtilQuantizeU16( pixel.m_fRoughness ) );
		rData.encode_u16( nOffset + 6, t3dUtilQuantizeU16( pixel.m_fDisplacement ) );
		nOffset += 8;
	}
}

static void t3dUtilAppendSurfaceLevel(
	PackedByteArray														&rData,
	int64_t																&nOffset,
	const eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > &rPixels ) {
	for ( size_t iPixel = 0u; iPixel < ( size_t ) ( ( rPixels ).size() ); iPixel++ ) {
		const t3dPackedPBRPixel_t &pixel = ( rPixels )[iPixel];
		rData.set( nOffset++, t3dUtilQuantizeU8( pixel.m_normal.x * 0.5f + 0.5f ) );
		rData.set( nOffset++, t3dUtilQuantizeU8( pixel.m_normal.y * 0.5f + 0.5f ) );
		rData.set( nOffset++, t3dUtilQuantizeU8( pixel.m_fRoughness ) );
		rData.set( nOffset++, t3dUtilQuantizeU8( pixel.m_fAo ) );
	}
}

static bool t3dUtilValidateOptionalImage(
	const Ref< Image > &rImage,
	const Vector2i	   &rSize,
	const char		   *pszName ) {
	if ( rImage.is_null() ) {
		return true;
	}
	if ( rImage->is_empty() || rImage->get_size() != rSize ) {
		UtilityFunctions::push_error( "Terrain3DUtil: ",
									  pszName,
									  " image size must match the normal image size: ",
									  rSize );
		return false;
	}
	return true;
}

typedef struct t3dPackedPBRBuildContext_s
{
	const uint8_t												  *m_pNormal = NULL;
	const uint8_t												  *m_pRoughness = NULL;
	const uint8_t												  *m_pDisplacement = NULL;
	const uint8_t												  *m_pAo = NULL;
	const uint8_t												  *m_pMetallic = NULL;
	eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > *m_pOutput = NULL;
	bool														   m_bInvertGreen = false;
	int															   m_nRoughnessChannel = 0;
	int															   m_nDisplacementChannel = 0;
	int															   m_nAoChannel = 0;
	int															   m_nMetallicChannel = 0;
} t3dPackedPBRBuildContext_t;

static void t3dUtilBuildPackedPbrRange(
	void		*pContext,
	const size_t nBegin,
	const size_t nEnd,
	int32_t ) {
	t3dPackedPBRBuildContext_t *context = ( t3dPackedPBRBuildContext_t * ) ( pContext );
	for ( size_t index = nBegin; index < nEnd; ++index ) {
		real_t		 fNx = t3dUtilRgbaFloatChannel( context->m_pNormal, index, 0 ) * 2.0f - 1.0f;
		const real_t fGreen = t3dUtilRgbaFloatChannel( context->m_pNormal, index, 1 );
		real_t		 fNy = ( context->m_bInvertGreen ? 1.0f - fGreen : fGreen ) * 2.0f - 1.0f;
		const real_t fXyLengthSquared = fNx * fNx + fNy * fNy;
		if ( fXyLengthSquared > 1.0f ) {
			const real_t fInvLength = 1.0f / Math::sqrt( fXyLengthSquared );
			fNx *= fInvLength;
			fNy *= fInvLength;
		}
		const real_t		 fNz = Math::sqrt( MAX( 1.0f - fNx * fNx - fNy * fNy, 0.0f ) );
		t3dPackedPBRPixel_t &pixel = ( *context->m_pOutput )[index];
		pixel.m_normal = Vector3( fNx, fNy, fNz ).normalized();
		pixel.m_fRoughness =
			CLAMP( t3dUtilRgbaFloatChannel( context->m_pRoughness, index, context->m_nRoughnessChannel ), 0.0f, 1.0f );
		pixel.m_fDisplacement =
			CLAMP( t3dUtilRgbaFloatChannel( context->m_pDisplacement, index, context->m_nDisplacementChannel ),
				   0.0f,
				   1.0f );
		pixel.m_fAo = context->m_pAo
						  ? CLAMP( t3dUtilRgbaFloatChannel( context->m_pAo, index, context->m_nAoChannel ), 0.0f, 1.0f )
						  : 1.0f;
		pixel.m_fMetallic =
			context->m_pMetallic
				? CLAMP( t3dUtilRgbaFloatChannel( context->m_pMetallic, index, context->m_nMetallicChannel ),
						 0.0f,
						 1.0f )
				: 0.0f;
	}
}

typedef struct t3dSurfaceBuildContext_s
{
	const uint8_t												  *m_pNormal = NULL;
	const uint8_t												  *m_pRoughness = NULL;
	const uint8_t												  *m_pAo = NULL;
	eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > *m_pOutput = NULL;
	bool														   m_bInvertGreen = false;
	int															   m_nRoughnessChannel = 0;
	int															   m_nAoChannel = 0;
} t3dSurfaceBuildContext_t;

static void t3dUtilBuildSurfaceRange(
	void		*pContext,
	const size_t nBegin,
	const size_t nEnd,
	int32_t ) {
	t3dSurfaceBuildContext_t *context = ( t3dSurfaceBuildContext_t * ) ( pContext );
	for ( size_t index = nBegin; index < nEnd; ++index ) {
		real_t		 fNx = t3dUtilRgbaFloatChannel( context->m_pNormal, index, 0 ) * 2.0f - 1.0f;
		const real_t fGreen = t3dUtilRgbaFloatChannel( context->m_pNormal, index, 1 );
		real_t		 fNy = ( context->m_bInvertGreen ? 1.0f - fGreen : fGreen ) * 2.0f - 1.0f;
		const real_t fLengthSquared = fNx * fNx + fNy * fNy;
		if ( fLengthSquared > 1.0f ) {
			const real_t fInvLength = 1.0f / Math::sqrt( fLengthSquared );
			fNx *= fInvLength;
			fNy *= fInvLength;
		}
		const real_t		 fNz = Math::sqrt( MAX( 1.0f - fNx * fNx - fNy * fNy, 0.0f ) );
		t3dPackedPBRPixel_t &pixel = ( *context->m_pOutput )[index];
		pixel.m_normal = Vector3( fNx, fNy, fNz ).normalized();
		pixel.m_fRoughness =
			CLAMP( t3dUtilRgbaFloatChannel( context->m_pRoughness, index, context->m_nRoughnessChannel ), 0.0f, 1.0f );
		pixel.m_fAo = context->m_pAo
						  ? CLAMP( t3dUtilRgbaFloatChannel( context->m_pAo, index, context->m_nAoChannel ), 0.0f, 1.0f )
						  : 1.0f;
	}
}

typedef struct t3dPackedMipContext_s
{
	const eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > *m_pSource = NULL;
	eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator >		*m_pDestination = NULL;
	int																	 m_nSourceWidth = 0;
	int																	 m_nSourceHeight = 0;
	int																	 m_nDestinationWidth = 0;
} t3dPackedMipContext_t;

static void t3dUtilBuildPackedMipRows(
	void		*pContext,
	const size_t nBegin,
	const size_t nEnd,
	int32_t ) {
	t3dPackedMipContext_t *context = ( t3dPackedMipContext_t * ) ( pContext );
	for ( size_t y = nBegin; y < nEnd; ++y ) {
		for ( int x = 0; x < context->m_nDestinationWidth; ++x ) {
			( *context->m_pDestination )[y * ( size_t ) ( context->m_nDestinationWidth ) + ( size_t ) ( x )] =
				t3dUtilAveragePackedPbrPixels( *context->m_pSource,
											   context->m_nSourceWidth,
											   context->m_nSourceHeight,
											   x,
											   ( int ) ( y ) );
		}
	}
}

// t3dDisplacementBuildContext_s centralized in Terrain3DTypes.h.

static void t3dUtilBuildDisplacementRange(
	void		*pContext,
	const size_t nBegin,
	const size_t nEnd,
	int32_t ) {
	t3dDisplacementBuildContext_t *context = ( t3dDisplacementBuildContext_t * ) ( pContext );
	for ( size_t index = nBegin; index < nEnd; ++index ) {
		const float fValue =
			CLAMP( t3dUtilRgbaFloatChannel( context->m_pSource, index, context->m_nChannel ), 0.0f, 1.0f );
		context->m_pDestination[index] = t3dUtilQuantizeU8( fValue );
	}
}

// -----------------------------------------------------------------------------
// Public functions

void Terrain3DUtil::printArr(
	const String &strName,
	const Array	 &rArr,
	const int	  nLevel ) {
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
			Object *const pObj = cast_to< Object >( var );
			const String  str = String( "Object#" ) + String::num_uint64( pObj->get_instance_id() ) + String( ", " ) +
							   t3dPtrToStr( pObj );
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

void Terrain3DUtil::printDict(
	const String	 &strName,
	const Dictionary &rDict,
	const int		  nLevel ) {
	LOG( nLevel, "Dictionary: ", strName );
	const Array keys = rDict.keys();
	for ( size_t iKey = 0u; iKey < ( size_t ) ( ( keys ).size() ); iKey++ ) {
		const Variant key_variant = keys[( int64_t ) ( iKey )];
		const String  key = key_variant.stringify();
		const Variant var = rDict.get( key_variant, Variant() );
		switch ( var.get_type() ) {
		case Variant::ARRAY: {
			printArr( key, var, nLevel );
			break;
		}
		case Variant::DICTIONARY: {
			printDict( key, var, nLevel );
			break;
		}
		case Variant::OBJECT: {
			Object *const pObj = cast_to< Object >( var );
			const String  str = pObj ? String( "Object#" ) + String::num_uint64( pObj->get_instance_id() ) +
										   String( ", " ) + t3dPtrToStr( pObj )
									 : String( "<null>" );
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

void Terrain3DUtil::dumpGentex(
	const Terrain3DGeneratedTexture &rGen,
	const String					&strName ) {
	LOG( MESG,
		 "Generated ",
		 strName,
		 " RID: ",
		 rGen.getRid(),
		 ", dirty: ",
		 rGen.isDirty(),
		 ", image: ",
		 t3dPtrToStr( *rGen.getImage() ) );
}

void Terrain3DUtil::dumpMaps(
	const TypedArray< Image > &rMaps,
	const String			  &strName ) {
	LOG( MESG, "Dumping ", strName, " array. Size: ", rMaps.size() );
	for ( size_t iMap = 0u; iMap < ( size_t ) ( ( rMaps ).size() ); iMap++ ) {
		const Ref< Image > pImg = ( rMaps )[iMap];
		LOG( MESG, "Map size: ", pImg->get_size(), ", eFormat: ", pImg->get_format(), ", ", t3dPtrToStr( *pImg ) );
	}
}

// Expects a filename in a String like: "terrain3d-01_02.res" which returns (-1, 2)
Vector2i Terrain3DUtil::filenameToLocation(
	const String &rFilename ) {
	String location_string = rFilename.trim_prefix( "terrain3d" ).trim_suffix( ".res" );
	return stringToLocation( location_string );
}

// Expects a string formatted as: "±##±##" which returns (##,##)
Vector2i Terrain3DUtil::stringToLocation(
	const String &rString ) {
	String x_str = rString.left( 3 ).replace( "_", "" );
	String y_str = rString.right( 3 ).replace( "_", "" );
	if ( !x_str.is_valid_int() || !y_str.is_valid_int() ) {
		LOG( ERROR, "Malformed string '", rString, "'. Result: ", x_str, ", ", y_str );
		return s_v2iMax;
	}
	return Vector2i( x_str.to_int(), y_str.to_int() );
}

// Expects a v2i(-1,2) and returns terrain3d-01_02.res
String Terrain3DUtil::locationToFilename(
	const Vector2i &rRegionLoc ) {
	return String( "terrain3d" ) + locationToString( rRegionLoc ) + String( ".res" );
}

// Expects a v2i(-1,2) and returns -01_02
String Terrain3DUtil::locationToString(
	const Vector2i &rRegionLoc ) {
	const String POS_REGION_FORMAT = "_%02d";
	const String NEG_REGION_FORMAT = "%03d";
	String		 x_str, y_str;
	x_str = vformat( ( rRegionLoc.x >= 0 ) ? POS_REGION_FORMAT : NEG_REGION_FORMAT, rRegionLoc.x );
	y_str = vformat( ( rRegionLoc.y >= 0 ) ? POS_REGION_FORMAT : NEG_REGION_FORMAT, rRegionLoc.y );
	return x_str + y_str;
}

PackedStringArray Terrain3DUtil::getFiles(
	const String &rDir,
	const String &rGlob ) {
	PackedStringArray files;
	Ref< DirAccess >  pDa = DirAccess::open( rDir );
	if ( pDa.is_null() ) {
		LOG( ERROR, "Cannot open directory: ", rDir );
		return files;
	}
	const PackedStringArray dir_files = pDa->get_files();
	for ( size_t iFile = 0u; iFile < ( size_t ) ( ( dir_files ).size() ); iFile++ ) {
		const String &file_name = ( dir_files )[iFile];
		const String  fname = file_name.trim_suffix( ".remap" );
		if ( !fname.matchn( rGlob ) ) {
			continue;
		}
		LOG( DEBUG, "Found file: ", rDir + String( "/" ) + fname );
		files.push_back( fname );
	}
	return files;
}

Ref< Image > Terrain3DUtil::blackToAlpha(
	const Ref< Image > &rImage ) {
	const Ref< Image > pSource = t3dUtilPrepareSourceImage( rImage );
	if ( pSource.is_null() ) {
		return Ref< Image >();
	}
	Ref< Image > pImg = Image::create_empty( pSource->get_width(), pSource->get_height(), false, Image::FORMAT_RGBAF );
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
Vector2 Terrain3DUtil::getMinMax(
	const Ref< Image > &rImage ) {
	if ( rImage.is_null() ) {
		LOG( ERROR, "Provided image is not valid. Nothing to analyze" );
		return V2( INFINITY );
	} else if ( rImage->is_empty() ) {
		LOG( ERROR, "Provided image is empty. Nothing to analyze" );
		return V2( INFINITY );
	}

	const Ref< Image > pSource = t3dUtilPrepareSourceImage( rImage );
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
Ref< Image > Terrain3DUtil::getThumbnail(
	const Ref< Image > &rImage,
	const Vector2i	   &rSize ) {
	if ( rImage.is_null() ) {
		LOG( ERROR, "Provided image is not valid. Nothing to process" );
		return Ref< Image >();
	} else if ( rImage->is_empty() ) {
		LOG( ERROR, "Provided image is empty. Nothing to process" );
		return Ref< Image >();
	}
	const Vector2i size = Vector2i( CLAMP( rSize.x, 8, 16384 ), CLAMP( rSize.y, 8, 16384 ) );

	LOG( INFO, "Drawing a thumbnail sized: ", size );
	// Create a temporary work image scaled to desired width
	Ref< Image > pImg = t3dUtilPrepareSourceImage( rImage );
	if ( pImg.is_null() ) {
		LOG( ERROR, "Provided image could not be decompressed. Cannot create thumbnail" );
		return Ref< Image >();
	}
	pImg->resize( size.x, size.y, Image::INTERPOLATE_LANCZOS );

	// Get minimum and maximum height values on the scaled image
	const Vector2 minmax = getMinMax( pImg );
	real_t		  fHmin = minmax.x;
	real_t		  fHmax = minmax.y;
	// Define maximum range
	fHmin = abs( fHmin );
	fHmax = abs( fHmax ) + fHmin;
	// Avoid divide by zero
	fHmax = ( fHmax == 0 ) ? 0.001f : fHmax;

	// Create a new image w / normalized values
	Ref< Image > pThumb = Image::create_empty( size.x, size.y, false, Image::FORMAT_RGB8 );
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
Ref< Image > Terrain3DUtil::getFilledImage(
	const Vector2i	   &rSize,
	const Color		   &rColor,
	const bool			bCreateMipmaps,
	const Image::Format eRequestedFormat ) {
	Image::Format eFormat = eRequestedFormat;
	if ( eFormat < 0 || eFormat >= Image::FORMAT_MAX ) {
		eFormat = Image::FORMAT_DXT5;
	}

	Image::CompressMode compression_format = Image::COMPRESS_MAX;
	Image::UsedChannels channels = Image::USED_CHANNELS_RGBA;
	bool				bCompress = false;
	bool				bFillImage = true;

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

	Ref< Image > pImg = Image::create_empty( rSize.x, rSize.y, bCreateMipmaps, eFormat );

	Color color = rColor;
	if ( bFillImage ) {
		if ( color.a < 0.0f ) {
			color.a = 1.0f;
			const Color col_a = Color( 0.8f, 0.8f, 0.8f, 1.0 ) * color;
			const Color col_b = Color( 0.5f, 0.5f, 0.5f, 1.0 ) * color;
			pImg->fill_rect( Rect2i( s_v2iZero, rSize / 2 ), col_a );
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
Ref< Image > Terrain3DUtil::loadImage(
	const String   &rFileName,
	const int		nCacheMode,
	const Vector2  &rR16HeightRange,
	const Vector2i &rR16Size ) {
	if ( rFileName.is_empty() ) {
		LOG( ERROR, "No file specified. Nothing imported" );
		return Ref< Image >();
	}
	if ( !FileAccess::file_exists( rFileName ) ) {
		LOG( ERROR, "File ", rFileName, " does not exist. Nothing to import" );
		return Ref< Image >();
	}

	// Load file based on extension
	Ref< Image > pImg;
	LOG( INFO, "Attempting to load: ", rFileName );
	String					ext = rFileName.get_extension().to_lower();
	const PackedStringArray imgloader_extensions =
		PackedStringArray( Array::make( "bmp", "dds", "exr", "hdr", "jpg", "jpeg", "png", "tga", "svg", "webp" ) );

	// If R16 integer eFormat (read/writeable by Krita)
	if ( ext == "r16" || ext == "raw" ) {
		LOG( DEBUG, "Loading file as an r16" );
		Ref< FileAccess > pFile = FileAccess::open( rFileName, FileAccess::READ );
		if ( !pFile.is_valid() ) {
			LOG( ERROR, "Could not open R16 file: ", rFileName );
			return Ref< Image >();
		}
		const uint64_t nFileBytes = pFile->get_length();
		if ( nFileBytes < 2u || nFileBytes > INT32_MAX ) {
			LOG( ERROR, "R16 file length is invalid or exceeds the importer limit: ", nFileBytes );
			return Ref< Image >();
		}
		// If p_size is zero, assume square and try to auto detect size.
		Vector2i r16_size = rR16Size;
		if ( r16_size <= s_v2iZero ) {
			const int fwidth = ( int ) sqrt( ( double ) ( nFileBytes / 2u ) );
			r16_size = V2I( fwidth );
			LOG( DEBUG, "Total file size is: ", nFileBytes, " calculated width: ", fwidth, " dimensions: ", r16_size );
		}
		if ( r16_size.x <= 0 || r16_size.y <= 0 ||
			 ( uint64_t ) r16_size.x * ( uint64_t ) r16_size.y * 2u > nFileBytes ) {
			LOG( ERROR, "R16 dimensions exceed the available input bytes: ", r16_size );
			return Ref< Image >();
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
		pImg = ResourceLoader::get_singleton()->load( rFileName, "", ( ResourceLoader::CacheMode )( nCacheMode ) );
	}

	if ( !pImg.is_valid() ) {
		LOG( ERROR, "File", rFileName, " cannot be loaded" );
		return Ref< Image >();
	}
	if ( pImg->is_empty() ) {
		LOG( ERROR, "File", rFileName, " is empty" );
		return Ref< Image >();
	}
	LOG( DEBUG, "Loaded Image size: ", pImg->get_size(), " eFormat: ", pImg->get_format() );
	return pImg;
}

/* From source RGB and selected source for Alpha channel, create a new RGBA image.
 * If p_invert_green is true, the destination green channel will be 1.0 - input green channel.
 * If p_invert_alpha is true, the destination alpha channel will be 1.0 - input source channel.
 */
Ref< Image > Terrain3DUtil::packImage(
	const Ref< Image > &rSrcRgb,
	const Ref< Image > &rSrcA,
	const Ref< Image > &rSrcAo,
	const bool			bInvertGreen,
	const bool			bInvertAlpha,
	const bool			bNormalizeAlpha,
	const int			nAlphaChannel,
	const int			nAoChannel ) {
	if ( !rSrcRgb.is_valid() || !rSrcA.is_valid() ) {
		LOG( ERROR, "Provided images are not valid. Cannot pack" );
		return Ref< Image >();
	}
	const Ref< Image > pSrcRgb = t3dUtilPrepareSourceImage( rSrcRgb );
	const Ref< Image > pSrcA = t3dUtilPrepareSourceImage( rSrcA );
	const Ref< Image > pSrcAo = rSrcAo.is_valid() ? t3dUtilPrepareSourceImage( rSrcAo ) : Ref< Image >();
	if ( pSrcRgb.is_null() || pSrcA.is_null() || ( rSrcAo.is_valid() && pSrcAo.is_null() ) ) {
		LOG( ERROR, "A source image could not be decompressed. Cannot pack" );
		return Ref< Image >();
	}
	if ( rSrcRgb->get_size() != rSrcA->get_size() ) {
		LOG( ERROR, "Provided images are not the same size. Cannot pack" );
		return Ref< Image >();
	}
	if ( rSrcRgb->is_empty() || rSrcA->is_empty() ) {
		LOG( ERROR, "Provided images are empty. Cannot pack" );
		return Ref< Image >();
	}
	if ( nAlphaChannel < 0 || nAlphaChannel > 3 ) {
		LOG( ERROR, "Source channel of Height/Roughness invalid. Cannot Pack" );
		return Ref< Image >();
	}

	const bool bPackAo = pSrcAo.is_valid();
	if ( bPackAo ) {
		if ( rSrcRgb->get_size() != rSrcAo->get_size() ) {
			LOG( ERROR, "Provided AO and normal images are not the same size. Cannot pack" );
			return Ref< Image >();
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

	Ref< Image > pDst = Image::create_empty( rSrcRgb->get_width(), rSrcRgb->get_height(), false, Image::FORMAT_RGBA8 );
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

Ref< Image > Terrain3DUtil::packPbrRgba16ui(
	const Ref< Image > &rNormal,
	const Ref< Image > &rRoughness,
	const Ref< Image > &rDisplacement,
	const Ref< Image > &rAo,
	const Ref< Image > &rMetallic,
	const bool			bInvertNormalGreen,
	const int			nRoughnessChannel,
	const int			nDisplacementChannel,
	const int			nAoChannel,
	const int			nMetallicChannel,
	const bool			bGenerateMipmaps ) {
	if ( rNormal.is_null() || rRoughness.is_null() || rDisplacement.is_null() ) {
		LOG( ERROR, "Normal, roughness, and displacement images are required for packed PBR generation." );
		return Ref< Image >();
	}
	const int channels[] = { nRoughnessChannel, nDisplacementChannel, nAoChannel, nMetallicChannel };
	for ( size_t iChannel = 0u; iChannel < T3D_ARRAY_COUNT( channels ); iChannel++ ) {
		const int channel = ( channels )[iChannel];
		if ( channel < 0 || channel > 3 ) {
			LOG( ERROR, "Packed PBR source channel must be between 0 and 3." );
			return Ref< Image >();
		}
	}

	const Ref< Image > pNormal = t3dUtilPrepareRgbaFloatSource( rNormal );
	const Ref< Image > pRoughness = t3dUtilPrepareRgbaFloatSource( rRoughness );
	const Ref< Image > pDisplacement = t3dUtilPrepareRgbaFloatSource( rDisplacement );
	const Ref< Image > pAo = t3dUtilPrepareRgbaFloatSource( rAo );
	const Ref< Image > pMetallic = t3dUtilPrepareRgbaFloatSource( rMetallic );
	if ( pNormal.is_null() || pRoughness.is_null() || pDisplacement.is_null() ) {
		LOG( ERROR, "A packed PBR source image could not be decompressed." );
		return Ref< Image >();
	}
	if ( pNormal->is_empty() || pRoughness->is_empty() || pDisplacement->is_empty() ) {
		LOG( ERROR, "Packed PBR source images cannot be empty." );
		return Ref< Image >();
	}

	const Vector2i size = pNormal->get_size();
	if ( pRoughness->get_size() != size || pDisplacement->get_size() != size ||
		 !t3dUtilValidateOptionalImage( pAo, size, "AO" ) ||
		 !t3dUtilValidateOptionalImage( pMetallic, size, "Metallic" ) ) {
		LOG( ERROR, "All packed PBR source images must have identical dimensions." );
		return Ref< Image >();
	}
	if ( !Math::is_power_of_2( size.x ) || !Math::is_power_of_2( size.y ) ) {
		LOG( WARN,
			 "Packed PBR image dimensions are not powers of two: ",
			 size,
			 ". Mipmaps remain valid but may be less efficient." );
	}

	const size_t pixel_count = ( size_t ) ( size.x ) * ( size_t ) ( size.y );
	eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > levelPixels( pixel_count );
	eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > mip_scratch;
	mip_scratch.reserve( ( size_t ) ( MAX( size.x / 2, 1 ) ) * ( size_t ) ( MAX( size.y / 2, 1 ) ) );
	const PackedByteArray			normal_data = pNormal->get_data();
	const PackedByteArray			roughness_data = pRoughness->get_data();
	const PackedByteArray			displacement_data = pDisplacement->get_data();
	const PackedByteArray			ao_data = pAo.is_valid() ? pAo->get_data() : PackedByteArray();
	const PackedByteArray			metallic_data = pMetallic.is_valid() ? pMetallic->get_data() : PackedByteArray();
	const uint8_t				   *pNormalBytes = normal_data.ptr();
	const uint8_t				   *pRoughnessBytes = roughness_data.ptr();
	const uint8_t				   *pDisplacementBytes = displacement_data.ptr();
	const uint8_t				   *pAoBytes = ao_data.is_empty() ? NULL : ao_data.ptr();
	const uint8_t				   *pMetallicBytes = metallic_data.is_empty() ? NULL : metallic_data.ptr();
	const Terrain3DParallelExecutor executor = t3dUtilAuthoringExecutor( pixel_count );
	t3dPackedPBRBuildContext_t		build_context;
	build_context.m_pNormal = pNormalBytes;
	build_context.m_pRoughness = pRoughnessBytes;
	build_context.m_pDisplacement = pDisplacementBytes;
	build_context.m_pAo = pAoBytes;
	build_context.m_pMetallic = pMetallicBytes;
	build_context.m_pOutput = &levelPixels;
	build_context.m_bInvertGreen = bInvertNormalGreen;
	build_context.m_nRoughnessChannel = nRoughnessChannel;
	build_context.m_nDisplacementChannel = nDisplacementChannel;
	build_context.m_nAoChannel = nAoChannel;
	build_context.m_nMetallicChannel = nMetallicChannel;
	executor.runStatic( pixel_count, 256u, t3dUtilBuildPackedPbrRange, &build_context );

	int		mip_count = 1;
	int		mip_width = size.x;
	int		mip_height = size.y;
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
	int		current_width = size.x;
	int		current_height = size.y;
	t3dUtilAppendPackedPbrLevel( data, byte_offset, levelPixels );

	for ( int mip = 1; mip < mip_count; mip++ ) {
		const int next_width = MAX( current_width / 2, 1 );
		const int next_height = MAX( current_height / 2, 1 );
		mip_scratch.resize( ( size_t ) ( next_width ) * ( size_t ) ( next_height ) );
		t3dPackedMipContext_t mip_context;
		mip_context.m_pSource = &levelPixels;
		mip_context.m_pDestination = &mip_scratch;
		mip_context.m_nSourceWidth = current_width;
		mip_context.m_nSourceHeight = current_height;
		mip_context.m_nDestinationWidth = next_width;
		executor.runStatic( ( size_t ) ( next_height ), 1u, t3dUtilBuildPackedMipRows, &mip_context );
		t3dUtilAppendPackedPbrLevel( data, byte_offset, mip_scratch );
		levelPixels.swap( mip_scratch );
		current_width = next_width;
		current_height = next_height;
	}

	if ( byte_offset != total_bytes ) {
		LOG( ERROR, "Packed PBR byte count mismatch. Expected ", total_bytes, ", wrote ", byte_offset );
		return Ref< Image >();
	}
	return Image::create_from_data( size.x, size.y, mip_count > 1, Image::FORMAT_RGBA16I, data );
}

Ref< Image > Terrain3DUtil::getDefaultPackedPbrImage(
	const Vector2i &rSize,
	const bool		bCreateMipmaps ) {
	if ( rSize.x <= 0 || rSize.y <= 0 ) {
		return Ref< Image >();
	}
	Ref< Image > pNormal = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_RGB8 );
	Ref< Image > pRoughness = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_L8 );
	Ref< Image > pDisplacement = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_L8 );
	pNormal->fill( Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
	pRoughness->fill( Color( 1.0f, 1.0f, 1.0f, 1.0f ) );
	pDisplacement->fill( Color( 0.5f, 0.5f, 0.5f, 1.0f ) );
	return packPbrRgba16ui( pNormal,
							pRoughness,
							pDisplacement,
							Ref< Image >(),
							Ref< Image >(),
							false,
							0,
							0,
							0,
							0,
							bCreateMipmaps );
}

Ref< Image > Terrain3DUtil::packSurfaceRgba8(
	const Ref< Image > &rNormal,
	const Ref< Image > &rRoughness,
	const Ref< Image > &rAo,
	const bool			bInvertNormalGreen,
	const int			nRoughnessChannel,
	const int			nAoChannel,
	const bool			bGenerateMipmaps ) {
	if ( rNormal.is_null() || rRoughness.is_null() ) {
		LOG( ERROR, "Normal and roughness images are required for surface texture generation." );
		return Ref< Image >();
	}
	if ( nRoughnessChannel < 0 || nRoughnessChannel > 3 || nAoChannel < 0 || nAoChannel > 3 ) {
		LOG( ERROR, "Surface source channels must be between 0 and 3." );
		return Ref< Image >();
	}
	const Ref< Image > pNormal = t3dUtilPrepareRgbaFloatSource( rNormal );
	const Ref< Image > pRoughness = t3dUtilPrepareRgbaFloatSource( rRoughness );
	const Ref< Image > pAo = t3dUtilPrepareRgbaFloatSource( rAo );
	if ( pNormal.is_null() || pRoughness.is_null() || pNormal->is_empty() || pRoughness->is_empty() ) {
		LOG( ERROR, "A surface source image could not be prepared." );
		return Ref< Image >();
	}
	const Vector2i size = pNormal->get_size();
	if ( pRoughness->get_size() != size || ( pAo.is_valid() && ( pAo->is_empty() || pAo->get_size() != size ) ) ) {
		LOG( ERROR, "All surface source images must have identical dimensions." );
		return Ref< Image >();
	}

	const size_t pixel_count = ( size_t ) ( size.x ) * ( size_t ) ( size.y );
	eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > levelPixels( pixel_count );
	eastl::vector< t3dPackedPBRPixel_t, Terrain3DEastlAllocator > mip_scratch;
	mip_scratch.reserve( ( size_t ) ( MAX( size.x / 2, 1 ) ) * ( size_t ) ( MAX( size.y / 2, 1 ) ) );
	const PackedByteArray			normal_data = pNormal->get_data();
	const PackedByteArray			roughness_data = pRoughness->get_data();
	const PackedByteArray			ao_data = pAo.is_valid() ? pAo->get_data() : PackedByteArray();
	const uint8_t				   *pNormalBytes = normal_data.ptr();
	const uint8_t				   *pRoughnessBytes = roughness_data.ptr();
	const uint8_t				   *pAoBytes = ao_data.is_empty() ? NULL : ao_data.ptr();
	const Terrain3DParallelExecutor executor = t3dUtilAuthoringExecutor( pixel_count );
	t3dSurfaceBuildContext_t		surface_context;
	surface_context.m_pNormal = pNormalBytes;
	surface_context.m_pRoughness = pRoughnessBytes;
	surface_context.m_pAo = pAoBytes;
	surface_context.m_pOutput = &levelPixels;
	surface_context.m_bInvertGreen = bInvertNormalGreen;
	surface_context.m_nRoughnessChannel = nRoughnessChannel;
	surface_context.m_nAoChannel = nAoChannel;
	executor.runStatic( pixel_count, 256u, t3dUtilBuildSurfaceRange, &surface_context );

	int		mip_count = 1;
	int		mip_width = size.x;
	int		mip_height = size.y;
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
	int		current_width = size.x;
	int		current_height = size.y;
	t3dUtilAppendSurfaceLevel( data, byte_offset, levelPixels );
	for ( int mip = 1; mip < mip_count; mip++ ) {
		const int next_width = MAX( current_width / 2, 1 );
		const int next_height = MAX( current_height / 2, 1 );
		mip_scratch.resize( ( size_t ) ( next_width ) * ( size_t ) ( next_height ) );
		t3dPackedMipContext_t mip_context;
		mip_context.m_pSource = &levelPixels;
		mip_context.m_pDestination = &mip_scratch;
		mip_context.m_nSourceWidth = current_width;
		mip_context.m_nSourceHeight = current_height;
		mip_context.m_nDestinationWidth = next_width;
		executor.runStatic( ( size_t ) ( next_height ), 1u, t3dUtilBuildPackedMipRows, &mip_context );
		t3dUtilAppendSurfaceLevel( data, byte_offset, mip_scratch );
		levelPixels.swap( mip_scratch );
		current_width = next_width;
		current_height = next_height;
	}
	return Image::create_from_data( size.x, size.y, mip_count > 1, Image::FORMAT_RGBA8, data );
}

Ref< Image > Terrain3DUtil::packDisplacementR8(
	const Ref< Image > &rDisplacement,
	const int			nDisplacementChannel,
	const bool			bGenerateMipmaps ) {
	if ( rDisplacement.is_null() || nDisplacementChannel < 0 || nDisplacementChannel > 3 ) {
		LOG( ERROR, "A displacement image and source channel 0-3 are required." );
		return Ref< Image >();
	}
	const Ref< Image > pSource = t3dUtilPrepareRgbaFloatSource( rDisplacement );
	if ( pSource.is_null() || pSource->is_empty() ) {
		return Ref< Image >();
	}
	const size_t		  pixel_count = ( size_t ) ( pSource->get_width() ) * ( size_t ) ( pSource->get_height() );
	const PackedByteArray source_data = pSource->get_data();
	const uint8_t		 *pSourceBytes = source_data.ptr();
	PackedByteArray		  output;
	output.resize( ( int64_t ) ( pixel_count ) );
	uint8_t *const					pDestination = output.ptrw();
	const Terrain3DParallelExecutor executor = t3dUtilAuthoringExecutor( pixel_count );
	t3dDisplacementBuildContext_t	displacement_context;
	displacement_context.m_pSource = pSourceBytes;
	displacement_context.m_pDestination = pDestination;
	displacement_context.m_nChannel = nDisplacementChannel;
	executor.runStatic( pixel_count, 512u, t3dUtilBuildDisplacementRange, &displacement_context );
	Ref< Image > pResult =
		Image::create_from_data( pSource->get_width(), pSource->get_height(), false, Image::FORMAT_R8, output );
	if ( bGenerateMipmaps ) {
		pResult->generate_mipmaps();
	}
	return pResult;
}

Ref< Image > Terrain3DUtil::getDefaultSurfaceImage(
	const Vector2i &rSize,
	const bool		bCreateMipmaps ) {
	if ( rSize.x <= 0 || rSize.y <= 0 ) {
		return Ref< Image >();
	}
	Ref< Image > pResult = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_RGBA8 );
	pResult->fill( Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
	if ( bCreateMipmaps ) {
		pResult->generate_mipmaps( true );
	}
	return pResult;
}

Ref< Image > Terrain3DUtil::getDefaultDisplacementImage(
	const Vector2i &rSize,
	const bool		bCreateMipmaps ) {
	if ( rSize.x <= 0 || rSize.y <= 0 ) {
		return Ref< Image >();
	}
	Ref< Image > pResult = Image::create_empty( rSize.x, rSize.y, false, Image::FORMAT_R8 );
	pResult->fill( Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
	if ( bCreateMipmaps ) {
		pResult->generate_mipmaps();
	}
	return pResult;
}

Ref< Image > Terrain3DUtil::convertPackedPbrRgba16uiToSurface(
	const Ref< Image > &rPacked,
	const bool			bGenerateMipmaps ) {
	if ( rPacked.is_null() || rPacked->is_empty() || rPacked->get_format() != Image::FORMAT_RGBA16I ) {
		return Ref< Image >();
	}
	const int			  width = rPacked->get_width();
	const int			  height = rPacked->get_height();
	const PackedByteArray data = rPacked->get_data();
	if ( data.size() < ( int64_t ) ( width ) * ( int64_t ) ( height ) * 8 ) {
		return Ref< Image >();
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
	Ref< Image > pResult = Image::create_from_data( width, height, false, Image::FORMAT_RGBA8, out );
	if ( bGenerateMipmaps ) {
		// Repack through the surface mip generator so normal XY is renormalized.
		Ref< Image > pNormal = Image::create_empty( width, height, false, Image::FORMAT_RGB8 );
		Ref< Image > pRoughness = Image::create_empty( width, height, false, Image::FORMAT_L8 );
		Ref< Image > pAo = Image::create_empty( width, height, false, Image::FORMAT_L8 );
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

Ref< Image > Terrain3DUtil::convertPackedPbrRgba16uiToDisplacement(
	const Ref< Image > &rPacked,
	const bool			bGenerateMipmaps ) {
	if ( rPacked.is_null() || rPacked->is_empty() || rPacked->get_format() != Image::FORMAT_RGBA16I ) {
		return Ref< Image >();
	}
	const int			  width = rPacked->get_width();
	const int			  height = rPacked->get_height();
	const PackedByteArray data = rPacked->get_data();
	if ( data.size() < ( int64_t ) ( width ) * ( int64_t ) ( height ) * 8 ) {
		return Ref< Image >();
	}
	PackedByteArray out;
	out.resize( ( int64_t ) ( width ) * ( int64_t ) ( height ) );
	int64_t in_offset = 6;
	for ( int i = 0; i < width * height; i++ ) {
		out.set( i, ( uint8_t ) ( data.decode_u16( in_offset ) >> 8u ) );
		in_offset += 8;
	}
	Ref< Image > pResult = Image::create_from_data( width, height, false, Image::FORMAT_R8, out );
	if ( bGenerateMipmaps ) {
		pResult->generate_mipmaps();
	}
	return pResult;
}

Error Terrain3DUtil::saveDisplacementR8Dds(
	const Ref< Image > &rDisplacement,
	const String	   &rPath ) {
	if ( rDisplacement.is_null() || rDisplacement->is_empty() || rDisplacement->get_format() != Image::FORMAT_R8 ) {
		LOG( ERROR, "DDS displacement export requires Image::FORMAT_R8." );
		return ERR_INVALID_DATA;
	}
	return rDisplacement->save_dds( rPath );
}

// From source RGB, create a new L image that is scaled to use full 0 - 1 range.
Ref< Image > Terrain3DUtil::luminanceToHeight(
	const Ref< Image > &rSrcRgb ) {
	if ( !rSrcRgb.is_valid() ) {
		LOG( ERROR, "Provided images are not valid. Cannot pack" );
		return Ref< Image >();
	}
	if ( rSrcRgb->is_empty() ) {
		LOG( ERROR, "Provided images are empty. Cannot pack" );
		return Ref< Image >();
	}
	const Ref< Image > pSource = t3dUtilPrepareSourceImage( rSrcRgb );
	if ( pSource.is_null() ) {
		LOG( ERROR, "Provided image could not be decompressed. Cannot pack" );
		return Ref< Image >();
	}
	real_t fLumContrast;
	real_t fLMax = 0.0f;
	real_t fLMin = 1.0f;
	// Calculate contrast and offset so that we can make the most use of the height channel range.
	for ( int y = 0; y < pSource->get_height(); y++ ) {
		for ( int x = 0; x < pSource->get_width(); x++ ) {
			const Color	 col = pSource->get_pixel( x, y );
			const real_t fL = 0.299f * col.r + 0.587f * col.g + 0.114f * col.b;
			fLMax = MAX( fL, fLMax );
			fLMin = MIN( fL, fLMin );
		}
	}
	fLumContrast = 1.0f / MAX( fLMax - fLMin, 1e-6 );
	Ref< Image > pDst = Image::create_empty( pSource->get_width(), pSource->get_height(), false, Image::FORMAT_RGB8 );
	for ( int y = 0; y < pSource->get_height(); y++ ) {
		for ( int x = 0; x < pSource->get_width(); x++ ) {
			Color  col = pSource->get_pixel( x, y );
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

void Terrain3DUtil::benchmark(
	Object *const pObject ) {
	Terrain3D *const pTerrain = Object::cast_to< Terrain3D >( pObject );
	if ( !pTerrain ) {
		return;
	}
	const Terrain3DData *pData = pTerrain->getData();
	if ( !pData ) {
		return;
	}
	uint64_t start_time;
	Vector3	 vec;
	Color	 col;
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

#if defined( T3D_EDITOR_NATIVE )
	for ( int i = 0; i < 2; i++ ) {
		start_time = Time::get_singleton()->get_ticks_msec();
		pTerrain->bakeMesh( 0 );
		LOG( MESG, "Bake ArrayMesh: ", Time::get_singleton()->get_ticks_msec() - start_time, "ms" );
	}
#endif // T3D_EDITOR_NATIVE
}

// -----------------------------------------------------------------------------
// Protected functions

String Terrain3DUtil::getNativeBuildFlavor(
	void ) {
	return String( t3dGetNativeBuildFlavor() );
}

int64_t Terrain3DUtil::getNativeApiRevision(
	void ) {
	return T3D_NATIVE_API_REVISION;
}

String Terrain3DUtil::getNativeBuildId(
	void ) {
	return String( s_pszNativeBuildId );
}

void Terrain3DUtil::_bind_methods(
	void ) {
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "get_native_build_flavor" ),
								 &Terrain3DUtil::getNativeBuildFlavor );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "get_native_api_revision" ),
								 &Terrain3DUtil::getNativeApiRevision );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "get_native_build_id" ), &Terrain3DUtil::getNativeBuildId );
	// Control map converters
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "as_float", "value" ), &t3dAsFloat );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "as_uint", "value" ), &t3dAsUint );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "get_base", "pixel" ), &t3dGdGetBase );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_base", "base" ), &t3dGdEncBase );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "get_overlay", "pixel" ), &t3dGdGetOverlay );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_overlay", "overlay" ), &t3dGdEncOverlay );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "get_blend", "pixel" ), &t3dGdGetBlend );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_blend", "blend" ), &t3dGdEncBlend );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "get_uv_rotation", "pixel" ), &t3dGdGetUvRotation );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_uv_rotation", "rotation" ), &t3dGdEncUvRotation );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "get_uv_scale", "pixel" ), &t3dGdGetUvScale );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_uv_scale", "scale" ), &t3dGdEncUvScale );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "is_hole", "pixel" ), &t3dGdIsHole );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_hole", "pixel" ), &t3dEncHole );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "is_nav", "pixel" ), &t3dGdIsNav );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_nav", "pixel" ), &t3dEncNav );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "is_auto", "pixel" ), &t3dGdIsAuto );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "enc_auto", "pixel" ), &t3dEncAuto );

	// String functions
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "filename_to_location", "filename" ),
								 &Terrain3DUtil::filenameToLocation );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "location_to_filename", "region_location" ),
								 &Terrain3DUtil::locationToFilename );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "location_to_string", "region_location" ),
								 &Terrain3DUtil::locationToString );

	// Image handling
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "black_to_alpha", "image" ), &Terrain3DUtil::blackToAlpha );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "get_min_max", "image" ), &Terrain3DUtil::getMinMax );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "get_thumbnail", "image", "size" ),
								 &Terrain3DUtil::getThumbnail,
								 DEFVAL( V2I( 256 ) ) );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "get_filled_image", "size", "color", "create_mipmaps", "eFormat" ),
								 &Terrain3DUtil::getFilledImage );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "load_image", "file_name", "cache_mode", "r16_height_range", "r16_size" ),
								 &Terrain3DUtil::loadImage,
								 DEFVAL( ResourceLoader::CACHE_MODE_IGNORE ),
								 DEFVAL( Vector2( 0.f, 255.f ) ),
								 DEFVAL( s_v2iZero ) );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "pack_image",
										   "src_rgb",
										   "src_a",
										   "src_ao",
										   "invert_green",
										   "invert_alpha",
										   "normalize_alpha",
										   "alpha_channel",
										   "ao_channel" ),
								 &Terrain3DUtil::packImage,
								 DEFVAL( false ),
								 DEFVAL( false ),
								 DEFVAL( false ),
								 DEFVAL( 0 ),
								 DEFVAL( 0 ) );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "pack_pbr_rgba16ui",
										   "normal",
										   "roughness",
										   "displacement",
										   "ao",
										   "metallic",
										   "invert_normal_green",
										   "roughness_channel",
										   "displacement_channel",
										   "ao_channel",
										   "metallic_channel",
										   "generate_mipmaps" ),
								 &Terrain3DUtil::packPbrRgba16ui,
								 DEFVAL( Ref< Image >() ),
								 DEFVAL( Ref< Image >() ),
								 DEFVAL( false ),
								 DEFVAL( 0 ),
								 DEFVAL( 0 ),
								 DEFVAL( 0 ),
								 DEFVAL( 0 ),
								 DEFVAL( true ) );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "pack_surface_rgba8",
										   "normal",
										   "roughness",
										   "ao",
										   "invert_normal_green",
										   "roughness_channel",
										   "ao_channel",
										   "generate_mipmaps" ),
								 &Terrain3DUtil::packSurfaceRgba8,
								 DEFVAL( Ref< Image >() ),
								 DEFVAL( false ),
								 DEFVAL( 0 ),
								 DEFVAL( 0 ),
								 DEFVAL( true ) );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "pack_displacement_r8", "displacement", "channel", "generate_mipmaps" ),
								 &Terrain3DUtil::packDisplacementR8,
								 DEFVAL( 0 ),
								 DEFVAL( true ) );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "save_displacement_r8_dds", "displacement", "path" ),
								 &Terrain3DUtil::saveDisplacementR8Dds );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "luminance_to_height", "src_rgb" ),
								 &Terrain3DUtil::luminanceToHeight );
}
