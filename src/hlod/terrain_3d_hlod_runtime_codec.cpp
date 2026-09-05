// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "Terrain3DConstants.h"
#include "hlod/terrain_3d_hlod_runtime_codec.h"

#include "core/terrain_3d_byte_writer.h"
#include "core/terrain_3d_compiler.h"
#include "eastl_config.h"

#include <EASTL/sort.h>
#include <string.h>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>


typedef struct t3dHlodRuntimeNodeTemp_s {
	uint16_t m_nRegion;
	int64_t m_nSpawn;
	uint8_t m_nLevel;
} t3dHlodRuntimeNodeTemp_t;

static bool t3dHlodRuntimeNodeTempLess( const t3dHlodRuntimeNodeTemp_t &rA, const t3dHlodRuntimeNodeTemp_t &rB ) {
	return rA.m_nRegion < rB.m_nRegion || ( rA.m_nRegion == rB.m_nRegion && rA.m_nSpawn < rB.m_nSpawn );
}

EA_FORCE_INLINE static int t3dHlodRuntimeCodecFloorDiv( const int value, const int divisor ) {
	if ( divisor <= 0 ) {
		return 0;
	}
	int quotient = value / divisor;
	if ( value < 0 && ( value % divisor ) != 0 ) {
		--quotient;
	}
	return quotient;
}
EA_FORCE_INLINE static uint8_t t3dHlodRuntimeCodecRu8( const uint8_t *p ) {
	return p[0];
}
EA_FORCE_INLINE static uint16_t t3dHlodRuntimeCodecRu16( const uint8_t *p ) {
	return ( uint16_t )( p[0] | ( ( uint16_t )p[1] << 8 ) );
}
EA_FORCE_INLINE static uint32_t t3dHlodRuntimeCodecRu32( const uint8_t *p ) {
	return ( uint32_t )p[0] | ( ( uint32_t )p[1] << 8 ) | ( ( uint32_t )p[2] << 16 ) | ( ( uint32_t )p[3] << 24 );
}
EA_FORCE_INLINE static int32_t t3dHlodRuntimeCodecRi32( const uint8_t *p ) {
	return ( int32_t )t3dHlodRuntimeCodecRu32( p );
}
EA_FORCE_INLINE static uint64_t t3dHlodRuntimeCodecRu64( const uint8_t *p ) {
	return ( uint64_t )t3dHlodRuntimeCodecRu32( p ) | ( ( uint64_t )t3dHlodRuntimeCodecRu32( p + 4 ) << 32 );
}
EA_FORCE_INLINE static int64_t t3dHlodRuntimeCodecRi64( const uint8_t *p ) {
	return ( int64_t )t3dHlodRuntimeCodecRu64( p );
}
EA_FORCE_INLINE static float t3dHlodRuntimeCodecRf32( const uint8_t *p ) {
	uint32_t bits = t3dHlodRuntimeCodecRu32( p );
	float v = 0;
	memcpy( &v, &bits, 4 );
	return v;
}
EA_FORCE_INLINE static bool t3dHlodRuntimeCodecHasBytes( const size_t nOffset, const size_t nBytes, const size_t nTotalBytes ) {
	return nOffset <= nTotalBytes && nBytes <= nTotalBytes - nOffset;
}

static bool t3dHlodRuntimeCodecParseEntry( const Variant &rValue, t3dHlodRuntimeEntryRecord_t &rEntry ) {
	if ( rValue.get_type() != Variant::DICTIONARY ) {
		return false;
	}
	const Dictionary d = rValue;
	if ( !d.has( "level" ) || !d.has( "cluster" ) || !d.has( "region_min" ) || !d.has( "region_max" ) || !d.has( "package_path" ) ) {
		return false;
	}
	rEntry.m_nLevel = CLAMP( ( int )d["level"], 1, 6 );
	rEntry.setCluster( d["cluster"] );
	rEntry.setRegionMin( d["region_min"] );
	rEntry.setRegionMax( d["region_max"] );
	rEntry.m_localBounds = d.get( "local_bounds", AABB() );
	rEntry.m_packagePath = String( d["package_path"] );
	rEntry.m_fGeometricError = MAX( ( real_t )d.get( "geometric_error", 1.0 ), 0.001f );
	rEntry.m_nSourceSignature = ( int64_t )d.get( "source_signature", 0 );
	rEntry.m_nBuildSignature = ( int64_t )d.get( "build_signature", 0 );
	rEntry.m_nEstimatedGpuBytes = MAX<int64_t>( ( int64_t )d.get( "estimated_gpu_bytes", 0 ), 0 );
	return !rEntry.m_packagePath.is_empty();
}

PackedByteArray Terrain3DHLODRuntimeCodec::encode( const Array &rEntries,
        const PackedByteArray &rInstancerMinLevels,
        const PackedInt32Array &rNodeRegionIndices,
        const PackedInt64Array &rNodeSpawnIds,
        const PackedByteArray &rNodeMinLevels ) {
	PackedByteArray out;
	eastl::vector<t3dHlodRuntimeEntryRecord_t, Terrain3DEastlAllocator> entries;
	entries.reserve( ( size_t )rEntries.size() );
	for ( int i = 0; i < rEntries.size(); ++i ) {
		t3dHlodRuntimeEntryRecord_t entry;
		if ( !t3dHlodRuntimeCodecParseEntry( rEntries[i], entry ) ) {
			return PackedByteArray();
		}
		entries.push_back( entry );
	}
	for ( int i = 0; i < ( int )entries.size(); ++i ) {
		t3dHlodRuntimeEntryRecord_t &entry = entries[( size_t )i];
		const Vector2i parent_cluster(
		        t3dHlodRuntimeCodecFloorDiv( entry.getCluster().x, 2 ), t3dHlodRuntimeCodecFloorDiv( entry.getCluster().y, 2 ) );
		int parent = -1;
		for ( int j = 0; j < ( int )entries.size(); ++j ) {
			if ( entries[( size_t )j].m_nLevel == entry.m_nLevel + 1 && entries[( size_t )j].getCluster() == parent_cluster ) {
				parent = j;
				break;
			}
		}
		entry.m_nParent = parent;
		if ( parent >= 0 ) {
			const int local_x = entry.getCluster().x - parent_cluster.x * 2, local_y = entry.getCluster().y - parent_cluster.y * 2;
			entries[( size_t )parent].m_szChildren[CLAMP( local_y, 0, 1 ) * 2 + CLAMP( local_x, 0, 1 )] = i;
		}
	}
	eastl::vector<int, Terrain3DEastlAllocator> roots;
	roots.reserve( entries.size() );
	for ( int i = 0; i < ( int )entries.size(); ++i ) {
		if ( entries[( size_t )i].m_nParent < 0 ) {
			roots.push_back( i );
		}
	}
	eastl::vector<t3dHlodRuntimeNodeTemp_t, Terrain3DEastlAllocator> nodes;
	const int node_count = MIN( rNodeRegionIndices.size(), MIN( rNodeSpawnIds.size(), rNodeMinLevels.size() ) );
	nodes.reserve( ( size_t )node_count );
	for ( int i = 0; i < node_count; ++i ) {
		const int region = rNodeRegionIndices[i];
		const uint8_t level = rNodeMinLevels[i];
		if ( region >= 0 && region < s_nRegionSlots && level > 0 ) {
			nodes.push_back( { ( uint16_t )region, rNodeSpawnIds[i], level } );
		}
	}
	eastl::sort( nodes.begin(), nodes.end(), t3dHlodRuntimeNodeTempLess );

	eastl::vector<CharString, Terrain3DEastlAllocator> paths;
	paths.resize( entries.size() );
	size_t total_bytes = 24u + roots.size() * 4u + ( size_t )rInstancerMinLevels.size() + nodes.size() * 12u + entries.size() * 104u;
	for ( size_t i = 0; i < entries.size(); ++i ) {
		paths[i] = entries[i].m_packagePath.utf8();
		total_bytes += ( size_t )MAX( paths[i].length(), 0 );
	}
	if ( total_bytes > ( size_t )INT64_MAX ) {
		return PackedByteArray();
	}
	out.resize( ( int64_t )total_bytes );
	if ( ( size_t )out.size() != total_bytes ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( out.ptrw(), total_bytes );
	writer.writeU32( s_nHlodRuntimeMagic );
	writer.writeU16( s_nHlodRuntimeVersion );
	writer.writeU16( 0u );
	writer.writeU32( ( uint32_t )entries.size() );
	writer.writeU32( ( uint32_t )roots.size() );
	writer.writeU32( ( uint32_t )rInstancerMinLevels.size() );
	writer.writeU32( ( uint32_t )nodes.size() );
	for ( size_t i = 0; i < entries.size(); ++i ) {
		const t3dHlodRuntimeEntryRecord_t &entry = entries[i];
		writer.writeU8( ( uint8_t )entry.m_nLevel );
		writer.writeU8( 0u );
		writer.writeU16( 0u );
		writer.writeI32( entry.getCluster().x );
		writer.writeI32( entry.getCluster().y );
		writer.writeI32( entry.getRegionMin().x );
		writer.writeI32( entry.getRegionMin().y );
		writer.writeI32( entry.getRegionMax().x );
		writer.writeI32( entry.getRegionMax().y );
		writer.writeF32( entry.m_localBounds.position.x );
		writer.writeF32( entry.m_localBounds.position.y );
		writer.writeF32( entry.m_localBounds.position.z );
		writer.writeF32( entry.m_localBounds.size.x );
		writer.writeF32( entry.m_localBounds.size.y );
		writer.writeF32( entry.m_localBounds.size.z );
		writer.writeF32( ( float )entry.m_fGeometricError );
		writer.writeI64( entry.m_nSourceSignature );
		writer.writeI64( entry.m_nBuildSignature );
		writer.writeI64( entry.m_nEstimatedGpuBytes );
		writer.writeI32( entry.m_nParent );
		for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( entry.m_szChildren ); iChild++ ) {
			int child = ( entry.m_szChildren )[iChild];
			writer.writeI32( child );
		}
		const CharString &path = paths[i];
		const uint32_t len = ( uint32_t )MAX( path.length(), 0 );
		writer.writeU32( len );
		writer.writeBytes( path.get_data(), len );
	}
	for ( size_t iRoot = 0u; iRoot < ( size_t )( ( roots ).size() ); iRoot++ ) {
		int root = ( roots )[iRoot];
		writer.writeU32( ( uint32_t )root );
	}
	writer.writeBytes( rInstancerMinLevels.ptr(), ( size_t )rInstancerMinLevels.size() );
	for ( size_t iNode = 0u; iNode < ( size_t )( ( nodes ).size() ); iNode++ ) {
		const t3dHlodRuntimeNodeTemp_t &node = ( nodes )[iNode];
		writer.writeU16( node.m_nRegion );
		writer.writeU8( node.m_nLevel );
		writer.writeU8( 0u );
		writer.writeI64( node.m_nSpawn );
	}
	return writer.complete() ? out : PackedByteArray();
}

bool Terrain3DHLODRuntimeCodec::decode( const PackedByteArray &rBlob, t3dHlodRuntimeData_t &rData ) {
	rData = t3dHlodRuntimeData_t{};
	if ( rBlob.size() < 24 ) {
		return false;
	}
	const uint8_t *p = rBlob.ptr();
	size_t offset = 0u;
	if ( t3dHlodRuntimeCodecRu32( p ) != s_nHlodRuntimeMagic || t3dHlodRuntimeCodecRu16( p + 4 ) != s_nHlodRuntimeVersion ) {
		return false;
	}
	offset = 8u;
	const uint32_t entry_count = t3dHlodRuntimeCodecRu32( p + offset );
	offset += 4u;
	const uint32_t root_count = t3dHlodRuntimeCodecRu32( p + offset );
	offset += 4u;
	const uint32_t mesh_bytes = t3dHlodRuntimeCodecRu32( p + offset );
	offset += 4u;
	const uint32_t node_count = t3dHlodRuntimeCodecRu32( p + offset );
	offset += 4u;
	if ( entry_count > 65535u || root_count > entry_count || mesh_bytes > ( uint32_t )( s_nRegionSlots * s_nMaxMeshAssets )
	        || node_count > 1000000u ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_entries, entry_count ) ) {
		return false;
	}
	for ( uint32_t i = 0u; i < entry_count; ++i ) {
		if ( !t3dHlodRuntimeCodecHasBytes( offset, 104u, ( size_t )rBlob.size() ) ) {
			return false;
		}
		t3dHlodRuntimeEntryRecord_t &entry = rData.m_entries[i];
		entry.m_nLevel = t3dHlodRuntimeCodecRu8( p + offset );
		offset += 4u;
		entry.setCluster( Vector2i( t3dHlodRuntimeCodecRi32( p + offset ), t3dHlodRuntimeCodecRi32( p + offset + 4u ) ) );
		offset += 8u;
		entry.setRegionMin( Vector2i( t3dHlodRuntimeCodecRi32( p + offset ), t3dHlodRuntimeCodecRi32( p + offset + 4u ) ) );
		offset += 8u;
		entry.setRegionMax( Vector2i( t3dHlodRuntimeCodecRi32( p + offset ), t3dHlodRuntimeCodecRi32( p + offset + 4u ) ) );
		offset += 8u;
		entry.m_localBounds = AABB( Vector3( t3dHlodRuntimeCodecRf32( p + offset ),
		                                    t3dHlodRuntimeCodecRf32( p + offset + 4u ),
		                                    t3dHlodRuntimeCodecRf32( p + offset + 8u ) ),
		        Vector3( t3dHlodRuntimeCodecRf32( p + offset + 12u ),
		                t3dHlodRuntimeCodecRf32( p + offset + 16u ),
		                t3dHlodRuntimeCodecRf32( p + offset + 20u ) ) );
		offset += 24u;
		entry.m_fGeometricError = t3dHlodRuntimeCodecRf32( p + offset );
		offset += 4u;
		entry.m_nSourceSignature = t3dHlodRuntimeCodecRi64( p + offset );
		offset += 8u;
		entry.m_nBuildSignature = t3dHlodRuntimeCodecRi64( p + offset );
		offset += 8u;
		entry.m_nEstimatedGpuBytes = t3dHlodRuntimeCodecRi64( p + offset );
		offset += 8u;
		entry.m_nParent = t3dHlodRuntimeCodecRi32( p + offset );
		offset += 4u;
		for ( int child = 0; child < 4; ++child ) {
			entry.m_szChildren[child] = t3dHlodRuntimeCodecRi32( p + offset );
			offset += 4u;
		}
		if ( !t3dHlodRuntimeCodecHasBytes( offset, 4u, ( size_t )rBlob.size() ) ) {
			return false;
		}
		const uint32_t len = t3dHlodRuntimeCodecRu32( p + offset );
		offset += 4u;
		if ( !t3dHlodRuntimeCodecHasBytes( offset, len, ( size_t )rBlob.size() ) ) {
			return false;
		}
		entry.m_packagePath = String::utf8( ( const char * )( p + offset ), ( int )len );
		offset += len;
		if ( entry.m_nLevel < 1 || entry.m_nLevel > 6 || entry.m_packagePath.is_empty() || entry.m_nParent < -1
		        || entry.m_nParent >= ( int )entry_count ) {
			return false;
		}
		for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( entry.m_szChildren ); iChild++ ) {
			int child = ( entry.m_szChildren )[iChild];
			if ( child < -1 || child >= ( int )entry_count ) {
				return false;
			}
		}
	}
	if ( !t3dHlodRuntimeCodecHasBytes( offset, ( size_t )root_count * 4u, ( size_t )rBlob.size() ) ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_roots, root_count ) ) {
		return false;
	}
	for ( uint32_t i = 0u; i < root_count; ++i ) {
		const int root = ( int )t3dHlodRuntimeCodecRu32( p + offset );
		offset += 4u;
		if ( root < 0 || root >= ( int )entry_count ) {
			return false;
		}
		rData.m_roots[i] = root;
	}
	if ( !t3dHlodRuntimeCodecHasBytes( offset, mesh_bytes, ( size_t )rBlob.size() ) ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_instancerMinLevels, mesh_bytes ) ) {
		return false;
	}
	if ( mesh_bytes ) {
		memcpy( rData.m_instancerMinLevels.data(), p + offset, mesh_bytes );
	}
	offset += mesh_bytes;
	if ( !t3dHlodRuntimeCodecHasBytes( offset, ( size_t )node_count * 12u, ( size_t )rBlob.size() ) ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_nodePolicies, node_count ) ) {
		return false;
	}
	eastl::fill_n( rData.m_szNodePolicyOffsets, s_nRegionSlots + 1u, 0u );
	uint16_t previous_region = 0u;
	bool have_region = false;
	for ( uint32_t i = 0u; i < node_count; ++i ) {
		const uint16_t region = t3dHlodRuntimeCodecRu16( p + offset );
		const uint8_t level = t3dHlodRuntimeCodecRu8( p + offset + 2u );
		const int64_t spawn = t3dHlodRuntimeCodecRi64( p + offset + 4u );
		offset += 12u;
		if ( region >= s_nRegionSlots || level == 0u || ( have_region && region < previous_region ) ) {
			return false;
		}
		previous_region = region;
		have_region = true;
		rData.m_nodePolicies[i] = { spawn, level };
		++rData.m_szNodePolicyOffsets[( size_t )region + 1u];
	}
	for ( int i = 0; i < s_nRegionSlots; ++i ) {
		rData.m_szNodePolicyOffsets[( size_t )i + 1u] += rData.m_szNodePolicyOffsets[( size_t )i];
	}
	return offset == ( size_t )rBlob.size();
}
