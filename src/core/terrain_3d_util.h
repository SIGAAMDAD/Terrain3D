// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_UTIL_CLASS_H
#define TERRAIN3D_UTIL_CLASS_H

#include "Terrain3DConstants.h"
#include "assets/generated_texture.h"
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

class T3DTerrain;

// This file holds stateless utility functions for both C++ and GDScript
// The class exposes static member and inline functions to GDscript
// The inline helpers below are free functions shared by Terrain3D subsystems.
// However some of these inline functions are also exposed to GDScript

class T3DUtil : public Object {
	GDCLASS( T3DUtil, Object );
	CLASS_NAME_STATIC( "T3DUtil" );

public:
	// Print info to the console
	static void printArr( const String &rName, const Array &rArr, const int nLevel = 2 ); // Level 2: DEBUG
	static void printDict( const String &rName, const Dictionary &rDict, const int nLevel = 2 ); // Level 2: DEBUG
	static void dumpGentex( const T3DGeneratedTexture &rGen, const String &rName = "" );
	static void dumpMaps( const TypedArray<Image> &rMaps, const String &rName = "" );

	// String functions
	static Vector2i filenameToLocation( const String &rFilename );
	static Vector2i stringToLocation( const String &rString );
	static String locationToFilename( const Vector2i &rRegionLoc );
	static String locationToString( const Vector2i &rRegionLoc );
	static PackedStringArray getFiles( const String &rDir, const String &rGlob = "*" );

	// Image operations
	static Ref<Image> blackToAlpha( const Ref<Image> &rImage );
	static Vector2 getMinMax( const Ref<Image> &rImage );
	static Ref<Image> getThumbnail( const Ref<Image> &rImage, const Vector2i &rSize = V2I( 256 ) );
	static Ref<Image> getFilledImage( const Vector2i &rSize,
			const Color &rColor = COLOR_BLACK,
			const bool bCreateMipmaps = true,
			const Image::Format eFormat = Image::FORMAT_MAX );
	static Ref<Image> loadImage( const String &rFileName,
			const int nCacheMode = ResourceLoader::CACHE_MODE_IGNORE,
			const Vector2 &rR16HeightRange = Vector2( 0.f, 255.f ),
			const Vector2i &rR16Size = s_v2iZero );
	static Ref<Image> packImage( const Ref<Image> &rSrcRgb,
			const Ref<Image> &rSrcA,
			const Ref<Image> &rSrcAo,
			const bool bInvertGreen = false,
			const bool bInvertAlpha = false,
			const bool bNormalizeAlpha = false,
			const int nAlphaChannel = 0,
			const int nAoChannel = 0 );
	// Packs six independently decoded PBR values into one RGBA16UI image:
	// R high/low = normal X / AO, G high/low = normal Y / metallic,
	// B = 16-bit roughness, A = 16-bit displacement. Mipmaps are generated
	// from decoded values before each level is repacked.
	static Ref<Image> packPbrRgba16ui( const Ref<Image> &rNormal,
			const Ref<Image> &rRoughness,
			const Ref<Image> &rDisplacement,
			const Ref<Image> &rAo = Ref<Image>(),
			const Ref<Image> &rMetallic = Ref<Image>(),
			const bool bInvertNormalGreen = false,
			const int nRoughnessChannel = 0,
			const int nDisplacementChannel = 0,
			const int nAoChannel = 0,
			const int nMetallicChannel = 0,
			const bool bGenerateMipmaps = true );
	static Ref<Image> getDefaultPackedPbrImage( const Vector2i &rSize, const bool bCreateMipmaps = true );
	// Optimized runtime layout: RGBA8 surface (normal XY, roughness, AO) and
	// independent R8_UNORM displacement. Terrain displacement is a normalized
	// material mask, not authoritative terrain elevation, so 8-bit storage avoids
	// wasting twice the memory and bandwidth for imperceptible precision.
	static Ref<Image> packSurfaceRgba8( const Ref<Image> &rNormal,
			const Ref<Image> &rRoughness,
			const Ref<Image> &rAo = Ref<Image>(),
			const bool bInvertNormalGreen = false,
			const int nRoughnessChannel = 0,
			const int nAoChannel = 0,
			const bool bGenerateMipmaps = true );
	static Ref<Image> packDisplacementR8( const Ref<Image> &rDisplacement,
			const int nDisplacementChannel = 0,
			const bool bGenerateMipmaps = true );
	// Deprecated compatibility alias. Returns an R8 image.
	EA_FORCE_INLINE static Ref<Image> packDisplacementR16( const Ref<Image> &rDisplacement,
			const int nDisplacementChannel = 0,
			const bool bGenerateMipmaps = true ) {
		return packDisplacementR8( rDisplacement, nDisplacementChannel, bGenerateMipmaps );
	}
	static Ref<Image> getDefaultSurfaceImage( const Vector2i &rSize, const bool bCreateMipmaps = true );
	static Ref<Image> getDefaultDisplacementImage( const Vector2i &rSize, const bool bCreateMipmaps = true );
	static Ref<Image> convertPackedPbrRgba16uiToSurface( const Ref<Image> &rPacked,
			const bool bGenerateMipmaps = true );
	static Ref<Image> convertPackedPbrRgba16uiToDisplacement( const Ref<Image> &rPacked,
			const bool bGenerateMipmaps = true );
	static Error saveDisplacementR8Dds( const Ref<Image> &rDisplacement, const String &rPath );
	// Deprecated compatibility alias. Accepts R8 data.
	EA_FORCE_INLINE static Error saveDisplacementR16Dds( const Ref<Image> &rDisplacement, const String &rPath ) {
		return saveDisplacementR8Dds( rDisplacement, rPath );
	}
	static Ref<Image> luminanceToHeight( const Ref<Image> &rSrcRgb );
	static void benchmark( T3DTerrain *pTerrain );

protected:
	static void _bind_methods( void );
};

// -----------------------------------------------------------------------------
// Inline functions

// -----------------------------------------------------------------------------
// Type conversion

// Convert Vector3 to Vector2i, ignoring Y
EA_FORCE_INLINE Vector2i t3dVector3ToVector2i( const Vector3 &rV3 ) {
	return Vector2i( rV3.x, rV3.z );
}

// Convert Vector2i to Vector3, ignoring Y
EA_FORCE_INLINE Vector3 t3dVector2iToVector3( const Vector2i &rV2 ) {
	return Vector3( rV2.x, 0., rV2.y );
}

// Convert Vector3 to Vector2, ignoring Y
EA_FORCE_INLINE Vector2 t3dVector3ToVector2( const Vector3 &rV3 ) {
	return Vector2( rV3.x, rV3.z );
}

// Convert Vector2 to Vector3, ignoring Y
EA_FORCE_INLINE Vector3 t3dVector2ToVector3( const Vector2 &rV2 ) {
	return Vector3( rV2.x, 0., rV2.y );
}

// -----------------------------------------------------------------------------
// Math

EA_FORCE_INLINE bool t3dIsValidRegionSize( int value ) {
	return value >= 64 && value <= 2048 && is_power_of_2( value );
}

// Integer round to multiples
// https://stackoverflow.com/questions/3407012/rounding-up-to-the-nearest-multiple-of-a-number

// Integer round up to a multiple
template <typename T> EA_FORCE_INLINE T t3dIntCeilMult( const T numToRound, const T multiple ) {
	static_assert( std::numeric_limits<T>::is_integer, "Only integer types are allowed" );
	ASSERT( multiple != 0, 0 );
	T isPositive = ( T )( numToRound >= 0 );
	return ( ( numToRound + isPositive * ( multiple - 1 ) ) / multiple ) * multiple;
}

// Integer round up to a power of 2 multiple (3.7x faster)
template <typename T> EA_FORCE_INLINE T t3dIntCeilPow2( T numToRound, T multiple ) {
	static_assert( std::numeric_limits<T>::is_integer, "Only integer types are allowed" );
	ASSERT( is_power_of_2( multiple ), t3dIntCeilMult( numToRound, multiple ) );
	return ( numToRound + multiple - 1 ) & -multiple;
}

// Integer round to nearest +/- multiple
// https://stackoverflow.com/questions/29557459/round-to-nearest-multiple-of-a-number
template <typename T> inline T t3dIntRoundMult( const T numToRound, const T multiple ) {
	static_assert( std::numeric_limits<T>::is_integer, "Only integer types are allowed" );
	ASSERT( multiple != 0, 0 );
	T abs_num;
	EA_FORCE_INLINE if constexpr ( std::is_signed_v<T> ) {
		abs_num = std::abs( numToRound );
	} else {
		abs_num = numToRound;
	}
	T result = abs_num + multiple / 2;
	result -= result % multiple;
	result *= numToRound > 0 ? 1 : -1;
	return result;
}

// Integer division with rounding up, down, nearest
// https : //stackoverflow.com/questions/2422712/rounding-integer-division-instead-of-truncating/58568736#58568736
#define V2I_DIVIDE_CEIL( v, f ) \
	Vector2i( t3dIntDivideCeil( v.x, ( int32_t )( f ) ), t3dIntDivideCeil( v.y, ( int32_t )( f ) ) )
#define V2I_DIVIDE_FLOOR( v, f ) \
	Vector2i( t3dIntDivideFloor( v.x, ( int32_t )( f ) ), t3dIntDivideFloor( v.y, ( int32_t )( f ) ) )

// Integer division rounding up
template <typename T> EA_FORCE_INLINE T t3dIntDivideCeil( const T numer, const T denom ) {
	static_assert( std::numeric_limits<T>::is_integer, "Only integer types are allowed" );
	T result = ( ( numer ) < 0 ) != ( ( denom ) < 0 )
			? ( numer ) / ( denom )
			: ( ( numer ) + ( ( denom ) < 0 ? ( denom ) + 1 : ( denom )-1 ) ) / ( denom );
	return result;
}

// Integer division rounding down
template <typename T> EA_FORCE_INLINE T t3dIntDivideFloor( const T numer, const T denom ) {
	static_assert( std::numeric_limits<T>::is_integer, "Only integer types are allowed" );
	T result = ( ( numer ) < 0 ) != ( ( denom ) < 0 )
			? ( ( numer ) - ( ( denom ) < 0 ? ( denom ) + 1 : ( denom )-1 ) ) / ( denom )
			: ( numer ) / ( denom );
	return result;
}

// Integer division rounding to nearest int
template <typename T> EA_FORCE_INLINE T t3dIntDivideRound( const T numer, const T denom ) {
	static_assert( std::numeric_limits<T>::is_integer, "Only integer types are allowed" );
	T result = ( ( numer ) < 0 ) != ( ( denom ) < 0 ) ? ( ( numer ) - ( ( denom ) / 2 ) ) / ( denom )
													  : ( ( numer ) + ( ( denom ) / 2 ) ) / ( denom );
	return result;
}

// Returns the bilinearly interpolated value derived from parameters:
// * 4 values to be interpolated
// * Positioned at the 4 corners of the p_pos00 - p_pos11 rectangle
// * Interpolated to the position p_pos, which is global, not a 0-1 percentage
inline real_t t3dBilerp( const real_t fV00,
		const real_t fV01,
		const real_t fV10,
		const real_t fV11,
		const Vector2 &rPos00,
		const Vector2 &rPos11,
		const Vector2 &rPos ) {
	real_t fX2x1 = rPos11.x - rPos00.x;
	real_t fY2y1 = rPos11.y - rPos00.y;
	real_t fX2x = rPos11.x - rPos.x;
	real_t fY2y = rPos11.y - rPos.y;
	real_t fXx1 = rPos.x - rPos00.x;
	real_t fYy1 = rPos.y - rPos00.y;
	return ( fV00 * fX2x * fY2y + fV01 * fX2x * fYy1 + fV10 * fXx1 * fY2y + fV11 * fXx1 * fYy1 ) / ( fX2x1 * fY2y1 );
}

EA_FORCE_INLINE real_t t3dBilerp( const real_t fV00,
		const real_t fV01,
		const real_t fV10,
		const real_t fV11,
		const Vector3 &rPos00,
		const Vector3 &rPos11,
		const Vector3 &rPos ) {
	Vector2 pos00 = Vector2( rPos00.x, rPos00.z );
	Vector2 pos11 = Vector2( rPos11.x, rPos11.z );
	Vector2 pos = Vector2( rPos.x, rPos.z );
	return t3dBilerp( fV00, fV01, fV10, fV11, pos00, pos11, pos );
}

EA_FORCE_INLINE Rect2 t3dAabbToRect( const AABB &rAabb ) {
	Rect2 rect;
	rect.position = Vector2( rAabb.position.x, rAabb.position.z );
	rect.size = Vector2( rAabb.size.x, rAabb.size.z );
	return rect;
}

// Functions to match GLSL when needing to duplicate shader code CPU side.
EA_FORCE_INLINE real_t t3dSmoothstep( const real_t fLow, const real_t fHigh, const real_t fValue ) {
	real_t fT = CLAMP( ( fValue - fLow ) / ( fHigh - fLow ), 0.f, 1.f );
	return fT * fT * ( 3.f - 2.f * fT );
}

EA_FORCE_INLINE Vector2 t3dSmoothstep( const real_t fLow, const real_t fHigh, const Vector2 &vValue ) {
	Vector2 t = ( vValue - Vector2( fLow, fLow ) ) / ( fHigh - fLow );
	t.x = CLAMP( t.x, 0.f, 1.f );
	t.y = CLAMP( t.y, 0.f, 1.f );
	return t * t * ( Vector2( 3.f, 3.f ) - 2.f * t );
}

EA_FORCE_INLINE Vector3 t3dSmoothstep( const real_t fLow, const real_t fHigh, const Vector3 &vValue ) {
	Vector3 t = ( vValue - Vector3( fLow, fLow, fLow ) ) / ( fHigh - fLow );
	t.x = CLAMP( t.x, 0.f, 1.f );
	t.y = CLAMP( t.y, 0.f, 1.f );
	t.z = CLAMP( t.z, 0.f, 1.f );
	return t * t * ( Vector3( 3.f, 3.f, 3.f ) - 2.f * t );
}

// -----------------------------------------------------------------------------
// Control-map handling

// Getters read the 32-bit float as a 32-bit uint, then mask bits to retreive value
// Encoders return a full 32-bit uint with bits in the proper place for ORing
// Aliases for GDScript prefixed with gd_ since it can't handle overridden functions
EA_FORCE_INLINE float t3dAsFloat( const uint32_t nValue ) {
	return *( float * )&nValue;
}
EA_FORCE_INLINE uint32_t t3dAsUint( const float fValue ) {
	return *( uint32_t * )&fValue;
}

EA_FORCE_INLINE uint8_t t3dGetBase( const uint32_t nPixel ) {
	return nPixel >> 27 & 0x1F;
}
EA_FORCE_INLINE uint8_t t3dGetBase( const float fPixel ) {
	return t3dGetBase( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncBase( const uint8_t nBase ) {
	return ( nBase & 0x1F ) << 27;
}
EA_FORCE_INLINE uint32_t t3dGdGetBase( const uint32_t nPixel ) {
	return t3dGetBase( nPixel );
}
EA_FORCE_INLINE uint32_t t3dGdEncBase( const uint32_t nBase ) {
	return t3dEncBase( nBase );
}

EA_FORCE_INLINE uint8_t t3dGetOverlay( const uint32_t nPixel ) {
	return nPixel >> 22 & 0x1F;
}
EA_FORCE_INLINE uint8_t t3dGetOverlay( const float fPixel ) {
	return t3dGetOverlay( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncOverlay( const uint8_t nOver ) {
	return ( nOver & 0x1F ) << 22;
}
EA_FORCE_INLINE uint32_t t3dGdGetOverlay( const uint32_t nPixel ) {
	return t3dGetOverlay( nPixel );
}
EA_FORCE_INLINE uint32_t t3dGdEncOverlay( const uint32_t nOver ) {
	return t3dEncOverlay( nOver );
}

EA_FORCE_INLINE uint8_t t3dGetBlend( const uint32_t nPixel ) {
	return nPixel >> 14 & 0xFF;
}
EA_FORCE_INLINE uint8_t t3dGetBlend( const float fPixel ) {
	return t3dGetBlend( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncBlend( const uint8_t nBlend ) {
	return ( nBlend & 0xFF ) << 14;
}
EA_FORCE_INLINE uint32_t t3dGdGetBlend( const uint32_t nPixel ) {
	return t3dGetBlend( nPixel );
}
EA_FORCE_INLINE uint32_t t3dGdEncBlend( const uint32_t nBlend ) {
	return t3dEncBlend( nBlend );
}

EA_FORCE_INLINE uint8_t t3dGetUvRotation( const uint32_t nPixel ) {
	return nPixel >> 10 & 0xF;
}
EA_FORCE_INLINE uint8_t t3dGetUvRotation( const float fPixel ) {
	return t3dGetUvRotation( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncUvRotation( const uint8_t nRotation ) {
	return ( nRotation & 0xF ) << 10;
}
EA_FORCE_INLINE uint32_t t3dGdGetUvRotation( const uint32_t nPixel ) {
	return t3dGetUvRotation( nPixel );
}
EA_FORCE_INLINE uint32_t t3dGdEncUvRotation( const uint32_t nRotation ) {
	return t3dEncUvRotation( nRotation );
}

EA_FORCE_INLINE uint8_t t3dGetUvScale( const uint32_t nPixel ) {
	return nPixel >> 7 & 0x7;
}
EA_FORCE_INLINE uint8_t t3dGetUvScale( const float fPixel ) {
	return t3dGetUvScale( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncUvScale( const uint8_t nScale ) {
	return ( nScale & 0x7 ) << 7;
}
EA_FORCE_INLINE uint32_t t3dGdGetUvScale( const uint32_t nPixel ) {
	return t3dGetUvScale( nPixel );
}
EA_FORCE_INLINE uint32_t t3dGdEncUvScale( const uint32_t nScale ) {
	return t3dEncUvScale( nScale );
}

EA_FORCE_INLINE bool t3dIsHole( const uint32_t nPixel ) {
	return ( nPixel >> 2 & 0x1 ) == 1;
}
EA_FORCE_INLINE bool t3dIsHole( const float fPixel ) {
	return t3dIsHole( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncHole( const bool bHole ) {
	return ( bHole & 0x1 ) << 2;
}
EA_FORCE_INLINE bool t3dGdIsHole( const uint32_t nPixel ) {
	return t3dIsHole( nPixel );
}

EA_FORCE_INLINE bool t3dIsNav( const uint32_t nPixel ) {
	return ( nPixel >> 1 & 0x1 ) == 1;
}
EA_FORCE_INLINE bool t3dIsNav( const float fPixel ) {
	return t3dIsNav( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncNav( const bool bNav ) {
	return ( bNav & 0x1 ) << 1;
}
EA_FORCE_INLINE bool t3dGdIsNav( const uint32_t nPixel ) {
	return t3dIsNav( nPixel );
}

EA_FORCE_INLINE bool t3dIsAuto( const uint32_t nPixel ) {
	return ( nPixel & 0x1 ) == 1;
}
EA_FORCE_INLINE bool t3dIsAuto( const float fPixel ) {
	return t3dIsAuto( t3dAsUint( fPixel ) );
}
EA_FORCE_INLINE uint32_t t3dEncAuto( const bool bAuto ) {
	return bAuto & 0x1;
}
EA_FORCE_INLINE bool t3dGdIsAuto( const uint32_t nPixel ) {
	return t3dIsAuto( nPixel );
}

// -----------------------------------------------------------------------------
// Memory helpers

EA_FORCE_INLINE bool t3dRemoveFromTree( Node *pNode ) {
	// Note: is_in_tree() doesn't work in Godot-cpp 4.1.3
	if ( pNode ) {
		Node *parent = pNode->get_parent();
		if ( parent ) {
			parent->remove_child( pNode );
			return true;
		}
	}
	return false;
}

EA_FORCE_INLINE String t3dPtrToStr( const void *pPtr ) {
	return "0x" + String::num_uint64( ( uint64_t )( pPtr ), 16, true );
}

// Returns if A is shallow different from B
// O(1) size and pointer compare for Array/Dictionary/String/StringName/NodePath
// Operator==() otherwise
// Two arrays/dicts with different pointers could have the same value, this will return true
// Two strings or ints with different pointers but the same value will return false
// Could be extended for PackedArray and other special types
template <typename T> EA_FORCE_INLINE bool t3dDiffers( const T &a, const T &b ) {
	if constexpr ( std::is_base_of_v<String, T> ) {
		if ( a.length() != b.length() ) {
			return true;
		}
		if ( a.is_empty() ) { // Both empty
			return false;
		}
		// Both non-empty, but same length
		if ( a.ptr() == b.ptr() ) { // Same COW buffer
			return false;
		}
		return a != b; // Full comparison
	} else if constexpr ( std::is_base_of_v<Array, T> || std::is_base_of_v<Dictionary, T> ) {
		if ( a.size() != b.size() ) { // Redundant but common path predictable
			return true;
		}
		// These store a real GDExtensionTypePtr (void**) at offset 0
		return *( const void ** )a._native_ptr() != *( const void ** )b._native_ptr();
	} else {
		return a != b;
	}
}

// Sets A if different from B, otherwise returns
#define SET_IF_DIFF( a, b ) \
	if ( t3dDiffers( a, b ) ) { \
		a = b; \
	} else { \
		return; \
	}
#endif // TERRAIN3D_UTIL_CLASS_H
