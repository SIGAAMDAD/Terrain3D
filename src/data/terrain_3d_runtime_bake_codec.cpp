// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_compiler.h"
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

EA_FORCE_INLINE static void appendU16( PackedByteArray &rBytes, const uint16_t nValue ) { const int64_t old = rBytes.size(); rBytes.resize( old + 2 ); uint8_t *p = rBytes.ptrw() + old; p[0] = ( uint8_t ) ( nValue & 0xffu ); p[1] = ( uint8_t ) ( nValue >> 8u ); }
static void appendU32( PackedByteArray &rBytes, const uint32_t nValue ) {
	const int64_t old = rBytes.size(); rBytes.resize( old + 4 ); uint8_t *p = rBytes.ptrw() + old;
	p[0] = ( uint8_t ) ( nValue & 0xffu ); p[1] = ( uint8_t ) ( nValue >> 8u );
	p[2] = ( uint8_t ) ( nValue >> 16u ); p[3] = ( uint8_t ) ( nValue >> 24u );
}
EA_FORCE_INLINE static void appendI32( PackedByteArray &rBytes, const int32_t nValue ) { appendU32( rBytes, ( uint32_t ) nValue ); }
EA_FORCE_INLINE static void appendU64( PackedByteArray &rBytes, const uint64_t nValue ) { appendU32( rBytes, ( uint32_t ) nValue ); appendU32( rBytes, ( uint32_t ) ( nValue >> 32u ) ); }
EA_FORCE_INLINE static void appendF32( PackedByteArray &rBytes, const float fValue ) { uint32_t bits = 0u; std::memcpy( &bits, &fValue, 4 ); appendU32( rBytes, bits ); }
EA_FORCE_INLINE static uint16_t readU16( const uint8_t *p ) { return ( uint16_t ) ( p[0] | ( ( uint16_t ) p[1] << 8u ) ); }
EA_FORCE_INLINE static uint32_t readU32( const uint8_t *p ) { return ( uint32_t ) p[0] | ( ( uint32_t ) p[1] << 8u ) | ( ( uint32_t ) p[2] << 16u ) | ( ( uint32_t ) p[3] << 24u ); }
EA_FORCE_INLINE static int32_t readI32( const uint8_t *p ) { return ( int32_t ) readU32( p ); }
EA_FORCE_INLINE static uint64_t readU64( const uint8_t *p ) { return ( uint64_t ) readU32( p ) | ( ( uint64_t ) readU32( p + 4 ) << 32u ); }
EA_FORCE_INLINE static float readF32( const uint8_t *p ) { const uint32_t bits = readU32( p ); float v = 0.f; std::memcpy( &v, &bits, 4 ); return v; }

static uint32_t readControlPixel( const Ref<Image> &rControl, const int nIndex ) {
	if ( rControl.is_null() ) return TERRAIN3D_REGION_DEFAULT_CONTROL;
	const PackedByteArray bytes = rControl->get_data();
	if ( bytes.size() < ( int64_t ) ( ( nIndex + 1 ) * 4 ) ) return TERRAIN3D_REGION_DEFAULT_CONTROL;
	uint32_t value = 0u; std::memcpy( &value, bytes.ptr() + ( size_t ) nIndex * 4u, 4u ); return value;
}

EA_FORCE_INLINE static void setBit( PackedByteArray &rBytes, const int64_t nBase, const uint32_t nIndex ) { rBytes.ptrw()[nBase + nIndex / 8u] |= ( uint8_t ) ( 1u << ( nIndex & 7u ) ); }

EA_FORCE_INLINE static String descriptorScenePath( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rDescriptor ) { if ( !rDescriptor.scenePath.is_empty() ) return rDescriptor.scenePath; return rDescriptor.pScene.is_valid() ? rDescriptor.pScene->get_path() : String(); }
EA_FORCE_INLINE static String descriptorShadowPath( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rDescriptor ) { if ( !rDescriptor.shadowProxyScenePath.is_empty() ) return rDescriptor.shadowProxyScenePath; return rDescriptor.pShadowProxyScene.is_valid() ? rDescriptor.pShadowProxyScene->get_path() : String(); }
}

PackedByteArray T3DRuntimeBakeCodec::encodeHoleMask( const Ref<Image> &rControl, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( nRegionSize <= 0 || ( rControl.is_valid() && ( rControl->get_width() != nRegionSize || rControl->get_height() != nRegionSize || rControl->get_format() != Image::FORMAT_RF ) ) ) return bytes;
	const uint32_t samples = ( uint32_t ) nRegionSize * ( uint32_t ) nRegionSize;
	appendU32( bytes, HOLE_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) nRegionSize ); appendU32( bytes, samples );
	const int64_t base = bytes.size(); bytes.resize( base + ( samples + 7u ) / 8u ); std::memset( bytes.ptrw() + base, 0, ( size_t ) ( bytes.size() - base ) );
	for ( uint32_t i = 0u; i < samples; ++i ) if ( isHole( readControlPixel( rControl, ( int ) i ) ) ) setBit( bytes, base, i );
	return bytes;
}

PackedByteArray T3DRuntimeBakeCodec::encodeNavigationMask( const Ref<Image> &rControl, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( nRegionSize <= 0 || ( rControl.is_valid() && ( rControl->get_width() != nRegionSize || rControl->get_height() != nRegionSize || rControl->get_format() != Image::FORMAT_RF ) ) ) return bytes;
	const uint32_t samples = ( uint32_t ) nRegionSize * ( uint32_t ) nRegionSize;
	const uint32_t plane = ( samples + 7u ) / 8u;
	appendU32( bytes, NAV_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) nRegionSize ); appendU32( bytes, samples ); appendU32( bytes, plane );
	const int64_t valid_base = bytes.size(); bytes.resize( valid_base + plane * 2u ); std::memset( bytes.ptrw() + valid_base, 0, plane * 2u );
	const int64_t nav_base = valid_base + plane;
	for ( uint32_t i = 0u; i < samples; ++i ) {
		const uint32_t control = readControlPixel( rControl, ( int ) i );
		if ( !isHole( control ) ) setBit( bytes, valid_base, i );
		if ( !isHole( control ) && isNav( control ) ) setBit( bytes, nav_base, i );
	}
	return bytes;
}

PackedByteArray T3DRuntimeBakeCodec::encodeNavigationMask( const PackedInt32Array &rControls, const int nWidth ) {
	PackedByteArray bytes;
	if ( nWidth <= 0 || rControls.size() != nWidth * nWidth || nWidth > UINT16_MAX ) return bytes;
	const uint32_t samples = ( uint32_t ) nWidth * ( uint32_t ) nWidth;
	const uint32_t plane = ( samples + 7u ) / 8u;
	appendU32( bytes, NAV_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) nWidth ); appendU32( bytes, samples ); appendU32( bytes, plane );
	const int64_t valid_base = bytes.size(); bytes.resize( valid_base + plane * 2u ); std::memset( bytes.ptrw() + valid_base, 0, plane * 2u );
	const int64_t nav_base = valid_base + plane;
	for ( uint32_t i = 0u; i < samples; ++i ) {
		const uint32_t control = ( uint32_t ) rControls[( int ) i];
		if ( !isHole( control ) ) setBit( bytes, valid_base, i );
		if ( !isHole( control ) && isNav( control ) ) setBit( bytes, nav_base, i );
	}
	return bytes;
}

bool T3DRuntimeBakeCodec::decodeNavigationMask(
	const PackedByteArray &rBlob, int &nWidth, PackedByteArray &rValidBits, PackedByteArray &rNavigationBits
) {
	nWidth = 0; rValidBits.clear(); rNavigationBits.clear();
	if ( rBlob.size() < 16 ) return false;
	const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != NAV_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	nWidth = readU16( p + 6 );
	const uint32_t samples = readU32( p + 8 );
	const uint32_t plane = readU32( p + 12 );
	if ( nWidth <= 0 || samples != ( uint32_t ) nWidth * nWidth ||
		plane != ( samples + 7u ) / 8u || rBlob.size() != 16 + ( int64_t ) plane * 2 ) return false;
	rValidBits.resize( plane ); rNavigationBits.resize( plane );
	std::memcpy( rValidBits.ptrw(), p + 16, plane );
	std::memcpy( rNavigationBits.ptrw(), p + 16 + plane, plane );
	return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeMinMaxHierarchy( const Ref<Image> &rHeight, const int nRegionSize ) {
	PackedByteArray bytes;
	if ( rHeight.is_null() || rHeight->get_width() != nRegionSize || rHeight->get_height() != nRegionSize || rHeight->get_format() != Image::FORMAT_R16 ) return bytes;
	const PackedByteArray source = rHeight->get_data();
	if ( source.size() < ( int64_t ) nRegionSize * nRegionSize * 2 ) return bytes;
	appendU32( bytes, MINMAX_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) nRegionSize );
	int levels = 1; for ( int side = nRegionSize; side > 1; side = ( side + 1 ) / 2 ) ++levels;
	appendU32( bytes, ( uint32_t ) levels );
	eastl::vector<float> mins( ( size_t ) nRegionSize * nRegionSize );
	eastl::vector<float> maxs( mins.size() );
	for ( int i = 0; i < nRegionSize * nRegionSize; ++i ) mins[( size_t ) i] = maxs[( size_t ) i] = T3DRegionCodec::readHeightBytes( source.ptr(), i, Image::FORMAT_R16 );
	int side = nRegionSize;
	while ( true ) {
		appendU32( bytes, ( uint32_t ) side ); appendU32( bytes, ( uint32_t ) ( side * side ) );
		for ( int i = 0; i < side * side; ++i ) { appendF32( bytes, mins[( size_t ) i] ); appendF32( bytes, maxs[( size_t ) i] ); }
		if ( side == 1 ) break;
		const int next = ( side + 1 ) / 2;
		eastl::vector<float> next_mins( ( size_t ) next * next, INFINITY );
		eastl::vector<float> next_maxs( ( size_t ) next * next, -INFINITY );
		for ( int z = 0; z < side; ++z ) for ( int x = 0; x < side; ++x ) {
			const int dst = ( z / 2 ) * next + x / 2; const int src = z * side + x;
			next_mins[( size_t ) dst] = MIN( next_mins[( size_t ) dst], mins[( size_t ) src] );
			next_maxs[( size_t ) dst] = MAX( next_maxs[( size_t ) dst], maxs[( size_t ) src] );
		}
		mins.swap( next_mins ); maxs.swap( next_maxs ); side = next;
	}
	return bytes;
}

bool T3DRuntimeBakeCodec::decodeMinMaxLevel(
	const PackedByteArray &rBlob, const int nMaximumSide, int &nSourceSide,
	int &nLevelSide, eastl::vector<Vector2> &rRanges
) {
	nSourceSide = 0;
	nLevelSide = 0;
	rRanges.clear();
	if ( rBlob.size() < 12 || nMaximumSide <= 0 ) {
		return false;
	}
	const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != MINMAX_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) {
		return false;
	}
	nSourceSide = ( int ) readU16( p + 6 );
	const uint32_t level_count = readU32( p + 8 );
	if ( nSourceSide <= 0 || level_count == 0u || level_count > 32u ) {
		return false;
	}
	size_t offset = 12u;
	size_t selected_offset = 0u;
	uint32_t selected_count = 0u;
	int selected_side = 0;
	for ( uint32_t level = 0u; level < level_count; ++level ) {
		if ( offset + 8u > ( size_t ) rBlob.size() ) {
			return false;
		}
		const uint32_t side = readU32( p + offset );
		const uint32_t count = readU32( p + offset + 4u );
		offset += 8u;
		if ( side == 0u || count != side * side ||
			( uint64_t ) count * 8u > ( uint64_t ) rBlob.size() - offset ) {
			return false;
		}
		if ( selected_side == 0 && ( int ) side <= nMaximumSide ) {
			selected_side = ( int ) side;
			selected_count = count;
			selected_offset = offset;
		}
		offset += ( size_t ) count * 8u;
	}
	if ( offset != ( size_t ) rBlob.size() ) {
		return false;
	}
	if ( selected_side == 0 ) {
		return false;
	}
	nLevelSide = selected_side;
	rRanges.resize( selected_count );
	for ( uint32_t index = 0u; index < selected_count; ++index ) {
		const size_t range_offset = selected_offset + ( size_t ) index * 8u;
		rRanges[index] = Vector2( readF32( p + range_offset ), readF32( p + range_offset + 4u ) );
	}
	return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeCollisionHeight( const PackedFloat32Array &rHeights, const int nWidth ) {
	PackedByteArray bytes; if ( nWidth <= 0 || rHeights.size() != nWidth * nWidth ) return bytes;
	appendU32( bytes, COLLISION_HEIGHT_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) nWidth ); appendU32( bytes, ( uint32_t ) rHeights.size() );
	const int64_t base = bytes.size(); bytes.resize( base + rHeights.size() * 4 ); std::memcpy( bytes.ptrw() + base, rHeights.ptr(), ( size_t ) rHeights.size() * 4u ); return bytes;
}
PackedByteArray T3DRuntimeBakeCodec::encodeCollisionBounds( const float fMinHeight, const float fMaxHeight, const int nWidth ) {
	PackedByteArray bytes; appendU32( bytes, COLLISION_BOUNDS_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) nWidth ); appendF32( bytes, fMinHeight ); appendF32( bytes, fMaxHeight ); return bytes;
}
bool T3DRuntimeBakeCodec::decodeCollisionHeight( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth ) {
	rHeights.clear(); nWidth = 0; if ( rBlob.size() < 12 ) return false; const uint8_t *p = rBlob.ptr(); if ( readU32( p ) != COLLISION_HEIGHT_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	nWidth = readU16( p + 6 ); const uint32_t count = readU32( p + 8 ); if ( nWidth <= 0 || count != ( uint32_t ) nWidth * nWidth || rBlob.size() != 12 + ( int64_t ) count * 4 ) return false;
	rHeights.resize( count ); std::memcpy( rHeights.ptrw(), p + 12, ( size_t ) count * 4u ); return true;
}
bool T3DRuntimeBakeCodec::decodeCollisionBounds( const PackedByteArray &rBlob, float &fMinHeight, float &fMaxHeight, int &nWidth ) {
	if ( rBlob.size() != 16 ) return false; const uint8_t *p = rBlob.ptr(); if ( readU32( p ) != COLLISION_BOUNDS_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	nWidth = readU16( p + 6 ); fMinHeight = readF32( p + 8 ); fMaxHeight = readF32( p + 12 ); return nWidth > 0;
}

PackedByteArray T3DRuntimeBakeCodec::encodeNodeDescriptors( const T3DRegion &rRegion, const T3DRuntimeWorldManifest &rManifest ) {
	PackedByteArray bytes; const auto &nodes = rRegion.getStreamedNodesRuntimeInternal();
	appendU32( bytes, NODE_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) MIN( nodes.size(), ( size_t ) UINT16_MAX ) );
	for ( const auto &node : nodes ) {
		const int scene_id = rManifest.findSceneDependency( descriptorScenePath( node ) ); const int shadow_id = rManifest.findSceneDependency( descriptorShadowPath( node ) );
		if ( scene_id < 0 || scene_id > UINT16_MAX ) return PackedByteArray();
		appendU64( bytes, ( uint64_t ) node.id ); appendU16( bytes, ( uint16_t ) scene_id ); appendU16( bytes, shadow_id >= 0 && shadow_id <= UINT16_MAX ? ( uint16_t ) shadow_id : UINT16_MAX );
		appendI32( bytes, node.flags ); appendU16( bytes, ( uint16_t ) node.hlodMinLevel ); appendU16( bytes, ( uint16_t ) node.boundaryPolicy );
		appendI32( bytes, node.coverageMin.x ); appendI32( bytes, node.coverageMin.y ); appendI32( bytes, node.coverageMax.x ); appendI32( bytes, node.coverageMax.y );
		appendU16( bytes, ( uint16_t ) node.lightingClass ); appendU16( bytes, ( uint16_t ) node.bakedLightingMode );
		uint32_t bools = node.bContributeBakedGi ? 1u : 0u; if ( node.bReceiveDynamicDirectional ) bools |= 2u; if ( node.bDynamicShadowEligible ) bools |= 4u; appendU32( bytes, bools );
		for ( int row = 0; row < 3; ++row ) { appendF32( bytes, node.transform.basis[row].x ); appendF32( bytes, node.transform.basis[row].y ); appendF32( bytes, node.transform.basis[row].z ); }
		appendF32( bytes, node.transform.origin.x ); appendF32( bytes, node.transform.origin.y ); appendF32( bytes, node.transform.origin.z );
		const PackedByteArray state = node.state.is_empty() ? PackedByteArray() : UtilityFunctions::var_to_bytes( node.state ); appendU32( bytes, ( uint32_t ) state.size() );
		if ( !state.is_empty() ) { const int64_t old = bytes.size(); bytes.resize( old + state.size() ); std::memcpy( bytes.ptrw() + old, state.ptr(), ( size_t ) state.size() ); }
	}
	return bytes;
}

bool T3DRuntimeBakeCodec::decodeNodeDescriptors( const PackedByteArray &rBlob, const T3DRuntimeWorldManifest &rManifest, T3DRegion &rRegion ) {
	if ( rBlob.size() < 8 ) return false; const uint8_t *p = rBlob.ptr(); if ( readU32( p ) != NODE_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false; const uint16_t count = readU16( p + 6 ); size_t offset = 8u;
	eastl::vector<T3DRegion::t3dstreamedNodeRuntimeDescriptor_t> nodes; nodes.reserve( count );
	for ( uint16_t i = 0u; i < count; ++i ) {
		static constexpr size_t fixed = 8u + 2u + 2u + 4u + 2u + 2u + 16u + 2u + 2u + 4u + 48u + 4u;
		if ( offset + fixed > ( size_t ) rBlob.size() ) return false; T3DRegion::t3dstreamedNodeRuntimeDescriptor_t node;
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
	appendU32( bytes, INSTANCE_META_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, ( uint16_t ) store.meshCount() ); appendU32( bytes, cell_count ); appendU32( bytes, rRegion.getInstanceCount() );
	uint32_t ordinal = 0u; for ( const auto &mesh : store.meshes() ) for ( const auto &cell : mesh.cells ) {
		appendU16( bytes, ( uint16_t ) mesh.id ); appendU16( bytes, 0u ); appendI32( bytes, cell.location.x ); appendI32( bytes, cell.location.y ); appendU32( bytes, ( uint32_t ) cell.transforms.size() ); appendU32( bytes, ordinal );
		Vector3 minimum( INFINITY, INFINITY, INFINITY ), maximum( -INFINITY, -INFINITY, -INFINITY );
		for ( size_t i = 0; i < cell.transforms.size(); ++i ) { const Vector3 o = cell.transforms.getOrigin( i ); minimum.x = MIN( minimum.x, o.x ); minimum.y = MIN( minimum.y, o.y ); minimum.z = MIN( minimum.z, o.z ); maximum.x = MAX( maximum.x, o.x ); maximum.y = MAX( maximum.y, o.y ); maximum.z = MAX( maximum.z, o.z ); }
		if ( cell.transforms.empty() ) minimum = maximum = V3_ZERO;
		appendF32( bytes, minimum.x ); appendF32( bytes, minimum.y ); appendF32( bytes, minimum.z ); appendF32( bytes, maximum.x ); appendF32( bytes, maximum.y ); appendF32( bytes, maximum.z ); ordinal += ( uint32_t ) cell.transforms.size();
	}
	return bytes;
}

bool T3DRuntimeBakeCodec::decodeInstanceMetadata(
	const PackedByteArray &rBlob, eastl::vector<T3DRuntimeInstanceCellMetadata> &rCells
) {
	rCells.clear();
	if ( rBlob.size() < 16 ) return false;
	const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != INSTANCE_META_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	const uint32_t cell_count = readU32( p + 8 );
	if ( cell_count > 1000000u || ( size_t ) rBlob.size() != 16u + ( size_t ) cell_count * 44u ) return false;
	rCells.reserve( cell_count );
	size_t offset = 16u;
	for ( uint32_t i = 0u; i < cell_count; ++i ) {
		T3DRuntimeInstanceCellMetadata record;
		record.meshId = readU16( p + offset ); offset += 4u;
		record.cell.x = readI32( p + offset ); offset += 4u;
		record.cell.y = readI32( p + offset ); offset += 4u;
		record.instanceCount = readU32( p + offset ); offset += 4u;
		record.ordinal = readU32( p + offset ); offset += 4u;
		const Vector3 minimum( readF32( p + offset ), readF32( p + offset + 4u ), readF32( p + offset + 8u ) );
		offset += 12u;
		const Vector3 maximum( readF32( p + offset ), readF32( p + offset + 4u ), readF32( p + offset + 8u ) );
		offset += 12u;
		record.originBounds = AABB( minimum, maximum - minimum );
		rCells.push_back( record );
	}
	return true;
}

PackedByteArray T3DRuntimeBakeCodec::encodeLightingReceivers( const T3DRegion &rRegion ) {
	PackedByteArray bytes;
	const Ref<T3DRegionLighting> lighting = rRegion.getLighting();
	if ( lighting.is_null() ) return bytes;
	const Array &users = lighting->getBakedUserDataInternal();
	if ( users.is_empty() || ( users.size() % 4 ) != 0 ) return bytes;
	appendU32( bytes, LIGHTING_MAGIC );
	appendU16( bytes, BLOB_VERSION );
	appendU16( bytes, 0u );
	appendU32( bytes, ( uint32_t ) ( users.size() / 4 ) );
	for ( int i = 0; i + 3 < users.size(); i += 4 ) {
		const NodePath path = users[i];
		const Rect2 uv = users[i + 1];
		const int32_t slice = ( int32_t ) ( ( int ) users[i + 2] );
		const int32_t sub_instance = ( int32_t ) ( ( int ) users[i + 3] );
		const CharString utf8 = String( path ).utf8();
		const uint32_t path_bytes = ( uint32_t ) MAX( 0, utf8.length() );
		appendU32( bytes, path_bytes );
		const int64_t old = bytes.size();
		bytes.resize( old + path_bytes );
		if ( path_bytes > 0u ) std::memcpy( bytes.ptrw() + old, utf8.get_data(), path_bytes );
		appendF32( bytes, uv.position.x ); appendF32( bytes, uv.position.y );
		appendF32( bytes, uv.size.x ); appendF32( bytes, uv.size.y );
		appendI32( bytes, slice ); appendI32( bytes, sub_instance );
	}
	return bytes;
}

bool T3DRuntimeBakeCodec::decodeLightingReceivers(
	const PackedByteArray &rBlob, eastl::vector<T3DRuntimeLightingReceiverRecord> &rReceivers
) {
	rReceivers.clear();
	if ( rBlob.size() < 12 ) return false;
	const uint8_t *const p = rBlob.ptr();
	if ( readU32( p ) != LIGHTING_MAGIC || readU16( p + 4 ) != BLOB_VERSION ) return false;
	const uint32_t count = readU32( p + 8 );
	if ( count > 1000000u ) return false;
	size_t offset = 12u;
	rReceivers.reserve( count );
	for ( uint32_t i = 0u; i < count; ++i ) {
		if ( offset + 4u > ( size_t ) rBlob.size() ) return false;
		const uint32_t path_bytes = readU32( p + offset ); offset += 4u;
		if ( path_bytes > 1024u * 1024u || offset + path_bytes + 24u > ( size_t ) rBlob.size() ) return false;
		T3DRuntimeLightingReceiverRecord receiver;
		receiver.path = NodePath( String::utf8( reinterpret_cast<const char *>( p + offset ), ( int ) path_bytes ) );
		offset += path_bytes;
		receiver.uvScale = Rect2( readF32( p + offset ), readF32( p + offset + 4 ), readF32( p + offset + 8 ), readF32( p + offset + 12 ) );
		offset += 16u;
		receiver.sliceIndex = readI32( p + offset ); offset += 4u;
		receiver.subInstance = readI32( p + offset ); offset += 4u;
		if ( receiver.path.is_empty() ) return false;
		rReceivers.push_back( receiver );
	}
	return offset == ( size_t ) rBlob.size();
}

PackedByteArray T3DRuntimeBakeCodec::encodeMultimeshFastStart( const T3DRegion &rRegion, const int nMinimumCellInstances ) {
	PackedByteArray bytes; if ( !rRegion.ensureInstanceStoreResident() ) return bytes; const T3DInstanceStore &store = rRegion.getInstanceStore();
	uint32_t eligible = 0u; for ( const auto &mesh : store.meshes() ) for ( const auto &cell : mesh.cells ) if ( ( int ) cell.transforms.size() >= nMinimumCellInstances ) ++eligible;
	if ( eligible == 0u ) return bytes; appendU32( bytes, MULTIMESH_MAGIC ); appendU16( bytes, BLOB_VERSION ); appendU16( bytes, 16u ); appendU32( bytes, eligible );
	for ( const auto &mesh : store.meshes() ) for ( const auto &cell : mesh.cells ) { if ( ( int ) cell.transforms.size() < nMinimumCellInstances ) continue; appendU16( bytes, ( uint16_t ) mesh.id ); appendU16( bytes, 0u ); appendI32( bytes, cell.location.x ); appendI32( bytes, cell.location.y ); appendU32( bytes, ( uint32_t ) cell.transforms.size() );
		for ( size_t i = 0; i < cell.transforms.size(); ++i ) { const Transform3D t = cell.transforms[i]; for ( int row = 0; row < 3; ++row ) { appendF32( bytes, t.basis[row].x ); appendF32( bytes, t.basis[row].y ); appendF32( bytes, t.basis[row].z ); appendF32( bytes, row == 0 ? t.origin.x : row == 1 ? t.origin.y : t.origin.z ); } const Color c = cell.getColor( i ); appendF32( bytes, c.r ); appendF32( bytes, c.g ); appendF32( bytes, c.b ); appendF32( bytes, c.a ); }
	}
	return bytes;
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
