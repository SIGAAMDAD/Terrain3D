#include "hlod/terrain_3d_hlod_package.h"

#include "rendering/rd/vt/terrain_3d_vt_material_identity.h"

#include <godot_cpp/core/class_db.hpp>

#define HLOD_SET_VALUE( method, field, type, normalize )                                                                                   \
	void Terrain3DHLODPackage::method( type value ) {                                                                                      \
		type normalized = ( normalize );                                                                                                   \
		if ( field == normalized ) {                                                                                                       \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field = normalized;                                                                                                                \
		emit_changed();                                                                                                                    \
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

String Terrain3DHLODPackage::getVtMaterialOwnerIdentityCold( void ) const {
	const String package_path = get_path();
	if ( !package_path.is_empty() ) {
		return String( "hlod-package:" ) + package_path;
	}
	return String( "hlod-level:" ) + String::num_int64( m_nLevel ) + String( ":cluster:" ) + String::num_int64( m_clusterLocation.x )
	        + String( "," ) + String::num_int64( m_clusterLocation.y ) + String( ":source:" ) + String::num_int64( m_nSourceSignature );
}

bool Terrain3DHLODPackage::ensureRenderingMaterialStableKeysCold( void ) {
	const String owner = getVtMaterialOwnerIdentityCold();
	for ( int64_t iMaterial = 0; iMaterial < m_renderingMaterials.size(); ++iMaterial ) {
		const Ref<Terrain3DRenderingMaterial> material = m_renderingMaterials[iMaterial];
		if ( material.is_null() ) {
			continue;
		}
		const String role = String( "surface:" ) + String::num_int64( iMaterial );
		if ( !material->ensureVtStableKeyCold( owner, role ) ) {
			return false;
		}
	}
	return true;
}

void Terrain3DHLODPackage::setRenderingMaterials( const TypedArray<Terrain3DRenderingMaterial> &rMaterials ) {
	if ( m_renderingMaterials == rMaterials ) {
		return;
	}
	m_renderingMaterials = rMaterials;
	ensureRenderingMaterialStableKeysCold();
	emit_changed();
}

bool Terrain3DHLODPackage::captureRenderingMaterialsCold( void ) {
	if ( m_mesh.is_null() ) {
		return false;
	}
	TypedArray<Terrain3DRenderingMaterial> materials;
	materials.resize( m_mesh->get_surface_count() );
	for ( int surface = 0; surface < m_mesh->get_surface_count(); ++surface ) {
		Ref<Terrain3DRenderingMaterial> renderingMaterial;
		renderingMaterial.instantiate();
		const Ref<Material> sourceMaterial = m_mesh->surface_get_material( surface );
		if ( sourceMaterial.is_valid() && !renderingMaterial->importBaseMaterialCold( sourceMaterial ) ) {
			return false;
		}
		if ( sourceMaterial.is_valid() && !sourceMaterial->get_path().is_empty() && !sourceMaterial->get_path().contains( "::" ) ) {
			renderingMaterial->setVtStableKey( t3d_vt_material_stable_key_from_resource_path( sourceMaterial->get_path() ) );
		} else if ( !renderingMaterial->ensureVtStableKeyCold(
		                    getVtMaterialOwnerIdentityCold(), String( "surface:" ) + String::num_int64( surface ) ) ) {
			return false;
		}
		materials[surface] = renderingMaterial;
	}
	setRenderingMaterials( materials );
	return true;
}

bool Terrain3DHLODPackage::hasCanonicalRenderingMaterials( void ) const {
	if ( m_mesh.is_null() || m_renderingMaterials.size() != m_mesh->get_surface_count() ) {
		return false;
	}
	for ( int surface = 0; surface < m_renderingMaterials.size(); ++surface ) {
		const Ref<Terrain3DRenderingMaterial> material = m_renderingMaterials[surface];
		if ( material.is_null() || !material->isOneSubmitCompatible() ) {
			return false;
		}
	}
	return true;
}

const Terrain3DRenderingMaterial *Terrain3DHLODPackage::getRenderingMaterialPtr( const uint32_t nSurface ) const {
	if ( nSurface >= ( uint32_t )( m_renderingMaterials.size() ) ) {
		return NULL;
	}
	const Ref<Terrain3DRenderingMaterial> material = m_renderingMaterials[( int64_t )( nSurface )];
	return material.ptr();
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
	ClassDB::bind_method( D_METHOD( "set_rendering_materials", "materials" ), &Terrain3DHLODPackage::setRenderingMaterials );
	ClassDB::bind_method( D_METHOD( "get_rendering_materials" ), &Terrain3DHLODPackage::getRenderingMaterials );
	ClassDB::bind_method( D_METHOD( "capture_rendering_materials" ), &Terrain3DHLODPackage::captureRenderingMaterialsCold );
	ClassDB::bind_method( D_METHOD( "has_canonical_rendering_materials" ), &Terrain3DHLODPackage::hasCanonicalRenderingMaterials );
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
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I, "cluster_location" ), "set_cluster_location", "get_cluster_location" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I, "region_min" ), "set_region_min", "get_region_min" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I, "region_max" ), "set_region_max", "get_region_max" );
	ADD_PROPERTY( PropertyInfo( Variant::AABB, "local_bounds" ), "set_local_bounds", "get_local_bounds" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh" ), "set_mesh", "get_mesh" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "rendering_materials", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DRenderingMaterial" ),
	        "set_rendering_materials",
	        "get_rendering_materials" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "geometric_error" ), "set_geometric_error", "get_geometric_error" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "source_signature" ), "set_source_signature", "get_source_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "build_signature" ), "set_build_signature", "get_build_signature" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "source_triangles" ), "set_source_triangles", "get_source_triangles" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "proxy_triangles" ), "set_proxy_triangles", "get_proxy_triangles" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "estimated_gpu_bytes" ), "set_estimated_gpu_bytes", "get_estimated_gpu_bytes" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "built_unix" ), "set_built_unix", "get_built_unix" );
}
