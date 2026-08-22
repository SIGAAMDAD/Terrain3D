#include "hlod/terrain_3d_hlod_manifest.h"

#include "hlod/terrain_3d_hlod_runtime_codec.h"

#include <godot_cpp/core/class_db.hpp>

bool Terrain3DHLODManifest::compileRuntimeBlob( void ) {
	PackedByteArray compiled = Terrain3DHLODRuntimeCodec::encode(
			m_entries, m_instancerMinLevels, m_nodeRegionIndices, m_nodeSpawnIds, m_nodeMinLevels );
	if ( compiled.is_empty() ) {
		return false;
	}
	m_runtimeBlob = compiled;
	emit_changed();
	return true;
}

void Terrain3DHLODManifest::_bind_methods( void ) {
#define BIND_RW( name, setter, getter ) \
	ClassDB::bind_method( D_METHOD( setter, "value" ), &Terrain3DHLODManifest::name ); \
	ClassDB::bind_method( D_METHOD( getter ), &Terrain3DHLODManifest::get##name )
	ClassDB::bind_method( D_METHOD( "set_format", "value" ), &Terrain3DHLODManifest::setFormat );
	ClassDB::bind_method( D_METHOD( "get_format" ), &Terrain3DHLODManifest::getFormat );
	ClassDB::bind_method( D_METHOD( "set_region_map_size", "value" ), &Terrain3DHLODManifest::setRegionMapSize );
	ClassDB::bind_method( D_METHOD( "get_region_map_size" ), &Terrain3DHLODManifest::getRegionMapSize );
	ClassDB::bind_method( D_METHOD( "set_region_size", "value" ), &Terrain3DHLODManifest::setRegionSize );
	ClassDB::bind_method( D_METHOD( "get_region_size" ), &Terrain3DHLODManifest::getRegionSize );
	ClassDB::bind_method( D_METHOD( "set_vertex_spacing", "value" ), &Terrain3DHLODManifest::setVertexSpacing );
	ClassDB::bind_method( D_METHOD( "get_vertex_spacing" ), &Terrain3DHLODManifest::getVertexSpacing );
	ClassDB::bind_method( D_METHOD( "set_max_level", "value" ), &Terrain3DHLODManifest::setMaxLevel );
	ClassDB::bind_method( D_METHOD( "get_max_level" ), &Terrain3DHLODManifest::getMaxLevel );
	ClassDB::bind_method( D_METHOD( "set_build_signature", "value" ), &Terrain3DHLODManifest::setBuildSignature );
	ClassDB::bind_method( D_METHOD( "get_build_signature" ), &Terrain3DHLODManifest::getBuildSignature );
	ClassDB::bind_method( D_METHOD( "set_built_unix", "value" ), &Terrain3DHLODManifest::setBuiltUnix );
	ClassDB::bind_method( D_METHOD( "get_built_unix" ), &Terrain3DHLODManifest::getBuiltUnix );
	ClassDB::bind_method( D_METHOD( "set_entries", "value" ), &Terrain3DHLODManifest::setEntries );
	ClassDB::bind_method( D_METHOD( "get_entries" ), &Terrain3DHLODManifest::getEntries );
	ClassDB::bind_method( D_METHOD( "set_instancer_min_levels", "value" ), &Terrain3DHLODManifest::setInstancerMinLevels );
	ClassDB::bind_method( D_METHOD( "get_instancer_min_levels" ), &Terrain3DHLODManifest::getInstancerMinLevels );
	ClassDB::bind_method( D_METHOD( "set_node_region_indices", "value" ), &Terrain3DHLODManifest::setNodeRegionIndices );
	ClassDB::bind_method( D_METHOD( "get_node_region_indices" ), &Terrain3DHLODManifest::getNodeRegionIndices );
	ClassDB::bind_method( D_METHOD( "set_node_spawn_ids", "value" ), &Terrain3DHLODManifest::setNodeSpawnIds );
	ClassDB::bind_method( D_METHOD( "get_node_spawn_ids" ), &Terrain3DHLODManifest::getNodeSpawnIds );
	ClassDB::bind_method( D_METHOD( "set_node_min_levels", "value" ), &Terrain3DHLODManifest::setNodeMinLevels );
	ClassDB::bind_method( D_METHOD( "get_node_min_levels" ), &Terrain3DHLODManifest::getNodeMinLevels );
	ClassDB::bind_method( D_METHOD( "set_runtime_blob", "value" ), &Terrain3DHLODManifest::setRuntimeBlob );
	ClassDB::bind_method( D_METHOD( "get_runtime_blob" ), &Terrain3DHLODManifest::getRuntimeBlob );
	ClassDB::bind_method( D_METHOD( "compile_runtime_blob" ), &Terrain3DHLODManifest::compileRuntimeBlob );
	ClassDB::bind_method( D_METHOD( "get_entry_count" ), &Terrain3DHLODManifest::getEntryCount );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "format" ), "set_format", "get_format" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_map_size" ), "set_region_map_size", "get_region_map_size" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_size" ), "set_region_size", "get_region_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "vertex_spacing" ), "set_vertex_spacing", "get_vertex_spacing" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "max_level" ), "set_max_level", "get_max_level" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "build_signature" ), "set_build_signature", "get_build_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "built_unix" ), "set_built_unix", "get_built_unix" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "entries" ), "set_entries", "get_entries" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instancer_min_levels" ),
			"set_instancer_min_levels",
			"get_instancer_min_levels" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_INT32_ARRAY, "node_region_indices" ),
			"set_node_region_indices",
			"get_node_region_indices" );
	ADD_PROPERTY(
			PropertyInfo( Variant::PACKED_INT64_ARRAY, "node_spawn_ids" ), "set_node_spawn_ids", "get_node_spawn_ids" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_BYTE_ARRAY, "node_min_levels" ),
			"set_node_min_levels",
			"get_node_min_levels" );
	ADD_PROPERTY(
			PropertyInfo( Variant::PACKED_BYTE_ARRAY, "runtime_blob", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
			"set_runtime_blob",
			"get_runtime_blob" );
}
