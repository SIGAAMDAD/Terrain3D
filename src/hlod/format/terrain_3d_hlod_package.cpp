#include "integration/godot/terrain_3d_binding.h"
#include "hlod/format/terrain_3d_hlod_package.h"

#include "rendering/rd/vt/core/terrain_3d_vt_material_identity.h"

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

	ClassDB::bind_method( D_METHOD( "capture_rendering_materials" ), &Terrain3DHLODPackage::captureRenderingMaterialsCold );
	ClassDB::bind_method( D_METHOD( "has_canonical_rendering_materials" ), &Terrain3DHLODPackage::hasCanonicalRenderingMaterials );

	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setLevel, getLevel, "level",
	                   PropertyInfo( Variant::INT, "level", PROPERTY_HINT_RANGE, "1,6,1" ), "set_level", "get_level" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setClusterLocation, getClusterLocation, "location",
	                   PropertyInfo( Variant::VECTOR2I, "cluster_location" ), "set_cluster_location", "get_cluster_location" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setRegionMin, getRegionMin, "location",
	                   PropertyInfo( Variant::VECTOR2I, "region_min" ), "set_region_min", "get_region_min" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setRegionMax, getRegionMax, "location",
	                   PropertyInfo( Variant::VECTOR2I, "region_max" ), "set_region_max", "get_region_max" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setLocalBounds, getLocalBounds, "bounds",
	                   PropertyInfo( Variant::AABB, "local_bounds" ), "set_local_bounds", "get_local_bounds" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setMesh, getMesh, "mesh",
	                   PropertyInfo( Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh" ), "set_mesh", "get_mesh" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setRenderingMaterials, getRenderingMaterials, "materials",
	                   PropertyInfo( Variant::ARRAY, "rendering_materials", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DRenderingMaterial" ), "set_rendering_materials", "get_rendering_materials" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setGeometricError, getGeometricError, "error",
	                   PropertyInfo( Variant::FLOAT, "geometric_error" ), "set_geometric_error", "get_geometric_error" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setSourceSignature, getSourceSignature, "signature",
	                   PropertyInfo( Variant::INT, "source_signature" ), "set_source_signature", "get_source_signature" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setBuildSignature, getBuildSignature, "signature",
	                   PropertyInfo( Variant::INT, "build_signature" ), "set_build_signature", "get_build_signature" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setSourceTriangles, getSourceTriangles, "count",
	                   PropertyInfo( Variant::INT, "source_triangles" ), "set_source_triangles", "get_source_triangles" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setProxyTriangles, getProxyTriangles, "count",
	                   PropertyInfo( Variant::INT, "proxy_triangles" ), "set_proxy_triangles", "get_proxy_triangles" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setEstimatedGpuBytes, getEstimatedGpuBytes, "bytes",
	                   PropertyInfo( Variant::INT, "estimated_gpu_bytes" ), "set_estimated_gpu_bytes", "get_estimated_gpu_bytes" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage, setBuiltUnix, getBuiltUnix, "unix_time",
	                   PropertyInfo( Variant::INT, "built_unix" ), "set_built_unix", "get_built_unix" );
}
