#include "hlod/terrain_3d_hlod_manifest.h"
#include <godot_cpp/core/class_db.hpp>

#define HLOD_SET(method, field, type, normalized) \
void T3DHLODManifest::method( type value ) { auto v = ( normalized ); if ( field == v ) { return; } field = v; emit_changed(); }
HLOD_SET( setFormat, m_nFormat, int, MAX( value, 1 ) )
HLOD_SET( setRegionMapSize, m_nRegionMapSize, int, MAX( value, 1 ) )
HLOD_SET( setRegionSize, m_nRegionSize, int, MAX( value, 1 ) )
HLOD_SET( setVertexSpacing, m_fVertexSpacing, real_t, MAX( value, 0.001f ) )
HLOD_SET( setMaxLevel, m_nMaxLevel, int, CLAMP( value, 1, 6 ) )
HLOD_SET( setBuildSignature, m_nBuildSignature, int64_t, value )
HLOD_SET( setBuiltUnix, m_nBuiltUnix, int64_t, MAX<int64_t>( value, 0 ) )
void T3DHLODManifest::setEntries( const Array &rValue ) { m_entries = rValue.duplicate( true ); emit_changed(); }
void T3DHLODManifest::setInstancerMinLevels( const PackedByteArray &rValue ) { m_instancerMinLevels = rValue; emit_changed(); }
void T3DHLODManifest::setNodeRegionIndices( const PackedInt32Array &rValue ) { m_nodeRegionIndices = rValue; emit_changed(); }
void T3DHLODManifest::setNodeSpawnIds( const PackedInt64Array &rValue ) { m_nodeSpawnIds = rValue; emit_changed(); }
void T3DHLODManifest::setNodeMinLevels( const PackedByteArray &rValue ) { m_nodeMinLevels = rValue; emit_changed(); }

void T3DHLODManifest::_bind_methods( void ) {
#define BIND_RW(name, setter, getter) ClassDB::bind_method( D_METHOD( setter, "value" ), &T3DHLODManifest::name ); ClassDB::bind_method( D_METHOD( getter ), &T3DHLODManifest::get##name )
	ClassDB::bind_method( D_METHOD( "set_format", "value" ), &T3DHLODManifest::setFormat ); ClassDB::bind_method( D_METHOD( "get_format" ), &T3DHLODManifest::getFormat );
	ClassDB::bind_method( D_METHOD( "set_region_map_size", "value" ), &T3DHLODManifest::setRegionMapSize ); ClassDB::bind_method( D_METHOD( "get_region_map_size" ), &T3DHLODManifest::getRegionMapSize );
	ClassDB::bind_method( D_METHOD( "set_region_size", "value" ), &T3DHLODManifest::setRegionSize ); ClassDB::bind_method( D_METHOD( "get_region_size" ), &T3DHLODManifest::getRegionSize );
	ClassDB::bind_method( D_METHOD( "set_vertex_spacing", "value" ), &T3DHLODManifest::setVertexSpacing ); ClassDB::bind_method( D_METHOD( "get_vertex_spacing" ), &T3DHLODManifest::getVertexSpacing );
	ClassDB::bind_method( D_METHOD( "set_max_level", "value" ), &T3DHLODManifest::setMaxLevel ); ClassDB::bind_method( D_METHOD( "get_max_level" ), &T3DHLODManifest::getMaxLevel );
	ClassDB::bind_method( D_METHOD( "set_build_signature", "value" ), &T3DHLODManifest::setBuildSignature ); ClassDB::bind_method( D_METHOD( "get_build_signature" ), &T3DHLODManifest::getBuildSignature );
	ClassDB::bind_method( D_METHOD( "set_built_unix", "value" ), &T3DHLODManifest::setBuiltUnix ); ClassDB::bind_method( D_METHOD( "get_built_unix" ), &T3DHLODManifest::getBuiltUnix );
	ClassDB::bind_method( D_METHOD( "set_entries", "value" ), &T3DHLODManifest::setEntries ); ClassDB::bind_method( D_METHOD( "get_entries" ), &T3DHLODManifest::getEntries );
	ClassDB::bind_method( D_METHOD( "set_instancer_min_levels", "value" ), &T3DHLODManifest::setInstancerMinLevels ); ClassDB::bind_method( D_METHOD( "get_instancer_min_levels" ), &T3DHLODManifest::getInstancerMinLevels );
	ClassDB::bind_method( D_METHOD( "set_node_region_indices", "value" ), &T3DHLODManifest::setNodeRegionIndices ); ClassDB::bind_method( D_METHOD( "get_node_region_indices" ), &T3DHLODManifest::getNodeRegionIndices );
	ClassDB::bind_method( D_METHOD( "set_node_spawn_ids", "value" ), &T3DHLODManifest::setNodeSpawnIds ); ClassDB::bind_method( D_METHOD( "get_node_spawn_ids" ), &T3DHLODManifest::getNodeSpawnIds );
	ClassDB::bind_method( D_METHOD( "set_node_min_levels", "value" ), &T3DHLODManifest::setNodeMinLevels ); ClassDB::bind_method( D_METHOD( "get_node_min_levels" ), &T3DHLODManifest::getNodeMinLevels );
	ClassDB::bind_method( D_METHOD( "get_entry_count" ), &T3DHLODManifest::getEntryCount );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "format" ), "set_format", "get_format" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_map_size" ), "set_region_map_size", "get_region_map_size" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_size" ), "set_region_size", "get_region_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "vertex_spacing" ), "set_vertex_spacing", "get_vertex_spacing" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "max_level" ), "set_max_level", "get_max_level" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "build_signature" ), "set_build_signature", "get_build_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "built_unix" ), "set_built_unix", "get_built_unix" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "entries" ), "set_entries", "get_entries" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instancer_min_levels" ), "set_instancer_min_levels", "get_instancer_min_levels" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_INT32_ARRAY, "node_region_indices" ), "set_node_region_indices", "get_node_region_indices" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_INT64_ARRAY, "node_spawn_ids" ), "set_node_spawn_ids", "get_node_spawn_ids" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_BYTE_ARRAY, "node_min_levels" ), "set_node_min_levels", "get_node_min_levels" );
}
