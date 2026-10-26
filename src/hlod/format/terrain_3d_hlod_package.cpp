#include "hlod/format/terrain_3d_hlod_manifest.h"
#include "hlod/format/terrain_3d_hlod_runtime_codec.h"
#include "integration/godot/terrain_3d_binding.h"
#include "hlod/format/terrain_3d_hlod_package.h"
#include "hlod/format/terrain_3d_hlod_instance_codec.h"

#include "rendering/rd/vt/core/terrain_3d_vt_material_identity.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#define HLOD_SET_VALUE( method, field, type, normalize )                                                               \
	void Terrain3DHLODPackage::method( type value ) {                                                                  \
		type normalized = ( normalize );                                                                               \
		if ( field == normalized ) {                                                                                   \
			return;                                                                                                    \
		}                                                                                                              \
		field = normalized;                                                                                            \
		emit_changed();                                                                                                \
	}

HLOD_SET_VALUE(
	setLevel,
	m_nLevel,
	int,
	CLAMP( value, -1, 6 ) )
HLOD_SET_VALUE(
	setClusterLocation,
	m_clusterLocation,
	const Vector2i &,
	value )
HLOD_SET_VALUE(
	setRegionMin,
	m_regionMin,
	const Vector2i &,
	value )
HLOD_SET_VALUE(
	setRegionMax,
	m_regionMax,
	const Vector2i &,
	value )
HLOD_SET_VALUE(
	setLocalBounds,
	m_localBounds,
	const AABB &,
	value )
HLOD_SET_VALUE(
	setGeometricError,
	m_fGeometricError,
	real_t,
	MAX( value, 0.001f ) )
HLOD_SET_VALUE(
	setSourceSignature,
	m_nSourceSignature,
	int64_t,
	value )
HLOD_SET_VALUE(
	setBuildSignature,
	m_nBuildSignature,
	int64_t,
	value )
HLOD_SET_VALUE(
	setSourceTriangles,
	m_nSourceTriangles,
	int64_t,
	MAX< int64_t >( value, 0 ) )
HLOD_SET_VALUE(
	setProxyTriangles,
	m_nProxyTriangles,
	int64_t,
	MAX< int64_t >( value, 0 ) )
HLOD_SET_VALUE(
	setEstimatedGpuBytes,
	m_nEstimatedGpuBytes,
	int64_t,
	MAX< int64_t >( value, 0 ) )
HLOD_SET_VALUE(
	setBuiltUnix,
	m_nBuiltUnix,
	int64_t,
	MAX< int64_t >( value, 0 ) )

void Terrain3DHLODPackage::setMesh(
	const Ref< ArrayMesh > &rMesh ) {
	if ( m_mesh == rMesh ) {
		return;
	}
	m_mesh = rMesh;
	emit_changed();
}

String Terrain3DHLODPackage::getVtMaterialOwnerIdentityCold(
	void ) const {
	const String package_path = get_path();
	if ( !package_path.is_empty() ) {
		return String( "hlod-package:" ) + package_path;
	}
	return String( "hlod-level:" ) + String::num_int64( m_nLevel ) + String( ":cluster:" ) +
		   String::num_int64( m_clusterLocation.x ) + String( "," ) + String::num_int64( m_clusterLocation.y ) +
		   String( ":source:" ) + String::num_int64( m_nSourceSignature );
}

bool Terrain3DHLODPackage::ensureRenderingMaterialStableKeysCold(
	void ) {
	const String owner = getVtMaterialOwnerIdentityCold();
	for ( int64_t iMaterial = 0; iMaterial < m_renderingMaterials.size(); ++iMaterial ) {
		const Ref< Terrain3DRenderingMaterial > material = m_renderingMaterials[iMaterial];
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

void Terrain3DHLODPackage::setRenderingMaterials(
	const TypedArray< Terrain3DRenderingMaterial > &rMaterials ) {
	if ( m_renderingMaterials == rMaterials ) {
		return;
	}
	m_renderingMaterials = rMaterials;
	ensureRenderingMaterialStableKeysCold();
	emit_changed();
}

bool Terrain3DHLODPackage::captureRenderingMaterialsCold(
	void ) {
	if ( m_mesh.is_null() ) {
		return false;
	}
	TypedArray< Terrain3DRenderingMaterial > materials;
	materials.resize( m_mesh->get_surface_count() );
	for ( int surface = 0; surface < m_mesh->get_surface_count(); ++surface ) {
		Ref< Terrain3DRenderingMaterial > renderingMaterial;
		renderingMaterial.instantiate();
		const Ref< Material > sourceMaterial = m_mesh->surface_get_material( surface );
		if ( sourceMaterial.is_valid() && !renderingMaterial->importBaseMaterialCold( sourceMaterial ) ) {
			return false;
		}
		if ( sourceMaterial.is_valid() && !sourceMaterial->get_path().is_empty() &&
			 !sourceMaterial->get_path().contains( "::" ) ) {
			renderingMaterial->setVtStableKey(
				t3d_vt_material_stable_key_from_resource_path( sourceMaterial->get_path() ) );
		} else if ( !renderingMaterial->ensureVtStableKeyCold( getVtMaterialOwnerIdentityCold(),
															   String( "surface:" ) + String::num_int64( surface ) ) ) {
			return false;
		}
		materials[surface] = renderingMaterial;
	}
	setRenderingMaterials( materials );
	return true;
}

bool Terrain3DHLODPackage::hasCanonicalRenderingMaterials( void ) const
{
    if ( m_mesh.is_null() || !( isRuntimeCookedMesh() ||
         ( ( m_nFormatVersion == 3 || m_nFormatVersion == 4 ) &&
             m_nPayloadType == T3D_HLOD_INSTANCE_GROUP ) ) ) {
        return false;
    }
    int64_t surfaceCount = m_mesh->get_surface_count();
    if ( m_nFormatVersion == 4 ) {
        surfaceCount = 0;
        for ( int i = 0; i < m_prototypeMeshes.size(); ++i ) {
            const Ref< ArrayMesh > mesh = m_prototypeMeshes[i];
            if ( mesh.is_null() ) { return false; }
            surfaceCount += mesh->get_surface_count();
        }
    }
    if ( m_renderingMaterials.size() != surfaceCount ) { return false; }
    for ( int i = 0; i < m_renderingMaterials.size(); ++i ) {
        const Ref< Terrain3DRenderingMaterial > material = m_renderingMaterials[i];
        if ( material.is_null() || !material->isOneSubmitCompatible() ) { return false; }
    }
    return true;
}

const Terrain3DRenderingMaterial *Terrain3DHLODPackage::getRenderingMaterialPtr(
	const uint32_t nSurface ) const {
	if ( nSurface >= ( uint32_t ) ( m_renderingMaterials.size() ) ) {
		return NULL;
	}
	const Ref< Terrain3DRenderingMaterial > material = m_renderingMaterials[( int64_t ) ( nSurface )];
	return material.ptr();
}

void Terrain3DHLODPackage::_bind_methods(
    void ) {
    ClassDB::bind_method( D_METHOD( "is_runtime_cooked_mesh" ), &Terrain3DHLODPackage::isRuntimeCookedMesh );
    ClassDB::bind_method( D_METHOD( "is_runtime_instance_group" ), &Terrain3DHLODPackage::isRuntimeInstanceGroup );
    T3D_BIND_PROPERTY( Terrain3DHLODPackage, setFormatVersion, getFormatVersion, "version",
        PropertyInfo( Variant::INT, "format_version", PROPERTY_HINT_RANGE, "1,4,1" ), "set_format_version", "get_format_version" );
    T3D_BIND_PROPERTY( Terrain3DHLODPackage, setPayloadType, getPayloadType, "type",
        PropertyInfo( Variant::INT, "payload_type", PROPERTY_HINT_ENUM, "Cooked Mesh,Instance Group" ), "set_payload_type", "get_payload_type" );
    T3D_BIND_PROPERTY( Terrain3DHLODPackage, setLayerType, getLayerType, "type",
        PropertyInfo( Variant::INT, "layer_type", PROPERTY_HINT_ENUM, "Legacy Simplified,Instancing,Merged,Simplified" ), "set_layer_type", "get_layer_type" );
    T3D_BIND_PROPERTY( Terrain3DHLODPackage, setRecipeVersion, getRecipeVersion, "version",
        PropertyInfo( Variant::INT, "recipe_version" ), "set_recipe_version", "get_recipe_version" );
    T3D_BIND_PROPERTY( Terrain3DHLODPackage, setRecipeHash, getRecipeHash, "hash",
        PropertyInfo( Variant::INT, "recipe_hash" ), "set_recipe_hash", "get_recipe_hash" );
    T3D_BIND_PROPERTY( Terrain3DHLODPackage, setPrototypeMeshes, getPrototypeMeshes, "prototypes",
        PropertyInfo( Variant::ARRAY, "prototype_meshes", PROPERTY_HINT_ARRAY_TYPE, "ArrayMesh", PROPERTY_USAGE_STORAGE ),
        "set_prototype_meshes", "get_prototype_meshes" );
    T3D_BIND_PROPERTY( Terrain3DHLODPackage, setInstanceGroupPayload, getInstanceGroupPayload, "payload",
        PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instance_group_payload", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
        "set_instance_group_payload", "get_instance_group_payload" );
	ClassDB::bind_method( D_METHOD( "capture_rendering_materials" ),
						  &Terrain3DHLODPackage::captureRenderingMaterialsCold );
	ClassDB::bind_method( D_METHOD( "has_canonical_rendering_materials" ),
						  &Terrain3DHLODPackage::hasCanonicalRenderingMaterials );

	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setLevel,
					   getLevel,
					   "level",
					   PropertyInfo( Variant::INT, "level", PROPERTY_HINT_RANGE, "-1,6,1" ),
					   "set_level",
					   "get_level" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setClusterLocation,
					   getClusterLocation,
					   "location",
					   PropertyInfo( Variant::VECTOR2I, "cluster_location" ),
					   "set_cluster_location",
					   "get_cluster_location" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setRegionMin,
					   getRegionMin,
					   "location",
					   PropertyInfo( Variant::VECTOR2I, "region_min" ),
					   "set_region_min",
					   "get_region_min" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setRegionMax,
					   getRegionMax,
					   "location",
					   PropertyInfo( Variant::VECTOR2I, "region_max" ),
					   "set_region_max",
					   "get_region_max" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setLocalBounds,
					   getLocalBounds,
					   "bounds",
					   PropertyInfo( Variant::AABB, "local_bounds" ),
					   "set_local_bounds",
					   "get_local_bounds" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setMesh,
					   getMesh,
					   "mesh",
					   PropertyInfo( Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh" ),
					   "set_mesh",
					   "get_mesh" );
	T3D_BIND_PROPERTY(
		Terrain3DHLODPackage,
		setRenderingMaterials,
		getRenderingMaterials,
		"materials",
		PropertyInfo( Variant::ARRAY, "rendering_materials", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DRenderingMaterial" ),
		"set_rendering_materials",
		"get_rendering_materials" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setGeometricError,
					   getGeometricError,
					   "error",
					   PropertyInfo( Variant::FLOAT, "geometric_error" ),
					   "set_geometric_error",
					   "get_geometric_error" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setSourceSignature,
					   getSourceSignature,
					   "signature",
					   PropertyInfo( Variant::INT, "source_signature" ),
					   "set_source_signature",
					   "get_source_signature" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setBuildSignature,
					   getBuildSignature,
					   "signature",
					   PropertyInfo( Variant::INT, "build_signature" ),
					   "set_build_signature",
					   "get_build_signature" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setSourceTriangles,
					   getSourceTriangles,
					   "count",
					   PropertyInfo( Variant::INT, "source_triangles" ),
					   "set_source_triangles",
					   "get_source_triangles" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setProxyTriangles,
					   getProxyTriangles,
					   "count",
					   PropertyInfo( Variant::INT, "proxy_triangles" ),
					   "set_proxy_triangles",
					   "get_proxy_triangles" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setEstimatedGpuBytes,
					   getEstimatedGpuBytes,
					   "bytes",
					   PropertyInfo( Variant::INT, "estimated_gpu_bytes" ),
					   "set_estimated_gpu_bytes",
					   "get_estimated_gpu_bytes" );
	T3D_BIND_PROPERTY( Terrain3DHLODPackage,
					   setBuiltUnix,
					   getBuiltUnix,
					   "unix_time",
					   PropertyInfo( Variant::INT, "built_unix" ),
					   "set_built_unix",
					   "get_built_unix" );
}

// -------- Consolidated from hlod/format/terrain_3d_hlod_manifest.cpp --------
bool Terrain3DHLODManifest::compileRuntimeBlob(
	void ) {
	PackedByteArray compiled = Terrain3DHLODRuntimeCodec::encode( m_entries,
																  m_instancerMinLevels,
																  m_nodeRegionIndices,
																  m_nodeSpawnIds,
																  m_nodeMinLevels, m_instancerSubcellMasks, m_nodeSubcellMasks );
	if ( compiled.is_empty() ) {
		return false;
	}
	m_runtimeBlob = compiled;
	emit_changed();
	return true;
}

bool Terrain3DHLODManifest::validateRuntimeBlob(
	void ) const {
	if ( m_runtimeBlob.is_empty() ) {
		return false;
	}
	t3dHLODRuntimeData_t decoded;
	if ( !Terrain3DHLODRuntimeCodec::decode( m_runtimeBlob, decoded ) ) {
		return false;
	}
	return decoded.m_entries.size() == ( size_t ) m_entries.size();
}

void Terrain3DHLODManifest::_bind_methods(
	void ) {
	ClassDB::bind_method( D_METHOD( "compile_runtime_blob" ), &Terrain3DHLODManifest::compileRuntimeBlob );
	ClassDB::bind_method( D_METHOD( "validate_runtime_blob" ), &Terrain3DHLODManifest::validateRuntimeBlob );
	ClassDB::bind_method( D_METHOD( "get_entry_count" ), &Terrain3DHLODManifest::getEntryCount );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setFormat,
					   getFormat,
					   "value",
					   PropertyInfo( Variant::INT, "format" ),
					   "set_format",
					   "get_format" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setMaxLevel,
					   getMaxLevel,
					   "value",
					   PropertyInfo( Variant::INT, "max_level" ),
					   "set_max_level",
					   "get_max_level" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setBuildSignature,
					   getBuildSignature,
					   "value",
					   PropertyInfo( Variant::INT, "build_signature" ),
					   "set_build_signature",
					   "get_build_signature" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setBuiltUnix,
					   getBuiltUnix,
					   "value",
					   PropertyInfo( Variant::INT, "built_unix" ),
					   "set_built_unix",
					   "get_built_unix" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setEntries,
					   getEntries,
					   "value",
					   PropertyInfo( Variant::ARRAY, "entries" ),
					   "set_entries",
					   "get_entries" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setInstancerMinLevels,
					   getInstancerMinLevels,
					   "value",
					   PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instancer_min_levels" ),
					   "set_instancer_min_levels",
					   "get_instancer_min_levels" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
                       setInstancerSubcellMasks,
                       getInstancerSubcellMasks,
                       "value", PropertyInfo( Variant::PACKED_BYTE_ARRAY, "instancer_subcell_masks" ),
                       "set_instancer_subcell_masks", "get_instancer_subcell_masks" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setNodeRegionIndices,
					   getNodeRegionIndices,
					   "value",
					   PropertyInfo( Variant::PACKED_INT32_ARRAY, "node_region_indices" ),
					   "set_node_region_indices",
					   "get_node_region_indices" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setNodeSpawnIds,
					   getNodeSpawnIds,
					   "value",
					   PropertyInfo( Variant::PACKED_INT64_ARRAY, "node_spawn_ids" ),
					   "set_node_spawn_ids",
					   "get_node_spawn_ids" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
                       setNodeSubcellMasks,
                       getNodeSubcellMasks,
                       "value", PropertyInfo( Variant::PACKED_BYTE_ARRAY, "node_subcell_masks" ),
                       "set_node_subcell_masks", "get_node_subcell_masks" );
	T3D_BIND_PROPERTY( Terrain3DHLODManifest,
					   setNodeMinLevels,
					   getNodeMinLevels,
					   "value",
					   PropertyInfo( Variant::PACKED_BYTE_ARRAY, "node_min_levels" ),
					   "set_node_min_levels",
					   "get_node_min_levels" );
	T3D_BIND_PROPERTY(
		Terrain3DHLODManifest,
		setRuntimeBlob,
		getRuntimeBlob,
		"value",
		PropertyInfo( Variant::PACKED_BYTE_ARRAY, "runtime_blob", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_runtime_blob",
		"get_runtime_blob" );
}

void Terrain3DHLODPackage::setFormatVersion( int nVersion )
{
    const int nNext = MAX( nVersion, 1 );
    if ( m_nFormatVersion == nNext ) { return; }
    m_nFormatVersion = nNext;
    emit_changed();
}
void Terrain3DHLODPackage::setPayloadType( int nType )
{
    const int nNext = MAX( nType, 0 );
    if ( m_nPayloadType == nNext ) { return; }
    m_nPayloadType = nNext;
    emit_changed();
}
void Terrain3DHLODPackage::setLayerType( int nType )
{
    const int nNext = CLAMP( nType, 0, 3 );
    if ( m_nLayerType == nNext ) { return; }
    m_nLayerType = nNext;
    emit_changed();
}
void Terrain3DHLODPackage::setRecipeVersion( int nVersion )
{
    const int nNext = MAX( nVersion, 0 );
    if ( m_nRecipeVersion == nNext ) { return; }
    m_nRecipeVersion = nNext;
    emit_changed();
}
void Terrain3DHLODPackage::setRecipeHash( int64_t nHash )
{
    if ( m_nRecipeHash == nHash ) { return; }
    m_nRecipeHash = nHash;
    emit_changed();
}
void Terrain3DHLODPackage::setInstanceGroupPayload( const PackedByteArray &rPayload )
{
    if ( m_instanceGroupPayload == rPayload ) { return; }
    m_instanceGroupPayload = rPayload;
    emit_changed();
}
bool Terrain3DHLODPackage::isRuntimeCookedMesh( void ) const
{
    return ( m_nFormatVersion == 1 || m_nFormatVersion == 2 ) &&
           m_nPayloadType == T3D_HLOD_COOKED_MESH && m_mesh.is_valid();
}

// v3 retains the original single prototype; v4 provides an ordered table of
// independent prototype resources and one flattened canonical material table.
void Terrain3DHLODPackage::setPrototypeMeshes( const TypedArray< ArrayMesh > &rMeshes )
{
    if ( m_prototypeMeshes == rMeshes ) { return; }
    m_prototypeMeshes = rMeshes;
    emit_changed();
}

Ref< ArrayMesh > Terrain3DHLODPackage::getPrototypeMesh( const uint32_t nPrototype ) const
{
    if ( m_nFormatVersion == 4 ) {
        return nPrototype < (uint32_t) m_prototypeMeshes.size() ?
            (Ref< ArrayMesh >) m_prototypeMeshes[(int64_t) nPrototype] : Ref< ArrayMesh >();
    }
    return nPrototype == 0u ? m_mesh : Ref< ArrayMesh >();
}

uint32_t Terrain3DHLODPackage::getPrototypeMaterialOffset( const uint32_t nPrototype ) const
{
    uint32_t offset = 0u;
    if ( m_nFormatVersion == 4 ) {
        for ( uint32_t i = 0u; i < nPrototype && i < (uint32_t) m_prototypeMeshes.size(); ++i ) {
            const Ref< ArrayMesh > mesh = m_prototypeMeshes[(int64_t) i];
            if ( mesh.is_null() ) { return UINT32_MAX; }
            offset += (uint32_t) mesh->get_surface_count();
        }
    }
    return offset;
}

bool Terrain3DHLODPackage::isRuntimeInstanceGroup( void ) const
{
    t3dHLODGroupData_t decoded;
    return Terrain3DHLODInstanceCodec::decode( m_instanceGroupPayload, decoded ) &&
        validateRuntimeInstanceGroupDecodedCold( decoded );
}

bool Terrain3DHLODPackage::validateRuntimeInstanceGroupDecodedCold(
    const t3dHLODGroupData_t &decoded ) const
{
    if ( ( m_nFormatVersion != 3 && m_nFormatVersion != 4 ) ||
         m_nPayloadType != T3D_HLOD_INSTANCE_GROUP ||
         m_nLayerType != T3D_HLOD_LAYER_INSTANCING || m_mesh.is_null() ||
         decoded.m_prototypes.empty() || decoded.m_instances.empty() ||
         ( m_nFormatVersion == 3 && decoded.m_prototypes.size() != 1u ) ||
         ( m_nFormatVersion == 4 && decoded.m_prototypes.size() != (size_t) m_prototypeMeshes.size() ) ) {
        return false;
    }
    if ( m_nFormatVersion == 4 && getPrototypeMesh( 0u ) != m_mesh ) {
        return false;
    }
    eastl::vector< uint8_t > used;
    used.resize( decoded.m_prototypes.size(), 0u );
    for ( size_t i = 0u; i < decoded.m_instances.size(); ++i ) {
        const uint32_t index = decoded.m_instances[i].m_nPrototype;
        if ( index >= used.size() ) { return false; }
        used[index] = 1u;
    }
    for ( size_t i = 0u; i < used.size(); ++i ) {
        if ( used[i] == 0u ) { return false; }
    }
    uint32_t surfaces = 0u;
    uint64_t packageBytes = 0u;
    for ( uint32_t i = 0u; i < (uint32_t) decoded.m_prototypes.size(); ++i ) {
        const Ref< ArrayMesh > mesh = getPrototypeMesh( i );
        if ( mesh.is_null() || mesh->get_surface_count() <= 0 ) { return false; }
        const t3dHLODGroupPrototype_t &prototype = decoded.m_prototypes[i];
        if ( prototype.m_nVertexBytes == 0u || prototype.m_nIndexBytes == 0u ) { return false; }
        uint64_t vertexBytes = 0u;
        uint64_t indexBytes = 0u;
        for ( int surface = 0; surface < mesh->get_surface_count(); ++surface ) {
            const Array arrays = mesh->surface_get_arrays( surface );
            const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
            const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
            const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
            const PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
            const PackedVector2Array uv0 = arrays[Mesh::ARRAY_TEX_UV];
            const PackedVector2Array uv1 = arrays[Mesh::ARRAY_TEX_UV2];
            const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
            vertexBytes += (uint64_t) vertices.size() * 12u + (uint64_t) normals.size() * 12u;
            vertexBytes += (uint64_t) tangents.size() * 4u + (uint64_t) colors.size() * 16u;
            vertexBytes += (uint64_t) uv0.size() * 8u + (uint64_t) uv1.size() * 8u;
            indexBytes += (uint64_t) indices.size() * 4u;
        }
        if ( vertexBytes != prototype.m_nVertexBytes || indexBytes != prototype.m_nIndexBytes ) {
            return false;
        }
        packageBytes += vertexBytes + indexBytes;
        const AABB bounds = mesh->get_aabb();
        const float expected[6] = { (float) bounds.position.x, (float) bounds.position.y,
            (float) bounds.position.z, (float) bounds.size.x, (float) bounds.size.y, (float) bounds.size.z };
        for ( int k = 0; k < 6; ++k ) {
            if ( Math::abs( expected[k] - prototype.m_fBounds[k] ) > 0.001f ) { return false; }
        }
        surfaces += (uint32_t) mesh->get_surface_count();
    }
    // A v4 manifest must not understate physical prototype residency.
    if ( m_nFormatVersion == 4 && (uint64_t) MAX< int64_t >( m_nEstimatedGpuBytes, 0 ) < packageBytes ) {
        return false;
    }
    if ( surfaces != (uint32_t) m_renderingMaterials.size() ) { return false; }
    for ( int i = 0; i < m_renderingMaterials.size(); ++i ) {
        const Ref< Terrain3DRenderingMaterial > material = m_renderingMaterials[i];
        if ( material.is_null() || !material->isOneSubmitCompatible() ) { return false; }
    }
    return true;
}
