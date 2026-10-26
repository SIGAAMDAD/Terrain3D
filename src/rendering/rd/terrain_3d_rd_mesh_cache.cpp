// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_mesh_cache.h"

#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "assets/terrain_3d_rendering_material.h"
#include "data/terrain_3d_half_buffer.h"
#include "core/logger.h"

#include <EASTL/vector.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static uint64_t t3dRdHashU64( uint64_t nHash, const uint64_t nValue )
{
	for ( uint32_t i = 0u; i < 8u; ++i ) {
		nHash ^= ( nValue >> ( i * 8u ) ) & 0xffu;
		nHash *= s_nFnvPrime;
	}
	return nHash;
}

static uint64_t t3dRdObjectId( const Object *const pObject )
{
	return pObject ? (uint64_t)( pObject->get_instance_id() ) : 0u;
}

static uint64_t t3dRdHashF32( uint64_t nHash, const float fValue )
{
	uint32_t bits = 0u;
	static_assert( sizeof( bits ) == sizeof( fValue ), "float hash ABI changed" );
	memcpy( &bits, &fValue, sizeof( bits ) );
	return t3dRdHashU64( nHash, bits );
}

static uint64_t t3dRdHashVector3( uint64_t nHash, const Vector3& rValue )
{
	nHash = t3dRdHashF32( nHash, (float)( rValue.x ) );
	nHash = t3dRdHashF32( nHash, (float)( rValue.y ) );
	return t3dRdHashF32( nHash, (float)( rValue.z ) );
}

static uint64_t t3dRdHashColor( uint64_t nHash, const Color& rValue )
{
	nHash = t3dRdHashF32( nHash, (float)( rValue.r ) );
	nHash = t3dRdHashF32( nHash, (float)( rValue.g ) );
	nHash = t3dRdHashF32( nHash, (float)( rValue.b ) );
	return t3dRdHashF32( nHash, (float)( rValue.a ) );
}

static uint64_t t3dRdHashMemory( uint64_t nHash, const void *const pData, const size_t nBytes )
{
	const uint8_t *const pBytes = (const uint8_t *const)( pData );
	for ( size_t iByte = 0u; iByte < nBytes; iByte++ ) {
		nHash ^= pBytes[ iByte ];
		nHash *= s_nFnvPrime;
	}
	return nHash;
}

static uint64_t t3dRdHashTexture( uint64_t nHash, const Ref<Texture2D>& rTexture )
{
	nHash = t3dRdHashU64( nHash, t3dRdObjectId( rTexture.ptr() ) );
	if ( rTexture.is_null() ) {
		return nHash;
	}
	const RID texture_rid = rTexture->get_rid();
	nHash = t3dRdHashU64( nHash, (uint64_t)( texture_rid.get_id() ) );
	nHash = t3dRdHashU64( nHash, (uint64_t)( rTexture->get_width() ) );
	return t3dRdHashU64( nHash, (uint64_t)( rTexture->get_height() ) );
}

static uint64_t t3dRdHashRenderingMaterial( uint64_t nHash, const Terrain3DRenderingMaterial *const pMaterial )
{
	nHash = t3dRdHashU64( nHash, t3dRdObjectId( pMaterial ) );
	if ( !pMaterial ) {
		return nHash;
	}
	nHash = t3dRdHashU64( nHash, pMaterial->getVtStableKey() );
	t3dRenderingMaterialVtBindings_t vtBindings;
	vtBindings.m_nVtAllocation = T3D_VT_INVALID_ALLOCATION;
	vtBindings.m_nOverlayVtAllocation = T3D_VT_INVALID_ALLOCATION;
	t3dRenderingMaterialGpu_t packed;
	pMaterial->compileVtRecordCold( vtBindings, packed );
	nHash = t3dRdHashMemory( nHash, &packed, sizeof( packed ) );
	const Vector2 uv_scale = pMaterial->getUvScale();
	const Vector2 uv_offset = pMaterial->getUvOffset();
	nHash = t3dRdHashF32( nHash, (float)( uv_scale.x ) );
	nHash = t3dRdHashF32( nHash, (float)( uv_scale.y ) );
	nHash = t3dRdHashF32( nHash, (float)( uv_offset.x ) );
	nHash = t3dRdHashF32( nHash, (float)( uv_offset.y ) );
	nHash = t3dRdHashTexture( nHash, pMaterial->getAlbedoTexture() );
	nHash = t3dRdHashTexture( nHash, pMaterial->getNormalTexture() );
	nHash = t3dRdHashTexture( nHash, pMaterial->getSurfaceTexture() );
	return t3dRdHashTexture( nHash, pMaterial->getHeightTexture() );
}

static uint64_t t3dRdHashRenderingMaterial( uint64_t nHash, const Ref<Terrain3DRenderingMaterial>& rMaterial )
{
	return t3dRdHashRenderingMaterial( nHash, rMaterial.ptr() );
}

static int16_t t3dRdPackSnorm16( const float fValue )
{
	return (int16_t)( lround( CLAMP( fValue, -1.0f, 1.0f ) * 32767.0f ) );
}

static uint32_t t3dRdPackWorldSnorm10( const float fValue )
{
	const int32_t iValue = (int32_t)lround( CLAMP( fValue, -1.0f, 1.0f ) * 511.0f );
	return (uint32_t)iValue & 0x3ffu;
}

static uint32_t t3dRdPackWorldNormal( const Vector3& rNormal )
{
	Vector3 n = rNormal.length_squared() > 0.000001f ? rNormal.normalized() : Vector3( 0.0f, 1.0f, 0.0f );
	return t3dRdPackWorldSnorm10( (float)n.x ) | ( t3dRdPackWorldSnorm10( (float)n.y ) << 10u ) | ( t3dRdPackWorldSnorm10( (float)n.z ) << 20u );
}

static uint32_t t3dRdPackWorldTangent( const float fX, const float fY, const float fZ, const float fW )
{
	return t3dRdPackWorldSnorm10( fX ) | ( t3dRdPackWorldSnorm10( fY ) << 10u ) | ( t3dRdPackWorldSnorm10( fZ ) << 20u ) | ( fW < 0.0f ? ( 1u << 30u ) : 0u );
}

static bool t3dRdBuildChunkHashes( const PackedByteArray& rBytes, eastl::vector<uint64_t, Terrain3DEastlAllocator>& rHashes )
{
	const uint32_t size = (uint32_t)( rBytes.size() );
	const uint32_t count = size == 0u ? 0u : ( size + s_nDirtyUploadChunkBytes - 1u ) / s_nDirtyUploadChunkBytes;
	if ( !t3dVectorResizeCold( rHashes, count ) ) {
		return false;
	}
	const uint8_t *const pBytes = rBytes.ptr();
	for ( uint32_t chunk = 0u; chunk < count; ++chunk ) {
		const uint32_t begin = chunk * s_nDirtyUploadChunkBytes;
		const uint32_t end = MIN( size, begin + s_nDirtyUploadChunkBytes );
		uint64_t hash = s_nFnvOffset;
		for ( uint32_t i = begin; i < end; ++i ) {
			hash ^= pBytes[ i ];
			hash *= s_nFnvPrime;
		}
		rHashes[ chunk ] = hash;
	}
	return true;
}

static bool t3dRdAddRenderingMaterial( const Terrain3DRenderingMaterial *const pMaterial,
                                       eastl::vector<t3dRdInstanceMaterial_t, Terrain3DEastlAllocator>& rMaterials,
                                       eastl::vector<t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator>& rVtMaterials,
                                       eastl::vector<uint64_t, Terrain3DEastlAllocator>& rMaterialKeys,
                                       eastl::vector<uint64_t, Terrain3DEastlAllocator>& rVtStableKeys,
                                       uint16_t& rMaterialIndex,
                                       String *const pError )
{
	rMaterialIndex = UINT16_MAX;
	if ( !pMaterial ) {
		return true;
	}
	if ( rMaterials.size() >= (size_t)( UINT16_MAX ) ) {
		if ( pError ) {
			*pError = "RD material table exceeds 16-bit material-index capacity";
		}
		return false;
	}
	t3dRdInstanceMaterial_t packed;
	if ( !pMaterial->compileInstanceRecordCold( packed ) ) {
		if ( pError ) {
			*pError = "RD canonical material could not compile its instance record";
		}
		return false;
	}
	uint64_t key = t3dRdHashRenderingMaterial( s_nFnvOffset, pMaterial );
	if ( key == 0u ) {
		key = 1u;
	}
	const uint64_t nStableKey = pMaterial->getVtStableKey();
	if ( nStableKey == 0u ) {
		if ( pError ) {
			*pError = "RD canonical material has no cook-stable VT material key";
		}
		return false;
	}
	for ( uint16_t iMaterial = 0u; iMaterial < (uint16_t)( rMaterials.size() ); ++iMaterial ) {
		if ( iMaterial >= rVtStableKeys.size() || rVtStableKeys[ iMaterial ] != nStableKey ) {
			continue;
		}
		const bool bSameFingerprint = iMaterial < rMaterialKeys.size() && rMaterialKeys[ iMaterial ] == key;
		const bool bSamePacked = memcmp( &rMaterials[ iMaterial ], &packed, sizeof( packed ) ) == 0;
		if ( !bSameFingerprint || !bSamePacked ) {
			if ( pError ) {
				*pError = String( "VT stable material key collision; key=" ) + String::num_uint64( nStableKey );
			}
			return false;
		}
		rMaterialIndex = iMaterial;
		return true;
	}
	const uint16_t material_index = (uint16_t)( rMaterials.size() );

	t3dRenderingMaterialVtBindings_t vt_bindings;
	vt_bindings.m_nVtAllocation = T3D_VT_INVALID_ALLOCATION;
	vt_bindings.m_nOverlayVtAllocation = T3D_VT_INVALID_ALLOCATION;
	t3dRenderingMaterialGpu_t vtPacked;

	if ( !pMaterial->compileVtRecordCold( vt_bindings, vtPacked ) ) {
		if ( pError ) {
			*pError = "RD canonical material could not compile its VT record";
		}
		return false;
	}
	const size_t next_capacity = rMaterials.capacity() ? rMaterials.capacity() + rMaterials.capacity() / 2u + 1u : 16u;
	if ( rMaterials.size() == rMaterials.capacity() && !t3dContainerReserveCold( rMaterials, next_capacity ) ) {
		return false;
	}
	if ( rVtMaterials.size() == rVtMaterials.capacity() && !t3dContainerReserveCold( rVtMaterials, next_capacity ) ) {
		return false;
	}
	if ( rMaterialKeys.size() == rMaterialKeys.capacity() && !t3dContainerReserveCold( rMaterialKeys, next_capacity ) ) {
		return false;
	}
	if ( rVtStableKeys.size() == rVtStableKeys.capacity() && !t3dContainerReserveCold( rVtStableKeys, next_capacity ) ) {
		return false;
	}
	if ( !t3dVectorPushBackWithinCapacity( rMaterials, packed ) ) {
		return false;
	}
	if ( !t3dVectorPushBackWithinCapacity( rVtMaterials, vtPacked ) ) {
		rMaterials.pop_back();
		return false;
	}
	if ( !t3dVectorPushBackWithinCapacity( rMaterialKeys, key ) ) {
		rVtMaterials.pop_back();
		rMaterials.pop_back();
		return false;
	}
	if ( !t3dVectorPushBackWithinCapacity( rVtStableKeys, nStableKey ) ) {
		rMaterialKeys.pop_back();
		rVtMaterials.pop_back();
		rMaterials.pop_back();
		return false;
	}
	rMaterialIndex = material_index;
	return true;
}

static bool t3dRdAddRenderingMaterial( const Ref<Terrain3DRenderingMaterial>& rMaterial,
                                       eastl::vector<t3dRdInstanceMaterial_t, Terrain3DEastlAllocator>& rMaterials,
                                       eastl::vector<t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator>& rVtMaterials,
                                       eastl::vector<uint64_t, Terrain3DEastlAllocator>& rMaterialKeys,
                                       eastl::vector<uint64_t, Terrain3DEastlAllocator>& rVtStableKeys,
                                       uint16_t& rMaterialIndex,
                                       String *const pError )
{
	return t3dRdAddRenderingMaterial( rMaterial.ptr(), rMaterials, rVtMaterials, rMaterialKeys, rVtStableKeys, rMaterialIndex, pError );
}

static bool t3dRdAppendBytes( PackedByteArray& rDestination, const void *const pSource, const uint32_t nBytes )
{
	if ( nBytes == 0u ) {
		return true;
	}
	const int64_t old_size = rDestination.size();
	if ( old_size > (int64_t)(UINT32_MAX)-nBytes ) {
		return false;
	}
	rDestination.resize( old_size + nBytes );
	if ( rDestination.size() != old_size + nBytes ) {
		return false;
	}
	memcpy( rDestination.ptrw() + old_size, pSource, nBytes );
	return true;
}

static void t3dRdSetError( String *const pError, const String& rError )
{
	if ( pError ) {
		*pError = rError;
	}
}

static bool t3dRdAppendMeshSource( const Ref<Mesh>& rMesh,
                                   const uint64_t nSourceKey,
                                   const uint16_t nAssetId,
                                   const int16_t nLod,
                                   const uint16_t nOverrideMaterial,
                                   const uint16_t nOverlayMaterial,
                                   t3dRdMeshSourceSnapshot_t& rResult,
                                   String *const pError,
                                   const t3dRdExternalMeshSource_t *const pExternalSource = NULL )
{
	if ( rMesh.is_null() || rMesh->get_surface_count() <= 0 ) {
		return true;
	}
	if ( nSourceKey == 0u ) {
		t3dRdSetError( pError, "RD static mesh source key is invalid" );
		return false;
	}
	ArrayMesh *const pArrayMesh = Object::cast_to<ArrayMesh>( rMesh.ptr() );
	if ( !pArrayMesh ) {
		t3dRdSetError( pError, "RD static mesh source is not an ArrayMesh" );
		return false;
	}
	const bool bOcclusionProxySource = t3dRdIsOcclusionProxySourceKey( nSourceKey );
	const size_t surface_capacity = rResult.m_surfaces.size() + (size_t)( rMesh->get_surface_count() );
	if ( !t3dContainerReserveCold( rResult.m_surfaces, surface_capacity ) || !t3dContainerReserveCold( rResult.m_lods, rResult.m_lods.size() + 1u ) ) {
		t3dRdSetError( pError, "RD static mesh metadata capacity allocation failed" );
		return false;
	}
	t3dRdMeshSourceLod_t lod_record {};
	lod_record.m_nSourceKey = nSourceKey;
	lod_record.m_nAssetId = nAssetId;
	lod_record.m_nLod = nLod;
	lod_record.m_nFirstSurface = (uint32_t)( rResult.m_surfaces.size() );
	lod_record.m_bounds = rMesh->get_aabb();
	for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
		if ( pArrayMesh->surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		const Array arrays = rMesh->surface_get_arrays( surface );
		if ( arrays.size() <= Mesh::ARRAY_VERTEX || arrays[ Mesh::ARRAY_VERTEX ].get_type() != Variant::PACKED_VECTOR3_ARRAY ) {
			continue;
		}
		const PackedVector3Array positions = arrays[ Mesh::ARRAY_VERTEX ];
		if ( positions.is_empty() || positions.size() > (int64_t)( UINT32_MAX ) ) {
			continue;
		}
		const PackedVector3Array normals =
		    arrays.size() > Mesh::ARRAY_NORMAL && arrays[ Mesh::ARRAY_NORMAL ].get_type() == Variant::PACKED_VECTOR3_ARRAY
		        ? (PackedVector3Array)( arrays[ Mesh::ARRAY_NORMAL ] )
		        : PackedVector3Array();
		const PackedFloat32Array tangents =
		    arrays.size() > Mesh::ARRAY_TANGENT && arrays[ Mesh::ARRAY_TANGENT ].get_type() == Variant::PACKED_FLOAT32_ARRAY
		        ? (PackedFloat32Array)( arrays[ Mesh::ARRAY_TANGENT ] )
		        : PackedFloat32Array();
		const PackedColorArray colors =
		    arrays.size() > Mesh::ARRAY_COLOR && arrays[ Mesh::ARRAY_COLOR ].get_type() == Variant::PACKED_COLOR_ARRAY
		        ? (PackedColorArray)( arrays[ Mesh::ARRAY_COLOR ] )
		        : PackedColorArray();
		const PackedVector2Array uvs =
		    arrays.size() > Mesh::ARRAY_TEX_UV && arrays[ Mesh::ARRAY_TEX_UV ].get_type() == Variant::PACKED_VECTOR2_ARRAY
		        ? (PackedVector2Array)( arrays[ Mesh::ARRAY_TEX_UV ] )
		        : PackedVector2Array();
		const PackedVector2Array uv2s =
		    arrays.size() > Mesh::ARRAY_TEX_UV2 && arrays[ Mesh::ARRAY_TEX_UV2 ].get_type() == Variant::PACKED_VECTOR2_ARRAY
		        ? (PackedVector2Array)( arrays[ Mesh::ARRAY_TEX_UV2 ] )
		        : PackedVector2Array();
		const PackedInt32Array indices =
		    arrays.size() > Mesh::ARRAY_INDEX && arrays[ Mesh::ARRAY_INDEX ].get_type() == Variant::PACKED_INT32_ARRAY
		        ? (PackedInt32Array)( arrays[ Mesh::ARRAY_INDEX ] )
		        : PackedInt32Array();
		const uint32_t vertexCount = (uint32_t)( positions.size() );
		const uint32_t vertexOffset = (uint32_t)( rResult.m_vertexBytes.size() / sizeof( t3dRdMeshVertex_t ) );
		const uint32_t world_vertex_offset = bOcclusionProxySource ? 0u : (uint32_t)( rResult.m_worldVertices.size() );
		if ( !bOcclusionProxySource && !t3dContainerReserveCold( rResult.m_worldVertices, rResult.m_worldVertices.size() + vertexCount ) ) {
			t3dRdSetError( pError, "RD canonical world vertex capacity allocation failed" );
			return false;
		}
		for ( uint32_t vertex = 0u; vertex < vertexCount; ++vertex ) {
			t3dRdMeshVertex_t packed {};
			const Vector3 position = positions[ (int64_t)( vertex ) ];
			packed.m_szPosition[ 0 ] = (float)( position.x );
			packed.m_szPosition[ 1 ] = (float)( position.y );
			packed.m_szPosition[ 2 ] = (float)( position.z );
			packed.m_nColorRgba8 = vertex < (uint32_t)( colors.size() )
			                           ? t3dRdPackGodotRgba32ForGpu( colors[ (int64_t)( vertex ) ].to_rgba32() )
			                           : 0xffffffffu;
			const Vector3 normal = vertex < (uint32_t)( normals.size() ) ? normals[ (int64_t)( vertex ) ] : Vector3( 0, 1, 0 );
			packed.m_szNormal[ 0 ] = t3dRdPackSnorm16( (float)( normal.x ) );
			packed.m_szNormal[ 1 ] = t3dRdPackSnorm16( (float)( normal.y ) );
			packed.m_szNormal[ 2 ] = t3dRdPackSnorm16( (float)( normal.z ) );
			packed.m_szNormal[ 3 ] = 0;
			if ( tangents.size() >= (int64_t)( vertex + 1u ) * 4 ) {
				for ( uint32_t component = 0u; component < 4u; ++component ) {
					packed.m_szTangent[ component ] = t3dRdPackSnorm16( tangents[ (int64_t)( vertex * 4u + component ) ] );
				}
			}
			const Vector2 uv = vertex < (uint32_t)( uvs.size() ) ? uvs[ (int64_t)( vertex ) ] : Vector2();
			const Vector2 uv2 = vertex < (uint32_t)( uv2s.size() ) ? uv2s[ (int64_t)( vertex ) ] : Vector2();
			packed.m_szUv[ 0 ] = t3dHalfEncode( (float)( uv.x ) );
			packed.m_szUv[ 1 ] = t3dHalfEncode( (float)( uv.y ) );
			packed.m_szUv2[ 0 ] = t3dHalfEncode( (float)( uv2.x ) );
			packed.m_szUv2[ 1 ] = t3dHalfEncode( (float)( uv2.y ) );

			if ( !bOcclusionProxySource ) {
				t3dWorldVertex_t worldVertex {};
				worldVertex.m_fPosition[ 0 ] = (float)( position.x );
				worldVertex.m_fPosition[ 1 ] = (float)( position.y );
				worldVertex.m_fPosition[ 2 ] = (float)( position.z );
				worldVertex.m_nPackedNormal = t3dRdPackWorldNormal( normal );
				worldVertex.m_nPackedTangent = tangents.size() >= (int64_t)( vertex + 1u ) * 4
				                                   ? t3dRdPackWorldTangent( tangents[ (int64_t)( vertex * 4u + 0u ) ],
				                                                            tangents[ (int64_t)( vertex * 4u + 1u ) ],
				                                                            tangents[ (int64_t)( vertex * 4u + 2u ) ],
				                                                            tangents[ (int64_t)( vertex * 4u + 3u ) ] )
				                                   : t3dRdPackWorldTangent( 1.0f, 0.0f, 0.0f, 1.0f );
				worldVertex.m_nPackedUv0 = (uint32_t)packed.m_szUv[ 0 ] | ( (uint32_t)packed.m_szUv[ 1 ] << 16u );
				worldVertex.m_nPackedUv1 = (uint32_t)packed.m_szUv2[ 0 ] | ( (uint32_t)packed.m_szUv2[ 1 ] << 16u );
				worldVertex.m_nColorRgba8 = packed.m_nColorRgba8;
				if ( !t3dVectorPushBackWithinCapacity( rResult.m_worldVertices, worldVertex ) ) {
					t3dRdSetError( pError, "RD canonical world vertex append failed" );
					return false;
				}
			}
			if ( !t3dRdAppendBytes( rResult.m_vertexBytes, &packed, sizeof( packed ) ) ) {
				t3dRdSetError( pError, "RD static mesh vertex arena exceeds 32-bit address space" );
				return false;
			}
		}
		uint32_t max_index = vertexCount - 1u;
		const uint32_t index_count = indices.is_empty() ? vertexCount : (uint32_t)( indices.size() );
		if ( ( index_count % 3u ) != 0u ) {
			t3dRdSetError( pError, "RD static mesh has a non-triangle index count" );
			return false;
		}
		if ( !indices.is_empty() ) {
			max_index = 0u;
			for ( int64_t i = 0; i < indices.size(); ++i ) {
				if ( indices[ i ] < 0 || (uint32_t)( indices[ i ] ) >= vertexCount ) {
					t3dRdSetError( pError, "RD static mesh has an out-of-range index" );
					return false;
				}
				max_index = MAX( max_index, (uint32_t)( indices[ i ] ) );
			}
		}
		t3dRdMeshSourceSurface_t record {};
		record.m_nSourceKey = nSourceKey;
		record.m_nAssetId = nAssetId;
		record.m_nLod = nLod;
		record.m_nSurface = (uint16_t)( MIN( surface, (int)( UINT16_MAX ) ) );
		record.m_nVertexOffset = vertexOffset;
		record.m_nVertexCount = vertexCount;
		record.m_nIndexCount = index_count;
		record.m_nWorldVertexOffset = world_vertex_offset;
		record.m_nWorldVertexCount = bOcclusionProxySource ? 0u : vertexCount;
		bool bounds_initialized = false;
		AABB surface_bounds;
		for ( int64_t vertex = 0; vertex < positions.size(); ++vertex ) {
			const Vector3 position = positions[ vertex ];
			if ( !bounds_initialized ) {
				surface_bounds = AABB( position, Vector3() );
				bounds_initialized = true;
			} else {
				surface_bounds.expand_to( position );
			}
		}
		record.m_bounds = surface_bounds;
		const Terrain3DRenderingMaterial *const pRenderingMaterial = pExternalSource && pExternalSource->m_pResolveRenderingMaterial
		                                                                 ? pExternalSource->m_pResolveRenderingMaterial( pExternalSource->m_pMaterialContext, (uint32_t)( surface ) )
		                                                                 : NULL;
		if ( bOcclusionProxySource ) {
			record.m_nMaterial = UINT16_MAX;
			record.m_nOverlayMaterial = UINT16_MAX;
		} else if ( nOverrideMaterial != UINT16_MAX ) {
			record.m_nMaterial = nOverrideMaterial;
			record.m_nOverlayMaterial = nOverlayMaterial;
		} else {
			const bool require_canonical =
			    pExternalSource && ( pExternalSource->m_nFlags & T3D_RD_EXTERNAL_MESH_REQUIRE_RENDERING_MATERIAL ) != 0u;
			if ( require_canonical && !pRenderingMaterial ) {
				t3dRdSetError( pError,
				               nAssetId == UINT16_MAX ? "RD HLOD mesh surface is missing its canonical Terrain3DRenderingMaterial"
				                                      : "RD instance mesh surface is missing its canonical Terrain3DRenderingMaterial" );
				return false;
			}
			if ( !pRenderingMaterial ) {
				t3dRdSetError( pError, "RD mesh publication has no canonical Terrain3DRenderingMaterial" );
				return false;
			}
			if ( !t3dRdAddRenderingMaterial( pRenderingMaterial,
			                                 rResult.m_materials,
			                                 rResult.m_vtMaterials,
			                                 rResult.m_materialKeys,
			                                 rResult.m_vtStableKeys,
			                                 record.m_nMaterial,
			                                 pError ) ) {
				return false;
			}
			if ( record.m_nMaterial == UINT16_MAX ) {
				t3dRdSetError( pError,
				               nAssetId == UINT16_MAX ? "RD HLOD canonical material could not be compiled into the shared material table"
				                                      : "RD instance canonical material could not be compiled into the shared material table" );
				return false;
			}
			record.m_nOverlayMaterial = nOverlayMaterial;
		}
		const uint32_t base_flags = record.m_nMaterial == UINT16_MAX ? 0u : rResult.m_materials[ record.m_nMaterial ].m_nFlags;
		const uint32_t overlay_flags =
		    record.m_nOverlayMaterial == UINT16_MAX ? base_flags : rResult.m_materials[ record.m_nOverlayMaterial ].m_nFlags;
		const bool base_double = ( base_flags & T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED ) != 0u;
		const bool overlay_double = ( overlay_flags & T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED ) != 0u;
		const bool base_front = ( base_flags & T3D_RD_INSTANCE_MATERIAL_CULL_FRONT ) != 0u;
		const bool overlay_front = ( overlay_flags & T3D_RD_INSTANCE_MATERIAL_CULL_FRONT ) != 0u;
		if ( base_double || overlay_double || ( record.m_nOverlayMaterial != UINT16_MAX && base_front != overlay_front ) ) {
			record.m_nFlags |= T3D_RD_INSTANCE_DOUBLE_SIDED;
		} else if ( base_front ) {
			record.m_nFlags |= T3D_RD_INSTANCE_CULL_FRONT;
		}
		if ( ( ( base_flags | overlay_flags ) & T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND ) != 0u ) {
			record.m_nFlags |= T3D_RD_INSTANCE_ALPHA_BLEND;
		}
		if ( ( ( base_flags | overlay_flags ) & T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS ) != 0u ) {
			record.m_nFlags |= T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS;
		}
		(void)( max_index );
		record.m_nIndexOffset = (uint32_t)( rResult.m_index32Bytes.size() / sizeof( uint32_t ) );
		for ( uint32_t i = 0u; i < index_count; ++i ) {
			const uint32_t packed = indices.is_empty() ? i : (uint32_t)( indices[ (int64_t)( i ) ] );
			if ( !t3dRdAppendBytes( rResult.m_index32Bytes, &packed, sizeof( packed ) ) ) {
				return false;
			}
		}

		if ( !bOcclusionProxySource ) {
			const bool bTwoSided = ( record.m_nFlags & T3D_RD_INSTANCE_DOUBLE_SIDED ) != 0u;
			const uint32_t nRasterTriangleCount = ( index_count / 3u ) * ( bTwoSided ? 2u : 1u );
			const uint32_t nMaxNewClusters = MAX( 1u, ( nRasterTriangleCount + 20u ) / 21u );
			const size_t nOldVertexIndexCount = rResult.m_worldClusterVertexIndices.size();
			const size_t nOldTriangleWordCount = rResult.m_worldTriangleWords.size();
			const size_t nOldClusterCount = rResult.m_worldClusters.size();
			if ( !t3dContainerReserveCold( rResult.m_worldClusterVertexIndices, nOldVertexIndexCount + (size_t)nMaxNewClusters * 64u ) || !t3dContainerReserveCold( rResult.m_worldTriangleWords, nOldTriangleWordCount + (size_t)nMaxNewClusters * 124u ) || !t3dContainerReserveCold( rResult.m_worldClusters, nOldClusterCount + nMaxNewClusters ) || !t3dContainerReserveCold( rResult.m_worldClusterVtMetadata, nOldClusterCount + nMaxNewClusters ) ) {
				t3dRdSetError( pError, "RD canonical world cluster capacity allocation failed" );
				return false;
			}
			rResult.m_worldClusterVertexIndices.resize( nOldVertexIndexCount + (size_t)nMaxNewClusters * 64u );
			rResult.m_worldTriangleWords.resize( nOldTriangleWordCount + (size_t)nMaxNewClusters * 124u );
			rResult.m_worldClusters.resize( nOldClusterCount + nMaxNewClusters );
			rResult.m_worldClusterVtMetadata.resize( nOldClusterCount + nMaxNewClusters );
			t3dWorldClusterCookBuffers_t WorldBuffers {};
			WorldBuffers.m_pClusterVertexIndices = rResult.m_worldClusterVertexIndices.data();
			WorldBuffers.m_nClusterVertexIndexCapacity = (uint32_t)rResult.m_worldClusterVertexIndices.size();
			WorldBuffers.m_nClusterVertexIndexCount = (uint32_t)nOldVertexIndexCount;
			WorldBuffers.m_pTriangleWords = rResult.m_worldTriangleWords.data();
			WorldBuffers.m_nTriangleWordCapacity = (uint32_t)rResult.m_worldTriangleWords.size();
			WorldBuffers.m_nTriangleWordCount = (uint32_t)nOldTriangleWordCount;
			WorldBuffers.m_pClusters = rResult.m_worldClusters.data();
			WorldBuffers.m_pVtMetadata = rResult.m_worldClusterVtMetadata.data();
			WorldBuffers.m_nClusterCapacity = (uint32_t)rResult.m_worldClusters.size();
			WorldBuffers.m_nClusterCount = (uint32_t)nOldClusterCount;
			t3dWorldClusterCookSource_t WorldSource {};
			WorldSource.m_pVertices = rResult.m_worldVertices.data() + world_vertex_offset;
			WorldSource.m_pIndices = (const uint32_t *)( rResult.m_index32Bytes.ptr() ) + record.m_nIndexOffset;
			WorldSource.m_nVertexBase = world_vertex_offset;
			WorldSource.m_nVertexCount = vertexCount;
			WorldSource.m_nIndexCount = index_count;
			WorldSource.m_nMaterial = record.m_nMaterial;
			WorldSource.m_nLod = nLod < 0 ? 0u : (uint32_t)nLod;
			WorldSource.m_nGeometryKind = ( nSourceKey & 0xf000000000000000ull ) == 0x2000000000000000ull ? T3D_WORLD_GEOMETRY_HLOD_CLUSTER
			                                                                                              : T3D_WORLD_GEOMETRY_MESH_CLUSTER;
			WorldSource.m_nClusterFlags = 0u;
			WorldSource.m_nCookFlags = bTwoSided ? T3D_WORLD_CLUSTER_COOK_TWO_SIDED : T3D_WORLD_CLUSTER_COOK_NONE;
			WorldSource.m_nSourceId = (uint32_t)( nSourceKey ^ ( nSourceKey >> 32u ) );
			t3dWorldMeshClusterSpan_t WorldSpan {};
			t3dWorldClusterCookStats_t WorldStats {};
			if ( !t3dWorldCookMeshClustersCold( WorldSource, WorldBuffers, WorldSpan, WorldStats ) ) {
				t3dRdSetError( pError, "RD canonical world cluster cook failed" );
				return false;
			}
			rResult.m_worldClusterVertexIndices.resize( WorldBuffers.m_nClusterVertexIndexCount );
			rResult.m_worldTriangleWords.resize( WorldBuffers.m_nTriangleWordCount );
			rResult.m_worldClusters.resize( WorldBuffers.m_nClusterCount );
			rResult.m_worldClusterVtMetadata.resize( WorldBuffers.m_nClusterCount );
			record.m_nWorldClusterOffset = WorldSpan.m_nClusterOffset;
			record.m_nWorldClusterCount = WorldSpan.m_nClusterCount;
		}

		if ( !t3dVectorPushBackWithinCapacity( rResult.m_surfaces, record ) ) {
			t3dRdSetError( pError, "RD static mesh surface capacity exhausted" );
			return false;
		}
	}
	lod_record.m_nSurfaceCount = (uint32_t)( rResult.m_surfaces.size() ) - lod_record.m_nFirstSurface;
	if ( lod_record.m_nSurfaceCount == 0u ) {
		t3dRdSetError( pError, "RD static mesh source produced no usable triangle surfaces" );
		return false;
	}
	if ( !t3dVectorPushBackWithinCapacity( rResult.m_lods, lod_record ) ) {
		t3dRdSetError( pError, "RD static mesh LOD capacity exhausted" );
		return false;
	}
	return true;
}

static bool t3dRdValidateMeshSourceSnapshotIdentity( const t3dRdMeshSourceSnapshot_t& rSnapshot, String *const pError )
{
	for ( size_t iLod = 0u; iLod < rSnapshot.m_lods.size(); ++iLod ) {
		const t3dRdMeshSourceLod_t& lod = rSnapshot.m_lods[ iLod ];
		if ( lod.m_nSourceKey == 0u ) {
			t3dRdSetError( pError, "RD mesh snapshot contains an invalid zero source key" );
			return false;
		}
		for ( size_t iEarlier = 0u; iEarlier < iLod; ++iEarlier ) {
			if ( rSnapshot.m_lods[ iEarlier ].m_nSourceKey == lod.m_nSourceKey ) {
				t3dRdSetError( pError, "RD mesh snapshot contains duplicate mesh source keys" );
				return false;
			}
		}
		const uint64_t source_class = lod.m_nSourceKey & 0xF000000000000000ull;
		if ( source_class == 0x1000000000000000ull ) {
			const uint16_t encoded_asset = (uint16_t)( lod.m_nSourceKey & 0xFFFFu );
			const int16_t encoded_lod = (int16_t)( ( lod.m_nSourceKey >> 16u ) & 0xFFFFu );
			if ( encoded_asset != lod.m_nAssetId || encoded_lod != lod.m_nLod ) {
				t3dRdSetError( pError, "RD regular mesh source key does not match its asset/LOD identity" );
				return false;
			}
		} else if ( source_class == 0x3000000000000000ull ) {
			const uint16_t encoded_asset = (uint16_t)( lod.m_nSourceKey & 0xFFFFu );
			if ( encoded_asset != lod.m_nAssetId || lod.m_nLod != -1 ) {
				t3dRdSetError( pError, "RD occlusion-proxy source key does not match its asset identity" );
				return false;
			}
		}
		const uint64_t surface_end = (uint64_t)( lod.m_nFirstSurface ) + lod.m_nSurfaceCount;
		if ( surface_end > rSnapshot.m_surfaces.size() ) {
			t3dRdSetError( pError, "RD mesh LOD surface span exceeds the published surface table" );
			return false;
		}
		for ( uint32_t iSurface = 0u; iSurface < lod.m_nSurfaceCount; ++iSurface ) {
			const t3dRdMeshSourceSurface_t& surface = rSnapshot.m_surfaces[ lod.m_nFirstSurface + iSurface ];
			if ( surface.m_nSourceKey != lod.m_nSourceKey || surface.m_nAssetId != lod.m_nAssetId || surface.m_nLod != lod.m_nLod ) {
				t3dRdSetError( pError, "RD mesh surface identity does not match its owning mesh source" );
				return false;
			}
		}
	}
	return true;
}

bool Terrain3DRdMeshCache::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy )
{
	if ( !pZone ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_surfaces.set_allocator( allocator );
	m_lods.set_allocator( allocator );
	m_materials.set_allocator( allocator );
	m_vtMaterials.set_allocator( allocator );
	m_vtStableKeys.set_allocator( allocator );
	m_vertexChunkHashes.set_allocator( allocator );
	m_index32ChunkHashes.set_allocator( allocator );
#if T3D_INSTANCER_SHARED_MESH_ARENA
	m_surfaceViews.set_allocator( allocator );
#endif
	return true;
}

Terrain3DRdMeshCache::Terrain3DRdMeshCache( void ) : m_context( Terrain3DRdContext::getShared() )
{
}

Terrain3DRdMeshCache::~Terrain3DRdMeshCache( void )
{
	shutdown();
}

void Terrain3DRdMeshCache::setError( const String& rError )
{
	m_lastError = rError;
}

uint32_t Terrain3DRdMeshCache::nextArenaCapacity( const uint32_t nRequiredBytes, const uint32_t nMinimumBytes )
{
	if ( nRequiredBytes == 0u ) {
		return 0u;
	}
	uint32_t capacity = 0u;
	return t3dRdGpuBufferGrownCapacity( MAX( nMinimumBytes, 4096u ), nRequiredBytes,
	               T3D_GPU_POWER_OF_TWO, capacity )
	        ? capacity
	        : 0u;
}

typedef struct t3dRdAssetMaterialResolveContext_s
{
	const Terrain3DMeshAsset *m_pAsset;
	int16_t m_nLod;
} t3dRdAssetMaterialResolveContext_t;

static const Terrain3DRenderingMaterial *t3dRdResolveAssetSurfaceRenderingMaterial( const void *const pContext, const uint32_t nSurface )
{
	const t3dRdAssetMaterialResolveContext_t *const pResolve = (const t3dRdAssetMaterialResolveContext_t *const)( pContext );
	return pResolve && pResolve->m_pAsset ? pResolve->m_pAsset->getSurfaceRenderingMaterialPtr( pResolve->m_nLod, nSurface ) : NULL;
}

static uint64_t t3dRdHashOcclusionProxyGeometry( uint64_t hash, const Ref<ArrayMesh>& rMesh )
{
	hash = t3dRdHashU64( hash, t3dRdObjectId( rMesh.ptr() ) );
	if ( rMesh.is_null() ) {
		return hash;
	}
	hash = t3dRdHashU64( hash, (uint64_t)( rMesh->get_surface_count() ) );
	const AABB bounds = rMesh->get_aabb();
	hash = t3dRdHashVector3( hash, bounds.position );
	hash = t3dRdHashVector3( hash, bounds.size );
	for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
		hash = t3dRdHashU64( hash, (uint64_t)( rMesh->surface_get_primitive_type( surface ) ) );
		const Array arrays = rMesh->surface_get_arrays( surface );
		if ( arrays.size() > Mesh::ARRAY_VERTEX && arrays[ Mesh::ARRAY_VERTEX ].get_type() == Variant::PACKED_VECTOR3_ARRAY ) {
			const PackedVector3Array positions = arrays[ Mesh::ARRAY_VERTEX ];
			hash = t3dRdHashU64( hash, (uint64_t)( positions.size() ) );
			for ( int64_t i = 0; i < positions.size(); ++i ) {
				hash = t3dRdHashVector3( hash, positions[ i ] );
			}
		}
		if ( arrays.size() > Mesh::ARRAY_INDEX && arrays[ Mesh::ARRAY_INDEX ].get_type() == Variant::PACKED_INT32_ARRAY ) {
			const PackedInt32Array indices = arrays[ Mesh::ARRAY_INDEX ];
			hash = t3dRdHashU64( hash, (uint64_t)( indices.size() ) );
			for ( int64_t i = 0; i < indices.size(); ++i ) {
				hash = t3dRdHashU64( hash, (uint32_t)( indices[ i ] ) );
			}
		}
	}
	return hash;
}

static uint64_t t3dRdHashAssetSource( uint64_t hash, const Ref<Terrain3DMeshAsset>& asset, const uint16_t asset_id )
{
	hash = t3dRdHashU64( hash, (uint64_t)( asset->isEnabled() ) );
	hash = t3dRdHashU64( hash, (uint64_t)( asset->isHighlighted() ) );
	hash = t3dRdHashU64( hash, (uint64_t)( asset->getInstanceCategory() ) );
	hash = t3dRdHashU64( hash, (uint64_t)( asset->getInstanceOcclusionProxyGeneration() ) );
	hash = t3dRdHashU64( hash, (uint64_t)( asset->getInstanceOcclusionProxyVertexCount() ) );
	hash = t3dRdHashU64( hash, (uint64_t)( asset->getInstanceOcclusionProxyTriangleCount() ) );
	hash = t3dRdHashU64( hash, (uint64_t)( asset->getLodCount() ) );
	if ( asset->getInstanceCategory() == INSTANCE_CATEGORY_OCCLUDER && asset->hasInstanceOcclusionProxy() ) {
		hash = t3dRdHashU64( hash, t3dRdOcclusionProxySourceKey( asset_id ) );
		hash = t3dRdHashOcclusionProxyGeometry( hash, asset->getInstanceOcclusionProxyMesh() );
	}
	hash = t3dRdHashRenderingMaterial( hash, asset->getRenderingMaterialOverride() );
	hash = t3dRdHashRenderingMaterial( hash, asset->getRenderingMaterialOverlay() );
	hash = t3dRdHashColor( hash, asset->getHighlightColor() );
	for ( int lod = 0; lod < asset->getLodCount(); ++lod ) {
		const Ref<Mesh> mesh = asset->getMesh( lod );
		hash = t3dRdHashU64( hash, t3dRdRegularMeshSourceKey( (uint16_t)( asset_id ), (int16_t)( lod ) ) );
		hash = t3dRdHashU64( hash, t3dRdObjectId( mesh.ptr() ) );
		if ( mesh.is_null() ) {
			continue;
		}
		hash = t3dRdHashU64( hash, (uint64_t)( mesh->get_surface_count() ) );
		const AABB bounds = mesh->get_aabb();
		hash = t3dRdHashVector3( hash, bounds.position );
		hash = t3dRdHashVector3( hash, bounds.size );
		ArrayMesh *const pArrayMesh = Object::cast_to<ArrayMesh>( mesh.ptr() );
		hash = t3dRdHashU64( hash, pArrayMesh ? 1u : 0u );
		for ( int surface = 0; surface < mesh->get_surface_count(); ++surface ) {
			if ( pArrayMesh ) {
				hash = t3dRdHashU64( hash, (uint64_t)( pArrayMesh->surface_get_primitive_type( surface ) ) );
				hash = t3dRdHashU64( hash, (uint64_t)( MAX( pArrayMesh->surface_get_array_len( surface ), 0 ) ) );
				hash = t3dRdHashU64( hash, (uint64_t)( MAX( pArrayMesh->surface_get_array_index_len( surface ), 0 ) ) );
			}
			hash = t3dRdHashRenderingMaterial( hash, asset->getSurfaceRenderingMaterial( lod, (uint32_t)( surface ) ) );
		}
	}
	return hash;
}

uint64_t Terrain3DRdMeshCache::computeSourceFingerprintForAsset( const Ref<Terrain3DAssets>& rAssets, const uint16_t nAssetId )
{
	if ( rAssets.is_null() || nAssetId >= rAssets->getMeshCount() ) {
		return 0u;
	}
	const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( nAssetId );
	if ( asset.is_null() ) {
		return 0u;
	}
	const uint64_t hash = t3dRdHashAssetSource( t3dRdHashU64( s_nFnvOffset, nAssetId ), asset, nAssetId );
	return hash == 0u ? 1u : hash;
}

uint64_t Terrain3DRdMeshCache::computeSourceFingerprint( const Ref<Terrain3DAssets>& rAssets )
{
	return computeSourceFingerprint( rAssets, NULL, 0u );
}

uint64_t Terrain3DRdMeshCache::computeSourceFingerprint(
    const Ref<Terrain3DAssets>& rAssets, const t3dRdExternalMeshSource_t *pExternalMeshes, const size_t nExternalMeshCount )
{
	if ( rAssets.is_null() && nExternalMeshCount == 0u ) {
		return 0u;
	}
	uint64_t hash = s_nFnvOffset;
	const int mesh_count = rAssets.is_valid() ? rAssets->getMeshCount() : 0;
	hash = t3dRdHashU64( hash, (uint64_t)( mesh_count ) );
	for ( int asset_id = 0; asset_id < mesh_count; ++asset_id ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( asset_id );
		if ( asset.is_null() ) {
			hash = t3dRdHashU64( hash, 0u );
			continue;
		}
		hash = t3dRdHashAssetSource( hash, asset, (uint16_t)asset_id );
	}
	hash = t3dRdHashU64( hash, (uint64_t)( nExternalMeshCount ) );
	for ( size_t iEntry = 0u; iEntry < nExternalMeshCount; iEntry++ ) {
		const t3dRdExternalMeshSource_t& source = pExternalMeshes[ iEntry ];
		hash = t3dRdHashU64( hash, source.m_nSourceKey );
		hash = t3dRdHashU64( hash, t3dRdObjectId( source.m_mesh.ptr() ) );
		if ( source.m_mesh.is_null() ) {
			continue;
		}
		hash = t3dRdHashU64( hash, (uint64_t)( source.m_mesh->get_surface_count() ) );
		const AABB bounds = source.m_mesh->get_aabb();
		hash = t3dRdHashVector3( hash, bounds.position );
		hash = t3dRdHashVector3( hash, bounds.size );
		ArrayMesh *const pArrayMesh = Object::cast_to<ArrayMesh>( source.m_mesh.ptr() );
		hash = t3dRdHashU64( hash, pArrayMesh ? 1u : 0u );
		for ( int surface = 0; surface < source.m_mesh->get_surface_count(); ++surface ) {
			if ( pArrayMesh ) {
				hash = t3dRdHashU64( hash, (uint64_t)( pArrayMesh->surface_get_primitive_type( surface ) ) );
				hash = t3dRdHashU64( hash, (uint64_t)( MAX( pArrayMesh->surface_get_array_len( surface ), 0 ) ) );
				hash = t3dRdHashU64( hash, (uint64_t)( MAX( pArrayMesh->surface_get_array_index_len( surface ), 0 ) ) );
			}
			const Terrain3DRenderingMaterial *const pRenderingMaterial = source.m_pResolveRenderingMaterial
			                                                                 ? source.m_pResolveRenderingMaterial( source.m_pMaterialContext, (uint32_t)( surface ) )
			                                                                 : NULL;
			hash = t3dRdHashRenderingMaterial( hash, pRenderingMaterial );
		}
		hash = t3dRdHashU64( hash, source.m_nFlags );
	}
	return hash == 0u ? 1u : hash;
}

static bool t3dRdAppendAssetSource(
    const Ref<Terrain3DMeshAsset>& asset, const uint16_t asset_id, t3dRdMeshSourceSnapshot_t& rResult, String *const pError )
{
	if ( !asset->ensureRenderingMaterialStableKeysCold() ) {
		t3dRdSetError( pError, "RD mesh asset could not establish stable VT material identities" );
		return false;
	}
	const bool highlighted = asset->isHighlighted();
	Ref<Terrain3DRenderingMaterial> highlight_material;
	if ( highlighted ) {
		highlight_material.instantiate();
		highlight_material->setBaseColor( asset->getHighlightColor() );
		highlight_material->setUnshaded( true );
		highlight_material->setTwoSided( true );
		const String owner = String( "highlight:" ) + asset->get_path() + String( ":" ) + asset->getName();
		if ( !highlight_material->ensureVtStableKeyCold( owner, String( "override" ) ) ) {
			t3dRdSetError( pError, "RD highlight material could not establish a stable VT identity" );
			return false;
		}
	}
	const Ref<Terrain3DRenderingMaterial> canonical_override = highlighted ? highlight_material : asset->getRenderingMaterialOverride();
	uint16_t rendering_override = UINT16_MAX;
	if ( !t3dRdAddRenderingMaterial( canonical_override,
	                                 rResult.m_materials,
	                                 rResult.m_vtMaterials,
	                                 rResult.m_materialKeys,
	                                 rResult.m_vtStableKeys,
	                                 rendering_override,
	                                 pError ) ) {
		return false;
	}
	uint16_t overlay_material = UINT16_MAX;
	if ( !highlighted && !t3dRdAddRenderingMaterial( asset->getRenderingMaterialOverlay(),
	                                                 rResult.m_materials,
	                                                 rResult.m_vtMaterials,
	                                                 rResult.m_materialKeys,
	                                                 rResult.m_vtStableKeys,
	                                                 overlay_material,
	                                                 pError ) ) {
		return false;
	}
	for ( int lod = 0; lod < asset->getLodCount(); ++lod ) {
		const Ref<Mesh> mesh = asset->getMesh( lod );
		t3dRdAssetMaterialResolveContext_t material_context;
		material_context.m_pAsset = asset.ptr();
		material_context.m_nLod = (int16_t)( lod );
		t3dRdExternalMeshSource_t material_source;
		material_source.m_pMaterialContext = &material_context;
		material_source.m_pResolveRenderingMaterial = &t3dRdResolveAssetSurfaceRenderingMaterial;
		material_source.m_nFlags = T3D_RD_EXTERNAL_MESH_REQUIRE_RENDERING_MATERIAL;
		if ( !t3dRdAppendMeshSource( mesh,
		                             t3dRdRegularMeshSourceKey( (uint16_t)( asset_id ), (int16_t)( lod ) ),
		                             (uint16_t)( asset_id ),
		                             (int16_t)( lod ),
		                             rendering_override,
		                             overlay_material,
		                             rResult,
		                             pError,
		                             &material_source ) ) {
			return false;
		}
	}
	if ( asset->getInstanceCategory() == INSTANCE_CATEGORY_OCCLUDER && asset->hasInstanceOcclusionProxy() ) {
		const Ref<ArrayMesh> proxy_mesh = asset->getInstanceOcclusionProxyMesh();
		if ( !t3dRdAppendMeshSource(
		         proxy_mesh, t3dRdOcclusionProxySourceKey( asset_id ), asset_id, -1, UINT16_MAX, UINT16_MAX, rResult, pError ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DRdMeshCache::buildSourceSnapshotForAsset( const Ref<Terrain3DAssets>& rAssets,
                                                        const uint16_t nAssetId,
                                                        t3dRdMeshSourceSnapshot_t& rResult,
                                                        const uint64_t nFingerprint,
                                                        const uint64_t nGeneration,
                                                        String *const pError )
{
	if ( rAssets.is_null() || nAssetId >= rAssets->getMeshCount() ) {
		t3dRdSetError( pError, "RD preview asset is unavailable" );
		return false;
	}
	const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( nAssetId );
	if ( asset.is_null() || asset->getLodCount() <= 0 || asset->getMesh( 0 ).is_null() || asset->getMesh( 0 )->get_surface_count() <= 0 ) {
		t3dRdSetError( pError, "RD preview asset has no base mesh surfaces" );
		return false;
	}
	// Commit only a successful build: a malformed asset cannot evict another cached asset.
	t3dRdMeshSourceSnapshot_t snapshot;
	snapshot.bindRuntimeAllocator( rResult.m_lods.get_allocator() );
	snapshot.m_nFingerprint = nFingerprint;
	snapshot.m_nGeneration = nGeneration == 0u ? 1u : nGeneration;
	if ( !t3dRdAppendAssetSource( asset, nAssetId, snapshot, pError ) ) {
		return false;
	}
	if ( !t3dRdValidateMeshSourceSnapshotIdentity( snapshot, pError ) ) {
		return false;
	}
	if ( !t3dRdBuildChunkHashes( snapshot.m_vertexBytes, snapshot.m_vertexChunkHashes ) || !t3dRdBuildChunkHashes( snapshot.m_index32Bytes, snapshot.m_index32ChunkHashes ) ) {
		t3dRdSetError( pError, "RD preview mesh chunk-hash capacity allocation failed" );
		return false;
	}
	rResult = eastl::move( snapshot );
	if ( pError ) {
		*pError = String();
	}
	return true;
}

bool Terrain3DRdMeshCache::buildSourceSnapshot( const Ref<Terrain3DAssets>& rAssets,
                                                t3dRdMeshSourceSnapshot_t& rResult,
                                                const uint64_t nFingerprint,
                                                const uint64_t nGeneration,
                                                String *const pError,
                                                const bool bIncludeDisabled )
{
	return buildSourceSnapshot( rAssets, NULL, 0u, rResult, nFingerprint, nGeneration, pError, bIncludeDisabled );
}

bool Terrain3DRdMeshCache::buildSourceSnapshot( const Ref<Terrain3DAssets>& rAssets,
                                                const t3dRdExternalMeshSource_t *pExternalMeshes,
                                                const size_t nExternalMeshCount,
                                                t3dRdMeshSourceSnapshot_t& rResult,
                                                const uint64_t nFingerprint,
                                                const uint64_t nGeneration,
                                                String *const pError,
                                                const bool bIncludeDisabled )
{
	if ( rAssets.is_null() && nExternalMeshCount == 0u ) {
		t3dRdSetError( pError, "Terrain3D mesh sources unavailable" );
		return false;
	}
	rResult.m_vertexBytes.clear();
	rResult.m_index32Bytes.clear();
	rResult.m_vertexChunkHashes.clear();
	rResult.m_index32ChunkHashes.clear();
	rResult.m_surfaces.clear();
	rResult.m_lods.clear();
	rResult.m_materials.clear();
	rResult.m_vtMaterials.clear();
	rResult.m_materialKeys.clear();
	rResult.m_vtStableKeys.clear();
	rResult.m_nFingerprint = nFingerprint;
	rResult.m_nGeneration = nGeneration == 0u ? 1u : nGeneration;
	const int mesh_count = rAssets.is_valid() ? rAssets->getMeshCount() : 0;
	for ( int asset_id = 0; asset_id < mesh_count; ++asset_id ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( asset_id );
		if ( asset.is_null() || ( !bIncludeDisabled && !asset->isEnabled() ) ) {
			continue;
		}
		if ( !t3dRdAppendAssetSource( asset, (uint16_t)asset_id, rResult, pError ) ) {
			return false;
		}
	}
	eastl::vector<uint64_t, Terrain3DEastlAllocator> external_keys;
	if ( !t3dContainerReserveCold( external_keys, nExternalMeshCount ) ) {
		t3dRdSetError( pError, "RD external mesh key scratch capacity allocation failed" );
		return false;
	}
	for ( size_t iEntry = 0u; iEntry < nExternalMeshCount; iEntry++ ) {
		const t3dRdExternalMeshSource_t& source = pExternalMeshes[ iEntry ];
		if ( source.m_nSourceKey == 0u || source.m_mesh.is_null() ) {
			continue;
		}
		if ( eastl::find( external_keys.begin(), external_keys.end(), source.m_nSourceKey ) != external_keys.end() ) {
			t3dRdSetError( pError, "Duplicate HLOD mesh source key in RD publication" );
			return false;
		}
		for ( size_t iLod = 0u; iLod < (size_t)( ( rResult.m_lods ).size() ); iLod++ ) {
			const t3dRdMeshSourceLod_t& existing = ( rResult.m_lods )[ iLod ];
			if ( existing.m_nSourceKey == source.m_nSourceKey ) {
				t3dRdSetError( pError, "HLOD mesh source key collides with an existing RD mesh source" );
				return false;
			}
		}
		if ( !t3dVectorPushBackWithinCapacity( external_keys, source.m_nSourceKey ) ) {
			t3dRdSetError( pError, "RD external mesh key scratch capacity exhausted" );
			return false;
		}
		if ( !t3dRdAppendMeshSource(
		         source.m_mesh, source.m_nSourceKey, UINT16_MAX, 0, UINT16_MAX, UINT16_MAX, rResult, pError, &source ) ) {
			return false;
		}
	}
	if ( !t3dRdValidateMeshSourceSnapshotIdentity( rResult, pError ) ) {
		return false;
	}
	if ( !t3dRdBuildChunkHashes( rResult.m_vertexBytes, rResult.m_vertexChunkHashes ) || !t3dRdBuildChunkHashes( rResult.m_index32Bytes, rResult.m_index32ChunkHashes ) ) {
		t3dRdSetError( pError, "RD mesh chunk-hash capacity allocation failed" );
		return false;
	}
	if ( pError ) {
		*pError = String();
	}
	return true;
}

bool Terrain3DRdMeshCache::ensureVertexFormat( void )
{
	if ( m_nVertexFormat != RenderingDevice::INVALID_FORMAT_ID ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	TypedArray<Ref<RDVertexAttribute>> attributes;
	const struct AttributeDesc
	{
		uint32_t location;
		RenderingDevice::DataFormat format;
		uint32_t offset;
	} desc[] = {
		{ 0u, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 0u },
		{ 1u, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, 12u },
		{ 2u, RenderingDevice::DATA_FORMAT_R16G16B16A16_SNORM, 16u },
		{ 3u, RenderingDevice::DATA_FORMAT_R16G16B16A16_SNORM, 24u },
		{ 4u, RenderingDevice::DATA_FORMAT_R16G16_SFLOAT, 32u },
		{ 5u, RenderingDevice::DATA_FORMAT_R16G16_SFLOAT, 36u },
	};
	for ( size_t iDescriptor = 0u; iDescriptor < T3D_ARRAY_COUNT( desc ); iDescriptor++ ) {
		const AttributeDesc& item = ( desc )[ iDescriptor ];
		Ref<RDVertexAttribute> attribute;
		attribute.instantiate();
		attribute->set_location( item.location );
		attribute->set_binding( 0u );
		attribute->set_format( item.format );
		attribute->set_offset( item.offset );
		attribute->set_stride( sizeof( t3dRdMeshVertex_t ) );
		attribute->set_frequency( RenderingDevice::VERTEX_FREQUENCY_VERTEX );
		attributes.push_back( attribute );
	}
	m_nVertexFormat = rd->vertex_format_create( attributes );
	return m_nVertexFormat != RenderingDevice::INVALID_FORMAT_ID && m_context.registerVertexFormat( m_nVertexFormat, "T3D.VertexFormat.InstanceMesh48" );
}

bool Terrain3DRdMeshCache::ensureArena( Terrain3DRdGpuBuffer& rBuffer,
                                        const uint32_t nRequiredBytes,
                                        const char *const pName,
                                        const Terrain3DRdContext::ResourceType resourceType,
                                        const Terrain3DRdContext::MemoryCategory memoryCategory )
{
	if ( nRequiredBytes == 0u ) {
		return true;
	}
	const uint32_t minimum = resourceType == Terrain3DRdContext::RESOURCE_VERTEX_BUFFER ? s_nMinVertexArenaBytes : s_nMinIndexArenaBytes;
	if ( !rBuffer.needsCapacity( nRequiredBytes ) ) {
		return true;
	}
	++m_nArenaCapacityMissCount;
	++m_nArenaGrowthRequestCount;
	const uint32_t capacity = nextArenaCapacity( nRequiredBytes, minimum );
	const String name = pName ? String::utf8( pName ) : String();
	if ( capacity == 0u ) {
		++m_nArenaRebuildFailureCount;
		setError( String( "RD mesh arena capacity calculation overflowed; resource=" ) + name + String( ", required=" ) + String::num_int64( nRequiredBytes ) + String( ", minimum=" ) + String::num_int64( minimum ) );
		return false;
	}
	if ( !m_context.getDevice() ) {
		++m_nArenaRebuildFailureCount;
		setError( String( "RD mesh arena lost the RenderingDevice; resource=" ) + name + String( ", required=" ) + String::num_int64( nRequiredBytes ) );
		return false;
	}
	t3dRdGpuBufferSpec_t spec;
	spec.m_pName = pName;
	spec.m_owner = Terrain3DRdContext::OWNER_INSTANCES;
	spec.m_type = resourceType;
	spec.m_memory = memoryCategory;
	spec.m_growth = T3D_GPU_EXACT;
	spec.m_indexFormat = RenderingDevice::INDEX_BUFFER_FORMAT_UINT32;
	Terrain3DRdGpuBuffer replacement;
	if ( !replacement.configure( spec ) || !replacement.createTracked( m_context, capacity ) ) {
		++m_nArenaRebuildFailureCount;
		setError( String( "RD mesh arena creation or registration failed; resource=" ) + name + String( ", capacity=" ) + String::num_int64( capacity ) );
		return false;
	}
	const bool replacing = rBuffer.isValid();
	if ( replacing ) {
		// Array RIDs reference their parent buffers. Destroy every child view
		// after the replacement is live, but before the old parent is freed.
		discardArrays();
	}
	rBuffer.swap( replacement );
	if ( replacing ) {
		m_context.recordRenderEvent( Terrain3DRdContext::OWNER_INSTANCES,
		        Terrain3DRdContext::RenderTelemetry::EVENT_BUFFER_REBUILD );
	}
	replacement.releaseTracked( m_context );
	++m_nArenaRebuildCount;
	return true;
}

#if T3D_INSTANCER_SHARED_MESH_ARENA
void Terrain3DRdMeshCache::discardSurfaceViews( void )
{
	for ( size_t iSurface = 0u; iSurface < m_surfaceViews.size(); ++iSurface ) {
		m_surfaceViews[iSurface].m_view.releaseTracked( m_context );
	}
	m_surfaceViews.clear();
}
#endif

void Terrain3DRdMeshCache::discardArrays( void )
{
#if T3D_INSTANCER_SHARED_MESH_ARENA
	discardSurfaceViews();
#endif
	m_arenaView.releaseTracked( m_context );
}

bool Terrain3DRdMeshCache::rebuildArrays( void )
{
	discardArrays();
	if ( !m_context.getDevice() ) {
		setError( "RD mesh array rebuild lost the RenderingDevice" );
		return false;
	}
	if ( m_vertexArena.isValid() ) {
		TypedArray<RID> buffers;
		buffers.push_back( m_vertexArena.rid() );
		if ( !m_arenaView.createVertexTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES,
		             "T3D.Instance.MeshVertexArray",
		             m_vertexArena.capacityBytes() / sizeof( t3dRdMeshVertex_t ),
		             m_nVertexFormat, buffers ) ) {
			setError( String( "RD mesh vertex-array creation/registration failed; vertex_capacity_bytes=" )
			        + String::num_int64( m_vertexArena.capacityBytes() ) + String( ", vertex_format=" )
			        + String::num_int64( m_nVertexFormat ) );
			return false;
		}
	}
	if ( m_index32Arena.isValid() ) {
		if ( !m_arenaView.createIndexTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES,
		             "T3D.Instance.MeshIndexArray32", m_index32Arena.rid(), 0u,
		             m_index32Arena.capacityBytes() / sizeof( uint32_t ) ) ) {
			m_arenaView.releaseTracked( m_context );
			setError( String( "RD mesh uint32 index-array creation/registration failed; index_capacity_bytes=" )
			        + String::num_int64( m_index32Arena.capacityBytes() ) );
			return false;
		}
	}
	return m_arenaView.hasVertex();
}

#if T3D_INSTANCER_SHARED_MESH_ARENA
bool Terrain3DRdMeshCache::rebuildSurfaceViews( const t3dRdMeshSourceSnapshot_t *const pSnapshot )
{
	discardSurfaceViews();
	if ( !pSnapshot ) {
		setError( "RD shared-arena surface-view rebuild received a null mesh snapshot" );
		return false;
	}
	if ( !m_context.getDevice() || !m_vertexArena.isValid() ) {
		setError( "RD shared-arena surface-view rebuild lost required vertex arena state" );
		return false;
	}
	if ( !t3dContainerReserveCold( m_surfaceViews, pSnapshot->m_surfaces.size() ) ) {
		setError( String( "RD shared-arena surface-view reserve failed; required=" ) + String::num_int64( pSnapshot->m_surfaces.size() ) );
		return false;
	}
	for ( uint32_t iSurface = 0u; iSurface < (uint32_t)( pSnapshot->m_surfaces.size() ); ++iSurface ) {
		const t3dRdMeshSourceSurface_t& source = pSnapshot->m_surfaces[ iSurface ];
		const uint64_t vertex_offset_bytes64 = (uint64_t)( source.m_nVertexOffset ) * sizeof( t3dRdMeshVertex_t );
		const uint64_t vertex_end_bytes64 = vertex_offset_bytes64 + (uint64_t)( source.m_nVertexCount ) * sizeof( t3dRdMeshVertex_t );
		const uint64_t index_end_bytes64 = ( (uint64_t)( source.m_nIndexOffset ) + source.m_nIndexCount ) * sizeof( uint32_t );
		const uint64_t index_used_bytes = (uint64_t)( pSnapshot->m_index32Bytes.size() );
		const RID index_buffer = m_index32Arena.rid();
		if ( source.m_nVertexCount == 0u || source.m_nIndexCount == 0u
		        || vertex_end_bytes64 > (uint64_t)( pSnapshot->m_vertexBytes.size() )
		        || index_end_bytes64 > index_used_bytes || !index_buffer.is_valid() ) {
			setError( String( "RD shared-arena surface range is invalid; surface=" ) + String::num_int64( iSurface )
			        + String( ", vertex_offset=" ) + String::num_int64( source.m_nVertexOffset )
			        + String( ", vertex_count=" ) + String::num_int64( source.m_nVertexCount )
			        + String( ", index_offset=" ) + String::num_int64( source.m_nIndexOffset )
			        + String( ", index_count=" ) + String::num_int64( source.m_nIndexCount ) );
			discardSurfaceViews();
			return false;
		}

		t3dRdArenaSurfaceView_t surface;
		TypedArray<RID> vertex_buffers;
		vertex_buffers.push_back( m_vertexArena.rid() );
		PackedInt64Array vertex_offsets;
		vertex_offsets.push_back( (int64_t)( vertex_offset_bytes64 ) );
		const String vertex_array_name = String( "T3D.Instance.MeshSurfaceVertexView." ) + String::num_int64( iSurface );
		const String index_array_name = String( "T3D.Instance.MeshSurfaceIndexView." ) + String::num_int64( iSurface );
		if ( !surface.m_view.createIndexedTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES,
		             vertex_array_name, index_array_name,
		             source.m_nVertexCount, m_nVertexFormat, vertex_buffers, vertex_offsets,
		             index_buffer, source.m_nIndexOffset, source.m_nIndexCount ) ) {
			setError( String( "RD shared-arena surface geometry-view creation/registration failed; surface=" )
			        + String::num_int64( iSurface ) );
			discardSurfaceViews();
			return false;
		}
		if ( !t3dVectorPushBackWithinCapacity( m_surfaceViews, eastl::move( surface ) ) ) {
			surface.m_view.releaseTracked( m_context );
			setError( String( "RD shared-arena surface-view table exceeded reserved capacity; surface=" ) + String::num_int64( iSurface ) );
			discardSurfaceViews();
			return false;
		}
	}
	return true;
}
#endif

bool Terrain3DRdMeshCache::uploadChangedChunks( const Terrain3DRdGpuBuffer& rBuffer,
                                                const PackedByteArray& rCurrent,
                                                const eastl::vector<uint64_t, Terrain3DEastlAllocator>& rCurrentHashes,
                                                const eastl::vector<uint64_t, Terrain3DEastlAllocator>& rPreviousHashes,
                                                const bool bForceFull,
                                                uint64_t& nUploadedBytes )
{
	RenderingDevice *const pRd = m_context.getDevice();
	if ( !pRd ) {
		return false;
	}
	const uint32_t nCurrentSize = (uint32_t)rCurrent.size();
	for ( uint32_t nChunk = 0u; nChunk < (uint32_t)rCurrentHashes.size(); ++nChunk ) {
		if ( !bForceFull && nChunk < rPreviousHashes.size() && rPreviousHashes[ nChunk ] == rCurrentHashes[ nChunk ] ) {
			continue;
		}
		const uint32_t nOffset = nChunk * s_nDirtyUploadChunkBytes;
		const uint32_t nBytes = MIN( s_nDirtyUploadChunkBytes, nCurrentSize - nOffset );
		if ( !rBuffer.writeTracked( m_context, nOffset, rCurrent.ptr() + nOffset, nBytes,
		        m_dirtyUploadScratch, T3D_GPU_PUBLICATION ) ) {
			return false;
		}
		nUploadedBytes += nBytes;
	}
	return true;
}

bool Terrain3DRdMeshCache::upload( const t3dRdMeshSourceSnapshot_t *const pSnapshot )
{
	++m_nUploadAttemptCount;
	m_lastError = String();
	if ( !pSnapshot ) {
		++m_nUploadFailureCount;
		setError( "RD mesh-cache upload received a null source snapshot" );
		return false;
	}
	if ( pSnapshot->m_nGeneration == m_nSourceGeneration && isReady() ) {
		m_lastError = String();
		return true;
	}
	if ( !m_context.initialize() ) {
		++m_nUploadFailureCount;
		setError( String( "RD mesh-cache upload could not initialize shared RenderingDevice context; state=" ) + String::num_int64( (int64_t)( m_context.getState() ) ) + String( ", failure=" ) + String::num_int64( (int64_t)( m_context.getFailureCode() ) ) );
		return false;
	}
	if ( !m_context.validateDevice() ) {
		++m_nUploadFailureCount;
		setError( String( "RD mesh-cache upload lost the shared RenderingDevice; state=" ) + String::num_int64( (int64_t)( m_context.getState() ) ) + String( ", failure=" ) + String::num_int64( (int64_t)( m_context.getFailureCode() ) ) );
		return false;
	}
	if ( !ensureVertexFormat() ) {
		++m_nUploadFailureCount;
		setError( "RD mesh-cache upload failed to create/register the instance vertex format" );
		return false;
	}
	const bool grow_vertex = m_vertexArena.needsCapacity( (uint32_t)( pSnapshot->m_vertexBytes.size() ) );
	const bool grow_i32 = !pSnapshot->m_index32Bytes.is_empty() && m_index32Arena.needsCapacity( (uint32_t)( pSnapshot->m_index32Bytes.size() ) );
	m_context.beginResourceRebuild();
	if ( !ensureArena( m_vertexArena,
	                   (uint32_t)( pSnapshot->m_vertexBytes.size() ),
	                   "T3D.Instance.MeshVertexArena",
	                   Terrain3DRdContext::RESOURCE_VERTEX_BUFFER,
	                   Terrain3DRdContext::MEMORY_MESH_VERTEX_ARENA ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD mesh-cache upload failed to establish the vertex arena" );
		}
		return false;
	}
	if ( !ensureArena( m_index32Arena,
	                   (uint32_t)( pSnapshot->m_index32Bytes.size() ),
	                   "T3D.Instance.MeshIndexArena32",
	                   Terrain3DRdContext::RESOURCE_INDEX_BUFFER,
	                   Terrain3DRdContext::MEMORY_MESH_INDEX_ARENA ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD mesh-cache upload failed to establish the uint32 index arena" );
		}
		return false;
	}
	const bool rebuild_arrays = grow_vertex || grow_i32 || !m_arenaView.hasVertex() || ( !pSnapshot->m_index32Bytes.is_empty() && !m_arenaView.hasIndex() );
	if ( rebuild_arrays && !rebuildArrays() ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD mesh-cache upload failed to rebuild vertex/index array views" );
		}
		return false;
	}

	if ( m_dirtyUploadScratch.size() != (int64_t)( s_nDirtyUploadChunkBytes ) ) {
		m_dirtyUploadScratch.resize( s_nDirtyUploadChunkBytes );
		if ( m_dirtyUploadScratch.size() != (int64_t)( s_nDirtyUploadChunkBytes ) ) {
			m_context.endResourceRebuild();
			++m_nUploadFailureCount;
			setError( "RD mesh-cache upload failed to establish dirty-upload scratch" );
			return false;
		}
	}
	uint64_t uploaded_bytes = 0u;
	const bool has_previous = m_nSourceGeneration != 0u;
	if ( !uploadChangedChunks( m_vertexArena,
	                           pSnapshot->m_vertexBytes,
	                           pSnapshot->m_vertexChunkHashes,
	                           m_vertexChunkHashes,
	                           grow_vertex || !has_previous,
	                           uploaded_bytes ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		setError( "RD mesh-cache vertex buffer_update failed" );
		return false;
	}
	if ( !uploadChangedChunks( m_index32Arena,
	                           pSnapshot->m_index32Bytes,
	                           pSnapshot->m_index32ChunkHashes,
	                           m_index32ChunkHashes,
	                           grow_i32 || !has_previous,
	                           uploaded_bytes ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		setError( "RD mesh-cache uint32 index buffer_update failed" );
		return false;
	}
	if ( !m_vertexArena.setUsedBytes( (uint32_t)( pSnapshot->m_vertexBytes.size() ) )
	        || !m_index32Arena.setUsedBytes( (uint32_t)( pSnapshot->m_index32Bytes.size() ) ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		setError( "RD mesh-cache used-byte publication exceeded arena capacity" );
		return false;
	}
#if T3D_INSTANCER_SHARED_MESH_ARENA
	if ( !rebuildSurfaceViews( pSnapshot ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD mesh-cache upload failed to rebuild shared-arena surface views" );
		}
		return false;
	}
#endif
	m_context.endResourceRebuild();
	if ( !t3dVectorCopyCold( m_surfaces, pSnapshot->m_surfaces ) || !t3dVectorCopyCold( m_lods, pSnapshot->m_lods ) || !t3dVectorCopyCold( m_materials, pSnapshot->m_materials ) || !t3dVectorCopyCold( m_vtMaterials, pSnapshot->m_vtMaterials ) || !t3dVectorCopyCold( m_vtStableKeys, pSnapshot->m_vtStableKeys ) || !t3dVectorCopyCold( m_vertexChunkHashes, pSnapshot->m_vertexChunkHashes ) || !t3dVectorCopyCold( m_index32ChunkHashes, pSnapshot->m_index32ChunkHashes ) ) {
		++m_nUploadFailureCount;
		setError( "RD mesh-cache upload failed to retain published mesh metadata" );
		return false;
	}
	m_nSourceGeneration = pSnapshot->m_nGeneration;
	if ( ++m_nGeneration == 0u ) {
		m_nGeneration = 1u;
	}
	m_lastError = String();
	return true;
}

void Terrain3DRdMeshCache::shutdown( void )
{
	discardArrays();
	m_vertexArena.releaseTracked( m_context );
	m_index32Arena.releaseTracked( m_context );
	m_context.releaseVertexFormat( m_nVertexFormat );
	m_nGeneration = 0u;
	m_nSourceGeneration = 0u;
	m_lastError = String();
	m_surfaces.clear();
	m_lods.clear();
	m_materials.clear();
	m_vtMaterials.clear();
	m_vtStableKeys.clear();
	m_vertexChunkHashes.clear();
	m_index32ChunkHashes.clear();
#if T3D_INSTANCER_SHARED_MESH_ARENA
	m_surfaceViews.clear();
#endif
	m_dirtyUploadScratch.clear();
}

t3dRdMeshHandle_t Terrain3DRdMeshCache::getMeshHandle( const uint16_t nAssetId, const int nLod ) const
{
	return getMeshHandleBySourceKey( t3dRdRegularMeshSourceKey( nAssetId, (int16_t)( nLod ) ) );
}

t3dRdMeshHandle_t Terrain3DRdMeshCache::getOcclusionProxyMeshHandle( const uint16_t nAssetId ) const
{
	return getMeshHandleBySourceKey( t3dRdOcclusionProxySourceKey( nAssetId ) );
}

t3dRdMeshHandle_t Terrain3DRdMeshCache::getMeshHandleBySourceKey( const uint64_t nSourceKey ) const
{
	for ( uint32_t i = 0u; i < (uint32_t)( m_lods.size() ); ++i ) {
		if ( m_lods[ i ].m_nSourceKey == nSourceKey ) {
			t3dRdMeshHandle_t handle;
			handle.m_nSlot = i;
			handle.m_nGeneration = m_nGeneration;
			return handle;
		}
	}
	t3dRdMeshHandle_t handle;
	handle.m_nSlot = UINT32_MAX;
	handle.m_nGeneration = 0u;
	return handle;
}
const t3dRdMeshSourceLod_t *Terrain3DRdMeshCache::resolveMesh( const t3dRdMeshHandle_t& rHandle ) const
{
	return rHandle.m_nGeneration == m_nGeneration && rHandle.m_nSlot < m_lods.size() ? &m_lods[ rHandle.m_nSlot ] : NULL;
}
t3dRdSurfaceHandle_t Terrain3DRdMeshCache::getSurfaceHandle( const t3dRdMeshHandle_t& rMesh, const uint32_t nSurface ) const
{
	const t3dRdMeshSourceLod_t *const pLod = resolveMesh( rMesh );
	return pLod && nSurface < pLod->m_nSurfaceCount ? t3dRdSurfaceHandle_t { pLod->m_nFirstSurface + nSurface, m_nGeneration }
	                                                : t3dRdSurfaceHandle_t {};
}
const t3dRdMeshSourceSurface_t *Terrain3DRdMeshCache::resolveSurface( const t3dRdSurfaceHandle_t& rHandle ) const
{
	return rHandle.m_nGeneration == m_nGeneration && rHandle.m_nSlot < m_surfaces.size() ? &m_surfaces[ rHandle.m_nSlot ] : NULL;
}
