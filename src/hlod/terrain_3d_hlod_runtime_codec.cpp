// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_byte_writer.h"
#include "hlod/terrain_3d_hlod_runtime_codec.h"

#include "eastl_config.h"
#include <EASTL/sort.h>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstring>
#include <limits>

namespace {
static constexpr uint32_t HLOD_RUNTIME_MAGIC = 0x32444c48u; // HLD2
static constexpr uint16_t HLOD_RUNTIME_VERSION = 1u;

EA_FORCE_INLINE static int floorDiv( const int value, const int divisor ) { if ( divisor <= 0 ) return 0; int quotient = value / divisor; if ( value < 0 && ( value % divisor ) != 0 ) --quotient; return quotient; }
EA_FORCE_INLINE static uint8_t ru8(const uint8_t*p) { return p[0]; }
EA_FORCE_INLINE static uint16_t ru16(const uint8_t*p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
EA_FORCE_INLINE static uint32_t ru32(const uint8_t*p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
EA_FORCE_INLINE static int32_t ri32(const uint8_t*p) { return (int32_t)ru32(p); }
EA_FORCE_INLINE static uint64_t ru64(const uint8_t*p) { return (uint64_t)ru32(p)|((uint64_t)ru32(p+4)<<32); }
EA_FORCE_INLINE static int64_t ri64(const uint8_t*p) { return (int64_t)ru64(p); }
EA_FORCE_INLINE static float rf32(const uint8_t*p) { uint32_t bits=ru32(p); float v=0; std::memcpy(&v,&bits,4); return v; }

static bool parseEntry( const Variant &rValue, T3DHLODRuntimeEntryRecord &rEntry ) {
	if ( rValue.get_type() != Variant::DICTIONARY ) return false;
	const Dictionary d = rValue;
	if ( !d.has("level") || !d.has("cluster") || !d.has("region_min") || !d.has("region_max") || !d.has("package_path") ) return false;
	rEntry.level = CLAMP( (int)d["level"], 1, 6 );
	rEntry.cluster = d["cluster"]; rEntry.regionMin=d["region_min"]; rEntry.regionMax=d["region_max"];
	rEntry.localBounds = d.get("local_bounds", AABB());
	rEntry.packagePath = String(d["package_path"]);
	rEntry.geometricError = MAX((real_t)d.get("geometric_error",1.0),0.001f);
	rEntry.sourceSignature=(int64_t)d.get("source_signature",0);
	rEntry.buildSignature=(int64_t)d.get("build_signature",0);
	rEntry.estimatedGpuBytes=MAX<int64_t>((int64_t)d.get("estimated_gpu_bytes",0),0);
	return !rEntry.packagePath.is_empty();
}
}

PackedByteArray T3DHLODRuntimeCodec::encode(
	const Array &rEntries, const PackedByteArray &rInstancerMinLevels,
	const PackedInt32Array &rNodeRegionIndices, const PackedInt64Array &rNodeSpawnIds,
	const PackedByteArray &rNodeMinLevels
) {
	PackedByteArray out;
	eastl::vector<T3DHLODRuntimeEntryRecord> entries; entries.reserve( ( size_t ) rEntries.size() );
	for ( int i = 0; i < rEntries.size(); ++i ) { T3DHLODRuntimeEntryRecord entry; if ( !parseEntry( rEntries[i], entry ) ) return PackedByteArray(); entries.push_back( entry ); }
	for ( int i = 0; i < ( int ) entries.size(); ++i ) {
		auto &entry = entries[( size_t ) i]; const Vector2i parent_cluster( floorDiv( entry.cluster.x, 2 ), floorDiv( entry.cluster.y, 2 ) ); int parent = -1;
		for ( int j = 0; j < ( int ) entries.size(); ++j ) if ( entries[( size_t ) j].level == entry.level + 1 && entries[( size_t ) j].cluster == parent_cluster ) { parent = j; break; }
		entry.parent = parent; if ( parent >= 0 ) { const int local_x = entry.cluster.x - parent_cluster.x * 2, local_y = entry.cluster.y - parent_cluster.y * 2; entries[( size_t ) parent].children[CLAMP( local_y, 0, 1 ) * 2 + CLAMP( local_x, 0, 1 )] = i; }
	}
	eastl::vector<int> roots; roots.reserve( entries.size() ); for ( int i = 0; i < ( int ) entries.size(); ++i ) if ( entries[( size_t ) i].parent < 0 ) roots.push_back( i );
	struct NodeTmp { uint16_t region; int64_t spawn; uint8_t level; }; eastl::vector<NodeTmp> nodes;
	const int node_count = MIN( rNodeRegionIndices.size(), MIN( rNodeSpawnIds.size(), rNodeMinLevels.size() ) ); nodes.reserve( ( size_t ) node_count );
	for ( int i = 0; i < node_count; ++i ) { const int region = rNodeRegionIndices[i]; const uint8_t level = rNodeMinLevels[i]; if ( region >= 0 && region < TERRAIN3D_MAX_REGION_SLOTS && level > 0 ) nodes.push_back( { ( uint16_t ) region, rNodeSpawnIds[i], level } ); }
	eastl::sort( nodes.begin(), nodes.end(), []( const NodeTmp &a, const NodeTmp &b ) { return a.region < b.region || ( a.region == b.region && a.spawn < b.spawn ); } );

	eastl::vector<CharString> paths; paths.resize( entries.size() ); size_t total_bytes = 24u + roots.size() * 4u + ( size_t ) rInstancerMinLevels.size() + nodes.size() * 12u + entries.size() * 104u;
	for ( size_t i = 0; i < entries.size(); ++i ) { paths[i] = entries[i].packagePath.utf8(); total_bytes += ( size_t ) MAX( paths[i].length(), 0 ); }
	if ( total_bytes > ( size_t ) INT64_MAX ) return PackedByteArray(); out.resize( ( int64_t ) total_bytes ); if ( ( size_t ) out.size() != total_bytes ) return PackedByteArray();
	T3DByteWriter writer( out.ptrw(), total_bytes ); writer.writeU32( HLOD_RUNTIME_MAGIC ); writer.writeU16( HLOD_RUNTIME_VERSION ); writer.writeU16( 0u ); writer.writeU32( ( uint32_t ) entries.size() ); writer.writeU32( ( uint32_t ) roots.size() ); writer.writeU32( ( uint32_t ) rInstancerMinLevels.size() ); writer.writeU32( ( uint32_t ) nodes.size() );
	for ( size_t i = 0; i < entries.size(); ++i ) {
		const auto &entry = entries[i]; writer.writeU8( ( uint8_t ) entry.level ); writer.writeU8( 0u ); writer.writeU16( 0u ); writer.writeI32( entry.cluster.x ); writer.writeI32( entry.cluster.y ); writer.writeI32( entry.regionMin.x ); writer.writeI32( entry.regionMin.y ); writer.writeI32( entry.regionMax.x ); writer.writeI32( entry.regionMax.y );
		writer.writeF32( entry.localBounds.position.x ); writer.writeF32( entry.localBounds.position.y ); writer.writeF32( entry.localBounds.position.z ); writer.writeF32( entry.localBounds.size.x ); writer.writeF32( entry.localBounds.size.y ); writer.writeF32( entry.localBounds.size.z ); writer.writeF32( ( float ) entry.geometricError );
		writer.writeI64( entry.sourceSignature ); writer.writeI64( entry.buildSignature ); writer.writeI64( entry.estimatedGpuBytes ); writer.writeI32( entry.parent ); for ( int child : entry.children ) writer.writeI32( child ); const CharString &path = paths[i]; const uint32_t len = ( uint32_t ) MAX( path.length(), 0 ); writer.writeU32( len ); writer.writeBytes( path.get_data(), len );
	}
	for ( int root : roots ) writer.writeU32( ( uint32_t ) root ); writer.writeBytes( rInstancerMinLevels.ptr(), ( size_t ) rInstancerMinLevels.size() );
	for ( const auto &node : nodes ) { writer.writeU16( node.region ); writer.writeU8( node.level ); writer.writeU8( 0u ); writer.writeI64( node.spawn ); }
	return writer.complete() ? out : PackedByteArray();
}

bool T3DHLODRuntimeCodec::decode( const PackedByteArray &rBlob, T3DHLODRuntimeData &rData ) {
	rData = T3DHLODRuntimeData{}; if ( rBlob.size() < 24 ) return false; const uint8_t *p = rBlob.ptr(); size_t offset = 0u; auto need = [&]( const size_t nBytes ) { return offset <= ( size_t ) rBlob.size() && nBytes <= ( size_t ) rBlob.size() - offset; };
	if ( ru32( p ) != HLOD_RUNTIME_MAGIC || ru16( p + 4 ) != HLOD_RUNTIME_VERSION ) return false; offset = 8u;
	const uint32_t entry_count = ru32( p + offset ); offset += 4u; const uint32_t root_count = ru32( p + offset ); offset += 4u; const uint32_t mesh_bytes = ru32( p + offset ); offset += 4u; const uint32_t node_count = ru32( p + offset ); offset += 4u;
	if ( entry_count > 65535u || root_count > entry_count || mesh_bytes > ( uint32_t ) ( TERRAIN3D_MAX_REGION_SLOTS * TERRAIN3D_MAX_MESH_ASSETS ) || node_count > 1000000u ) return false;
	rData.entries.resize( entry_count );
	for ( uint32_t i = 0u; i < entry_count; ++i ) {
		if ( !need( 104u ) ) return false; T3DHLODRuntimeEntryRecord &entry = rData.entries[i]; entry.level = ru8( p + offset ); offset += 4u; entry.cluster = Vector2i( ri32( p + offset ), ri32( p + offset + 4u ) ); offset += 8u; entry.regionMin = Vector2i( ri32( p + offset ), ri32( p + offset + 4u ) ); offset += 8u; entry.regionMax = Vector2i( ri32( p + offset ), ri32( p + offset + 4u ) ); offset += 8u;
		entry.localBounds = AABB( Vector3( rf32( p + offset ), rf32( p + offset + 4u ), rf32( p + offset + 8u ) ), Vector3( rf32( p + offset + 12u ), rf32( p + offset + 16u ), rf32( p + offset + 20u ) ) ); offset += 24u; entry.geometricError = rf32( p + offset ); offset += 4u; entry.sourceSignature = ri64( p + offset ); offset += 8u; entry.buildSignature = ri64( p + offset ); offset += 8u; entry.estimatedGpuBytes = ri64( p + offset ); offset += 8u; entry.parent = ri32( p + offset ); offset += 4u; for ( int child = 0; child < 4; ++child ) { entry.children[child] = ri32( p + offset ); offset += 4u; }
		if ( !need( 4u ) ) return false; const uint32_t len = ru32( p + offset ); offset += 4u; if ( !need( len ) ) return false; entry.packagePath = String::utf8( ( const char * ) ( p + offset ), ( int ) len ); offset += len;
		if ( entry.level < 1 || entry.level > 6 || entry.packagePath.is_empty() || entry.parent < -1 || entry.parent >= ( int ) entry_count ) return false; for ( int child : entry.children ) if ( child < -1 || child >= ( int ) entry_count ) return false;
	}
	if ( !need( ( size_t ) root_count * 4u ) ) return false; rData.roots.resize( root_count ); for ( uint32_t i = 0u; i < root_count; ++i ) { const int root = ( int ) ru32( p + offset ); offset += 4u; if ( root < 0 || root >= ( int ) entry_count ) return false; rData.roots[i] = root; }
	if ( !need( mesh_bytes ) ) return false; rData.instancerMinLevels.resize( mesh_bytes ); if ( mesh_bytes ) std::memcpy( rData.instancerMinLevels.data(), p + offset, mesh_bytes ); offset += mesh_bytes;
	if ( !need( ( size_t ) node_count * 12u ) ) return false; rData.nodePolicies.resize( node_count ); rData.nodePolicyOffsets.fill( 0u ); uint16_t previous_region = 0u; bool have_region = false;
	for ( uint32_t i = 0u; i < node_count; ++i ) { const uint16_t region = ru16( p + offset ); const uint8_t level = ru8( p + offset + 2u ); const int64_t spawn = ri64( p + offset + 4u ); offset += 12u; if ( region >= TERRAIN3D_MAX_REGION_SLOTS || level == 0u || ( have_region && region < previous_region ) ) return false; previous_region = region; have_region = true; rData.nodePolicies[i] = { spawn, level }; ++rData.nodePolicyOffsets[( size_t ) region + 1u]; }
	for ( int i = 0; i < TERRAIN3D_MAX_REGION_SLOTS; ++i ) rData.nodePolicyOffsets[( size_t ) i + 1u] += rData.nodePolicyOffsets[( size_t ) i]; return offset == ( size_t ) rBlob.size();
}
