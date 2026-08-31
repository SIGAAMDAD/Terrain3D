// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "data/terrain_3d_runtime_bake_codec.h"

#include "core/terrain_3d_byte_writer.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_world_manifest.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/variant/utility_functions.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>


EA_FORCE_INLINE static uint16_t t3dRuntimeBakeCodecReadU16( const uint8_t *p ) {
	return ( uint16_t )( p[0] | ( ( uint16_t )p[1] << 8u ) );
}
EA_FORCE_INLINE static uint32_t t3dRuntimeBakeCodecReadU32( const uint8_t *p ) {
	return ( uint32_t )p[0] | ( ( uint32_t )p[1] << 8u ) | ( ( uint32_t )p[2] << 16u ) | ( ( uint32_t )p[3] << 24u );
}
EA_FORCE_INLINE static int32_t t3dRuntimeBakeCodecReadI32( const uint8_t *p ) {
	return ( int32_t )t3dRuntimeBakeCodecReadU32( p );
}
EA_FORCE_INLINE static uint64_t t3dRuntimeBakeCodecReadU64( const uint8_t *p ) {
	return ( uint64_t )t3dRuntimeBakeCodecReadU32( p ) | ( ( uint64_t )t3dRuntimeBakeCodecReadU32( p + 4 ) << 32u );
}
EA_FORCE_INLINE static float t3dRuntimeBakeCodecReadF32( const uint8_t *p ) {
	const uint32_t bits = t3dRuntimeBakeCodecReadU32( p );
	float v = 0.f;
	memcpy( &v, &bits, 4 );
	return v;
}
EA_FORCE_INLINE static uint32_t t3dRuntimeBakeCodecReadControlPixel(
        const uint8_t *pControl, const size_t nControlBytes, const uint32_t nIndex ) {
	if ( pControl == NULL || ( size_t )( nIndex + 1u ) * 4u > nControlBytes ) {
		return s_nRegionDefaultControl;
	}
	uint32_t value = 0u;
	memcpy( &value, pControl + ( size_t )nIndex * 4u, 4u );
	return value;
}
EA_FORCE_INLINE static void t3dRuntimeBakeCodecSetBit( uint8_t *pBytes, const uint32_t nIndex ) {
	pBytes[nIndex >> 3u] |= ( uint8_t )( 1u << ( nIndex & 7u ) );
}
EA_FORCE_INLINE static bool t3dRuntimeBakeCodecResizeBytes( PackedByteArray &rBytes, const size_t nBytes ) {
	if ( nBytes > ( size_t )INT64_MAX ) {
		return false;
	}
	rBytes.resize( ( int64_t )nBytes );
	return ( size_t )rBytes.size() == nBytes;
}

EA_FORCE_INLINE static String t3dRuntimeBakeCodecDescriptorScenePath( const t3dStreamedNodeRuntimeDescriptor_t &rDescriptor ) {
	if ( !rDescriptor.m_scenePath.is_empty() ) {
		return rDescriptor.m_scenePath;
	}
	return rDescriptor.m_scene.is_valid() ? rDescriptor.m_scene->get_path() : String();
}
EA_FORCE_INLINE static String t3dRuntimeBakeCodecDescriptorShadowPath( const t3dStreamedNodeRuntimeDescriptor_t &rDescriptor ) {
	if ( !rDescriptor.m_shadowProxyScenePath.is_empty() ) {
		return rDescriptor.m_shadowProxyScenePath;
	}
	return rDescriptor.m_shadowProxyScene.is_valid() ? rDescriptor.m_shadowProxyScene->get_path() : String();
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeHoleMask( const Ref<Image> &rControl, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( nRegionSize <= 0 || nRegionSize > UINT16_MAX
	        || ( rControl.is_valid()
	                && ( rControl->get_width() != nRegionSize || rControl->get_height() != nRegionSize
	                        || rControl->get_format() != Image::FORMAT_RF ) ) ) {
		return bytes;
	}
	const uint32_t samples = ( uint32_t )nRegionSize * ( uint32_t )nRegionSize;
	const uint32_t plane = ( samples + 7u ) / 8u;
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, 12u + plane ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), ( size_t )bytes.size() );
	writer.writeU32( s_nHoleMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( ( uint16_t )nRegionSize );
	writer.writeU32( samples );
	uint8_t *const pBits = bytes.ptrw() + writer.offset();
	memset( pBits, 0, plane );
	const PackedByteArray control_bytes = rControl.is_valid() ? rControl->get_data() : PackedByteArray();
	const uint8_t *const pControl = control_bytes.is_empty() ? NULL : control_bytes.ptr();
	for ( uint32_t i = 0u; i < samples; ++i ) {
		if ( t3dIsHole( t3dRuntimeBakeCodecReadControlPixel( pControl, ( size_t )control_bytes.size(), i ) ) ) {
			t3dRuntimeBakeCodecSetBit( pBits, i );
		}
	}
	return bytes;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeNavigationMask( const Ref<Image> &rControl, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( nRegionSize <= 0 || nRegionSize > UINT16_MAX
	        || ( rControl.is_valid()
	                && ( rControl->get_width() != nRegionSize || rControl->get_height() != nRegionSize
	                        || rControl->get_format() != Image::FORMAT_RF ) ) ) {
		return bytes;
	}
	const uint32_t samples = ( uint32_t )nRegionSize * ( uint32_t )nRegionSize;
	const uint32_t plane = ( samples + 7u ) / 8u;
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, 16u + ( size_t )plane * 2u ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), ( size_t )bytes.size() );
	writer.writeU32( s_nNavMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( ( uint16_t )nRegionSize );
	writer.writeU32( samples );
	writer.writeU32( plane );
	uint8_t *const pValid = bytes.ptrw() + writer.offset();
	uint8_t *const pNavigation = pValid + plane;
	memset( pValid, 0, ( size_t )plane * 2u );
	const PackedByteArray control_bytes = rControl.is_valid() ? rControl->get_data() : PackedByteArray();
	const uint8_t *const pControl = control_bytes.is_empty() ? NULL : control_bytes.ptr();
	for ( uint32_t i = 0u; i < samples; ++i ) {
		const uint32_t control = t3dRuntimeBakeCodecReadControlPixel( pControl, ( size_t )control_bytes.size(), i );
		if ( !t3dIsHole( control ) ) {
			t3dRuntimeBakeCodecSetBit( pValid, i );
		}
		if ( !t3dIsHole( control ) && t3dIsNav( control ) ) {
			t3dRuntimeBakeCodecSetBit( pNavigation, i );
		}
	}
	return bytes;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeNavigationMask( const PackedInt32Array &rControls, const int nWidth ) {
	PackedByteArray bytes;
	if ( nWidth <= 0 || rControls.size() != nWidth * nWidth || nWidth > UINT16_MAX ) {
		return bytes;
	}
	const uint32_t samples = ( uint32_t )nWidth * ( uint32_t )nWidth;
	const uint32_t plane = ( samples + 7u ) / 8u;
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, 16u + ( size_t )plane * 2u ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), ( size_t )bytes.size() );
	writer.writeU32( s_nNavMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( ( uint16_t )nWidth );
	writer.writeU32( samples );
	writer.writeU32( plane );
	uint8_t *const pValid = bytes.ptrw() + writer.offset();
	uint8_t *const pNavigation = pValid + plane;
	memset( pValid, 0, ( size_t )plane * 2u );
	const int32_t *const pControls = rControls.ptr();
	for ( uint32_t i = 0u; i < samples; ++i ) {
		const uint32_t control = ( uint32_t )pControls[i];
		if ( !t3dIsHole( control ) ) {
			t3dRuntimeBakeCodecSetBit( pValid, i );
		}
		if ( !t3dIsHole( control ) && t3dIsNav( control ) ) {
			t3dRuntimeBakeCodecSetBit( pNavigation, i );
		}
	}
	return bytes;
}

bool Terrain3DRuntimeBakeCodec::getNavigationMaskView( const PackedByteArray &rBlob, t3dNavigationMaskView_t &rView ) {
	rView = t3dNavigationMaskView_t();
	if ( rBlob.size() < 16 ) {
		return false;
	}
	const uint8_t *const p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nNavMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	const uint16_t width = t3dRuntimeBakeCodecReadU16( p + 6 );
	const uint32_t samples = t3dRuntimeBakeCodecReadU32( p + 8 );
	const uint32_t plane = t3dRuntimeBakeCodecReadU32( p + 12 );
	const int64_t required_bytes = 16 + ( int64_t )plane * 2;
	if ( width == 0u || samples != ( uint32_t )width * width || plane != ( samples + 7u ) / 8u || rBlob.size() < required_bytes ) {
		return false;
	}
	rView.m_pValidBits = p + 16;
	rView.m_pNavigationBits = p + 16 + plane;
	rView.m_nPlaneBytes = plane;
	rView.m_nSamples = samples;
	rView.m_nWidth = width;
	return true;
}

bool Terrain3DRuntimeBakeCodec::decodeNavigationMask(
        const PackedByteArray &rBlob, int &nWidth, PackedByteArray &rValidBits, PackedByteArray &rNavigationBits ) {
	nWidth = 0;
	rValidBits.clear();
	rNavigationBits.clear();
	t3dNavigationMaskView_t view;
	if ( !getNavigationMaskView( rBlob, view ) ) {
		return false;
	}
	nWidth = ( int )view.m_nWidth;
	rValidBits.resize( view.m_nPlaneBytes );
	rNavigationBits.resize( view.m_nPlaneBytes );
	memcpy( rValidBits.ptrw(), view.m_pValidBits, view.m_nPlaneBytes );
	memcpy( rNavigationBits.ptrw(), view.m_pNavigationBits, view.m_nPlaneBytes );
	return true;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeMinMaxHierarchy( const Ref<Image> &rHeight, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( rHeight.is_null() || nRegionSize <= 0 || nRegionSize > UINT16_MAX || rHeight->get_width() != nRegionSize
	        || rHeight->get_height() != nRegionSize || rHeight->get_format() != Image::FORMAT_R16 ) {
		return bytes;
	}
	const PackedByteArray source = rHeight->get_data();
	if ( source.size() < ( int64_t )nRegionSize * nRegionSize * 2 ) {
		return bytes;
	}
	int levels = 1;
	size_t output_bytes = 12u;
	for ( int side = nRegionSize;; side = ( side + 1 ) / 2 ) {
		output_bytes += 8u + ( size_t )side * side * 8u;
		if ( side == 1 ) {
			break;
		}
		++levels;
	}
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, output_bytes ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), output_bytes );
	writer.writeU32( s_nMinmaxMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( ( uint16_t )nRegionSize );
	writer.writeU32( ( uint32_t )levels );
	const uint8_t *const pSource = source.ptr();
	const uint32_t source_count = ( uint32_t )nRegionSize * ( uint32_t )nRegionSize;
	writer.writeU32( ( uint32_t )nRegionSize );
	writer.writeU32( source_count );
	for ( uint32_t i = 0u; i < source_count; ++i ) {
		const float h = t3dRegionCodecReadHeightBytes( pSource, ( int )i, Image::FORMAT_R16 );
		writer.writeF32( h );
		writer.writeF32( h );
	}
	if ( nRegionSize == 1 ) {
		return writer.complete() ? bytes : PackedByteArray();
	}

	// Level 0 is emitted directly from the R16 source. Only the first reduced
	// level is retained, then every later level is reduced in-place into the
	// beginning of the same high-water buffer. For a 2048 region this reduces
	// scratch from ~40 MiB (four full float planes) to ~8 MiB.
	int side = ( nRegionSize + 1 ) / 2;
	eastl::vector<Vector2, Terrain3DEastlAllocator> ranges;
	ranges.resize( ( size_t )side * side );
	for ( int z = 0; z < side; ++z ) {
		for ( int x = 0; x < side; ++x ) {
			float minimum = INFINITY;
			float maximum = -INFINITY;
			for ( int dz = 0; dz < 2; ++dz ) {
				for ( int dx = 0; dx < 2; ++dx ) {
					const int sx = x * 2 + dx;
					const int sz = z * 2 + dz;
					if ( sx >= nRegionSize || sz >= nRegionSize ) {
						continue;
					}
					const float h = t3dRegionCodecReadHeightBytes( pSource, sz * nRegionSize + sx, Image::FORMAT_R16 );
					minimum = MIN( minimum, h );
					maximum = MAX( maximum, h );
				}
			}
			ranges[( size_t )z * side + x] = Vector2( minimum, maximum );
		}
	}
	while ( true ) {
		const uint32_t count = ( uint32_t )side * ( uint32_t )side;
		writer.writeU32( ( uint32_t )side );
		writer.writeU32( count );
		for ( uint32_t i = 0u; i < count; ++i ) {
			writer.writeF32( ranges[i].x );
			writer.writeF32( ranges[i].y );
		}
		if ( side == 1 ) {
			break;
		}
		const int next_side = ( side + 1 ) / 2;
		for ( int z = 0; z < next_side; ++z ) {
			for ( int x = 0; x < next_side; ++x ) {
				float minimum = INFINITY;
				float maximum = -INFINITY;
				for ( int dz = 0; dz < 2; ++dz ) {
					for ( int dx = 0; dx < 2; ++dx ) {
						const int sx = x * 2 + dx;
						const int sz = z * 2 + dz;
						if ( sx >= side || sz >= side ) {
							continue;
						}
						const Vector2 range = ranges[( size_t )sz * side + sx];
						minimum = MIN( minimum, range.x );
						maximum = MAX( maximum, range.y );
					}
				}
				ranges[( size_t )z * next_side + x] = Vector2( minimum, maximum );
			}
		}
		ranges.resize( ( size_t )next_side * next_side );
		side = next_side;
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool Terrain3DRuntimeBakeCodec::getMinMaxLevelView( const PackedByteArray &rBlob, const int nMaximumSide, t3dMinMaxLevelView_t &rView ) {
	rView = t3dMinMaxLevelView_t();
	if ( rBlob.size() < 12 || nMaximumSide <= 0 ) {
		return false;
	}
	const uint8_t *const p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nMinmaxMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	const uint16_t source_side = t3dRuntimeBakeCodecReadU16( p + 6 );
	const uint32_t level_count = t3dRuntimeBakeCodecReadU32( p + 8 );
	if ( source_side == 0u || level_count == 0u || level_count > 32u ) {
		return false;
	}
	size_t offset = 12u;
	const uint8_t *pSelected = NULL;
	uint32_t selected_count = 0u;
	uint16_t selected_side = 0u;
	for ( uint32_t level = 0u; level < level_count; ++level ) {
		if ( offset + 8u > ( size_t )rBlob.size() ) {
			return false;
		}
		const uint32_t side = t3dRuntimeBakeCodecReadU32( p + offset );
		const uint32_t count = t3dRuntimeBakeCodecReadU32( p + offset + 4u );
		offset += 8u;
		if ( side == 0u || side > UINT16_MAX || count != side * side || ( uint64_t )count * 8u > ( uint64_t )rBlob.size() - offset ) {
			return false;
		}
		if ( selected_side == 0u && ( int )side <= nMaximumSide ) {
			selected_side = ( uint16_t )side;
			selected_count = count;
			pSelected = p + offset;
		}
		offset += ( size_t )count * 8u;
	}
	if ( offset != ( size_t )rBlob.size() || selected_side == 0u ) {
		return false;
	}
	rView.m_pRanges = pSelected;
	rView.m_nRangeCount = selected_count;
	rView.m_nSourceSide = source_side;
	rView.m_nLevelSide = selected_side;
	return true;
}

Vector2 Terrain3DRuntimeBakeCodec::getMinMaxRange( const t3dMinMaxLevelView_t &rView, const uint32_t nIndex ) {
	if ( rView.m_pRanges == NULL || nIndex >= rView.m_nRangeCount ) {
		return Vector2();
	}
	const uint8_t *const p = rView.m_pRanges + ( size_t )nIndex * 8u;
	return Vector2( t3dRuntimeBakeCodecReadF32( p ), t3dRuntimeBakeCodecReadF32( p + 4u ) );
}

bool Terrain3DRuntimeBakeCodec::decodeMinMaxLevel( const PackedByteArray &rBlob,
        const int nMaximumSide,
        int &nSourceSide,
        int &nLevelSide,
        eastl::vector<Vector2, Terrain3DEastlAllocator> &rRanges ) {
	nSourceSide = 0;
	nLevelSide = 0;
	rRanges.clear();
	t3dMinMaxLevelView_t view;
	if ( !getMinMaxLevelView( rBlob, nMaximumSide, view ) ) {
		return false;
	}
	nSourceSide = view.m_nSourceSide;
	nLevelSide = view.m_nLevelSide;
	rRanges.resize( view.m_nRangeCount );
	for ( uint32_t i = 0u; i < view.m_nRangeCount; ++i ) {
		rRanges[i] = getMinMaxRange( view, i );
	}
	return true;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeCollisionHeight( const PackedFloat32Array &rHeights, const int nWidth ) {
	PackedByteArray bytes;
	if ( nWidth <= 0 || nWidth > UINT16_MAX || rHeights.size() != nWidth * nWidth ) {
		return bytes;
	}
	const size_t payload_bytes = ( size_t )rHeights.size() * sizeof( float );
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, 12u + payload_bytes ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), ( size_t )bytes.size() );
	writer.writeU32( s_nCollisionHeightMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( ( uint16_t )nWidth );
	writer.writeU32( ( uint32_t )rHeights.size() );
	writer.writeBytes( rHeights.ptr(), payload_bytes );
	return writer.complete() ? bytes : PackedByteArray();
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeCollisionBounds( const float fMinHeight, const float fMaxHeight, const int nWidth ) {
	PackedByteArray bytes;
	if ( nWidth <= 0 || nWidth > UINT16_MAX || !t3dRuntimeBakeCodecResizeBytes( bytes, 16u ) ) {
		return bytes;
	}
	t3dByteWriter_t writer( bytes.ptrw(), ( size_t )bytes.size() );
	writer.writeU32( s_nCollisionBoundsMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( ( uint16_t )nWidth );
	writer.writeF32( fMinHeight );
	writer.writeF32( fMaxHeight );
	return writer.complete() ? bytes : PackedByteArray();
}

bool Terrain3DRuntimeBakeCodec::decodeCollisionHeight( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth ) {
	nWidth = 0;
	if ( rBlob.size() < 12 ) {
		return false;
	}
	const uint8_t *p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nCollisionHeightMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	nWidth = t3dRuntimeBakeCodecReadU16( p + 6 );
	const uint32_t count = t3dRuntimeBakeCodecReadU32( p + 8 );
	if ( nWidth <= 0 || count != ( uint32_t )nWidth * nWidth || rBlob.size() < 12 + ( int64_t )count * 4 ) {
		nWidth = 0;
		return false;
	}
	if ( rHeights.size() != ( int64_t )count ) {
		rHeights.resize( count );
	}
	memcpy( rHeights.ptrw(), p + 12, ( size_t )count * 4u );
	return true;
}
bool Terrain3DRuntimeBakeCodec::decodeCollisionHeightInto( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth ) {
	nWidth = 0;
	if ( rBlob.size() < 12 ) {
		return false;
	}
	const uint8_t *p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nCollisionHeightMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	nWidth = t3dRuntimeBakeCodecReadU16( p + 6 );
	const uint32_t count = t3dRuntimeBakeCodecReadU32( p + 8 );
	if ( nWidth <= 0 || count != ( uint32_t )nWidth * nWidth || rBlob.size() < 12 + ( int64_t )count * 4
	        || rHeights.size() != ( int64_t )count ) {
		nWidth = 0;
		return false;
	}
	memcpy( rHeights.ptrw(), p + 12, ( size_t )count * 4u );
	return true;
}

bool Terrain3DRuntimeBakeCodec::decodeCollisionBounds( const PackedByteArray &rBlob, float &fMinHeight, float &fMaxHeight, int &nWidth ) {
	if ( rBlob.size() < 16 ) {
		return false;
	}
	const uint8_t *p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nCollisionBoundsMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	nWidth = t3dRuntimeBakeCodecReadU16( p + 6 );
	fMinHeight = t3dRuntimeBakeCodecReadF32( p + 8 );
	fMaxHeight = t3dRuntimeBakeCodecReadF32( p + 12 );
	return nWidth > 0;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeNodeDescriptors(
        const Terrain3DRegion &rRegion, const Terrain3DRuntimeWorldManifest &rManifest ) {
	PackedByteArray bytes;
	const eastl::vector<t3dStreamedNodeRuntimeDescriptor_t> &nodes = rRegion.getStreamedNodesRuntimeInternal();
	const uint16_t count = ( uint16_t )MIN( nodes.size(), ( size_t )UINT16_MAX );
	eastl::vector<PackedByteArray, Terrain3DEastlAllocator> states;
	if ( !t3dVectorResizeCold( states, count ) ) {
		return PackedByteArray();
	}
	size_t total_bytes = 8u + ( size_t )count * 96u;
	for ( uint16_t i = 0u; i < count; ++i ) {
		const t3dStreamedNodeRuntimeDescriptor_t &node = nodes[i];
		states[i] = node.m_state.is_empty() ? PackedByteArray() : UtilityFunctions::var_to_bytes( node.m_state );
		total_bytes += ( size_t )states[i].size();
	}
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, total_bytes ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), total_bytes );
	writer.writeU32( s_nNodeMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( count );
	for ( uint16_t i = 0u; i < count; ++i ) {
		const t3dStreamedNodeRuntimeDescriptor_t &node = nodes[i];
		const int scene_id = rManifest.findSceneDependency( t3dRuntimeBakeCodecDescriptorScenePath( node ) );
		const int shadow_id = rManifest.findSceneDependency( t3dRuntimeBakeCodecDescriptorShadowPath( node ) );
		if ( scene_id < 0 || scene_id > UINT16_MAX ) {
			return PackedByteArray();
		}
		writer.writeU64( ( uint64_t )node.m_nId );
		writer.writeU16( ( uint16_t )scene_id );
		writer.writeU16( shadow_id >= 0 && shadow_id <= UINT16_MAX ? ( uint16_t )shadow_id : UINT16_MAX );
		writer.writeI32( node.m_nFlags );
		writer.writeU16( ( uint16_t )node.m_nHlodMinLevel );
		writer.writeU16( ( uint16_t )node.m_nBoundaryPolicy );
		writer.writeI32( node.m_coverageMin.x );
		writer.writeI32( node.m_coverageMin.y );
		writer.writeI32( node.m_coverageMax.x );
		writer.writeI32( node.m_coverageMax.y );
		writer.writeU16( ( uint16_t )node.m_nLightingClass );
		writer.writeU16( ( uint16_t )node.m_nBakedLightingMode );
		uint32_t bools = node.m_bContributeBakedGi ? 1u : 0u;
		if ( node.m_bReceiveDynamicDirectional ) {
			bools |= 2u;
		}
		if ( node.m_bDynamicShadowEligible ) {
			bools |= 4u;
		}
		writer.writeU32( bools );
		for ( int row = 0; row < 3; ++row ) {
			writer.writeF32( node.m_transform.basis[row].x );
			writer.writeF32( node.m_transform.basis[row].y );
			writer.writeF32( node.m_transform.basis[row].z );
		}
		writer.writeF32( node.m_transform.origin.x );
		writer.writeF32( node.m_transform.origin.y );
		writer.writeF32( node.m_transform.origin.z );
		const PackedByteArray &state = states[i];
		writer.writeU32( ( uint32_t )state.size() );
		writer.writeBytes( state.ptr(), ( size_t )state.size() );
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool Terrain3DRuntimeBakeCodec::decodeNodeDescriptors(
        const PackedByteArray &rBlob, const Terrain3DRuntimeWorldManifest &rManifest, Terrain3DRegion &rRegion ) {
	if ( rBlob.size() < 8 ) {
		return false;
	}
	const uint8_t *p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nNodeMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	const uint16_t count = t3dRuntimeBakeCodecReadU16( p + 6 );
	size_t offset = 8u;
	eastl::vector<t3dStreamedNodeRuntimeDescriptor_t, Terrain3DEastlAllocator> nodes;
	if ( !t3dContainerReserveCold( nodes, count ) ) {
		return false;
	}
	for ( uint16_t i = 0u; i < count; ++i ) {
		if ( offset + s_nFixed > ( size_t )rBlob.size() ) {
			return false;
		}
		t3dStreamedNodeRuntimeDescriptor_t node;
		node.m_nId = ( int64_t )t3dRuntimeBakeCodecReadU64( p + offset );
		node.m_spawnName = StringName( String( "Spawn_" ) + String::num_int64( node.m_nId ) );
		offset += 8;
		const uint16_t scene_id = t3dRuntimeBakeCodecReadU16( p + offset );
		offset += 2;
		const uint16_t shadow_id = t3dRuntimeBakeCodecReadU16( p + offset );
		offset += 2;
		node.m_scenePath = rManifest.getSceneDependency( scene_id );
		if ( node.m_scenePath.is_empty() ) {
			return false;
		}
		if ( shadow_id != UINT16_MAX ) {
			node.m_shadowProxyScenePath = rManifest.getSceneDependency( shadow_id );
		}
		node.m_nFlags = t3dRuntimeBakeCodecReadI32( p + offset );
		offset += 4;
		node.m_nHlodMinLevel = t3dRuntimeBakeCodecReadU16( p + offset );
		offset += 2;
		node.m_nBoundaryPolicy = t3dRuntimeBakeCodecReadU16( p + offset );
		offset += 2;
		node.m_coverageMin.x = t3dRuntimeBakeCodecReadI32( p + offset );
		offset += 4;
		node.m_coverageMin.y = t3dRuntimeBakeCodecReadI32( p + offset );
		offset += 4;
		node.m_coverageMax.x = t3dRuntimeBakeCodecReadI32( p + offset );
		offset += 4;
		node.m_coverageMax.y = t3dRuntimeBakeCodecReadI32( p + offset );
		offset += 4;
		node.m_nLightingClass = t3dRuntimeBakeCodecReadU16( p + offset );
		offset += 2;
		node.m_nBakedLightingMode = t3dRuntimeBakeCodecReadU16( p + offset );
		offset += 2;
		const uint32_t bools = t3dRuntimeBakeCodecReadU32( p + offset );
		offset += 4;
		node.m_bContributeBakedGi = ( bools & 1u ) != 0u;
		node.m_bReceiveDynamicDirectional = ( bools & 2u ) != 0u;
		node.m_bDynamicShadowEligible = ( bools & 4u ) != 0u;
		Basis basis;
		for ( int row = 0; row < 3; ++row ) {
			basis[row] = Vector3( t3dRuntimeBakeCodecReadF32( p + offset + row * 12 ),
			        t3dRuntimeBakeCodecReadF32( p + offset + row * 12 + 4 ),
			        t3dRuntimeBakeCodecReadF32( p + offset + row * 12 + 8 ) );
		}
		offset += 36;
		const Vector3 origin( t3dRuntimeBakeCodecReadF32( p + offset ),
		        t3dRuntimeBakeCodecReadF32( p + offset + 4 ),
		        t3dRuntimeBakeCodecReadF32( p + offset + 8 ) );
		offset += 12;
		node.m_transform = Transform3D( basis, origin );
		const uint32_t state_bytes = t3dRuntimeBakeCodecReadU32( p + offset );
		offset += 4;
		if ( state_bytes > ( size_t )rBlob.size() - offset ) {
			return false;
		}
		if ( state_bytes > 0u ) {
			PackedByteArray state;
			state.resize( state_bytes );
			memcpy( state.ptrw(), p + offset, state_bytes );
			const Variant decoded = UtilityFunctions::bytes_to_var( state );
			if ( decoded.get_type() == Variant::DICTIONARY ) {
				node.m_state = decoded;
			}
		}
		offset += state_bytes;
		if ( !t3dVectorPushBackWithinCapacity( nodes, eastl::move( node ) ) ) {
			return false;
		}
	}
	rRegion.adoptCookedStreamedNodeRuntimeInternal( eastl::move( nodes ) );
	return true;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeInstanceMetadata( const Terrain3DRegion &rRegion ) {
	PackedByteArray bytes;
	if ( !rRegion.ensureInstanceStoreResident() ) {
		return bytes;
	}
	const Terrain3DInstanceStore &store = rRegion.getInstanceStore();
	uint32_t cell_count = 0u;
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( store.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( store.meshes() )[iMesh];
		cell_count += ( uint32_t )mesh.m_cells.size();
	}
	const size_t total_bytes = 16u + ( size_t )cell_count * 44u;
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, total_bytes ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), total_bytes );
	writer.writeU32( s_nInstanceMetaMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( ( uint16_t )store.meshCount() );
	writer.writeU32( cell_count );
	writer.writeU32( rRegion.getInstanceCount() );
	uint32_t ordinal = 0u;
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( store.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( store.meshes() )[iMesh];
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( mesh.m_cells )[iCell];
			writer.writeU16( ( uint16_t )mesh.m_nId );
			writer.writeU16( 0u );
			writer.writeI32( cell.m_location.x );
			writer.writeI32( cell.m_location.y );
			writer.writeU32( ( uint32_t )cell.m_transforms.size() );
			writer.writeU32( ordinal );
			Vector3 minimum( INFINITY, INFINITY, INFINITY ), maximum( -INFINITY, -INFINITY, -INFINITY );
			for ( size_t i = 0; i < cell.m_transforms.size(); ++i ) {
				const Vector3 o = cell.getTransformOrigin( i );
				minimum.x = MIN( minimum.x, o.x );
				minimum.y = MIN( minimum.y, o.y );
				minimum.z = MIN( minimum.z, o.z );
				maximum.x = MAX( maximum.x, o.x );
				maximum.y = MAX( maximum.y, o.y );
				maximum.z = MAX( maximum.z, o.z );
			}
			if ( cell.m_transforms.empty() ) {
				minimum = maximum = s_v3Zero;
			}
			writer.writeF32( minimum.x );
			writer.writeF32( minimum.y );
			writer.writeF32( minimum.z );
			writer.writeF32( maximum.x );
			writer.writeF32( maximum.y );
			writer.writeF32( maximum.z );
			ordinal += ( uint32_t )cell.m_transforms.size();
		}
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool Terrain3DRuntimeBakeCodec::getInstanceMetadataView( const PackedByteArray &rBlob, t3dInstanceMetadataView_t &rView ) {
	rView = t3dInstanceMetadataView_t();
	if ( rBlob.size() < 16 ) {
		return false;
	}
	const uint8_t *const p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nInstanceMetaMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	const uint16_t mesh_count = t3dRuntimeBakeCodecReadU16( p + 6 );
	const uint32_t cell_count = t3dRuntimeBakeCodecReadU32( p + 8 );
	const uint32_t instance_count = t3dRuntimeBakeCodecReadU32( p + 12 );
	if ( cell_count > 1000000u || ( size_t )rBlob.size() < 16u + ( size_t )cell_count * 44u ) {
		return false;
	}
	rView.m_pRecords = p + 16;
	rView.m_nCellCount = cell_count;
	rView.m_nInstanceCount = instance_count;
	rView.m_nMeshCount = mesh_count;
	return true;
}

t3dRuntimeInstanceCellMetadata_t Terrain3DRuntimeBakeCodec::getInstanceMetadataRecord(
        const t3dInstanceMetadataView_t &rView, const uint32_t nIndex ) {
	t3dRuntimeInstanceCellMetadata_t record;
	if ( rView.m_pRecords == NULL || nIndex >= rView.m_nCellCount ) {
		return record;
	}
	const uint8_t *p = rView.m_pRecords + ( size_t )nIndex * 44u;
	record.m_nMeshId = t3dRuntimeBakeCodecReadU16( p );
	p += 4u;
	record.m_cell.x = t3dRuntimeBakeCodecReadI32( p );
	p += 4u;
	record.m_cell.y = t3dRuntimeBakeCodecReadI32( p );
	p += 4u;
	record.m_nInstanceCount = t3dRuntimeBakeCodecReadU32( p );
	p += 4u;
	record.m_nOrdinal = t3dRuntimeBakeCodecReadU32( p );
	p += 4u;
	const Vector3 minimum( t3dRuntimeBakeCodecReadF32( p ), t3dRuntimeBakeCodecReadF32( p + 4u ), t3dRuntimeBakeCodecReadF32( p + 8u ) );
	p += 12u;
	const Vector3 maximum( t3dRuntimeBakeCodecReadF32( p ), t3dRuntimeBakeCodecReadF32( p + 4u ), t3dRuntimeBakeCodecReadF32( p + 8u ) );
	record.m_originBounds = AABB( minimum, maximum - minimum );
	return record;
}

bool Terrain3DRuntimeBakeCodec::decodeInstanceMetadata(
        const PackedByteArray &rBlob, eastl::vector<t3dRuntimeInstanceCellMetadata_t, Terrain3DEastlAllocator> &rCells ) {
	rCells.clear();
	t3dInstanceMetadataView_t view;
	if ( !getInstanceMetadataView( rBlob, view ) ) {
		return false;
	}
	rCells.resize( view.m_nCellCount );
	for ( uint32_t i = 0u; i < view.m_nCellCount; ++i ) {
		rCells[i] = getInstanceMetadataRecord( view, i );
	}
	return true;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeLightingReceivers( const Terrain3DRegion &rRegion ) {
	PackedByteArray bytes;
	const Ref<Terrain3DRegionLighting> lighting = rRegion.getLighting();
	if ( lighting.is_null() ) {
		return bytes;
	}
	const Array &users = lighting->getBakedUserDataInternal();
	if ( users.is_empty() || ( users.size() % 4 ) != 0 ) {
		return bytes;
	}
	const uint32_t count = ( uint32_t )( users.size() / 4 );
	eastl::vector<CharString, Terrain3DEastlAllocator> paths;
	paths.resize( count );
	size_t total_bytes = 12u + ( size_t )count * 28u;
	for ( uint32_t record = 0u; record < count; ++record ) {
		paths[record] = String( ( NodePath )users[( int )record * 4] ).utf8();
		total_bytes += ( size_t )MAX( 0, paths[record].length() );
	}
	if ( !t3dRuntimeBakeCodecResizeBytes( bytes, total_bytes ) ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( bytes.ptrw(), total_bytes );
	writer.writeU32( s_nLightingMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( 0u );
	writer.writeU32( count );
	for ( uint32_t record = 0u; record < count; ++record ) {
		const int i = ( int )record * 4;
		const Rect2 uv = users[i + 1];
		const int32_t slice = ( int32_t )( ( int )users[i + 2] );
		const int32_t sub_instance = ( int32_t )( ( int )users[i + 3] );
		const CharString &utf8 = paths[record];
		const uint32_t path_bytes = ( uint32_t )MAX( 0, utf8.length() );
		writer.writeU32( path_bytes );
		writer.writeBytes( utf8.get_data(), path_bytes );
		writer.writeF32( uv.position.x );
		writer.writeF32( uv.position.y );
		writer.writeF32( uv.size.x );
		writer.writeF32( uv.size.y );
		writer.writeI32( slice );
		writer.writeI32( sub_instance );
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool Terrain3DRuntimeBakeCodec::beginLightingReceivers( const PackedByteArray &rBlob, t3dLightingReceiverCursor_t &rCursor ) {
	rCursor = t3dLightingReceiverCursor_t();
	if ( rBlob.size() < 12 ) {
		return false;
	}
	const uint8_t *const p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nLightingMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion ) {
		return false;
	}
	const uint32_t count = t3dRuntimeBakeCodecReadU32( p + 8 );
	if ( count > 1000000u ) {
		return false;
	}
	size_t offset = 12u;
	for ( uint32_t i = 0u; i < count; ++i ) {
		if ( offset + 4u > ( size_t )rBlob.size() ) {
			return false;
		}
		const uint32_t path_bytes = t3dRuntimeBakeCodecReadU32( p + offset );
		offset += 4u;
		if ( path_bytes > 1024u * 1024u || offset + path_bytes + 24u > ( size_t )rBlob.size() ) {
			return false;
		}
		offset += path_bytes + 24u;
	}
	if ( offset != ( size_t )rBlob.size() ) {
		return false;
	}
	rCursor.m_pData = p;
	rCursor.m_nSize = ( size_t )rBlob.size();
	rCursor.m_iOffset = 12u;
	rCursor.m_nCount = count;
	return true;
}

bool Terrain3DRuntimeBakeCodec::nextLightingReceiver(
        t3dLightingReceiverCursor_t &rCursor, t3dRuntimeLightingReceiverRecord_t &rReceiver ) {
	if ( rCursor.m_pData == NULL || rCursor.m_iIndex >= rCursor.m_nCount || rCursor.m_iOffset + 4u > rCursor.m_nSize ) {
		return false;
	}
	const uint8_t *const p = rCursor.m_pData;
	const uint32_t path_bytes = t3dRuntimeBakeCodecReadU32( p + rCursor.m_iOffset );
	rCursor.m_iOffset += 4u;
	if ( rCursor.m_iOffset + path_bytes + 24u > rCursor.m_nSize ) {
		return false;
	}
	rReceiver.m_path = NodePath( String::utf8( ( const char * )( p + rCursor.m_iOffset ), ( int )path_bytes ) );
	rCursor.m_iOffset += path_bytes;
	rReceiver.m_uvScale = Rect2( t3dRuntimeBakeCodecReadF32( p + rCursor.m_iOffset ),
	        t3dRuntimeBakeCodecReadF32( p + rCursor.m_iOffset + 4u ),
	        t3dRuntimeBakeCodecReadF32( p + rCursor.m_iOffset + 8u ),
	        t3dRuntimeBakeCodecReadF32( p + rCursor.m_iOffset + 12u ) );
	rCursor.m_iOffset += 16u;
	rReceiver.m_iSliceIndex = t3dRuntimeBakeCodecReadI32( p + rCursor.m_iOffset );
	rCursor.m_iOffset += 4u;
	rReceiver.m_nSubInstance = t3dRuntimeBakeCodecReadI32( p + rCursor.m_iOffset );
	rCursor.m_iOffset += 4u;
	++rCursor.m_iIndex;
	return !rReceiver.m_path.is_empty();
}

bool Terrain3DRuntimeBakeCodec::decodeLightingReceivers(
        const PackedByteArray &rBlob, eastl::vector<t3dRuntimeLightingReceiverRecord_t, Terrain3DEastlAllocator> &rReceivers ) {
	rReceivers.clear();
	t3dLightingReceiverCursor_t cursor;
	if ( !beginLightingReceivers( rBlob, cursor ) ) {
		return false;
	}
	rReceivers.resize( cursor.m_nCount );
	for ( uint32_t i = 0u; i < cursor.m_nCount; ++i ) {
		if ( !nextLightingReceiver( cursor, rReceivers[i] ) ) {
			rReceivers.clear();
			return false;
		}
	}
	return true;
}

PackedByteArray Terrain3DRuntimeBakeCodec::encodeMultimeshFastStart( const Terrain3DRegion &rRegion, const int nMinimumCellInstances ) {
	PackedByteArray bytes;
	if ( !rRegion.ensureInstanceStoreResident() ) {
		return bytes;
	}
	const Terrain3DInstanceStore &store = rRegion.getInstanceStore();
	uint32_t eligible = 0u;
	size_t total_bytes = 12u;
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( store.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( store.meshes() )[iMesh];
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( mesh.m_cells )[iCell];
			if ( ( int )cell.m_transforms.size() >= nMinimumCellInstances ) {
				++eligible;
				total_bytes += 16u + cell.m_transforms.size() * 64u;
			}
		}
	}
	if ( eligible == 0u || !t3dRuntimeBakeCodecResizeBytes( bytes, total_bytes ) ) {
		return bytes;
	}
	t3dByteWriter_t writer( bytes.ptrw(), total_bytes );
	writer.writeU32( s_nMultimeshMagic );
	writer.writeU16( s_nBlobVersion );
	writer.writeU16( 16u );
	writer.writeU32( eligible );
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( store.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( store.meshes() )[iMesh];
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( mesh.m_cells )[iCell];
			if ( ( int )cell.m_transforms.size() < nMinimumCellInstances ) {
				continue;
			}
			writer.writeU16( ( uint16_t )mesh.m_nId );
			writer.writeU16( 0u );
			writer.writeI32( cell.m_location.x );
			writer.writeI32( cell.m_location.y );
			writer.writeU32( ( uint32_t )cell.m_transforms.size() );
			for ( size_t i = 0; i < cell.m_transforms.size(); ++i ) {
				const Transform3D t = cell.getTransform( i );
				for ( int row = 0; row < 3; ++row ) {
					writer.writeF32( t.basis[row].x );
					writer.writeF32( t.basis[row].y );
					writer.writeF32( t.basis[row].z );
					writer.writeF32( row == 0 ? t.origin.x : row == 1 ? t.origin.y : t.origin.z );
				}
				const Color c = cell.getColor( i );
				writer.writeF32( c.r );
				writer.writeF32( c.g );
				writer.writeF32( c.b );
				writer.writeF32( c.a );
			}
		}
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool Terrain3DRuntimeBakeCodec::findMultimeshFastStart( const PackedByteArray &rBlob,
        const uint16_t nMeshId,
        const Vector2i &rCell,
        PackedFloat32Array &rBuffer,
        uint32_t &nInstanceCount ) {
	rBuffer.clear();
	nInstanceCount = 0u;
	if ( rBlob.size() < 12 ) {
		return false;
	}
	const uint8_t *const p = rBlob.ptr();
	if ( t3dRuntimeBakeCodecReadU32( p ) != s_nMultimeshMagic || t3dRuntimeBakeCodecReadU16( p + 4 ) != s_nBlobVersion
	        || t3dRuntimeBakeCodecReadU16( p + 6 ) != 16u ) {
		return false;
	}
	const uint32_t record_count = t3dRuntimeBakeCodecReadU32( p + 8 );
	if ( record_count > 1000000u ) {
		return false;
	}
	size_t offset = 12u;
	for ( uint32_t record_index = 0u; record_index < record_count; ++record_index ) {
		if ( offset + 16u > ( size_t )rBlob.size() ) {
			return false;
		}
		const uint16_t mesh_id = t3dRuntimeBakeCodecReadU16( p + offset );
		offset += 4u;
		const int32_t cell_x = t3dRuntimeBakeCodecReadI32( p + offset );
		offset += 4u;
		const int32_t cell_y = t3dRuntimeBakeCodecReadI32( p + offset );
		offset += 4u;
		const uint32_t count = t3dRuntimeBakeCodecReadU32( p + offset );
		offset += 4u;
		if ( count > 10000000u || ( uint64_t )count * 64u > ( uint64_t )rBlob.size() - offset ) {
			return false;
		}
		if ( mesh_id == nMeshId && cell_x == rCell.x && cell_y == rCell.y ) {
			rBuffer.resize( ( int64_t )count * 16 );
			float *const out = rBuffer.ptrw();
			for ( uint64_t value = 0u; value < ( uint64_t )count * 16u; ++value ) {
				out[value] = t3dRuntimeBakeCodecReadF32( p + offset + value * 4u );
			}
			nInstanceCount = count;
			return true;
		}
		offset += ( size_t )count * 64u;
	}
	return false;
}
