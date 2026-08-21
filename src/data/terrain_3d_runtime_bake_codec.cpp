// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_byte_writer.h"
#include "data/terrain_3d_runtime_bake_codec.h"

#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_world_manifest.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
static constexpr uint32_t HOLE_MAGIC = 0x314c4f48u; // HOL1
static constexpr uint32_t NAV_MAGIC = 0x3156414eu; // NAV1
static constexpr uint32_t MINMAX_MAGIC = 0x31584d4du; // MMX1
static constexpr uint32_t COLLISION_HEIGHT_MAGIC = 0x31484c43u; // CLH1
static constexpr uint32_t COLLISION_BOUNDS_MAGIC = 0x31424c43u; // CLB1
static constexpr uint32_t NODE_MAGIC = 0x31444f4eu; // NOD1
static constexpr uint32_t INSTANCE_META_MAGIC = 0x314d4e49u; // INM1
static constexpr uint32_t LIGHTING_MAGIC = 0x3154474cu; // LGT1
static constexpr uint32_t MULTIMESH_MAGIC = 0x31494d4du; // MMI1
static constexpr uint16_t BLOB_VERSION = 1u;

EA_FORCE_INLINE static uint16_t readU16( const uint8_t *p ) { return ( uint16_t ) ( p[0] | ( ( uint16_t ) p[1] << 8u ) ); }
EA_FORCE_INLINE static uint32_t readU32( const uint8_t *p ) { return ( uint32_t ) p[0] | ( ( uint32_t ) p[1] << 8u ) | ( ( uint32_t ) p[2] << 16u ) | ( ( uint32_t ) p[3] << 24u ); }
EA_FORCE_INLINE static int32_t readI32( const uint8_t *p ) { return ( int32_t ) readU32( p ); }
EA_FORCE_INLINE static uint64_t readU64( const uint8_t *p ) { return ( uint64_t ) readU32( p ) | ( ( uint64_t ) readU32( p + 4 ) << 32u ); }
EA_FORCE_INLINE static float readF32( const uint8_t *p ) { const uint32_t bits = readU32( p ); float v = 0.f; std::memcpy( &v, &bits, 4 ); return v; }
EA_FORCE_INLINE static uint32_t readControlPixel( const uint8_t *pControl, const size_t nControlBytes, const uint32_t nIndex ) { if ( pControl == nullptr || ( size_t ) ( nIndex + 1u ) * 4u > nControlBytes ) return TERRAIN3D_REGION_DEFAULT_CONTROL; uint32_t value = 0u; std::memcpy( &value, pControl + ( size_t ) nIndex * 4u, 4u ); return value; }
EA_FORCE_INLINE static void setBit( uint8_t *pBytes, const uint32_t nIndex ) { pBytes[nIndex >> 3u] |= ( uint8_t ) ( 1u << ( nIndex & 7u ) ); }
EA_FORCE_INLINE static bool resizeBytes( PackedByteArray &rBytes, const size_t nBytes ) { if ( nBytes > ( size_t ) INT64_MAX ) return false; rBytes.resize( ( int64_t ) nBytes ); return ( size_t ) rBytes.size() == nBytes; }

EA_FORCE_INLINE static String descriptorScenePath( const T3DRegion::t3dStreamedNodeRuntimeDescriptor_t &rDescriptor ) { if ( !rDescriptor.scenePath.is_empty() ) return rDescriptor.scenePath; return rDescriptor.pScene.is_valid() ? rDescriptor.pScene->get_path() : String(); }
EA_FORCE_INLINE static String descriptorShadowPath( const T3DRegion::t3dStreamedNodeRuntimeDescriptor_t &rDescriptor ) { if ( !rDescriptor.shadowProxyScenePath.is_empty() ) return rDescriptor.shadowProxyScenePath; return rDescriptor.pShadowProxyScene.is_valid() ? rDescriptor.pShadowProxyScene->get_path() : String(); }
}

PackedByteArray T3DRuntimeBakeCodec::encodeHoleMask( const Ref<Image> &rControl, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( nRegionSize <= 0 || nRegionSize > UINT16_MAX || ( rControl.is_valid() && ( rControl->get_width() != nRegionSize || rControl->get_height() != nRegionSize || rControl->get_format() != Image::FORMAT_RF ) ) ) return bytes;
	const uint32_t samples = ( uint32_t ) nRegionSize * ( uint32_t ) nRegionSize;
	const uint32_t plane = ( samples + 7u ) / 8u;
	if ( !resizeBytes( bytes, 12u + plane ) ) return PackedByteArray();
	T3DByteWriter writer( bytes.ptrw(), ( size_t ) bytes.size() );
	writer.writeU32( HOLE_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( ( uint16_t ) nRegionSize ); writer.writeU32( samples );
	uint8_t *const pBits = bytes.ptrw() + writer.offset(); std::memset( pBits, 0, plane );
	const PackedByteArray control_bytes = rControl.is_valid() ? rControl->get_data() : PackedByteArray();
	const uint8_t *const pControl = control_bytes.is_empty() ? nullptr : control_bytes.ptr();
	for ( uint32_t i = 0u; i < samples; ++i ) if ( isHole( readControlPixel( pControl, ( size_t ) control_bytes.size(), i ) ) ) setBit( pBits, i );
	return bytes;
}

PackedByteArray T3DRuntimeBakeCodec::encodeNavigationMask( const Ref<Image> &rControl, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( nRegionSize <= 0 || nRegionSize > UINT16_MAX || ( rControl.is_valid() && ( rControl->get_width() != nRegionSize || rControl->get_height() != nRegionSize || rControl->get_format() != Image::FORMAT_RF ) ) ) return bytes;
	const uint32_t samples = ( uint32_t ) nRegionSize * ( uint32_t ) nRegionSize;
	const uint32_t plane = ( samples + 7u ) / 8u;
	if ( !resizeBytes( bytes, 16u + ( size_t ) plane * 2u ) ) return PackedByteArray();
	T3DByteWriter writer( bytes.ptrw(), ( size_t ) bytes.size() );
	writer.writeU32( NAV_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( ( uint16_t ) nRegionSize ); writer.writeU32( samples ); writer.writeU32( plane );
	uint8_t *const pValid = bytes.ptrw() + writer.offset(); uint8_t *const pNavigation = pValid + plane; std::memset( pValid, 0, ( size_t ) plane * 2u );
	const PackedByteArray control_bytes = rControl.is_valid() ? rControl->get_data() : PackedByteArray();
	const uint8_t *const pControl = control_bytes.is_empty() ? nullptr : control_bytes.ptr();
	for ( uint32_t i = 0u; i < samples; ++i ) {
		const uint32_t control = readControlPixel( pControl, ( size_t ) control_bytes.size(), i );
		if ( !isHole( control ) ) setBit( pValid, i );
		if ( !isHole( control ) && isNav( control ) ) setBit( pNavigation, i );
	}
	return bytes;
}

PackedByteArray T3DRuntimeBakeCodec::encodeNavigationMask( const PackedInt32Array &rControls, const int nWidth ) {
	PackedByteArray bytes;
	if ( nWidth <= 0 || rControls.size() != nWidth * nWidth || nWidth > UINT16_MAX ) return bytes;
	const uint32_t samples = ( uint32_t ) nWidth * ( uint32_t ) nWidth;
	const uint32_t plane = ( samples + 7u ) / 8u;
	if ( !resizeBytes( bytes, 16u + ( size_t ) plane * 2u ) ) return PackedByteArray();
	T3DByteWriter writer( bytes.ptrw(), ( size_t ) bytes.size() );
	writer.writeU32( NAV_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( ( uint16_t ) nWidth ); writer.writeU32( samples ); writer.writeU32( plane );
	uint8_t *const pValid = bytes.ptrw() + writer.offset(); uint8_t *const pNavigation = pValid + plane; std::memset( pValid, 0, ( size_t ) plane * 2u );
	const int32_t *const pControls = rControls.ptr();
	for ( uint32_t i = 0u; i < samples; ++i ) {
		const uint32_t control = ( uint32_t ) pControls[i];
		if ( !isHole( control ) ) setBit( pValid, i );
		if ( !isHole( control ) && isNav( control ) ) setBit( pNavigation, i );
	}
	return bytes;
}

bool T3DRuntimeBakeCodec::getNavigationMaskView( const PackedByteArray &rBlob, T3DNavigationMaskView &rView ) {
	rView = T3DNavigationMaskView();
	if ( rBlob.size() < 16 ) return false;
	const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != NAV_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	const uint16_t width = readU16( p + 6 ); const uint32_t samples = readU32( p + 8 ); const uint32_t plane = readU32( p + 12 );
	if ( width == 0u || samples != ( uint32_t ) width * width || plane != ( samples + 7u ) / 8u || rBlob.size() != 16 + ( int64_t ) plane * 2 ) return false;
	rView.pValidBits = p + 16; rView.pNavigationBits = p + 16 + plane; rView.nPlaneBytes = plane; rView.nSamples = samples; rView.nWidth = width; return true;
}

bool T3DRuntimeBakeCodec::decodeNavigationMask(
	const PackedByteArray &rBlob, int &nWidth, PackedByteArray &rValidBits, PackedByteArray &rNavigationBits
) {
	nWidth = 0; rValidBits.clear(); rNavigationBits.clear();
	T3DNavigationMaskView view; if ( !getNavigationMaskView( rBlob, view ) ) return false;
	nWidth = ( int ) view.nWidth; rValidBits.resize( view.nPlaneBytes ); rNavigationBits.resize( view.nPlaneBytes );
	std::memcpy( rValidBits.ptrw(), view.pValidBits, view.nPlaneBytes ); std::memcpy( rNavigationBits.ptrw(), view.pNavigationBits, view.nPlaneBytes ); return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeMinMaxHierarchy( const Ref<Image> &rHeight, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( rHeight.is_null() || nRegionSize <= 0 || nRegionSize > UINT16_MAX || rHeight->get_width() != nRegionSize || rHeight->get_height() != nRegionSize || rHeight->get_format() != Image::FORMAT_R16 ) return bytes;
	const PackedByteArray source = rHeight->get_data();
	if ( source.size() < ( int64_t ) nRegionSize * nRegionSize * 2 ) return bytes;
	int levels = 1; size_t output_bytes = 12u;
	for ( int side = nRegionSize; ; side = ( side + 1 ) / 2 ) { output_bytes += 8u + ( size_t ) side * side * 8u; if ( side == 1 ) break; ++levels; }
	if ( !resizeBytes( bytes, output_bytes ) ) return PackedByteArray();
	T3DByteWriter writer( bytes.ptrw(), output_bytes );
	writer.writeU32( MINMAX_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( ( uint16_t ) nRegionSize ); writer.writeU32( ( uint32_t ) levels );
	const uint8_t *const pSource = source.ptr();
	const uint32_t source_count = ( uint32_t ) nRegionSize * ( uint32_t ) nRegionSize;
	writer.writeU32( ( uint32_t ) nRegionSize ); writer.writeU32( source_count );
	for ( uint32_t i = 0u; i < source_count; ++i ) { const float h = T3DRegionCodec::readHeightBytes( pSource, ( int ) i, Image::FORMAT_R16 ); writer.writeF32( h ); writer.writeF32( h ); }
	if ( nRegionSize == 1 ) return writer.complete() ? bytes : PackedByteArray();

	// Level 0 is emitted directly from the R16 source. Only the first reduced
	// level is retained, then every later level is reduced in-place into the
	// beginning of the same high-water buffer. For a 2048 region this reduces
	// scratch from ~40 MiB (four full float planes) to ~8 MiB.
	int side = ( nRegionSize + 1 ) / 2;
	eastl::vector<Vector2> ranges;
	ranges.resize( ( size_t ) side * side );
	for ( int z = 0; z < side; ++z ) {
		for ( int x = 0; x < side; ++x ) {
			float minimum = INFINITY; float maximum = -INFINITY;
			for ( int dz = 0; dz < 2; ++dz ) for ( int dx = 0; dx < 2; ++dx ) {
				const int sx = x * 2 + dx; const int sz = z * 2 + dz; if ( sx >= nRegionSize || sz >= nRegionSize ) continue;
				const float h = T3DRegionCodec::readHeightBytes( pSource, sz * nRegionSize + sx, Image::FORMAT_R16 ); minimum = MIN( minimum, h ); maximum = MAX( maximum, h );
			}
			ranges[( size_t ) z * side + x] = Vector2( minimum, maximum );
		}
	}
	while ( true ) {
		const uint32_t count = ( uint32_t ) side * ( uint32_t ) side; writer.writeU32( ( uint32_t ) side ); writer.writeU32( count );
		for ( uint32_t i = 0u; i < count; ++i ) { writer.writeF32( ranges[i].x ); writer.writeF32( ranges[i].y ); }
		if ( side == 1 ) break;
		const int next_side = ( side + 1 ) / 2;
		for ( int z = 0; z < next_side; ++z ) {
			for ( int x = 0; x < next_side; ++x ) {
				float minimum = INFINITY; float maximum = -INFINITY;
				for ( int dz = 0; dz < 2; ++dz ) for ( int dx = 0; dx < 2; ++dx ) {
					const int sx = x * 2 + dx; const int sz = z * 2 + dz; if ( sx >= side || sz >= side ) continue;
					const Vector2 range = ranges[( size_t ) sz * side + sx]; minimum = MIN( minimum, range.x ); maximum = MAX( maximum, range.y );
				}
				ranges[( size_t ) z * next_side + x] = Vector2( minimum, maximum );
			}
		}
		ranges.resize( ( size_t ) next_side * next_side ); side = next_side;
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool T3DRuntimeBakeCodec::getMinMaxLevelView( const PackedByteArray &rBlob, const int nMaximumSide, T3DMinMaxLevelView &rView ) {
	rView = T3DMinMaxLevelView();
	if ( rBlob.size() < 12 || nMaximumSide <= 0 ) return false;
	const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != MINMAX_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	const uint16_t source_side = readU16( p + 6 ); const uint32_t level_count = readU32( p + 8 );
	if ( source_side == 0u || level_count == 0u || level_count > 32u ) return false;
	size_t offset = 12u; const uint8_t *pSelected = nullptr; uint32_t selected_count = 0u; uint16_t selected_side = 0u;
	for ( uint32_t level = 0u; level < level_count; ++level ) {
		if ( offset + 8u > ( size_t ) rBlob.size() ) return false;
		const uint32_t side = readU32( p + offset ); const uint32_t count = readU32( p + offset + 4u ); offset += 8u;
		if ( side == 0u || side > UINT16_MAX || count != side * side || ( uint64_t ) count * 8u > ( uint64_t ) rBlob.size() - offset ) return false;
		if ( selected_side == 0u && ( int ) side <= nMaximumSide ) { selected_side = ( uint16_t ) side; selected_count = count; pSelected = p + offset; }
		offset += ( size_t ) count * 8u;
	}
	if ( offset != ( size_t ) rBlob.size() || selected_side == 0u ) return false;
	rView.pRanges = pSelected; rView.nRangeCount = selected_count; rView.nSourceSide = source_side; rView.nLevelSide = selected_side; return true;
}

Vector2 T3DRuntimeBakeCodec::getMinMaxRange( const T3DMinMaxLevelView &rView, const uint32_t nIndex ) {
	if ( rView.pRanges == nullptr || nIndex >= rView.nRangeCount ) return Vector2();
	const uint8_t *const p = rView.pRanges + ( size_t ) nIndex * 8u; return Vector2( readF32( p ), readF32( p + 4u ) );
}

bool T3DRuntimeBakeCodec::decodeMinMaxLevel(
	const PackedByteArray &rBlob, const int nMaximumSide, int &nSourceSide,
	int &nLevelSide, eastl::vector<Vector2> &rRanges
) {
	nSourceSide = 0; nLevelSide = 0; rRanges.clear(); T3DMinMaxLevelView view;
	if ( !getMinMaxLevelView( rBlob, nMaximumSide, view ) ) return false;
	nSourceSide = view.nSourceSide; nLevelSide = view.nLevelSide; rRanges.resize( view.nRangeCount );
	for ( uint32_t i = 0u; i < view.nRangeCount; ++i ) rRanges[i] = getMinMaxRange( view, i );
	return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeCollisionHeight( const PackedFloat32Array &rHeights, const int nWidth ) {
	PackedByteArray bytes; if ( nWidth <= 0 || nWidth > UINT16_MAX || rHeights.size() != nWidth * nWidth ) return bytes;
	const size_t payload_bytes = ( size_t ) rHeights.size() * sizeof( float ); if ( !resizeBytes( bytes, 12u + payload_bytes ) ) return PackedByteArray();
	T3DByteWriter writer( bytes.ptrw(), ( size_t ) bytes.size() ); writer.writeU32( COLLISION_HEIGHT_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( ( uint16_t ) nWidth ); writer.writeU32( ( uint32_t ) rHeights.size() ); writer.writeBytes( rHeights.ptr(), payload_bytes );
	return writer.complete() ? bytes : PackedByteArray();
}

PackedByteArray T3DRuntimeBakeCodec::encodeCollisionBounds( const float fMinHeight, const float fMaxHeight, const int nWidth ) {
	PackedByteArray bytes; if ( nWidth <= 0 || nWidth > UINT16_MAX || !resizeBytes( bytes, 16u ) ) return bytes;
	T3DByteWriter writer( bytes.ptrw(), ( size_t ) bytes.size() ); writer.writeU32( COLLISION_BOUNDS_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( ( uint16_t ) nWidth ); writer.writeF32( fMinHeight ); writer.writeF32( fMaxHeight ); return writer.complete() ? bytes : PackedByteArray();
}

bool T3DRuntimeBakeCodec::decodeCollisionHeight( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth ) {
	nWidth = 0; if ( rBlob.size() < 12 ) return false; const uint8_t *p = rBlob.ptr(); if ( readU32( p ) != COLLISION_HEIGHT_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	nWidth = readU16( p + 6 ); const uint32_t count = readU32( p + 8 ); if ( nWidth <= 0 || count != ( uint32_t ) nWidth * nWidth || rBlob.size() != 12 + ( int64_t ) count * 4 ) { nWidth = 0; return false; }
	if ( rHeights.size() != ( int64_t ) count ) rHeights.resize( count ); std::memcpy( rHeights.ptrw(), p + 12, ( size_t ) count * 4u ); return true;
}
bool T3DRuntimeBakeCodec::decodeCollisionBounds( const PackedByteArray &rBlob, float &fMinHeight, float &fMaxHeight, int &nWidth ) {
	if ( rBlob.size() != 16 ) return false; const uint8_t *p = rBlob.ptr(); if ( readU32( p ) != COLLISION_BOUNDS_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	nWidth = readU16( p + 6 ); fMinHeight = readF32( p + 8 ); fMaxHeight = readF32( p + 12 ); return nWidth > 0;
}

PackedByteArray T3DRuntimeBakeCodec::encodeNodeDescriptors( const T3DRegion &rRegion, const T3DRuntimeWorldManifest &rManifest ) {
	PackedByteArray bytes; const auto &nodes = rRegion.getStreamedNodesRuntimeInternal(); const uint16_t count = ( uint16_t ) MIN( nodes.size(), ( size_t ) UINT16_MAX );
	eastl::vector<PackedByteArray> states; states.resize( count ); size_t total_bytes = 8u + ( size_t ) count * 96u;
	for ( uint16_t i = 0u; i < count; ++i ) { const auto &node = nodes[i]; states[i] = node.state.is_empty() ? PackedByteArray() : UtilityFunctions::var_to_bytes( node.state ); total_bytes += ( size_t ) states[i].size(); }
	if ( !resizeBytes( bytes, total_bytes ) ) return PackedByteArray(); T3DByteWriter writer( bytes.ptrw(), total_bytes ); writer.writeU32( NODE_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( count );
	for ( uint16_t i = 0u; i < count; ++i ) {
		const auto &node = nodes[i]; const int scene_id = rManifest.findSceneDependency( descriptorScenePath( node ) ); const int shadow_id = rManifest.findSceneDependency( descriptorShadowPath( node ) );
		if ( scene_id < 0 || scene_id > UINT16_MAX ) return PackedByteArray();
		writer.writeU64( ( uint64_t ) node.id ); writer.writeU16( ( uint16_t ) scene_id ); writer.writeU16( shadow_id >= 0 && shadow_id <= UINT16_MAX ? ( uint16_t ) shadow_id : UINT16_MAX );
		writer.writeI32( node.flags ); writer.writeU16( ( uint16_t ) node.hlodMinLevel ); writer.writeU16( ( uint16_t ) node.boundaryPolicy ); writer.writeI32( node.coverageMin.x ); writer.writeI32( node.coverageMin.y ); writer.writeI32( node.coverageMax.x ); writer.writeI32( node.coverageMax.y );
		writer.writeU16( ( uint16_t ) node.lightingClass ); writer.writeU16( ( uint16_t ) node.bakedLightingMode ); uint32_t bools = node.bContributeBakedGi ? 1u : 0u; if ( node.bReceiveDynamicDirectional ) bools |= 2u; if ( node.bDynamicShadowEligible ) bools |= 4u; writer.writeU32( bools );
		for ( int row = 0; row < 3; ++row ) { writer.writeF32( node.transform.basis[row].x ); writer.writeF32( node.transform.basis[row].y ); writer.writeF32( node.transform.basis[row].z ); }
		writer.writeF32( node.transform.origin.x ); writer.writeF32( node.transform.origin.y ); writer.writeF32( node.transform.origin.z ); const PackedByteArray &state = states[i]; writer.writeU32( ( uint32_t ) state.size() ); writer.writeBytes( state.ptr(), ( size_t ) state.size() );
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool T3DRuntimeBakeCodec::decodeNodeDescriptors( const PackedByteArray &rBlob, const T3DRuntimeWorldManifest &rManifest, T3DRegion &rRegion ) {
	if ( rBlob.size() < 8 ) return false; const uint8_t *p = rBlob.ptr(); if ( readU32( p ) != NODE_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false; const uint16_t count = readU16( p + 6 ); size_t offset = 8u;
	eastl::vector<T3DRegion::t3dStreamedNodeRuntimeDescriptor_t> nodes; nodes.reserve( count );
	for ( uint16_t i = 0u; i < count; ++i ) {
		static constexpr size_t fixed = 8u + 2u + 2u + 4u + 2u + 2u + 16u + 2u + 2u + 4u + 48u + 4u;
		if ( offset + fixed > ( size_t ) rBlob.size() ) return false; T3DRegion::t3dStreamedNodeRuntimeDescriptor_t node;
		node.id = ( int64_t ) readU64( p + offset ); offset += 8; const uint16_t scene_id = readU16( p + offset ); offset += 2; const uint16_t shadow_id = readU16( p + offset ); offset += 2;
		node.scenePath = rManifest.getSceneDependency( scene_id ); if ( node.scenePath.is_empty() ) return false; if ( shadow_id != UINT16_MAX ) node.shadowProxyScenePath = rManifest.getSceneDependency( shadow_id );
		node.flags = readI32( p + offset ); offset += 4; node.hlodMinLevel = readU16( p + offset ); offset += 2; node.boundaryPolicy = readU16( p + offset ); offset += 2;
		node.coverageMin.x = readI32( p + offset ); offset += 4; node.coverageMin.y = readI32( p + offset ); offset += 4; node.coverageMax.x = readI32( p + offset ); offset += 4; node.coverageMax.y = readI32( p + offset ); offset += 4;
		node.lightingClass = readU16( p + offset ); offset += 2; node.bakedLightingMode = readU16( p + offset ); offset += 2; const uint32_t bools = readU32( p + offset ); offset += 4;
		node.bContributeBakedGi = ( bools & 1u ) != 0u; node.bReceiveDynamicDirectional = ( bools & 2u ) != 0u; node.bDynamicShadowEligible = ( bools & 4u ) != 0u;
		Basis basis; for ( int row = 0; row < 3; ++row ) basis[row] = Vector3( readF32( p + offset + row * 12 ), readF32( p + offset + row * 12 + 4 ), readF32( p + offset + row * 12 + 8 ) ); offset += 36;
		const Vector3 origin( readF32( p + offset ), readF32( p + offset + 4 ), readF32( p + offset + 8 ) ); offset += 12; node.transform = Transform3D( basis, origin );
		const uint32_t state_bytes = readU32( p + offset ); offset += 4; if ( state_bytes > ( size_t ) rBlob.size() - offset ) return false;
		if ( state_bytes > 0u ) { PackedByteArray state; state.resize( state_bytes ); std::memcpy( state.ptrw(), p + offset, state_bytes ); const Variant decoded = UtilityFunctions::bytes_to_var( state, false ); if ( decoded.get_type() == Variant::DICTIONARY ) node.state = decoded; } offset += state_bytes;
		nodes.push_back( node );
	}
	if ( offset != ( size_t ) rBlob.size() ) return false; rRegion.adoptCookedStreamedNodeRuntimeInternal( eastl::move( nodes ) ); return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeInstanceMetadata( const T3DRegion &rRegion ) {
	PackedByteArray bytes; if ( !rRegion.ensureInstanceStoreResident() ) return bytes; const T3DInstanceStore &store = rRegion.getInstanceStore();
	uint32_t cell_count = 0u; for ( const auto &mesh : store.meshes() ) cell_count += ( uint32_t ) mesh.cells.size();
	const size_t total_bytes = 16u + ( size_t ) cell_count * 44u; if ( !resizeBytes( bytes, total_bytes ) ) return PackedByteArray(); T3DByteWriter writer( bytes.ptrw(), total_bytes );
	writer.writeU32( INSTANCE_META_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( ( uint16_t ) store.meshCount() ); writer.writeU32( cell_count ); writer.writeU32( rRegion.getInstanceCount() );
	uint32_t ordinal = 0u; for ( const auto &mesh : store.meshes() ) for ( const auto &cell : mesh.cells ) {
		writer.writeU16( ( uint16_t ) mesh.id ); writer.writeU16( 0u ); writer.writeI32( cell.location.x ); writer.writeI32( cell.location.y ); writer.writeU32( ( uint32_t ) cell.transforms.size() ); writer.writeU32( ordinal );
		Vector3 minimum( INFINITY, INFINITY, INFINITY ), maximum( -INFINITY, -INFINITY, -INFINITY );
		for ( size_t i = 0; i < cell.transforms.size(); ++i ) { const Vector3 o = cell.transforms.getOrigin( i ); minimum.x = MIN( minimum.x, o.x ); minimum.y = MIN( minimum.y, o.y ); minimum.z = MIN( minimum.z, o.z ); maximum.x = MAX( maximum.x, o.x ); maximum.y = MAX( maximum.y, o.y ); maximum.z = MAX( maximum.z, o.z ); }
		if ( cell.transforms.empty() ) minimum = maximum = V3_ZERO;
		writer.writeF32( minimum.x ); writer.writeF32( minimum.y ); writer.writeF32( minimum.z ); writer.writeF32( maximum.x ); writer.writeF32( maximum.y ); writer.writeF32( maximum.z ); ordinal += ( uint32_t ) cell.transforms.size();
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool T3DRuntimeBakeCodec::getInstanceMetadataView( const PackedByteArray &rBlob, T3DInstanceMetadataView &rView ) {
	rView = T3DInstanceMetadataView(); if ( rBlob.size() < 16 ) return false; const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != INSTANCE_META_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	const uint16_t mesh_count = readU16( p + 6 ); const uint32_t cell_count = readU32( p + 8 ); const uint32_t instance_count = readU32( p + 12 );
	if ( cell_count > 1000000u || ( size_t ) rBlob.size() != 16u + ( size_t ) cell_count * 44u ) return false;
	rView.pRecords = p + 16; rView.nCellCount = cell_count; rView.nInstanceCount = instance_count; rView.nMeshCount = mesh_count; return true;
}

T3DRuntimeInstanceCellMetadata T3DRuntimeBakeCodec::getInstanceMetadataRecord( const T3DInstanceMetadataView &rView, const uint32_t nIndex ) {
	T3DRuntimeInstanceCellMetadata record; if ( rView.pRecords == nullptr || nIndex >= rView.nCellCount ) return record;
	const uint8_t *p = rView.pRecords + ( size_t ) nIndex * 44u; record.meshId = readU16( p ); p += 4u; record.cell.x = readI32( p ); p += 4u; record.cell.y = readI32( p ); p += 4u; record.instanceCount = readU32( p ); p += 4u; record.ordinal = readU32( p ); p += 4u;
	const Vector3 minimum( readF32( p ), readF32( p + 4u ), readF32( p + 8u ) ); p += 12u; const Vector3 maximum( readF32( p ), readF32( p + 4u ), readF32( p + 8u ) ); record.originBounds = AABB( minimum, maximum - minimum ); return record;
}

bool T3DRuntimeBakeCodec::decodeInstanceMetadata( const PackedByteArray &rBlob, eastl::vector<T3DRuntimeInstanceCellMetadata> &rCells ) {
	rCells.clear(); T3DInstanceMetadataView view; if ( !getInstanceMetadataView( rBlob, view ) ) return false; rCells.resize( view.nCellCount );
	for ( uint32_t i = 0u; i < view.nCellCount; ++i ) rCells[i] = getInstanceMetadataRecord( view, i ); return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeLightingReceivers( const T3DRegion &rRegion ) {
	PackedByteArray bytes; const Ref<T3DRegionLighting> lighting = rRegion.getLighting(); if ( lighting.is_null() ) return bytes; const Array &users = lighting->getBakedUserDataInternal();
	if ( users.is_empty() || ( users.size() % 4 ) != 0 ) return bytes; const uint32_t count = ( uint32_t ) ( users.size() / 4 );
	eastl::vector<CharString> paths; paths.resize( count ); size_t total_bytes = 12u + ( size_t ) count * 28u;
	for ( uint32_t record = 0u; record < count; ++record ) { paths[record] = String( ( NodePath ) users[( int ) record * 4] ).utf8(); total_bytes += ( size_t ) MAX( 0, paths[record].length() ); }
	if ( !resizeBytes( bytes, total_bytes ) ) return PackedByteArray(); T3DByteWriter writer( bytes.ptrw(), total_bytes ); writer.writeU32( LIGHTING_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( 0u ); writer.writeU32( count );
	for ( uint32_t record = 0u; record < count; ++record ) {
		const int i = ( int ) record * 4; const Rect2 uv = users[i + 1]; const int32_t slice = ( int32_t ) ( ( int ) users[i + 2] ); const int32_t sub_instance = ( int32_t ) ( ( int ) users[i + 3] ); const CharString &utf8 = paths[record]; const uint32_t path_bytes = ( uint32_t ) MAX( 0, utf8.length() );
		writer.writeU32( path_bytes ); writer.writeBytes( utf8.get_data(), path_bytes ); writer.writeF32( uv.position.x ); writer.writeF32( uv.position.y ); writer.writeF32( uv.size.x ); writer.writeF32( uv.size.y ); writer.writeI32( slice ); writer.writeI32( sub_instance );
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool T3DRuntimeBakeCodec::beginLightingReceivers( const PackedByteArray &rBlob, T3DLightingReceiverCursor &rCursor ) {
	rCursor = T3DLightingReceiverCursor(); if ( rBlob.size() < 12 ) return false; const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != LIGHTING_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false; const uint32_t count = readU32( p + 8 ); if ( count > 1000000u ) return false;
	size_t offset = 12u; for ( uint32_t i = 0u; i < count; ++i ) { if ( offset + 4u > ( size_t ) rBlob.size() ) return false; const uint32_t path_bytes = readU32( p + offset ); offset += 4u; if ( path_bytes > 1024u * 1024u || offset + path_bytes + 24u > ( size_t ) rBlob.size() ) return false; offset += path_bytes + 24u; }
	if ( offset != ( size_t ) rBlob.size() ) return false; rCursor.pData = p; rCursor.nSize = ( size_t ) rBlob.size(); rCursor.nOffset = 12u; rCursor.nCount = count; return true;
}

bool T3DRuntimeBakeCodec::nextLightingReceiver( T3DLightingReceiverCursor &rCursor, T3DRuntimeLightingReceiverRecord &rReceiver ) {
	if ( rCursor.pData == nullptr || rCursor.nIndex >= rCursor.nCount || rCursor.nOffset + 4u > rCursor.nSize ) return false; const uint8_t *const p = rCursor.pData; const uint32_t path_bytes = readU32( p + rCursor.nOffset ); rCursor.nOffset += 4u;
	if ( rCursor.nOffset + path_bytes + 24u > rCursor.nSize ) return false; rReceiver.path = NodePath( String::utf8( reinterpret_cast<const char *>( p + rCursor.nOffset ), ( int ) path_bytes ) ); rCursor.nOffset += path_bytes;
	rReceiver.uvScale = Rect2( readF32( p + rCursor.nOffset ), readF32( p + rCursor.nOffset + 4u ), readF32( p + rCursor.nOffset + 8u ), readF32( p + rCursor.nOffset + 12u ) ); rCursor.nOffset += 16u; rReceiver.sliceIndex = readI32( p + rCursor.nOffset ); rCursor.nOffset += 4u; rReceiver.subInstance = readI32( p + rCursor.nOffset ); rCursor.nOffset += 4u; ++rCursor.nIndex; return !rReceiver.path.is_empty();
}

bool T3DRuntimeBakeCodec::decodeLightingReceivers( const PackedByteArray &rBlob, eastl::vector<T3DRuntimeLightingReceiverRecord> &rReceivers ) {
	rReceivers.clear(); T3DLightingReceiverCursor cursor; if ( !beginLightingReceivers( rBlob, cursor ) ) return false; rReceivers.resize( cursor.nCount );
	for ( uint32_t i = 0u; i < cursor.nCount; ++i ) if ( !nextLightingReceiver( cursor, rReceivers[i] ) ) { rReceivers.clear(); return false; } return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeMultimeshFastStart( const T3DRegion &rRegion, const int nMinimumCellInstances ) {
	PackedByteArray bytes; if ( !rRegion.ensureInstanceStoreResident() ) return bytes; const T3DInstanceStore &store = rRegion.getInstanceStore(); uint32_t eligible = 0u; size_t total_bytes = 12u;
	for ( const auto &mesh : store.meshes() ) for ( const auto &cell : mesh.cells ) if ( ( int ) cell.transforms.size() >= nMinimumCellInstances ) { ++eligible; total_bytes += 16u + cell.transforms.size() * 64u; }
	if ( eligible == 0u || !resizeBytes( bytes, total_bytes ) ) return bytes; T3DByteWriter writer( bytes.ptrw(), total_bytes ); writer.writeU32( MULTIMESH_MAGIC ); writer.writeU16( BLOB_VERSION ); writer.writeU16( 16u ); writer.writeU32( eligible );
	for ( const auto &mesh : store.meshes() ) for ( const auto &cell : mesh.cells ) {
		if ( ( int ) cell.transforms.size() < nMinimumCellInstances ) continue; writer.writeU16( ( uint16_t ) mesh.id ); writer.writeU16( 0u ); writer.writeI32( cell.location.x ); writer.writeI32( cell.location.y ); writer.writeU32( ( uint32_t ) cell.transforms.size() );
		for ( size_t i = 0; i < cell.transforms.size(); ++i ) { const Transform3D t = cell.transforms[i]; for ( int row = 0; row < 3; ++row ) { writer.writeF32( t.basis[row].x ); writer.writeF32( t.basis[row].y ); writer.writeF32( t.basis[row].z ); writer.writeF32( row == 0 ? t.origin.x : row == 1 ? t.origin.y : t.origin.z ); } const Color c = cell.getColor( i ); writer.writeF32( c.r ); writer.writeF32( c.g ); writer.writeF32( c.b ); writer.writeF32( c.a ); }
	}
	return writer.complete() ? bytes : PackedByteArray();
}

bool T3DRuntimeBakeCodec::findMultimeshFastStart(
	const PackedByteArray &rBlob, const uint16_t nMeshId, const Vector2i &rCell,
	PackedFloat32Array &rBuffer, uint32_t &nInstanceCount
) {
	rBuffer.clear();
	nInstanceCount = 0u;
	if ( rBlob.size() < 12 ) return false;
	const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != MULTIMESH_MAGIC || readU16( p + 4 ) != BLOB_VERSION || readU16( p + 6 ) != 16u ) return false;
	const uint32_t record_count = readU32( p + 8 );
	if ( record_count > 1000000u ) return false;
	size_t offset = 12u;
	for ( uint32_t record_index = 0u; record_index < record_count; ++record_index ) {
		if ( offset + 16u > ( size_t ) rBlob.size() ) return false;
		const uint16_t mesh_id = readU16( p + offset ); offset += 4u;
		const int32_t cell_x = readI32( p + offset ); offset += 4u;
		const int32_t cell_y = readI32( p + offset ); offset += 4u;
		const uint32_t count = readU32( p + offset ); offset += 4u;
		if ( count > 10000000u || ( uint64_t ) count * 64u > ( uint64_t ) rBlob.size() - offset ) return false;
		if ( mesh_id == nMeshId && cell_x == rCell.x && cell_y == rCell.y ) {
			rBuffer.resize( ( int64_t ) count * 16 );
			float *const out = rBuffer.ptrw();
			for ( uint64_t value = 0u; value < ( uint64_t ) count * 16u; ++value ) {
				out[value] = readF32( p + offset + value * 4u );
			}
			nInstanceCount = count;
			return true;
		}
		offset += ( size_t ) count * 64u;
	}
	return false;
}
