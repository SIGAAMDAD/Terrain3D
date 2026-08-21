// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "lighting/terrain_3d_region_lighting.h"

#include <godot_cpp/core/class_db.hpp>

#define T3D_SET_VALUE( methodName, methodSymbol, field_name, value_type ) \
	void T3DRegionLighting::methodSymbol( value_type value ) { \
		if ( field_name == value ) { \
			return; \
		} \
		field_name = value; \
		emit_changed(); \
	}

void T3DRegionLighting::setLightDataPath( const String &rPath ) {
	const String path = rPath.strip_edges();
	if ( m_lightDataPath == path ) {
		return;
	}
	m_lightDataPath = path;
	emit_changed();
}

void T3DRegionLighting::setTerrainShadowMaskPath( const String &rPath ) {
	const String path = rPath.strip_edges();
	if ( m_terrainShadowMaskPath == path ) {
		return;
	}
	m_terrainShadowMaskPath = path;
	emit_changed();
}

void T3DRegionLighting::setBakedUserData( const Array &rData ) {
	if ( ( rData.size() % 4 ) != 0 ) {
		ERR_PRINT( "T3DRegionLighting baked_user_data must contain four values per LightmapGI user" );
		return;
	}
	m_bakedUserData = rData.duplicate( true );
	emit_changed();
}

T3D_SET_VALUE( cluster_location, setClusterLocation, m_clusterLocation, const Vector2i & )
T3D_SET_VALUE( region_min, setRegionMin, m_regionMin, const Vector2i & )
T3D_SET_VALUE( region_max, setRegionMax, m_regionMax, const Vector2i & )

void T3DRegionLighting::setClusterSizeRegions( const int nSize ) {
	const int value = CLAMP( nSize, 1, 8 );
	if ( m_nClusterSizeRegions == value ) {
		return;
	}
	m_nClusterSizeRegions = value;
	emit_changed();
}

void T3DRegionLighting::setTerrainProxyLod( const int nLod ) {
	const int value = CLAMP( nLod, 0, 8 );
	if ( m_nTerrainProxyLod == value ) {
		return;
	}
	m_nTerrainProxyLod = value;
	emit_changed();
}

void T3DRegionLighting::setContextRadiusRegions( const int nRadius ) {
	const int value = CLAMP( nRadius, 0, 4 );
	if ( m_nContextRadiusRegions == value ) {
		return;
	}
	m_nContextRadiusRegions = value;
	emit_changed();
}

void T3DRegionLighting::setShadowmaskMode( const int nMode ) {
	const int value = CLAMP( nMode,
			( int )( LightmapGIData::SHADOWMASK_MODE_NONE ),
			( int )( LightmapGIData::SHADOWMASK_MODE_OVERLAY ) );
	if ( m_nShadowmaskMode == value ) {
		return;
	}
	m_nShadowmaskMode = value;
	emit_changed();
}

T3D_SET_VALUE( bake_signature, setBakeSignature, m_nBakeSignature, int64_t )

void T3DRegionLighting::setDirectionalLightSignature( const String &rSignature ) {
	const String signature = rSignature.strip_edges();
	if ( m_directionalLightSignature == signature ) {
		return;
	}
	m_directionalLightSignature = signature;
	emit_changed();
}
T3D_SET_VALUE( baked_unix, setBakedUnix, m_nBakedUnix, int64_t )

void T3DRegionLighting::setReceiverCount( const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nReceiverCount == value ) {
		return;
	}
	m_nReceiverCount = value;
	emit_changed();
}

void T3DRegionLighting::setContributorCount( const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nContributorCount == value ) {
		return;
	}
	m_nContributorCount = value;
	emit_changed();
}

void T3DRegionLighting::setLightmapTextureCount( const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nLightmapTextureCount == value ) {
		return;
	}
	m_nLightmapTextureCount = value;
	emit_changed();
}

void T3DRegionLighting::setShadowmaskTextureCount( const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nShadowmaskTextureCount == value ) {
		return;
	}
	m_nShadowmaskTextureCount = value;
	emit_changed();
}

void T3DRegionLighting::setHasProbeData( const bool bHasProbes ) {
	if ( m_bHasProbeData == bHasProbes ) {
		return;
	}
	m_bHasProbeData = bHasProbes;
	emit_changed();
}

void T3DRegionLighting::setDiskBytes( const int64_t nBytes ) {
	const int64_t value = MAX( nBytes, ( int64_t )( 0 ) );
	if ( m_nDiskBytes == value ) {
		return;
	}
	m_nDiskBytes = value;
	emit_changed();
}

void T3DRegionLighting::setEstimatedGpuBytes( const int64_t nBytes ) {
	const int64_t value = MAX( nBytes, ( int64_t )( 0 ) );
	if ( m_nEstimatedGpuBytes == value ) {
		return;
	}
	m_nEstimatedGpuBytes = value;
	emit_changed();
}

T3D_SET_VALUE( source_scene, setSourceScene, m_sourceScene, const String & )

#undef T3D_SET_VALUE

int T3DRegionLighting::stripBakedUsersWithPathFragment( const String &rFragment ) {
	if ( m_bakedUserData.is_empty() || rFragment.is_empty() ) {
		return 0;
	}
	Array kept;
	int removed = 0;
	for ( int i = 0; i < m_bakedUserData.size(); i += 4 ) {
		const NodePath path = m_bakedUserData[i];
		if ( String( path ).contains( rFragment ) ) {
			++removed;
			continue;
		}
		kept.push_back( m_bakedUserData[i] );
		kept.push_back( m_bakedUserData[i + 1] );
		kept.push_back( m_bakedUserData[i + 2] );
		kept.push_back( m_bakedUserData[i + 3] );
	}
	if ( removed > 0 ) {
		m_bakedUserData = kept;
		emit_changed();
	}
	return removed;
}

void T3DRegionLighting::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_light_data_path", "path" ), &T3DRegionLighting::setLightDataPath );
	ClassDB::bind_method( D_METHOD( "get_light_data_path" ), &T3DRegionLighting::getLightDataPath );
	ClassDB::bind_method( D_METHOD( "has_light_data" ), &T3DRegionLighting::hasLightData );
	ClassDB::bind_method(
			D_METHOD( "set_terrain_shadow_mask_path", "path" ), &T3DRegionLighting::setTerrainShadowMaskPath );
	ClassDB::bind_method( D_METHOD( "get_terrain_shadow_mask_path" ), &T3DRegionLighting::getTerrainShadowMaskPath );
	ClassDB::bind_method( D_METHOD( "has_terrain_shadow_mask" ), &T3DRegionLighting::hasTerrainShadowMask );
	ClassDB::bind_method( D_METHOD( "set_baked_user_data", "data" ), &T3DRegionLighting::setBakedUserData );
	ClassDB::bind_method( D_METHOD( "get_baked_user_data" ), &T3DRegionLighting::getBakedUserData );
	ClassDB::bind_method( D_METHOD( "set_cluster_location", "location" ), &T3DRegionLighting::setClusterLocation );
	ClassDB::bind_method( D_METHOD( "get_cluster_location" ), &T3DRegionLighting::getClusterLocation );
	ClassDB::bind_method( D_METHOD( "set_region_min", "location" ), &T3DRegionLighting::setRegionMin );
	ClassDB::bind_method( D_METHOD( "get_region_min" ), &T3DRegionLighting::getRegionMin );
	ClassDB::bind_method( D_METHOD( "set_region_max", "location" ), &T3DRegionLighting::setRegionMax );
	ClassDB::bind_method( D_METHOD( "get_region_max" ), &T3DRegionLighting::getRegionMax );
	ClassDB::bind_method( D_METHOD( "set_cluster_size_regions", "size" ), &T3DRegionLighting::setClusterSizeRegions );
	ClassDB::bind_method( D_METHOD( "get_cluster_size_regions" ), &T3DRegionLighting::getClusterSizeRegions );
	ClassDB::bind_method( D_METHOD( "set_terrain_proxy_lod", "lod" ), &T3DRegionLighting::setTerrainProxyLod );
	ClassDB::bind_method( D_METHOD( "get_terrain_proxy_lod" ), &T3DRegionLighting::getTerrainProxyLod );
	ClassDB::bind_method(
			D_METHOD( "set_context_radius_regions", "radius" ), &T3DRegionLighting::setContextRadiusRegions );
	ClassDB::bind_method( D_METHOD( "get_context_radius_regions" ), &T3DRegionLighting::getContextRadiusRegions );
	ClassDB::bind_method( D_METHOD( "set_shadowmask_mode", "mode" ), &T3DRegionLighting::setShadowmaskMode );
	ClassDB::bind_method( D_METHOD( "get_shadowmask_mode" ), &T3DRegionLighting::getShadowmaskMode );
	ClassDB::bind_method( D_METHOD( "set_bake_signature", "signature" ), &T3DRegionLighting::setBakeSignature );
	ClassDB::bind_method( D_METHOD( "get_bake_signature" ), &T3DRegionLighting::getBakeSignature );
	ClassDB::bind_method( D_METHOD( "set_directional_light_signature", "signature" ),
			&T3DRegionLighting::setDirectionalLightSignature );
	ClassDB::bind_method(
			D_METHOD( "get_directional_light_signature" ), &T3DRegionLighting::getDirectionalLightSignature );
	ClassDB::bind_method( D_METHOD( "set_baked_unix", "unix_time" ), &T3DRegionLighting::setBakedUnix );
	ClassDB::bind_method( D_METHOD( "get_baked_unix" ), &T3DRegionLighting::getBakedUnix );
	ClassDB::bind_method( D_METHOD( "set_receiver_count", "count" ), &T3DRegionLighting::setReceiverCount );
	ClassDB::bind_method( D_METHOD( "get_receiver_count" ), &T3DRegionLighting::getReceiverCount );
	ClassDB::bind_method( D_METHOD( "set_contributor_count", "count" ), &T3DRegionLighting::setContributorCount );
	ClassDB::bind_method( D_METHOD( "get_contributor_count" ), &T3DRegionLighting::getContributorCount );
	ClassDB::bind_method(
			D_METHOD( "set_lightmap_texture_count", "count" ), &T3DRegionLighting::setLightmapTextureCount );
	ClassDB::bind_method( D_METHOD( "get_lightmap_texture_count" ), &T3DRegionLighting::getLightmapTextureCount );
	ClassDB::bind_method(
			D_METHOD( "set_shadowmask_texture_count", "count" ), &T3DRegionLighting::setShadowmaskTextureCount );
	ClassDB::bind_method( D_METHOD( "get_shadowmask_texture_count" ), &T3DRegionLighting::getShadowmaskTextureCount );
	ClassDB::bind_method( D_METHOD( "set_has_probe_data", "has_probes" ), &T3DRegionLighting::setHasProbeData );
	ClassDB::bind_method( D_METHOD( "has_probe_data" ), &T3DRegionLighting::hasProbeData );
	ClassDB::bind_method( D_METHOD( "set_disk_bytes", "bytes" ), &T3DRegionLighting::setDiskBytes );
	ClassDB::bind_method( D_METHOD( "get_disk_bytes" ), &T3DRegionLighting::getDiskBytes );
	ClassDB::bind_method( D_METHOD( "set_estimated_gpu_bytes", "bytes" ), &T3DRegionLighting::setEstimatedGpuBytes );
	ClassDB::bind_method( D_METHOD( "get_estimated_gpu_bytes" ), &T3DRegionLighting::getEstimatedGpuBytes );
	ClassDB::bind_method( D_METHOD( "set_source_scene", "path" ), &T3DRegionLighting::setSourceScene );
	ClassDB::bind_method( D_METHOD( "get_source_scene" ), &T3DRegionLighting::getSourceScene );
	ClassDB::bind_method( D_METHOD( "has_shadowmask" ), &T3DRegionLighting::hasShadowmask );
	ClassDB::bind_method( D_METHOD( "strip_baked_users_with_path_fragment", "path_fragment" ),
			&T3DRegionLighting::stripBakedUsersWithPathFragment );

	ADD_GROUP( "Bake Configuration", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cluster_size_regions", PROPERTY_HINT_RANGE, "1,8,1" ),
			"set_cluster_size_regions",
			"get_cluster_size_regions" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "terrain_proxy_lod", PROPERTY_HINT_RANGE, "0,8,1" ),
			"set_terrain_proxy_lod",
			"get_terrain_proxy_lod" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "context_radius_regions", PROPERTY_HINT_RANGE, "0,4,1" ),
			"set_context_radius_regions",
			"get_context_radius_regions" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shadowmask_mode", PROPERTY_HINT_ENUM, "None,Replace,Overlay" ),
			"set_shadowmask_mode",
			"get_shadowmask_mode" );

	ADD_GROUP( "Package Reference", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "light_data_path", PROPERTY_HINT_FILE, "*.lmbake" ),
			"set_light_data_path",
			"get_light_data_path" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "terrain_shadow_mask_path", PROPERTY_HINT_FILE, "*.res,*.tres" ),
			"set_terrain_shadow_mask_path",
			"get_terrain_shadow_mask_path" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY,
						  "baked_user_data",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL ),
			"set_baked_user_data",
			"get_baked_user_data" );

	ADD_GROUP( "Generated Bake Metadata", "" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I,
						  "cluster_location",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_cluster_location",
			"get_cluster_location" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I,
						  "region_min",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_region_min",
			"get_region_min" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I,
						  "region_max",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_region_max",
			"get_region_max" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "bake_signature",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_bake_signature",
			"get_bake_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
						  "directional_light_signature",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_directional_light_signature",
			"get_directional_light_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "baked_unix",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_baked_unix",
			"get_baked_unix" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "receiver_count",
						  PROPERTY_HINT_RANGE,
						  "0,1000000,1",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_receiver_count",
			"get_receiver_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "contributor_count",
						  PROPERTY_HINT_RANGE,
						  "0,1000000,1",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_contributor_count",
			"get_contributor_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "lightmap_texture_count",
						  PROPERTY_HINT_RANGE,
						  "0,1024,1",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_lightmap_texture_count",
			"get_lightmap_texture_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "shadowmask_texture_count",
						  PROPERTY_HINT_RANGE,
						  "0,1024,1",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_shadowmask_texture_count",
			"get_shadowmask_texture_count" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL,
						  "has_probe_data",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_has_probe_data",
			"has_probe_data" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "disk_bytes",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_disk_bytes",
			"get_disk_bytes" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "estimated_gpu_bytes",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_estimated_gpu_bytes",
			"get_estimated_gpu_bytes" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
						  "source_scene",
						  PROPERTY_HINT_FILE,
						  "*.tscn,*.scn",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_source_scene",
			"get_source_scene" );
}
