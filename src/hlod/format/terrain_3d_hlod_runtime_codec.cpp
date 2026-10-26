// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_numeric.h"
#include "core/utility/terrain_3d_checked_math.h"
#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"
#include "hlod/format/terrain_3d_hlod_runtime_codec.h"
#include "hlod/format/terrain_3d_hlod_cell_index.h"

#include "core/utility/terrain_3d_binary_io.h"
#include "core/config/terrain_3d_compiler.h"
#include "eastl_config.h"

#include <EASTL/sort.h>
#include <string.h>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

// t3dHLODRuntimeNodeTemp_s centralized in Terrain3DTypes.h.

static bool t3dHlodRuntimeNodeTempLess(
	const t3dHLODRuntimeNodeTemp_t &rA,
	const t3dHLODRuntimeNodeTemp_t &rB ) {
	return rA.m_nRegion < rB.m_nRegion || ( rA.m_nRegion == rB.m_nRegion && rA.m_nSpawn < rB.m_nSpawn );
}

// Cold-only validator for the fixed Terrain3D region grid. A damaged blob
// must not publish a cell whose region footprint differs from its packed key.
// Higher level cells may extend beyond the final map edge, but the origin
// always belongs to the fixed region grid.
static bool t3dHlodEntrySpatialValid( const t3dHLODEntryDescriptor_t &rEntry )
{
    const int level = rEntry.m_nLevel;
    if ( level < -1 || level > 6 ) { return false; }
    const int span = level < 0 ? 1 : 1 << level;
    const Vector2i cell = rEntry.getCluster();
    const int scale = level < 0 ? 2 : 1;
    if ( cell.x < 0 || cell.y < 0 || cell.x / scale * span >= T3D_REGION_MAP_SIZE ||
         cell.y / scale * span >= T3D_REGION_MAP_SIZE ) { return false; }
    const Vector2i expectedMin( -T3D_REGION_MAP_HALF + ( cell.x / scale ) * span,
                                -T3D_REGION_MAP_HALF + ( cell.y / scale ) * span );
    const Vector2i expectedMax = expectedMin + Vector2i( span - 1, span - 1 );
    return rEntry.getRegionMin() == expectedMin && rEntry.getRegionMax() == expectedMax;
}

bool Terrain3DHLODRuntimeCodec::decodeEntryDictionary(
	const Variant			 &rValue,
	t3dHLODEntryDescriptor_t &rEntry,
	const bool				  bRequireNonEmptyPackagePath ) {
	if ( rValue.get_type() != Variant::DICTIONARY ) {
		return false;
	}
	const Dictionary d = rValue;
	if ( !d.has( "level" ) || !d.has( "cluster" ) || !d.has( "region_min" ) || !d.has( "region_max" ) ||
		 !d.has( "package_path" ) ) {
		return false;
	}
	rEntry.m_nLevel = CLAMP( ( int ) d["level"], -1, 6 );
	rEntry.setCluster( d["cluster"] );
	rEntry.setRegionMin( d["region_min"] );
	rEntry.setRegionMax( d["region_max"] );
	rEntry.m_localBounds = d.get( "local_bounds", AABB() );
	rEntry.m_packagePath = String( d["package_path"] );
	rEntry.m_fGeometricError = MAX( ( real_t ) d.get( "geometric_error", 1.0 ), 0.001f );
	rEntry.m_nSourceSignature = ( int64_t ) d.get( "source_signature", 0 );
	rEntry.m_nBuildSignature = ( int64_t ) d.get( "build_signature", 0 );
	rEntry.m_nEstimatedGpuBytes = MAX< int64_t >( ( int64_t ) d.get( "estimated_gpu_bytes", 0 ), 0 );
	return !bRequireNonEmptyPackagePath || !rEntry.m_packagePath.is_empty();
}

PackedByteArray Terrain3DHLODRuntimeCodec::encode(
	const Array			   &rEntries,
	const PackedByteArray  &rInstancerMinLevels,
	const PackedInt32Array &rNodeRegionIndices,
	const PackedInt64Array &rNodeSpawnIds,
	const PackedByteArray  &rNodeMinLevels,
    const PackedByteArray  &rInstancerSubcellMasks,
    const PackedByteArray  &rNodeSubcellMasks ) {
	PackedByteArray													   out;
	eastl::vector< t3dHLODEntryDescriptor_t, Terrain3DEastlAllocator > entries;
	entries.reserve( ( size_t ) rEntries.size() );
	for ( int i = 0; i < rEntries.size(); ++i ) {
		t3dHLODEntryDescriptor_t entry;
		if ( !decodeEntryDictionary( rEntries[i], entry ) || !t3dHlodEntrySpatialValid( entry ) ) {
			return PackedByteArray();
		}
		entries.push_back( entry );
	}
	eastl::vector< t3dHlodCellIndex_t, Terrain3DEastlAllocator > lookup;
	if ( !t3dBuildHlodCellIndex( entries, lookup ) ) {
		return PackedByteArray();
	}
	for ( int i = 0; i < ( int ) entries.size(); ++i ) {
		t3dHLODEntryDescriptor_t &entry = entries[( size_t ) i];
		const Vector2i parent_cluster( t3dDivideFloor( entry.getCluster().x, 2 ),
											  t3dDivideFloor( entry.getCluster().y, 2 ) );
		const int parent = t3dFindHlodCellIndex( lookup, entry.m_nLevel + 1,
											  parent_cluster.x, parent_cluster.y );
		entry.m_nParent = parent;
		if ( parent >= 0 ) {
			const int local_x = entry.getCluster().x - parent_cluster.x * 2,
					  local_y = entry.getCluster().y - parent_cluster.y * 2;
			entries[( size_t ) parent].m_szChildren[CLAMP( local_y, 0, 1 ) * 2 + CLAMP( local_x, 0, 1 )] = i;
		}
	}
	eastl::vector< int, Terrain3DEastlAllocator > roots;
	roots.reserve( entries.size() );
	for ( int i = 0; i < ( int ) entries.size(); ++i ) {
		if ( entries[( size_t ) i].m_nParent < 0 ) {
			roots.push_back( i );
		}
	}
	eastl::vector< t3dHLODRuntimeNodeTemp_t, Terrain3DEastlAllocator > nodes;
	const int node_count = MIN( rNodeRegionIndices.size(), MIN( rNodeSpawnIds.size(), rNodeMinLevels.size() ) );
	nodes.reserve( ( size_t ) node_count );
	for ( int i = 0; i < node_count; ++i ) {
		const int	  region = rNodeRegionIndices[i];
		const uint8_t level = rNodeMinLevels[i];
		if ( region >= 0 && region < T3D_REGION_SLOTS && level > 0 ) {
			nodes.push_back( { ( uint16_t ) region, rNodeSpawnIds[i], level,
                ( uint8_t ) ( i < rNodeSubcellMasks.size() ? rNodeSubcellMasks[i] & 0x0fu : 0u ) } );
		}
	}
	eastl::sort( nodes.begin(), nodes.end(), t3dHlodRuntimeNodeTempLess );
    // No partial policy table: an absent or truncated mask silently loses
    // independently suppressed geometry. V5 requires an exact parallel array.
    if ( rInstancerSubcellMasks.size() != 0 &&
         rInstancerSubcellMasks.size() != rInstancerMinLevels.size() ) { return PackedByteArray(); }
    if ( rNodeSubcellMasks.size() != 0 &&
         rNodeSubcellMasks.size() != rNodeMinLevels.size() ) { return PackedByteArray(); }
    const size_t mask_bytes = ( size_t ) rInstancerSubcellMasks.size();

	eastl::vector< CharString, Terrain3DEastlAllocator > paths;
	paths.resize( entries.size() );
	size_t total_bytes =
		28u + roots.size() * 4u + ( size_t ) rInstancerMinLevels.size() + mask_bytes + nodes.size() * 12u + entries.size() * 104u;
	for ( size_t i = 0; i < entries.size(); ++i ) {
		paths[i] = entries[i].m_packagePath.utf8();
		total_bytes += ( size_t ) MAX( paths[i].length(), 0 );
	}
	if ( total_bytes > ( size_t ) INT64_MAX ) {
		return PackedByteArray();
	}
	out.resize( ( int64_t ) total_bytes );
	if ( ( size_t ) out.size() != total_bytes ) {
		return PackedByteArray();
	}
	t3dByteWriter_t writer( out.ptrw(), total_bytes );
	writer.writeU32( T3D_HLOD_RUNTIME_MAGIC );
	writer.writeU16( T3D_HLOD_RUNTIME_VERSION );
	writer.writeU16( 0u );
	writer.writeU32( ( uint32_t ) entries.size() );
	writer.writeU32( ( uint32_t ) roots.size() );
	writer.writeU32( ( uint32_t ) rInstancerMinLevels.size() );
	writer.writeU32( ( uint32_t ) nodes.size() );
	writer.writeU32( ( uint32_t ) mask_bytes );
	for ( size_t i = 0; i < entries.size(); ++i ) {
		const t3dHLODEntryDescriptor_t &entry = entries[i];
		writer.writeU8( ( uint8_t ) entry.m_nLevel );
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
		writer.writeF32( ( float ) entry.m_fGeometricError );
		writer.writeI64( entry.m_nSourceSignature );
		writer.writeI64( entry.m_nBuildSignature );
		writer.writeI64( entry.m_nEstimatedGpuBytes );
		writer.writeI32( entry.m_nParent );
		for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( entry.m_szChildren ); iChild++ ) {
			int child = ( entry.m_szChildren )[iChild];
			writer.writeI32( child );
		}
		const CharString &path = paths[i];
		const uint32_t	  len = ( uint32_t ) MAX( path.length(), 0 );
		writer.writeU32( len );
		writer.writeBytes( path.get_data(), len );
	}
	for ( size_t iRoot = 0u; iRoot < ( size_t ) ( ( roots ).size() ); iRoot++ ) {
		int root = ( roots )[iRoot];
		writer.writeU32( ( uint32_t ) root );
	}
	writer.writeBytes( rInstancerMinLevels.ptr(), ( size_t ) rInstancerMinLevels.size() );
    if ( mask_bytes ) { writer.writeBytes( rInstancerSubcellMasks.ptr(), mask_bytes ); }
	for ( size_t iNode = 0u; iNode < ( size_t ) ( ( nodes ).size() ); iNode++ ) {
		const t3dHLODRuntimeNodeTemp_t &node = ( nodes )[iNode];
		writer.writeU16( node.m_nRegion );
		writer.writeU8( node.m_nLevel );
		writer.writeU8( node.m_nSubcellMask );
		writer.writeI64( node.m_nSpawn );
	}
	return writer.complete() ? out : PackedByteArray();
}

bool Terrain3DHLODRuntimeCodec::decode(
	const PackedByteArray &rBlob,
	t3dHLODRuntimeData_t  &rData ) {
	rData = t3dHLODRuntimeData_t{};
	if ( rBlob.size() < 24 ) {
		return false;
	}
	const uint8_t  *p = rBlob.ptr();
	t3dByteReader_t reader( p, ( size_t ) rBlob.size() );
	size_t		   &offset = reader.m_iOffset;
	offset = 0u;
	const uint16_t wire_version = t3dReadLittleUnsigned< uint16_t >( p + 4 );
	if ( t3dReadLittleUnsigned< uint32_t >( p ) != T3D_HLOD_RUNTIME_MAGIC ||
		( wire_version != 2u && wire_version != 3u && wire_version != 4u && wire_version != T3D_HLOD_RUNTIME_VERSION ) ) {
		return false;
	}
	offset = 8u;
	const uint32_t entry_count = reader.readU32();
	const uint32_t root_count = reader.readU32();
	const uint32_t mesh_bytes = reader.readU32();
	const uint32_t node_count = reader.readU32();
	const uint32_t mask_bytes = wire_version >= 5u ? reader.readU32() : 0u;
	if ( entry_count > 65535u || root_count > entry_count ||
		 mesh_bytes > ( uint32_t ) ( T3D_REGION_SLOTS * T3D_MAX_MESH_ASSETS ) ||
         mask_bytes > mesh_bytes || ( mask_bytes > 0u && mask_bytes != mesh_bytes ) ||
         node_count > 1000000u ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_entries, entry_count ) ) {
		return false;
	}
	for ( uint32_t i = 0u; i < entry_count; ++i ) {
		if ( !t3dByteRangeFits< size_t >( offset, 104u, ( size_t ) rBlob.size() ) ) {
			return false;
		}
		t3dHLODEntryDescriptor_t &entry = rData.m_entries[i];
		entry.m_nLevel = t3dReadLittleUnsigned< uint8_t >( p + offset );
        if ( wire_version >= 4u && entry.m_nLevel == 255 ) { entry.m_nLevel = -1; }
		offset += 4u;
		entry.setCluster( Vector2i( ( int32_t ) t3dReadLittleUnsigned< uint32_t >( p + offset ),
									( int32_t ) t3dReadLittleUnsigned< uint32_t >( p + offset + 4u ) ) );
		offset += 8u;
		entry.setRegionMin( Vector2i( ( int32_t ) t3dReadLittleUnsigned< uint32_t >( p + offset ),
									  ( int32_t ) t3dReadLittleUnsigned< uint32_t >( p + offset + 4u ) ) );
		offset += 8u;
		entry.setRegionMax( Vector2i( ( int32_t ) t3dReadLittleUnsigned< uint32_t >( p + offset ),
									  ( int32_t ) t3dReadLittleUnsigned< uint32_t >( p + offset + 4u ) ) );
		offset += 8u;
		entry.m_localBounds = AABB( Vector3( t3dReadLittleF32( p + offset ),
											 t3dReadLittleF32( p + offset + 4u ),
											 t3dReadLittleF32( p + offset + 8u ) ),
									Vector3( t3dReadLittleF32( p + offset + 12u ),
											 t3dReadLittleF32( p + offset + 16u ),
											 t3dReadLittleF32( p + offset + 20u ) ) );
		offset += 24u;
		entry.m_fGeometricError = reader.readF32();
		entry.m_nSourceSignature = ( int64_t ) reader.readU64();
		entry.m_nBuildSignature = ( int64_t ) reader.readU64();
		entry.m_nEstimatedGpuBytes = ( int64_t ) reader.readU64();
		entry.m_nParent = ( int32_t ) reader.readU32();
		for ( int child = 0; child < 4; ++child ) {
			entry.m_szChildren[child] = ( int32_t ) reader.readU32();
		}
		if ( !t3dByteRangeFits< size_t >( offset, 4u, ( size_t ) rBlob.size() ) ) {
			return false;
		}
		const uint32_t len = reader.readU32();
		if ( !t3dByteRangeFits< size_t >( offset, len, ( size_t ) rBlob.size() ) ) {
			return false;
		}
		entry.m_packagePath = String::utf8( ( const char * ) ( p + offset ), ( int ) len );
		offset += len;
		if ( entry.m_nLevel < ( wire_version >= 4u ? -1 : ( wire_version >= 3u ? 0 : 1 ) ) || entry.m_nLevel > 6 || entry.m_packagePath.is_empty() || entry.m_nParent < -1 ||
			 entry.m_nParent >= ( int ) entry_count ||
             !t3dHlodEntrySpatialValid( entry ) ) {
			return false;
		}
		for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( entry.m_szChildren ); iChild++ ) {
			int child = ( entry.m_szChildren )[iChild];
			if ( child < -1 || child >= ( int ) entry_count ) {
				return false;
			}
		}
	}
	if ( !t3dByteRangeFits< size_t >( offset, ( size_t ) root_count * 4u, ( size_t ) rBlob.size() ) ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_roots, root_count ) ) {
		return false;
	}
	for ( uint32_t i = 0u; i < root_count; ++i ) {
		const int root = ( int ) reader.readU32();
		if ( root < 0 || root >= ( int ) entry_count ) {
			return false;
		}
		rData.m_roots[i] = root;
	}
	eastl::vector< t3dHlodCellIndex_t, Terrain3DEastlAllocator > unique_cells;
	if ( !t3dBuildHlodCellIndex( rData.m_entries, unique_cells ) ) {
		return false;
	}
	// Cold-load topology verification. A spatially split hierarchy has many
	// independent roots; every root and every surviving child link must agree
	// with the encoded parent relationship before it reaches the streamer.
	// Keep this check in the codec so a damaged persisted manifest cannot
	// silently suppress detailed geometry or create orphaned proxy entries.
	eastl::vector< uint8_t, Terrain3DEastlAllocator > root_seen;
	if ( !t3dVectorResizeCold( root_seen, entry_count ) ) {
		return false;
	}
	eastl::fill( root_seen.begin(), root_seen.end(), ( uint8_t ) 0u );
	for ( uint32_t iRoot = 0u; iRoot < root_count; ++iRoot ) {
		const int root = rData.m_roots[iRoot];
		if ( rData.m_entries[( size_t ) root].m_nParent != -1 || root_seen[( size_t ) root] != 0u ) {
			return false;
		}
		root_seen[( size_t ) root] = 1u;
	}
	uint32_t expected_roots = 0u;
	for ( uint32_t i = 0u; i < entry_count; ++i ) {
		const t3dHLODEntryDescriptor_t &entry = rData.m_entries[i];
		if ( entry.m_nParent < 0 ) {
			++expected_roots;
			if ( root_seen[i] == 0u ) {
				return false;
			}
		} else {
			const t3dHLODEntryDescriptor_t &parent = rData.m_entries[( size_t ) entry.m_nParent];
			const Vector2i parent_cluster( t3dDivideFloor( entry.getCluster().x, 2 ),
											   t3dDivideFloor( entry.getCluster().y, 2 ) );
			const int local_x = entry.getCluster().x - parent_cluster.x * 2;
			const int local_y = entry.getCluster().y - parent_cluster.y * 2;
			if ( parent.m_nLevel != entry.m_nLevel + 1 || parent.getCluster() != parent_cluster ||
				 local_x < 0 || local_x > 1 || local_y < 0 || local_y > 1 ||
				 parent.m_szChildren[local_y * 2 + local_x] != ( int ) i ) {
				return false;
			}
		}
		for ( int slot = 0; slot < 4; ++slot ) {
			const int child_index = entry.m_szChildren[slot];
			if ( child_index < 0 ) {
				continue;
			}
			const t3dHLODEntryDescriptor_t &child = rData.m_entries[( size_t ) child_index];
			const Vector2i child_cluster = child.getCluster();
			const Vector2i child_parent( t3dDivideFloor( child_cluster.x, 2 ),
											 t3dDivideFloor( child_cluster.y, 2 ) );
			const int child_slot = ( child_cluster.y - child_parent.y * 2 ) * 2 +
									  ( child_cluster.x - child_parent.x * 2 );
			if ( child.m_nParent != ( int ) i || child.m_nLevel + 1 != entry.m_nLevel ||
				 child_parent != entry.getCluster() || child_slot != slot ) {
				return false;
			}
		}
	}
	if ( expected_roots != root_count ) {
		return false;
	}
	if ( !t3dByteRangeFits< size_t >( offset, mesh_bytes, ( size_t ) rBlob.size() ) ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_instancerMinLevels, mesh_bytes ) ) {
		return false;
	}
	if ( mesh_bytes ) {
		memcpy( rData.m_instancerMinLevels.data(), p + offset, mesh_bytes );
	}
	offset += mesh_bytes;
    if ( !t3dByteRangeFits< size_t >( offset, mask_bytes, ( size_t ) rBlob.size() ) ||
         !t3dVectorResizeCold( rData.m_instancerSubcellMasks, mask_bytes ) ) { return false; }
    if ( mask_bytes ) {
        memcpy( rData.m_instancerSubcellMasks.data(), p + offset, mask_bytes );
        for ( uint32_t i = 0u; i < mask_bytes; ++i ) {
            if ( ( rData.m_instancerSubcellMasks[i] & 0xf0u ) != 0u ||
                 ( rData.m_instancerSubcellMasks[i] != 0u &&
                   ( i >= rData.m_instancerMinLevels.size() || rData.m_instancerMinLevels[i] != 1u ) ) ) { return false; }
        }
    }
    offset += mask_bytes;
	if ( !t3dByteRangeFits< size_t >( offset, ( size_t ) node_count * 12u, ( size_t ) rBlob.size() ) ) {
		return false;
	}
	if ( !t3dVectorResizeCold( rData.m_nodePolicies, node_count ) ) {
		return false;
	}
	eastl::fill_n( rData.m_szNodePolicyOffsets, T3D_REGION_SLOTS + 1u, 0u );
	uint16_t previous_region = 0u;
	bool	 have_region = false;
	for ( uint32_t i = 0u; i < node_count; ++i ) {
		const uint16_t region = t3dReadLittleUnsigned< uint16_t >( p + offset );
		const uint8_t  level = t3dReadLittleUnsigned< uint8_t >( p + offset + 2u );
        const uint8_t mask = wire_version >= 5u ? p[offset + 3u] : 0u;
		const int64_t  spawn = ( int64_t ) t3dReadLittleUnsigned< uint64_t >( p + offset + 4u );
		offset += 12u;
		if ( region >= T3D_REGION_SLOTS || level == 0u || ( mask & 0xf0u ) != 0u ||
             ( mask != 0u && level != 1u ) || ( have_region && region < previous_region ) ) {
			return false;
		}
		previous_region = region;
		have_region = true;
		rData.m_nodePolicies[i] = { spawn, level, mask };
		++rData.m_szNodePolicyOffsets[( size_t ) region + 1u];
	}
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		rData.m_szNodePolicyOffsets[( size_t ) i + 1u] += rData.m_szNodePolicyOffsets[( size_t ) i];
	}
    // Every independently suppressible source must have an actual cooked leaf.
    // A corrupt or stale mask must never authorize hiding detailed geometry.
    uint8_t leafMask[T3D_REGION_SLOTS] = {};
    for ( size_t i = 0u; i < rData.m_entries.size(); ++i ) {
        const t3dHLODEntryDescriptor_t &entry = rData.m_entries[i];
        if ( entry.m_nLevel != -1 ) { continue; }
        const Vector2i region = entry.getRegionMin();
        const int rx = region.x + T3D_REGION_MAP_HALF;
        const int rz = region.y + T3D_REGION_MAP_HALF;
        if ( rx < 0 || rx >= T3D_REGION_MAP_SIZE || rz < 0 || rz >= T3D_REGION_MAP_SIZE ) {
            return false;
        }
        const Vector2i cell = entry.getCluster();
        leafMask[rz * T3D_REGION_MAP_SIZE + rx] |=
            (uint8_t)( 1u << ( (cell.y & 1) * 2 + (cell.x & 1) ) );
    }
    for ( size_t i = 0u; i < rData.m_instancerSubcellMasks.size(); ++i ) {
        const uint8_t mask = rData.m_instancerSubcellMasks[i];
        if ( ( mask & (uint8_t)~leafMask[i / T3D_MAX_MESH_ASSETS] ) != 0u ) { return false; }
    }
    uint32_t nodeRegion = 0u;
    for ( size_t i = 0u; i < rData.m_nodePolicies.size(); ++i ) {
        while ( nodeRegion + 1u < T3D_REGION_SLOTS &&
                i >= rData.m_szNodePolicyOffsets[nodeRegion + 1u] ) { ++nodeRegion; }
        if ( ( rData.m_nodePolicies[i].m_nSubcellMask & (uint8_t)~leafMask[nodeRegion] ) != 0u ) {
            return false;
        }
    }
	return reader.complete();
}
