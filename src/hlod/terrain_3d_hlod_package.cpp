#include "hlod/terrain_3d_hlod_package.h"

#include <godot_cpp/core/class_db.hpp>

#define HLOD_SET_VALUE( method, field, type, normalize ) \
	void Terrain3DHLODPackage::method( type value ) { \
		auto normalized = ( normalize ); \
		if ( field == normalized ) { \
			return; \
		} \
		field = normalized; \
		emit_changed(); \
	}

HLOD_SET_VALUE( setLevel, m_nLevel, int, CLAMP( value, 1, 6 ) )
HLOD_SET_VALUE( setClusterLocation, m_clusterLocation, const Vector2i &, value )
HLOD_SET_VALUE( setRegionMin, m_regionMin, const Vector2i &, value )
HLOD_SET_VALUE( setRegionMax, m_regionMax, const Vector2i &, value )
HLOD_SET_VALUE( setLocalBounds, m_localBounds, const AABB &, value )
HLOD_SET_VALUE( setGeometricError, m_fGeometricError, real_t, MAX( value, 0.001f ) )
HLOD_SET_VALUE( setSourceSignature, m_nSourceSignature, int64_t, value )
HLOD_SET_VALUE( setBuildSignature, m_nBuildSignature, int64_t, value )
HLOD_SET_VALUE( setSourceTriangles, m_nSourceTriangles, int64_t, MAX<int64_t>( value, 0 ) )
HLOD_SET_VALUE( setProxyTriangles, m_nProxyTriangles, int64_t, MAX<int64_t>( value, 0 ) )
HLOD_SET_VALUE( setEstimatedGpuBytes, m_nEstimatedGpuBytes, int64_t, MAX<int64_t>( value, 0 ) )
HLOD_SET_VALUE( setBuiltUnix, m_nBuiltUnix, int64_t, MAX<int64_t>( value, 0 ) )

void Terrain3DHLODPackage::setMesh( const Ref<ArrayMesh> &rMesh ) {
	if ( m_mesh == rMesh ) {
		return;
	}
	m_mesh = rMesh;
	emit_changed();
}

void Terrain3DHLODPackage::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_level", "level" ), &Terrain3DHLODPackage::setLevel );
	ClassDB::bind_method( D_METHOD( "get_level" ), &Terrain3DHLODPackage::getLevel );
	ClassDB::bind_method( D_METHOD( "set_cluster_location", "location" ), &Terrain3DHLODPackage::setClusterLocation );
	ClassDB::bind_method( D_METHOD( "get_cluster_location" ), &Terrain3DHLODPackage::getClusterLocation );
	ClassDB::bind_method( D_METHOD( "set_region_min", "location" ), &Terrain3DHLODPackage::setRegionMin );
	ClassDB::bind_method( D_METHOD( "get_region_min" ), &Terrain3DHLODPackage::getRegionMin );
	ClassDB::bind_method( D_METHOD( "set_region_max", "location" ), &Terrain3DHLODPackage::setRegionMax );
	ClassDB::bind_method( D_METHOD( "get_region_max" ), &Terrain3DHLODPackage::getRegionMax );
	ClassDB::bind_method( D_METHOD( "set_local_bounds", "bounds" ), &Terrain3DHLODPackage::setLocalBounds );
	ClassDB::bind_method( D_METHOD( "get_local_bounds" ), &Terrain3DHLODPackage::getLocalBounds );
	ClassDB::bind_method( D_METHOD( "set_mesh", "mesh" ), &Terrain3DHLODPackage::setMesh );
	ClassDB::bind_method( D_METHOD( "get_mesh" ), &Terrain3DHLODPackage::getMesh );
	ClassDB::bind_method( D_METHOD( "set_geometric_error", "error" ), &Terrain3DHLODPackage::setGeometricError );
	ClassDB::bind_method( D_METHOD( "get_geometric_error" ), &Terrain3DHLODPackage::getGeometricError );
	ClassDB::bind_method( D_METHOD( "set_source_signature", "signature" ), &Terrain3DHLODPackage::setSourceSignature );
	ClassDB::bind_method( D_METHOD( "get_source_signature" ), &Terrain3DHLODPackage::getSourceSignature );
	ClassDB::bind_method( D_METHOD( "set_build_signature", "signature" ), &Terrain3DHLODPackage::setBuildSignature );
	ClassDB::bind_method( D_METHOD( "get_build_signature" ), &Terrain3DHLODPackage::getBuildSignature );
	ClassDB::bind_method( D_METHOD( "set_source_triangles", "count" ), &Terrain3DHLODPackage::setSourceTriangles );
	ClassDB::bind_method( D_METHOD( "get_source_triangles" ), &Terrain3DHLODPackage::getSourceTriangles );
	ClassDB::bind_method( D_METHOD( "set_proxy_triangles", "count" ), &Terrain3DHLODPackage::setProxyTriangles );
	ClassDB::bind_method( D_METHOD( "get_proxy_triangles" ), &Terrain3DHLODPackage::getProxyTriangles );
	ClassDB::bind_method( D_METHOD( "set_estimated_gpu_bytes", "bytes" ), &Terrain3DHLODPackage::setEstimatedGpuBytes );
	ClassDB::bind_method( D_METHOD( "get_estimated_gpu_bytes" ), &Terrain3DHLODPackage::getEstimatedGpuBytes );
	ClassDB::bind_method( D_METHOD( "set_built_unix", "unix_time" ), &Terrain3DHLODPackage::setBuiltUnix );
	ClassDB::bind_method( D_METHOD( "get_built_unix" ), &Terrain3DHLODPackage::getBuiltUnix );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "level", PROPERTY_HINT_RANGE, "1,6,1" ), "set_level", "get_level" );
	ADD_PROPERTY(
			PropertyInfo( Variant::VECTOR2I, "cluster_location" ), "set_cluster_location", "get_cluster_location" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I, "region_min" ), "set_region_min", "get_region_min" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I, "region_max" ), "set_region_max", "get_region_max" );
	ADD_PROPERTY( PropertyInfo( Variant::AABB, "local_bounds" ), "set_local_bounds", "get_local_bounds" );
	ADD_PROPERTY(
			PropertyInfo( Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh" ), "set_mesh", "get_mesh" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "geometric_error" ), "set_geometric_error", "get_geometric_error" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "source_signature" ), "set_source_signature", "get_source_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "build_signature" ), "set_build_signature", "get_build_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "source_triangles" ), "set_source_triangles", "get_source_triangles" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "proxy_triangles" ), "set_proxy_triangles", "get_proxy_triangles" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "estimated_gpu_bytes" ), "set_estimated_gpu_bytes", "get_estimated_gpu_bytes" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "built_unix" ), "set_built_unix", "get_built_unix" );
}
