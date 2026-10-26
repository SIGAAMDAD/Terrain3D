#include "integration/godot/terrain_3d_binding.h"
#include "hlod/format/terrain_3d_hlod_manifest.h"

#include "hlod/format/terrain_3d_hlod_runtime_codec.h"

#include <godot_cpp/core/class_db.hpp>

bool Terrain3DHLODManifest::compileRuntimeBlob( void ) {
	PackedByteArray compiled =
	        Terrain3DHLODRuntimeCodec::encode( m_entries, m_instancerMinLevels, m_nodeRegionIndices, m_nodeSpawnIds, m_nodeMinLevels );
	if ( compiled.is_empty() ) {
		return false;
	}
	m_runtimeBlob = compiled;
	emit_changed();
	return true;
}

bool Terrain3DHLODManifest::validateRuntimeBlob( void ) const {
	if ( m_runtimeBlob.is_empty() ) {
		return false;
	}
	t3dHLODRuntimeData_t decoded;
	if ( !Terrain3DHLODRuntimeCodec::decode( m_runtimeBlob, decoded ) ) {
		return false;
	}
	return decoded.m_entries.size() == ( size_t )m_entries.size();
}

void Terrain3DHLODManifest::_bind_methods( void ) {

	ClassDB::bind_method( D_METHOD( "compile_runtime_blob" ), &Terrain3DHLODManifest::compileRuntimeBlob );
	ClassDB::bind_method( D_METHOD( "validate_runtime_blob" ), &Terrain3DHLODManifest::validateRuntimeBlob );
	ClassDB::bind_method( D_METHOD( "get_entry_count" ), &Terrain3DHLODManifest::getEntryCount );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setFormat, getFormat, "value",
	                   PropertyInfo( Variant::INT, "format" ), "set_format", "get_format" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setMaxLevel, getMaxLevel, "value",
	                   PropertyInfo( Variant::INT, "max_level" ), "set_max_level", "get_max_level" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setBuildSignature, getBuildSignature, "value",
	                   PropertyInfo( Variant::INT, "build_signature" ), "set_build_signature", "get_build_signature" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setBuiltUnix, getBuiltUnix, "value",
	                   PropertyInfo( Variant::INT, "built_unix" ), "set_built_unix", "get_built_unix" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setEntries, getEntries, "value",
	                   PropertyInfo( Variant::ARRAY, "entries" ), "set_entries", "get_entries" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setInstancerMinLevels, getInstancerMinLevels, "value",
	                   PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instancer_min_levels" ), "set_instancer_min_levels", "get_instancer_min_levels" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setNodeRegionIndices, getNodeRegionIndices, "value",
	                   PropertyInfo( Variant::PACKED_INT32_ARRAY, "node_region_indices" ), "set_node_region_indices", "get_node_region_indices" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setNodeSpawnIds, getNodeSpawnIds, "value",
	                   PropertyInfo( Variant::PACKED_INT64_ARRAY, "node_spawn_ids" ), "set_node_spawn_ids", "get_node_spawn_ids" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setNodeMinLevels, getNodeMinLevels, "value",
	                   PropertyInfo( Variant::PACKED_BYTE_ARRAY, "node_min_levels" ), "set_node_min_levels", "get_node_min_levels" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest, setRuntimeBlob, getRuntimeBlob, "value",
	                   PropertyInfo( Variant::PACKED_BYTE_ARRAY, "runtime_blob", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ), "set_runtime_blob", "get_runtime_blob" );
}
