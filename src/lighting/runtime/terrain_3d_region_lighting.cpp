// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"

#include <godot_cpp/core/class_db.hpp>

t3dAtomicU64_t Terrain3DRegionLighting::s_nGlobalPublicationGeneration{ 1u };

void Terrain3DRegionLighting::touchGlobalPublicationGeneration(
	void ) {
	s_nGlobalPublicationGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
}

#define T3D_SET_VALUE( methodName, methodSymbol, field_name, value_type )                                              \
	void Terrain3DRegionLighting::methodSymbol( value_type value ) {                                                   \
		if ( field_name == value ) {                                                                                   \
			return;                                                                                                    \
		}                                                                                                              \
		field_name = value;                                                                                            \
		touchGlobalPublicationGeneration();                                                                            \
		emit_changed();                                                                                                \
	}

void Terrain3DRegionLighting::setLightDataPath(
	const String &rPath ) {
	const String path = rPath.strip_edges();
	if ( m_lightDataPath == path ) {
		return;
	}
	m_lightDataPath = path;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setTerrainShadowMaskPath(
	const String &rPath ) {
	const String path = rPath.strip_edges();
	if ( m_terrainShadowMaskPath == path ) {
		return;
	}
	m_terrainShadowMaskPath = path;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setBakedUserData(
	const Array &rData ) {
	if ( ( rData.size() % 4 ) != 0 ) {
		ERR_PRINT( "Terrain3DRegionLighting baked_user_data must contain four values per LightmapGI user" );
		return;
	}
	m_bakedUserData = rData.duplicate( true );
	touchGlobalPublicationGeneration();
	emit_changed();
}

T3D_SET_VALUE(
	cluster_location,
	setClusterLocation,
	m_clusterLocation,
	const Vector2i & )
T3D_SET_VALUE(
	region_min,
	setRegionMin,
	m_regionMin,
	const Vector2i & )
T3D_SET_VALUE(
	region_max,
	setRegionMax,
	m_regionMax,
	const Vector2i & )

void Terrain3DRegionLighting::setClusterSizeRegions(
	const int nSize ) {
	const int value = CLAMP( nSize, 1, 8 );
	if ( m_nClusterSizeRegions == value ) {
		return;
	}
	m_nClusterSizeRegions = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setTerrainProxyLod(
	const int nLod ) {
	const int value = CLAMP( nLod, 0, 8 );
	if ( m_nTerrainProxyLod == value ) {
		return;
	}
	m_nTerrainProxyLod = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setContextRadiusRegions(
	const int nRadius ) {
	const int value = CLAMP( nRadius, 0, 4 );
	if ( m_nContextRadiusRegions == value ) {
		return;
	}
	m_nContextRadiusRegions = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setShadowmaskMode(
	const int nMode ) {
	const int value = CLAMP( nMode,
							 ( int ) ( LightmapGIData::SHADOWMASK_MODE_NONE ),
							 ( int ) ( LightmapGIData::SHADOWMASK_MODE_OVERLAY ) );
	if ( m_nShadowmaskMode == value ) {
		return;
	}
	m_nShadowmaskMode = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

T3D_SET_VALUE(
	bake_signature,
	setBakeSignature,
	m_nBakeSignature,
	int64_t )

void Terrain3DRegionLighting::setDirectionalLightSignature(
	const String &rSignature ) {
	const String signature = rSignature.strip_edges();
	if ( m_directionalLightSignature == signature ) {
		return;
	}
	m_directionalLightSignature = signature;
	touchGlobalPublicationGeneration();
	emit_changed();
}
T3D_SET_VALUE(
	baked_unix,
	setBakedUnix,
	m_nBakedUnix,
	int64_t )

void Terrain3DRegionLighting::setReceiverCount(
	const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nReceiverCount == value ) {
		return;
	}
	m_nReceiverCount = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setContributorCount(
	const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nContributorCount == value ) {
		return;
	}
	m_nContributorCount = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setLightmapTextureCount(
	const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nLightmapTextureCount == value ) {
		return;
	}
	m_nLightmapTextureCount = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setShadowmaskTextureCount(
	const int nCount ) {
	const int value = MAX( nCount, 0 );
	if ( m_nShadowmaskTextureCount == value ) {
		return;
	}
	m_nShadowmaskTextureCount = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setHasProbeData(
	const bool bHasProbes ) {
	if ( m_bHasProbeData == bHasProbes ) {
		return;
	}
	m_bHasProbeData = bHasProbes;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setDiskBytes(
	const int64_t nBytes ) {
	const int64_t value = MAX( nBytes, ( int64_t ) ( 0 ) );
	if ( m_nDiskBytes == value ) {
		return;
	}
	m_nDiskBytes = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

void Terrain3DRegionLighting::setEstimatedGpuBytes(
	const int64_t nBytes ) {
	const int64_t value = MAX( nBytes, ( int64_t ) ( 0 ) );
	if ( m_nEstimatedGpuBytes == value ) {
		return;
	}
	m_nEstimatedGpuBytes = value;
	touchGlobalPublicationGeneration();
	emit_changed();
}

T3D_SET_VALUE(
	source_scene,
	setSourceScene,
	m_sourceScene,
	const String & )

#undef T3D_SET_VALUE

int Terrain3DRegionLighting::stripBakedUsersWithPathFragment(
	const String &rFragment ) {
	if ( m_bakedUserData.is_empty() || rFragment.is_empty() ) {
		return 0;
	}
	Array kept;
	int	  removed = 0;
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
		touchGlobalPublicationGeneration();
		emit_changed();
	}
	return removed;
}

void Terrain3DRegionLighting::_bind_methods(
	void ) {
	ClassDB::bind_method( D_METHOD( "has_light_data" ), &Terrain3DRegionLighting::hasLightData );
	ClassDB::bind_method( D_METHOD( "has_terrain_shadow_mask" ), &Terrain3DRegionLighting::hasTerrainShadowMask );

	ClassDB::bind_method( D_METHOD( "has_shadowmask" ), &Terrain3DRegionLighting::hasShadowmask );
	ClassDB::bind_method( D_METHOD( "strip_baked_users_with_path_fragment", "path_fragment" ),
						  &Terrain3DRegionLighting::stripBakedUsersWithPathFragment );

	ADD_GROUP( "Bake Configuration", "" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setClusterSizeRegions,
					   getClusterSizeRegions,
					   "size",
					   PropertyInfo( Variant::INT, "cluster_size_regions", PROPERTY_HINT_RANGE, "1,8,1" ),
					   "set_cluster_size_regions",
					   "get_cluster_size_regions" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setTerrainProxyLod,
					   getTerrainProxyLod,
					   "lod",
					   PropertyInfo( Variant::INT, "terrain_proxy_lod", PROPERTY_HINT_RANGE, "0,8,1" ),
					   "set_terrain_proxy_lod",
					   "get_terrain_proxy_lod" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setContextRadiusRegions,
					   getContextRadiusRegions,
					   "radius",
					   PropertyInfo( Variant::INT, "context_radius_regions", PROPERTY_HINT_RANGE, "0,4,1" ),
					   "set_context_radius_regions",
					   "get_context_radius_regions" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setShadowmaskMode,
					   getShadowmaskMode,
					   "mode",
					   PropertyInfo( Variant::INT, "shadowmask_mode", PROPERTY_HINT_ENUM, "None,Replace,Overlay" ),
					   "set_shadowmask_mode",
					   "get_shadowmask_mode" );

	ADD_GROUP( "Package Reference", "" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setLightDataPath,
					   getLightDataPath,
					   "path",
					   PropertyInfo( Variant::STRING, "light_data_path", PROPERTY_HINT_FILE, "*.lmbake" ),
					   "set_light_data_path",
					   "get_light_data_path" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setTerrainShadowMaskPath,
					   getTerrainShadowMaskPath,
					   "path",
					   PropertyInfo( Variant::STRING, "terrain_shadow_mask_path", PROPERTY_HINT_FILE, "*.res,*.tres" ),
					   "set_terrain_shadow_mask_path",
					   "get_terrain_shadow_mask_path" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setBakedUserData,
					   getBakedUserData,
					   "data",
					   PropertyInfo( Variant::ARRAY,
									 "baked_user_data",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL ),
					   "set_baked_user_data",
					   "get_baked_user_data" );

	ADD_GROUP( "Generated Bake Metadata", "" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setClusterLocation,
					   getClusterLocation,
					   "location",
					   PropertyInfo( Variant::VECTOR2I,
									 "cluster_location",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_cluster_location",
					   "get_cluster_location" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setRegionMin,
					   getRegionMin,
					   "location",
					   PropertyInfo( Variant::VECTOR2I,
									 "region_min",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_region_min",
					   "get_region_min" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setRegionMax,
					   getRegionMax,
					   "location",
					   PropertyInfo( Variant::VECTOR2I,
									 "region_max",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_region_max",
					   "get_region_max" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setBakeSignature,
					   getBakeSignature,
					   "signature",
					   PropertyInfo( Variant::INT,
									 "bake_signature",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_bake_signature",
					   "get_bake_signature" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setDirectionalLightSignature,
					   getDirectionalLightSignature,
					   "signature",
					   PropertyInfo( Variant::STRING,
									 "directional_light_signature",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_directional_light_signature",
					   "get_directional_light_signature" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setBakedUnix,
					   getBakedUnix,
					   "unix_time",
					   PropertyInfo( Variant::INT,
									 "baked_unix",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_baked_unix",
					   "get_baked_unix" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setReceiverCount,
					   getReceiverCount,
					   "count",
					   PropertyInfo( Variant::INT,
									 "receiver_count",
									 PROPERTY_HINT_RANGE,
									 "0,1000000,1",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_receiver_count",
					   "get_receiver_count" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setContributorCount,
					   getContributorCount,
					   "count",
					   PropertyInfo( Variant::INT,
									 "contributor_count",
									 PROPERTY_HINT_RANGE,
									 "0,1000000,1",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_contributor_count",
					   "get_contributor_count" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setLightmapTextureCount,
					   getLightmapTextureCount,
					   "count",
					   PropertyInfo( Variant::INT,
									 "lightmap_texture_count",
									 PROPERTY_HINT_RANGE,
									 "0,1024,1",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_lightmap_texture_count",
					   "get_lightmap_texture_count" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setShadowmaskTextureCount,
					   getShadowmaskTextureCount,
					   "count",
					   PropertyInfo( Variant::INT,
									 "shadowmask_texture_count",
									 PROPERTY_HINT_RANGE,
									 "0,1024,1",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_shadowmask_texture_count",
					   "get_shadowmask_texture_count" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setHasProbeData,
					   hasProbeData,
					   "has_probes",
					   PropertyInfo( Variant::BOOL,
									 "has_probe_data",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_has_probe_data",
					   "get_has_probe_data" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setDiskBytes,
					   getDiskBytes,
					   "bytes",
					   PropertyInfo( Variant::INT,
									 "disk_bytes",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_disk_bytes",
					   "get_disk_bytes" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setEstimatedGpuBytes,
					   getEstimatedGpuBytes,
					   "bytes",
					   PropertyInfo( Variant::INT,
									 "estimated_gpu_bytes",
									 PROPERTY_HINT_NONE,
									 "",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_estimated_gpu_bytes",
					   "get_estimated_gpu_bytes" );
	T3D_BIND_PROPERTY( Terrain3DRegionLighting,
					   setSourceScene,
					   getSourceScene,
					   "path",
					   PropertyInfo( Variant::STRING,
									 "source_scene",
									 PROPERTY_HINT_FILE,
									 "*.tscn,*.scn",
									 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
					   "set_source_scene",
					   "get_source_scene" );
}
