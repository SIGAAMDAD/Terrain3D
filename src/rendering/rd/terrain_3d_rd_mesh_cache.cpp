// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_mesh_cache.h"

#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "data/terrain_3d_half_buffer.h"
#include "core/logger.h"

#include <EASTL/vector.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>


static uint64_t t3dRdHashU64( uint64_t nHash, const uint64_t nValue ) {
	for ( uint32_t i = 0u; i < 8u; ++i ) {
		nHash ^= ( nValue >> ( i * 8u ) ) & 0xffu;
		nHash *= s_nFnvPrime;
	}
	return nHash;
}

static uint64_t t3dRdObjectId( const Object *const pObject ) {
	return pObject ? ( uint64_t )( pObject->get_instance_id() ) : 0u;
}

static uint64_t t3dRdHashF32( uint64_t nHash, const float fValue ) {
	uint32_t bits = 0u;
	static_assert( sizeof( bits ) == sizeof( fValue ), "float hash ABI changed" );
	memcpy( &bits, &fValue, sizeof( bits ) );
	return t3dRdHashU64( nHash, bits );
}

static uint64_t t3dRdHashVector3( uint64_t nHash, const Vector3 &rValue ) {
	nHash = t3dRdHashF32( nHash, ( float )( rValue.x ) );
	nHash = t3dRdHashF32( nHash, ( float )( rValue.y ) );
	return t3dRdHashF32( nHash, ( float )( rValue.z ) );
}

static uint64_t t3dRdHashColor( uint64_t nHash, const Color &rValue ) {
	nHash = t3dRdHashF32( nHash, ( float )( rValue.r ) );
	nHash = t3dRdHashF32( nHash, ( float )( rValue.g ) );
	nHash = t3dRdHashF32( nHash, ( float )( rValue.b ) );
	return t3dRdHashF32( nHash, ( float )( rValue.a ) );
}

static uint64_t t3dRdHashTexture( uint64_t nHash, const Ref<Texture2D> &rTexture ) {
	nHash = t3dRdHashU64( nHash, t3dRdObjectId( rTexture.ptr() ) );
	if ( rTexture.is_null() ) {
		return nHash;
	}
	const RID texture_rid = rTexture->get_rid();
	nHash = t3dRdHashU64( nHash, ( uint64_t )( texture_rid.get_id() ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( rTexture->get_width() ) );
	return t3dRdHashU64( nHash, ( uint64_t )( rTexture->get_height() ) );
}

static uint64_t t3dRdHashMaterial( uint64_t nHash, const Ref<Material> &rMaterial ) {
	nHash = t3dRdHashU64( nHash, t3dRdObjectId( rMaterial.ptr() ) );
	BaseMaterial3D *const pBase = rMaterial.is_valid() ? Object::cast_to<BaseMaterial3D>( rMaterial.ptr() ) : NULL;
	if ( !pBase ) {
		return nHash;
	}
	nHash = t3dRdHashColor( nHash, pBase->get_albedo() );
	nHash = t3dRdHashF32( nHash, ( float )( pBase->get_metallic() ) );
	nHash = t3dRdHashF32( nHash, ( float )( pBase->get_roughness() ) );
	nHash = t3dRdHashF32( nHash, ( float )( pBase->get_ao_light_affect() ) );
	nHash = t3dRdHashF32( nHash, ( float )( pBase->get_alpha_scissor_threshold() ) );
	nHash = t3dRdHashF32( nHash, ( float )( pBase->get_normal_scale() ) );
	nHash = t3dRdHashColor( nHash, pBase->get_emission() );
	nHash = t3dRdHashF32( nHash, ( float )( pBase->get_emission_energy_multiplier() ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_transparency() ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_cull_mode() ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_shading_mode() ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_feature( BaseMaterial3D::FEATURE_EMISSION ) ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_feature( BaseMaterial3D::FEATURE_NORMAL_MAPPING ) ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_feature( BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION ) ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR ) ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_flag( BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR ) ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_flag( BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT ) ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_flag( BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS ) ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_texture_filter() ) );
	const BaseMaterial3D::TextureParam texture_slots[] = {
		BaseMaterial3D::TEXTURE_ALBEDO,
		BaseMaterial3D::TEXTURE_NORMAL,
		BaseMaterial3D::TEXTURE_ORM,
		BaseMaterial3D::TEXTURE_ROUGHNESS,
	};
	for ( size_t iTexture = 0u; iTexture < T3D_ARRAY_COUNT( texture_slots ); iTexture++ ) {
		const BaseMaterial3D::TextureParam slot = ( texture_slots )[iTexture];
		nHash = t3dRdHashTexture( nHash, pBase->get_texture( slot ) );
	}
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_roughness_texture_channel() ) );
	return nHash;
}

static int16_t t3dRdPackSnorm16( const float fValue ) {
	return ( int16_t )( lround( CLAMP( fValue, -1.0f, 1.0f ) * 32767.0f ) );
}

static uint64_t t3dRdHashBytes( uint64_t nHash, const PackedByteArray &rBytes ) {
	const uint8_t *const pBytes = rBytes.ptr();
	for ( int64_t i = 0; i < rBytes.size(); ++i ) {
		nHash ^= pBytes[i];
		nHash *= s_nFnvPrime;
	}
	return nHash;
}


static bool t3dRdBuildChunkHashes( const PackedByteArray &rBytes, eastl::vector<uint64_t, Terrain3DEastlAllocator> &rHashes ) {
	const uint32_t size = ( uint32_t )( rBytes.size() );
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
			hash ^= pBytes[i];
			hash *= s_nFnvPrime;
		}
		rHashes[chunk] = hash;
	}
	return true;
}

static bool t3dRdAddTextureSource(
        const Ref<Texture2D> &rTexture, eastl::vector<t3dRdImportedTextureSource_t, Terrain3DEastlAllocator> &rTextures ) {
	if ( rTexture.is_null() ) {
		return false;
	}
	const uint64_t source_id = t3dRdObjectId( rTexture.ptr() );
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( rTextures ).size() ); iTexture++ ) {
		const t3dRdImportedTextureSource_t &existing = ( rTextures )[iTexture];
		if ( existing.m_nSourceId == source_id ) {
			return true;
		}
	}
	const Ref<Image> source = rTexture->get_image();
	if ( source.is_null() || source->is_empty() ) {
		return false;
	}
	Ref<Image> upload;
	upload.instantiate();
	upload->copy_from( source );
	PackedByteArray bytes = upload->get_data();
	t3dRdTextureLayout_t layout;
	bool layout_valid = !bytes.is_empty()
	        && Terrain3DRdTextureManager::buildLayout(
	                upload, 1u, false, ( uint64_t )( bytes.size() ), T3D_RD_TEXTURE_LINEAR_DATA, layout );
	if ( !layout_valid && upload->has_mipmaps() ) {
		// Imported textures occasionally expose an invalid/overflowed mip count. Drop
		// only the broken mip chain and retain the source compression/base image.
		upload->clear_mipmaps();
		bytes = upload->get_data();
		layout_valid = !bytes.is_empty()
		        && Terrain3DRdTextureManager::buildLayout(
		                upload, 1u, false, ( uint64_t )( bytes.size() ), T3D_RD_TEXTURE_LINEAR_DATA, layout );
	}
	if ( !layout_valid ) {
		if ( upload->is_compressed() && upload->decompress() != OK ) {
			return false;
		}
		if ( upload->has_mipmaps() ) {
			upload->clear_mipmaps();
		}
		upload->convert( Image::FORMAT_RGBA8 );
		bytes = upload->get_data();
		layout_valid = !bytes.is_empty()
		        && Terrain3DRdTextureManager::buildLayout(
		                upload, 1u, false, ( uint64_t )( bytes.size() ), T3D_RD_TEXTURE_LINEAR_DATA, layout );
		if ( !layout_valid ) {
			return false;
		}
	}
	// RenderingDevice routes non-empty texture_create() initialization through the update-capable path.
	// Keep the resource immutable after publication, but retain CAN_UPDATE for the one cold initial upload.
	layout.m_nUsageBits = ( uint64_t )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT )
	        | ( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT );
	t3dRdImportedTextureSource_t record;
	record.m_nSourceId = source_id;
	record.m_layout = layout;
	uint64_t fingerprint = t3dRdHashU64( s_nFnvOffset, source_id );
	fingerprint = t3dRdHashU64( fingerprint, layout.m_nWidth );
	fingerprint = t3dRdHashU64( fingerprint, layout.m_nHeight );
	fingerprint = t3dRdHashU64( fingerprint, layout.m_nMipmaps );
	fingerprint = t3dRdHashU64( fingerprint, ( uint64_t )( layout.m_format ) );
	record.m_nFingerprint = t3dRdHashBytes( fingerprint, bytes );
	record.m_bytes = bytes;
	if ( rTextures.size() == rTextures.capacity()
	        && !t3dContainerReserveCold( rTextures, rTextures.capacity() ? rTextures.capacity() + rTextures.capacity() / 2u + 1u : 8u ) ) {
		return false;
	}
	return t3dVectorPushBackWithinCapacity( rTextures, eastl::move( record ) );
}

static uint16_t t3dRdAddMaterial( const Ref<Material> &rMaterial,
        eastl::vector<t3dRdInstanceMaterial_t, Terrain3DEastlAllocator> &rMaterials,
        eastl::vector<t3dRdImportedTextureSource_t, Terrain3DEastlAllocator> &rTextures ) {
	if ( rMaterial.is_null() ) {
		return UINT16_MAX;
	}
	if ( rMaterials.size() >= ( size_t )( UINT16_MAX ) ) {
		return UINT16_MAX;
	}
	t3dRdInstanceMaterial_t packed;
	BaseMaterial3D *const pBase = Object::cast_to<BaseMaterial3D>( rMaterial.ptr() );
	if ( pBase ) {
		const Color albedo = pBase->get_albedo();
		packed.m_szAlbedo[0] = ( float )( albedo.r );
		packed.m_szAlbedo[1] = ( float )( albedo.g );
		packed.m_szAlbedo[2] = ( float )( albedo.b );
		packed.m_szAlbedo[3] = ( float )( albedo.a );
		packed.m_fMetallic = ( float )( pBase->get_metallic() );
		packed.m_fRoughness = ( float )( pBase->get_roughness() );
		packed.m_fAo = ( float )( pBase->get_ao_light_affect() );
		packed.m_fAlphaScissor = ( float )( pBase->get_alpha_scissor_threshold() );
		packed.m_fNormalScale = ( float )( pBase->get_normal_scale() );
		const Color emission = pBase->get_emission();
		const float emission_energy = ( float )( pBase->get_emission_energy_multiplier() );
		packed.m_szEmission[0] = ( float )( emission.r ) * emission_energy;
		packed.m_szEmission[1] = ( float )( emission.g ) * emission_energy;
		packed.m_szEmission[2] = ( float )( emission.b ) * emission_energy;
		const BaseMaterial3D::Transparency transparency = pBase->get_transparency();
		if ( transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_ALPHA_SCISSOR;
		} else if ( transparency == BaseMaterial3D::TRANSPARENCY_ALPHA
		        || transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND;
			if ( transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS ) {
				packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS;
			}
		}
		if ( pBase->get_cull_mode() == BaseMaterial3D::CULL_DISABLED ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED;
		} else if ( pBase->get_cull_mode() == BaseMaterial3D::CULL_FRONT ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_CULL_FRONT;
		}
		if ( pBase->get_feature( BaseMaterial3D::FEATURE_EMISSION ) ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_EMISSIVE;
		}
		if ( pBase->get_feature( BaseMaterial3D::FEATURE_NORMAL_MAPPING ) ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_NORMAL_MAPPING;
		}
		if ( pBase->get_feature( BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION ) ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_AO_ENABLED;
		}
		if ( pBase->get_shading_mode() == BaseMaterial3D::SHADING_MODE_UNSHADED ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_UNSHADED;
		}
		if ( pBase->get_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR ) ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR;
		}
		if ( pBase->get_flag( BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR ) ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR_SRGB;
		}
		if ( pBase->get_flag( BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT ) ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT;
		}
		if ( pBase->get_flag( BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS ) ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_DONT_RECEIVE_SHADOWS;
		}
		const uint32_t filter = ( uint32_t )( CLAMP( ( int )( pBase->get_texture_filter() ),
		        ( int )( BaseMaterial3D::TEXTURE_FILTER_NEAREST ),
		        ( int )( BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC ) ) );
		packed.m_nFlags |= ( filter << T3D_RD_INSTANCE_MATERIAL_FILTER_SHIFT ) & T3D_RD_INSTANCE_MATERIAL_FILTER_MASK;
		const struct TextureSlot {
			BaseMaterial3D::TextureParam m_param;
			uint32_t m_flag;
			uint32_t *m_pLow;
			uint32_t *m_pHigh;
		} slots[] = {
			{ BaseMaterial3D::TEXTURE_ALBEDO,
			        T3D_RD_INSTANCE_MATERIAL_HAS_ALBEDO_TEXTURE,
			        &packed.m_nAlbedoTextureIdLow,
			        &packed.m_nAlbedoTextureIdHigh },
			{ BaseMaterial3D::TEXTURE_NORMAL,
			        T3D_RD_INSTANCE_MATERIAL_HAS_NORMAL_TEXTURE,
			        &packed.m_nNormalTextureIdLow,
			        &packed.m_nNormalTextureIdHigh },
			{ BaseMaterial3D::TEXTURE_ORM,
			        T3D_RD_INSTANCE_MATERIAL_HAS_ORM_TEXTURE,
			        &packed.m_nOrmTextureIdLow,
			        &packed.m_nOrmTextureIdHigh },
		};
		for ( size_t iSlot = 0u; iSlot < T3D_ARRAY_COUNT( slots ); iSlot++ ) {
			const TextureSlot &slot = ( slots )[iSlot];
			const Ref<Texture2D> texture = pBase->get_texture( slot.m_param );
			if ( texture.is_null() || !t3dRdAddTextureSource( texture, rTextures ) ) {
				continue;
			}
			const uint64_t id = t3dRdObjectId( texture.ptr() );
			*slot.m_pLow = ( uint32_t )( id );
			*slot.m_pHigh = ( uint32_t )( id >> 32u );
			packed.m_nFlags |= slot.m_flag;
		}
		if ( ( packed.m_nFlags & T3D_RD_INSTANCE_MATERIAL_HAS_ORM_TEXTURE ) == 0u ) {
			const Ref<Texture2D> roughness_texture = pBase->get_texture( BaseMaterial3D::TEXTURE_ROUGHNESS );
			if ( roughness_texture.is_valid() && t3dRdAddTextureSource( roughness_texture, rTextures ) ) {
				const uint64_t id = t3dRdObjectId( roughness_texture.ptr() );
				packed.m_nOrmTextureIdLow = ( uint32_t )( id );
				packed.m_nOrmTextureIdHigh = ( uint32_t )( id >> 32u );
				packed.m_nReserved0 = ( uint32_t )( pBase->get_roughness_texture_channel() );
				packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_HAS_ORM_TEXTURE | T3D_RD_INSTANCE_MATERIAL_ROUGHNESS_TEXTURE;
			}
		}
	}
	for ( uint16_t i = 0u; i < ( uint16_t )( rMaterials.size() ); ++i ) {
		if ( memcmp( &rMaterials[i], &packed, sizeof( packed ) ) == 0 ) {
			return i;
		}
	}
	if ( rMaterials.size() == rMaterials.capacity()
	        && !t3dContainerReserveCold(
	                rMaterials, rMaterials.capacity() ? rMaterials.capacity() + rMaterials.capacity() / 2u + 1u : 16u ) ) {
		return UINT16_MAX;
	}
	if ( !t3dVectorPushBackWithinCapacity( rMaterials, packed ) ) {
		return UINT16_MAX;
	}
	return ( uint16_t )( rMaterials.size() - 1u );
}

static bool t3dRdAppendBytes( PackedByteArray &rDestination, const void *const pSource, const uint32_t nBytes ) {
	if ( nBytes == 0u ) {
		return true;
	}
	const int64_t old_size = rDestination.size();
	if ( old_size > ( int64_t )( UINT32_MAX )-nBytes ) {
		return false;
	}
	rDestination.resize( old_size + nBytes );
	if ( rDestination.size() != old_size + nBytes ) {
		return false;
	}
	memcpy( rDestination.ptrw() + old_size, pSource, nBytes );
	return true;
}

static void t3dRdSetError( String *const pError, const String &rError ) {
	if ( pError ) {
		*pError = rError;
	}
}

static bool t3dRdAppendMeshSource( const Ref<Mesh> &rMesh,
        const uint64_t nSourceKey,
        const uint16_t nAssetId,
        const int16_t nLod,
        const Ref<Material> &rOverrideMaterial,
        const uint16_t nOverlayMaterial,
        t3dRdMeshSourceSnapshot_t &rResult,
        String *const pError ) {
	if ( rMesh.is_null() || rMesh->get_surface_count() <= 0 ) {
		return true;
	}
	if ( nSourceKey == 0u ) {
		t3dRdSetError( pError, "RD static mesh source key is invalid" );
		return false;
	}
	const size_t surface_capacity = rResult.m_surfaces.size() + ( size_t )( rMesh->get_surface_count() );
	if ( !t3dContainerReserveCold( rResult.m_surfaces, surface_capacity )
	        || !t3dContainerReserveCold( rResult.m_lods, rResult.m_lods.size() + 1u ) ) {
		t3dRdSetError( pError, "RD static mesh metadata capacity allocation failed" );
		return false;
	}
	t3dRdMeshSourceLod_t lod_record;
	lod_record.m_nSourceKey = nSourceKey;
	lod_record.m_nAssetId = nAssetId;
	lod_record.m_nLod = nLod;
	lod_record.m_nFirstSurface = ( uint32_t )( rResult.m_surfaces.size() );
	lod_record.m_bounds = rMesh->get_aabb();
	for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
		if ( rMesh->_surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		const Array arrays = rMesh->surface_get_arrays( surface );
		if ( arrays.size() <= Mesh::ARRAY_VERTEX || arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY ) {
			continue;
		}
		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		if ( positions.is_empty() || positions.size() > ( int64_t )( UINT32_MAX ) ) {
			continue;
		}
		const PackedVector3Array normals =
		        arrays.size() > Mesh::ARRAY_NORMAL && arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY
		        ? ( PackedVector3Array )( arrays[Mesh::ARRAY_NORMAL] )
		        : PackedVector3Array();
		const PackedFloat32Array tangents =
		        arrays.size() > Mesh::ARRAY_TANGENT && arrays[Mesh::ARRAY_TANGENT].get_type() == Variant::PACKED_FLOAT32_ARRAY
		        ? ( PackedFloat32Array )( arrays[Mesh::ARRAY_TANGENT] )
		        : PackedFloat32Array();
		const PackedColorArray colors =
		        arrays.size() > Mesh::ARRAY_COLOR && arrays[Mesh::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY
		        ? ( PackedColorArray )( arrays[Mesh::ARRAY_COLOR] )
		        : PackedColorArray();
		const PackedVector2Array uvs =
		        arrays.size() > Mesh::ARRAY_TEX_UV && arrays[Mesh::ARRAY_TEX_UV].get_type() == Variant::PACKED_VECTOR2_ARRAY
		        ? ( PackedVector2Array )( arrays[Mesh::ARRAY_TEX_UV] )
		        : PackedVector2Array();
		const PackedVector2Array uv2s =
		        arrays.size() > Mesh::ARRAY_TEX_UV2 && arrays[Mesh::ARRAY_TEX_UV2].get_type() == Variant::PACKED_VECTOR2_ARRAY
		        ? ( PackedVector2Array )( arrays[Mesh::ARRAY_TEX_UV2] )
		        : PackedVector2Array();
		const PackedInt32Array indices =
		        arrays.size() > Mesh::ARRAY_INDEX && arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY
		        ? ( PackedInt32Array )( arrays[Mesh::ARRAY_INDEX] )
		        : PackedInt32Array();
		const uint32_t vertex_count = ( uint32_t )( positions.size() );
		const uint32_t vertex_offset = ( uint32_t )( rResult.m_vertexBytes.size() / sizeof( t3dRdMeshVertex_t ) );
		for ( uint32_t vertex = 0u; vertex < vertex_count; ++vertex ) {
			t3dRdMeshVertex_t packed;
			const Vector3 position = positions[( int64_t )( vertex )];
			packed.m_szPosition[0] = ( float )( position.x );
			packed.m_szPosition[1] = ( float )( position.y );
			packed.m_szPosition[2] = ( float )( position.z );
			packed.m_nColorRgba8 = vertex < ( uint32_t )( colors.size() )
			        ? t3dRdPackGodotRgba32ForGpu( colors[( int64_t )( vertex )].to_rgba32() )
			        : 0xffffffffu;
			const Vector3 normal = vertex < ( uint32_t )( normals.size() ) ? normals[( int64_t )( vertex )] : Vector3( 0, 1, 0 );
			packed.m_szNormal[0] = t3dRdPackSnorm16( ( float )( normal.x ) );
			packed.m_szNormal[1] = t3dRdPackSnorm16( ( float )( normal.y ) );
			packed.m_szNormal[2] = t3dRdPackSnorm16( ( float )( normal.z ) );
			packed.m_szNormal[3] = 0;
			if ( tangents.size() >= ( int64_t )( vertex + 1u ) * 4 ) {
				for ( uint32_t component = 0u; component < 4u; ++component ) {
					packed.m_szTangent[component] = t3dRdPackSnorm16( tangents[( int64_t )( vertex * 4u + component )] );
				}
			}
			const Vector2 uv = vertex < ( uint32_t )( uvs.size() ) ? uvs[( int64_t )( vertex )] : Vector2();
			const Vector2 uv2 = vertex < ( uint32_t )( uv2s.size() ) ? uv2s[( int64_t )( vertex )] : Vector2();
			packed.m_szUv[0] = t3dHalfEncode( ( float )( uv.x ) );
			packed.m_szUv[1] = t3dHalfEncode( ( float )( uv.y ) );
			packed.m_szUv2[0] = t3dHalfEncode( ( float )( uv2.x ) );
			packed.m_szUv2[1] = t3dHalfEncode( ( float )( uv2.y ) );
			if ( !t3dRdAppendBytes( rResult.m_vertexBytes, &packed, sizeof( packed ) ) ) {
				t3dRdSetError( pError, "RD static mesh vertex arena exceeds 32-bit address space" );
				return false;
			}
		}
		uint32_t max_index = vertex_count - 1u;
		const uint32_t index_count = indices.is_empty() ? vertex_count : ( uint32_t )( indices.size() );
		if ( ( index_count % 3u ) != 0u ) {
			t3dRdSetError( pError, "RD static mesh has a non-triangle index count" );
			return false;
		}
		if ( !indices.is_empty() ) {
			max_index = 0u;
			for ( int64_t i = 0; i < indices.size(); ++i ) {
				if ( indices[i] < 0 || ( uint32_t )( indices[i] ) >= vertex_count ) {
					t3dRdSetError( pError, "RD static mesh has an out-of-range index" );
					return false;
				}
				max_index = MAX( max_index, ( uint32_t )( indices[i] ) );
			}
		}
		t3dRdMeshSourceSurface_t record;
		record.m_nSourceKey = nSourceKey;
		record.m_nAssetId = nAssetId;
		record.m_nLod = nLod;
		record.m_nSurface = ( uint16_t )( MIN( surface, ( int )( UINT16_MAX ) ) );
		record.m_nVertexOffset = vertex_offset;
		record.m_nVertexCount = vertex_count;
		record.m_nIndexCount = index_count;
		bool bounds_initialized = false;
		AABB surface_bounds;
		for ( int64_t vertex = 0; vertex < positions.size(); ++vertex ) {
			const Vector3 position = positions[vertex];
			if ( !bounds_initialized ) {
				surface_bounds = AABB( position, Vector3() );
				bounds_initialized = true;
			} else {
				surface_bounds.expand_to( position );
			}
		}
		record.m_bounds = surface_bounds;
		const Ref<Material> material = rOverrideMaterial.is_valid() ? rOverrideMaterial : rMesh->surface_get_material( surface );
		record.m_nMaterial = t3dRdAddMaterial( material, rResult.m_materials, rResult.m_textures );
		record.m_nOverlayMaterial = nOverlayMaterial;
		const uint32_t base_flags = record.m_nMaterial == UINT16_MAX ? 0u : rResult.m_materials[record.m_nMaterial].m_nFlags;
		const uint32_t overlay_flags =
		        record.m_nOverlayMaterial == UINT16_MAX ? base_flags : rResult.m_materials[record.m_nOverlayMaterial].m_nFlags;
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
		if ( max_index <= 65535u ) {
			record.m_nIndexFormat = ( uint32_t )( RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 );
			record.m_nIndexOffset = ( uint32_t )( rResult.m_index16Bytes.size() / sizeof( uint16_t ) );
			for ( uint32_t i = 0u; i < index_count; ++i ) {
				const uint16_t packed = ( uint16_t )( indices.is_empty() ? i : ( uint32_t )( indices[( int64_t )( i )] ) );
				if ( !t3dRdAppendBytes( rResult.m_index16Bytes, &packed, sizeof( packed ) ) ) {
					return false;
				}
			}
		} else {
			record.m_nIndexFormat = ( uint32_t )( RenderingDevice::INDEX_BUFFER_FORMAT_UINT32 );
			record.m_nIndexOffset = ( uint32_t )( rResult.m_index32Bytes.size() / sizeof( uint32_t ) );
			for ( uint32_t i = 0u; i < index_count; ++i ) {
				const uint32_t packed = indices.is_empty() ? i : ( uint32_t )( indices[( int64_t )( i )] );
				if ( !t3dRdAppendBytes( rResult.m_index32Bytes, &packed, sizeof( packed ) ) ) {
					return false;
				}
			}
		}
		if ( !t3dVectorPushBackWithinCapacity( rResult.m_surfaces, record ) ) {
			t3dRdSetError( pError, "RD static mesh surface capacity exhausted" );
			return false;
		}
	}
	lod_record.m_nSurfaceCount = ( uint32_t )( rResult.m_surfaces.size() ) - lod_record.m_nFirstSurface;
	if ( lod_record.m_nSurfaceCount > 0u ) {
		if ( !t3dVectorPushBackWithinCapacity( rResult.m_lods, lod_record ) ) {
			t3dRdSetError( pError, "RD static mesh LOD capacity exhausted" );
			return false;
		}
	}
	return true;
}


bool Terrain3DRdMeshCache::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_surfaces.set_allocator( allocator );
	m_lods.set_allocator( allocator );
	m_materials.set_allocator( allocator );
	m_textures.set_allocator( allocator );
	m_vertexChunkHashes.set_allocator( allocator );
	m_index16ChunkHashes.set_allocator( allocator );
	m_index32ChunkHashes.set_allocator( allocator );
	m_textureSyncNext.set_allocator( allocator );
	m_textureSyncReused.set_allocator( allocator );
#if T3D_INSTANCER_RD_PORT
	m_directSurfaces.set_allocator( allocator );
#endif
	return true;
}

Terrain3DRdMeshCache::Terrain3DRdMeshCache( void ) : m_context( Terrain3DRdContext::getShared() ) {
}

Terrain3DRdMeshCache::~Terrain3DRdMeshCache( void ) {
	shutdown();
}

void Terrain3DRdMeshCache::setError( const String &rError ) {
	m_lastError = rError;
}

uint32_t Terrain3DRdMeshCache::nextArenaCapacity( const uint32_t nRequiredBytes, const uint32_t nMinimumBytes ) {
	if ( nRequiredBytes == 0u ) {
		return 0u;
	}
	uint64_t capacity = MAX( ( uint64_t )( nMinimumBytes ), 4096ull );
	while ( capacity < nRequiredBytes ) {
		capacity <<= 1u;
	}
	return capacity > UINT32_MAX ? 0u : ( uint32_t )( capacity );
}

uint64_t Terrain3DRdMeshCache::computeSourceFingerprint( const Ref<Terrain3DAssets> &rAssets ) {
	return computeSourceFingerprint( rAssets, NULL, 0u );
}

uint64_t Terrain3DRdMeshCache::computeSourceFingerprint(
        const Ref<Terrain3DAssets> &rAssets, const t3dRdExternalMeshSource_t *pExternalMeshes, const size_t nExternalMeshCount ) {
	if ( rAssets.is_null() && nExternalMeshCount == 0u ) {
		return 0u;
	}
	uint64_t hash = s_nFnvOffset;
	const int mesh_count = rAssets.is_valid() ? rAssets->getMeshCount() : 0;
	hash = t3dRdHashU64( hash, ( uint64_t )( mesh_count ) );
	for ( int asset_id = 0; asset_id < mesh_count; ++asset_id ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( asset_id );
		if ( asset.is_null() ) {
			hash = t3dRdHashU64( hash, 0u );
			continue;
		}
		hash = t3dRdHashU64( hash, ( uint64_t )( asset->isEnabled() ) );
		hash = t3dRdHashU64( hash, ( uint64_t )( asset->isHighlighted() ) );
		hash = t3dRdHashU64( hash, ( uint64_t )( asset->getLodCount() ) );
		hash = t3dRdHashMaterial( hash, asset->getMaterialOverride() );
		hash = t3dRdHashMaterial( hash, asset->getMaterialOverlay() );
		hash = t3dRdHashMaterial( hash, asset->getHighlightMaterial() );
		for ( int lod = 0; lod < asset->getLodCount(); ++lod ) {
			const Ref<Mesh> mesh = asset->getMesh( lod );
			hash = t3dRdHashU64( hash, t3dRdRegularMeshSourceKey( ( uint16_t )( asset_id ), ( int16_t )( lod ) ) );
			hash = t3dRdHashU64( hash, t3dRdObjectId( mesh.ptr() ) );
			if ( mesh.is_null() ) {
				continue;
			}
			hash = t3dRdHashU64( hash, ( uint64_t )( mesh->get_surface_count() ) );
			const AABB bounds = mesh->get_aabb();
			hash = t3dRdHashVector3( hash, bounds.position );
			hash = t3dRdHashVector3( hash, bounds.size );
			for ( int surface = 0; surface < mesh->get_surface_count(); ++surface ) {
				hash = t3dRdHashU64( hash, ( uint64_t )( mesh->_surface_get_primitive_type( surface ) ) );
				hash = t3dRdHashU64( hash, ( uint64_t )( MAX( mesh->_surface_get_array_len( surface ), 0 ) ) );
				hash = t3dRdHashU64( hash, ( uint64_t )( MAX( mesh->_surface_get_array_index_len( surface ), 0 ) ) );
				hash = t3dRdHashMaterial( hash, mesh->surface_get_material( surface ) );
			}
		}
	}
	hash = t3dRdHashU64( hash, ( uint64_t )( nExternalMeshCount ) );
	for ( size_t iEntry = 0u; iEntry < nExternalMeshCount; iEntry++ ) {
		const t3dRdExternalMeshSource_t &source = pExternalMeshes[iEntry];
		hash = t3dRdHashU64( hash, source.m_nSourceKey );
		hash = t3dRdHashU64( hash, t3dRdObjectId( source.m_mesh.ptr() ) );
		if ( source.m_mesh.is_null() ) {
			continue;
		}
		hash = t3dRdHashU64( hash, ( uint64_t )( source.m_mesh->get_surface_count() ) );
		const AABB bounds = source.m_mesh->get_aabb();
		hash = t3dRdHashVector3( hash, bounds.position );
		hash = t3dRdHashVector3( hash, bounds.size );
		for ( int surface = 0; surface < source.m_mesh->get_surface_count(); ++surface ) {
			hash = t3dRdHashU64( hash, ( uint64_t )( source.m_mesh->_surface_get_primitive_type( surface ) ) );
			hash = t3dRdHashU64( hash, ( uint64_t )( MAX( source.m_mesh->_surface_get_array_len( surface ), 0 ) ) );
			hash = t3dRdHashU64( hash, ( uint64_t )( MAX( source.m_mesh->_surface_get_array_index_len( surface ), 0 ) ) );
			hash = t3dRdHashMaterial( hash, source.m_mesh->surface_get_material( surface ) );
		}
	}
	return hash == 0u ? 1u : hash;
}

bool Terrain3DRdMeshCache::buildSourceSnapshot( const Ref<Terrain3DAssets> &rAssets,
        t3dRdMeshSourceSnapshot_t &rResult,
        const uint64_t nFingerprint,
        const uint64_t nGeneration,
        String *const pError,
        const bool bIncludeDisabled ) {
	return buildSourceSnapshot( rAssets, NULL, 0u, rResult, nFingerprint, nGeneration, pError, bIncludeDisabled );
}

bool Terrain3DRdMeshCache::buildSourceSnapshot( const Ref<Terrain3DAssets> &rAssets,
        const t3dRdExternalMeshSource_t *pExternalMeshes,
        const size_t nExternalMeshCount,
        t3dRdMeshSourceSnapshot_t &rResult,
        const uint64_t nFingerprint,
        const uint64_t nGeneration,
        String *const pError,
        const bool bIncludeDisabled ) {
	if ( rAssets.is_null() && nExternalMeshCount == 0u ) {
		t3dRdSetError( pError, "Terrain3D mesh sources unavailable" );
		return false;
	}
	rResult.m_vertexBytes.clear();
	rResult.m_index16Bytes.clear();
	rResult.m_index32Bytes.clear();
	rResult.m_vertexChunkHashes.clear();
	rResult.m_index16ChunkHashes.clear();
	rResult.m_index32ChunkHashes.clear();
	rResult.m_surfaces.clear();
	rResult.m_lods.clear();
	rResult.m_materials.clear();
	rResult.m_textures.clear();
	rResult.m_nFingerprint = nFingerprint;
	rResult.m_nGeneration = nGeneration == 0u ? 1u : nGeneration;
	const int mesh_count = rAssets.is_valid() ? rAssets->getMeshCount() : 0;
	for ( int asset_id = 0; asset_id < mesh_count; ++asset_id ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( asset_id );
		if ( asset.is_null() || ( !bIncludeDisabled && !asset->isEnabled() ) ) {
			continue;
		}
		const Ref<Material> override_material = asset->isHighlighted() && asset->getHighlightMaterial().is_valid()
		        ? asset->getHighlightMaterial()
		        : asset->getMaterialOverride();
		const uint16_t overlay_material = asset->isHighlighted()
		        ? UINT16_MAX
		        : t3dRdAddMaterial( asset->getMaterialOverlay(), rResult.m_materials, rResult.m_textures );
		for ( int lod = 0; lod < asset->getLodCount(); ++lod ) {
			const Ref<Mesh> mesh = asset->getMesh( lod );
			if ( !t3dRdAppendMeshSource( mesh,
			             t3dRdRegularMeshSourceKey( ( uint16_t )( asset_id ), ( int16_t )( lod ) ),
			             ( uint16_t )( asset_id ),
			             ( int16_t )( lod ),
			             override_material,
			             overlay_material,
			             rResult,
			             pError ) ) {
				return false;
			}
		}
	}
	eastl::vector<uint64_t, Terrain3DEastlAllocator> external_keys;
	if ( !t3dContainerReserveCold( external_keys, nExternalMeshCount ) ) {
		t3dRdSetError( pError, "RD external mesh key scratch capacity allocation failed" );
		return false;
	}
	for ( size_t iEntry = 0u; iEntry < nExternalMeshCount; iEntry++ ) {
		const t3dRdExternalMeshSource_t &source = pExternalMeshes[iEntry];
		if ( source.m_nSourceKey == 0u || source.m_mesh.is_null() ) {
			continue;
		}
		if ( eastl::find( external_keys.begin(), external_keys.end(), source.m_nSourceKey ) != external_keys.end() ) {
			t3dRdSetError( pError, "Duplicate HLOD mesh source key in RD publication" );
			return false;
		}
		for ( size_t iLod = 0u; iLod < ( size_t )( ( rResult.m_lods ).size() ); iLod++ ) {
			const t3dRdMeshSourceLod_t &existing = ( rResult.m_lods )[iLod];
			if ( existing.m_nSourceKey == source.m_nSourceKey ) {
				t3dRdSetError( pError, "HLOD mesh source key collides with an existing RD mesh source" );
				return false;
			}
		}
		if ( !t3dVectorPushBackWithinCapacity( external_keys, source.m_nSourceKey ) ) {
			t3dRdSetError( pError, "RD external mesh key scratch capacity exhausted" );
			return false;
		}
		if ( !t3dRdAppendMeshSource( source.m_mesh, source.m_nSourceKey, UINT16_MAX, 0, Ref<Material>(), UINT16_MAX, rResult, pError ) ) {
			return false;
		}
	}
	if ( !t3dRdBuildChunkHashes( rResult.m_vertexBytes, rResult.m_vertexChunkHashes )
	        || !t3dRdBuildChunkHashes( rResult.m_index16Bytes, rResult.m_index16ChunkHashes )
	        || !t3dRdBuildChunkHashes( rResult.m_index32Bytes, rResult.m_index32ChunkHashes ) ) {
		t3dRdSetError( pError, "RD mesh chunk-hash capacity allocation failed" );
		return false;
	}
	if ( pError ) {
		*pError = String();
	}
	return true;
}

bool Terrain3DRdMeshCache::ensureVertexFormat( void ) {
	if ( m_nVertexFormat != RenderingDevice::INVALID_FORMAT_ID ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	TypedArray<Ref<RDVertexAttribute>> attributes;
	const struct AttributeDesc {
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
		const AttributeDesc &item = ( desc )[iDescriptor];
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
	return m_nVertexFormat != RenderingDevice::INVALID_FORMAT_ID
	        && m_context.registerVertexFormat( m_nVertexFormat, "T3D.VertexFormat.InstanceMesh48" );
}

bool Terrain3DRdMeshCache::ensureArena( RID &rBuffer,
        uint32_t &rCapacityBytes,
        const uint32_t nRequiredBytes,
        const String &rName,
        const Terrain3DRdContext::ResourceType resourceType,
        const Terrain3DRdContext::MemoryCategory memoryCategory ) {
	if ( nRequiredBytes == 0u ) {
		return true;
	}
	const uint32_t minimum = resourceType == Terrain3DRdContext::RESOURCE_VERTEX_BUFFER ? s_nMinVertexArenaBytes : s_nMinIndexArenaBytes;
	if ( rBuffer.is_valid() && rCapacityBytes >= nRequiredBytes ) {
		return true;
	}
	++m_nArenaCapacityMissCount;
	++m_nArenaGrowthRequestCount;
	const uint32_t capacity = nextArenaCapacity( nRequiredBytes, minimum );
	if ( capacity == 0u ) {
		++m_nArenaRebuildFailureCount;
		setError( String( "RD mesh arena capacity calculation overflowed; resource=" ) + rName + String( ", required=" )
		        + String::num_int64( nRequiredBytes ) + String( ", minimum=" ) + String::num_int64( minimum ) );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		++m_nArenaRebuildFailureCount;
		setError( String( "RD mesh arena lost the RenderingDevice; resource=" ) + rName + String( ", required=" )
		        + String::num_int64( nRequiredBytes ) );
		return false;
	}
	if ( rBuffer.is_valid() ) {
		// Array RIDs reference their parent buffers. Destroy every child view before
		// replacing an arena so Godot never auto-invalidates a RID still tracked by
		// Terrain3DRdContext.
		discardArrays();
		m_context.freeResource( rBuffer, rCapacityBytes );
	}
	if ( resourceType == Terrain3DRdContext::RESOURCE_VERTEX_BUFFER ) {
		rBuffer = rd->vertex_buffer_create( capacity );
	} else {
		const RenderingDevice::IndexBufferFormat format =
		        rName.find( "16" ) >= 0 ? RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 : RenderingDevice::INDEX_BUFFER_FORMAT_UINT32;
		rBuffer = rd->index_buffer_create( capacity / ( format == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 2u : 4u ), format );
	}
	if ( !rBuffer.is_valid() ) {
		++m_nArenaRebuildFailureCount;
		setError( String( "RD mesh arena creation returned an invalid RID; resource=" ) + rName + String( ", capacity=" )
		        + String::num_int64( capacity ) );
		return false;
	}
	if ( !m_context.registerResource(
	             rBuffer, rName, resourceType, memoryCategory, capacity, Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( rBuffer );
		rBuffer = RID();
		++m_nArenaRebuildFailureCount;
		setError( String( "RD mesh arena registration failed; resource=" ) + rName + String( ", capacity=" )
		        + String::num_int64( capacity ) );
		return false;
	}
	rCapacityBytes = capacity;
	++m_nArenaRebuildCount;
	return true;
}

#if T3D_INSTANCER_RD_PORT
void Terrain3DRdMeshCache::discardDirectSurfaces( void ) {
	for ( size_t iSurface = 0u; iSurface < m_directSurfaces.size(); ++iSurface ) {
		t3dRdDirectSurfaceGpu_t &surface = m_directSurfaces[iSurface];
		m_context.freeResource( surface.m_hVertexArray );
		m_context.freeResource( surface.m_hIndexArray );
		m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
		m_context.freeResource( surface.m_hIndexBuffer, surface.m_nIndexBytes );
	}
	m_directSurfaces.clear();
}
#endif

void Terrain3DRdMeshCache::discardArrays( void ) {
#if T3D_INSTANCER_RD_PORT
	discardDirectSurfaces();
#endif
	m_context.freeResource( m_hVertexArray );
	m_context.freeResource( m_hIndex16Array );
	m_context.freeResource( m_hIndex32Array );
}

bool Terrain3DRdMeshCache::rebuildArrays( void ) {
	discardArrays();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "RD mesh array rebuild lost the RenderingDevice" );
		return false;
	}
	if ( m_hVertexArena.is_valid() ) {
		TypedArray<RID> buffers;
		buffers.push_back( m_hVertexArena );
		m_hVertexArray = rd->vertex_array_create( m_nVertexCapacityBytes / sizeof( t3dRdMeshVertex_t ), m_nVertexFormat, buffers );
		if ( !m_hVertexArray.is_valid() ) {
			setError( String( "RD mesh vertex-array creation failed; vertex_capacity_bytes=" ) + String::num_int64( m_nVertexCapacityBytes )
			        + String( ", vertex_format=" ) + String::num_int64( m_nVertexFormat ) );
			return false;
		}
		if ( !m_context.registerResource( m_hVertexArray,
		             "T3D.Instance.MeshVertexArray",
		             Terrain3DRdContext::RESOURCE_VERTEX_ARRAY,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( m_hVertexArray );
			m_hVertexArray = RID();
			setError( "RD mesh vertex-array registration failed" );
			return false;
		}
	}
	if ( m_hIndex16Arena.is_valid() ) {
		m_hIndex16Array = rd->index_array_create( m_hIndex16Arena, 0u, m_nIndex16CapacityBytes / sizeof( uint16_t ) );
		if ( !m_hIndex16Array.is_valid() ) {
			setError( String( "RD mesh uint16 index-array creation failed; index_capacity_bytes=" )
			        + String::num_int64( m_nIndex16CapacityBytes ) );
			return false;
		}
		if ( !m_context.registerResource( m_hIndex16Array,
		             "T3D.Instance.MeshIndexArray16",
		             Terrain3DRdContext::RESOURCE_INDEX_ARRAY,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( m_hIndex16Array );
			m_hIndex16Array = RID();
			setError( "RD mesh uint16 index-array registration failed" );
			return false;
		}
	}
	if ( m_hIndex32Arena.is_valid() ) {
		m_hIndex32Array = rd->index_array_create( m_hIndex32Arena, 0u, m_nIndex32CapacityBytes / sizeof( uint32_t ) );
		if ( !m_hIndex32Array.is_valid() ) {
			setError( String( "RD mesh uint32 index-array creation failed; index_capacity_bytes=" )
			        + String::num_int64( m_nIndex32CapacityBytes ) );
			return false;
		}
		if ( !m_context.registerResource( m_hIndex32Array,
		             "T3D.Instance.MeshIndexArray32",
		             Terrain3DRdContext::RESOURCE_INDEX_ARRAY,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( m_hIndex32Array );
			m_hIndex32Array = RID();
			setError( "RD mesh uint32 index-array registration failed" );
			return false;
		}
	}
	return m_hVertexArray.is_valid();
}

#if T3D_INSTANCER_RD_PORT
bool Terrain3DRdMeshCache::rebuildDirectSurfaces( const t3dRdMeshSourceSnapshot_t *const pSnapshot ) {
	discardDirectSurfaces();
	if ( !pSnapshot ) {
		setError( "RD direct-surface rebuild received a null mesh snapshot" );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "RD direct-surface rebuild lost the RenderingDevice" );
		return false;
	}
	if ( !t3dContainerReserveCold( m_directSurfaces, pSnapshot->m_surfaces.size() ) ) {
		setError( String( "RD direct-surface view reserve failed; required=" ) + String::num_int64( pSnapshot->m_surfaces.size() ) );
		return false;
	}
	for ( uint32_t iSurface = 0u; iSurface < ( uint32_t )( pSnapshot->m_surfaces.size() ); ++iSurface ) {
		const t3dRdMeshSourceSurface_t &source = pSnapshot->m_surfaces[iSurface];
		const uint64_t vertex_offset64 = ( uint64_t )( source.m_nVertexOffset ) * sizeof( t3dRdMeshVertex_t );
		const uint64_t vertex_bytes64 = ( uint64_t )( source.m_nVertexCount ) * sizeof( t3dRdMeshVertex_t );
		const uint32_t index_stride = source.m_nIndexFormat == ( uint32_t )( RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 )
		        ? sizeof( uint16_t )
		        : sizeof( uint32_t );
		const uint64_t index_offset64 = ( uint64_t )( source.m_nIndexOffset ) * index_stride;
		const uint64_t index_bytes64 = ( uint64_t )( source.m_nIndexCount ) * index_stride;
		const PackedByteArray &index_source = source.m_nIndexFormat == ( uint32_t )( RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 )
		        ? pSnapshot->m_index16Bytes
		        : pSnapshot->m_index32Bytes;
		if ( vertex_bytes64 == 0u || index_bytes64 == 0u || vertex_offset64 + vertex_bytes64 > ( uint64_t )( pSnapshot->m_vertexBytes.size() )
		        || index_offset64 + index_bytes64 > ( uint64_t )( index_source.size() ) || vertex_bytes64 > UINT32_MAX
		        || index_bytes64 > UINT32_MAX ) {
			setError( String( "RD direct-surface source range is invalid; surface=" ) + String::num_int64( iSurface )
			        + String( ", vertex_offset=" ) + String::num_int64( vertex_offset64 ) + String( ", vertex_bytes=" )
			        + String::num_int64( vertex_bytes64 ) + String( ", index_offset=" ) + String::num_int64( index_offset64 )
			        + String( ", index_bytes=" ) + String::num_int64( index_bytes64 ) );
			discardDirectSurfaces();
			return false;
		}

		t3dRdDirectSurfaceGpu_t surface;
		surface.m_nVertexBytes = ( uint32_t )( vertex_bytes64 );
		surface.m_nIndexBytes = ( uint32_t )( index_bytes64 );
		const PackedByteArray vertex_bytes = pSnapshot->m_vertexBytes.slice( ( int64_t )( vertex_offset64 ), ( int64_t )( vertex_offset64 + vertex_bytes64 ) );
		const PackedByteArray index_bytes = index_source.slice( ( int64_t )( index_offset64 ), ( int64_t )( index_offset64 + index_bytes64 ) );
		surface.m_hVertexBuffer = rd->vertex_buffer_create( surface.m_nVertexBytes, vertex_bytes );
		if ( !surface.m_hVertexBuffer.is_valid() ) {
			setError( String( "RD direct-surface vertex-buffer creation failed; surface=" ) + String::num_int64( iSurface )
			        + String( ", bytes=" ) + String::num_int64( surface.m_nVertexBytes ) );
			discardDirectSurfaces();
			return false;
		}
		const String vertex_name = String( "T3D.Instance.DirectSurface.Vertex." ) + String::num_int64( iSurface );
		if ( !m_context.registerResource( surface.m_hVertexBuffer,
		             vertex_name,
		             Terrain3DRdContext::RESOURCE_VERTEX_BUFFER,
		             Terrain3DRdContext::MEMORY_MESH_VERTEX_ARENA,
		             surface.m_nVertexBytes,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( surface.m_hVertexBuffer );
			surface.m_hVertexBuffer = RID();
			setError( String( "RD direct-surface vertex-buffer registration failed; surface=" ) + String::num_int64( iSurface ) );
			discardDirectSurfaces();
			return false;
		}
		TypedArray<RID> vertex_buffers;
		vertex_buffers.push_back( surface.m_hVertexBuffer );
		surface.m_hVertexArray = rd->vertex_array_create( source.m_nVertexCount, m_nVertexFormat, vertex_buffers );
		if ( !surface.m_hVertexArray.is_valid() ) {
			m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
			setError( String( "RD direct-surface vertex-array creation failed; surface=" ) + String::num_int64( iSurface )
			        + String( ", vertices=" ) + String::num_int64( source.m_nVertexCount ) );
			discardDirectSurfaces();
			return false;
		}
		const String vertex_array_name = String( "T3D.Instance.DirectSurface.VertexArray." ) + String::num_int64( iSurface );
		if ( !m_context.registerResource( surface.m_hVertexArray,
		             vertex_array_name,
		             Terrain3DRdContext::RESOURCE_VERTEX_ARRAY,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( surface.m_hVertexArray );
			surface.m_hVertexArray = RID();
			m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
			setError( String( "RD direct-surface vertex-array registration failed; surface=" ) + String::num_int64( iSurface ) );
			discardDirectSurfaces();
			return false;
		}

		const RenderingDevice::IndexBufferFormat index_format = ( RenderingDevice::IndexBufferFormat )( source.m_nIndexFormat );
		surface.m_hIndexBuffer = rd->index_buffer_create( source.m_nIndexCount, index_format, index_bytes );
		if ( !surface.m_hIndexBuffer.is_valid() ) {
			m_context.freeResource( surface.m_hVertexArray );
			m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
			setError( String( "RD direct-surface index-buffer creation failed; surface=" ) + String::num_int64( iSurface )
			        + String( ", indices=" ) + String::num_int64( source.m_nIndexCount ) );
			discardDirectSurfaces();
			return false;
		}
		const String index_name = String( "T3D.Instance.DirectSurface.Index." ) + String::num_int64( iSurface );
		if ( !m_context.registerResource( surface.m_hIndexBuffer,
		             index_name,
		             Terrain3DRdContext::RESOURCE_INDEX_BUFFER,
		             Terrain3DRdContext::MEMORY_MESH_INDEX_ARENA,
		             surface.m_nIndexBytes,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( surface.m_hIndexBuffer );
			surface.m_hIndexBuffer = RID();
			m_context.freeResource( surface.m_hVertexArray );
			m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
			setError( String( "RD direct-surface index-buffer registration failed; surface=" ) + String::num_int64( iSurface ) );
			discardDirectSurfaces();
			return false;
		}
		surface.m_hIndexArray = rd->index_array_create( surface.m_hIndexBuffer, 0u, source.m_nIndexCount );
		if ( !surface.m_hIndexArray.is_valid() ) {
			m_context.freeResource( surface.m_hIndexBuffer, surface.m_nIndexBytes );
			m_context.freeResource( surface.m_hVertexArray );
			m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
			setError( String( "RD direct-surface index-array creation failed; surface=" ) + String::num_int64( iSurface )
			        + String( ", indices=" ) + String::num_int64( source.m_nIndexCount ) );
			discardDirectSurfaces();
			return false;
		}
		const String index_array_name = String( "T3D.Instance.DirectSurface.IndexArray." ) + String::num_int64( iSurface );
		if ( !m_context.registerResource( surface.m_hIndexArray,
		             index_array_name,
		             Terrain3DRdContext::RESOURCE_INDEX_ARRAY,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( surface.m_hIndexArray );
			surface.m_hIndexArray = RID();
			m_context.freeResource( surface.m_hIndexBuffer, surface.m_nIndexBytes );
			m_context.freeResource( surface.m_hVertexArray );
			m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
			setError( String( "RD direct-surface index-array registration failed; surface=" ) + String::num_int64( iSurface ) );
			discardDirectSurfaces();
			return false;
		}
		if ( !t3dVectorPushBackWithinCapacity( m_directSurfaces, surface ) ) {
			m_context.freeResource( surface.m_hIndexArray );
			m_context.freeResource( surface.m_hIndexBuffer, surface.m_nIndexBytes );
			m_context.freeResource( surface.m_hVertexArray );
			m_context.freeResource( surface.m_hVertexBuffer, surface.m_nVertexBytes );
			setError( String( "RD direct-surface table exceeded reserved capacity; surface=" ) + String::num_int64( iSurface ) );
			discardDirectSurfaces();
			return false;
		}
	}
	return true;
}
#endif

void Terrain3DRdMeshCache::discardTextures( void ) {
	Terrain3DRdTextureManager *const pManager = Terrain3DRdTextureManager::getSingleton();
	if ( pManager ) {
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textures ).size() ); iTexture++ ) {
			const t3dRdImportedTextureGpu_t &texture = ( m_textures )[iTexture];
			pManager->freeTexture( texture.m_hTexture, texture.m_nBytes );
		}
	}
	m_textures.clear();
}

bool Terrain3DRdMeshCache::syncTextures( const t3dRdMeshSourceSnapshot_t *const pSnapshot ) {
	if ( !pSnapshot ) {
		setError( "RD instance material texture synchronization received a null mesh snapshot" );
		return false;
	}
	Terrain3DRdTextureManager *const pManager = Terrain3DRdTextureManager::getSingleton();
	if ( !pManager ) {
		if ( !pSnapshot->m_textures.empty() ) {
			++m_nTextureFallbackCount;
			LOG( WARN, "RD instance material texture manager unavailable; retaining mesh geometry with fallback materials. textures=",
			        pSnapshot->m_textures.size() );
		}
		return true;
	}
	m_textureSyncNext.clear();
	m_textureSyncReused.clear();
	if ( m_textureSyncNext.capacity() < pSnapshot->m_textures.size() ) {
		if ( !t3dContainerReserveCold( m_textureSyncNext, pSnapshot->m_textures.size() ) ) {
			setError( String( "RD instance texture sync could not reserve next-state table; required=" )
			        + String::num_int64( pSnapshot->m_textures.size() ) );
			return false;
		}
	}
	if ( !t3dVectorResizeCold( m_textureSyncReused, m_textures.size(), 0u ) ) {
		setError( String( "RD instance texture sync could not resize reuse table; required=" ) + String::num_int64( m_textures.size() ) );
		return false;
	}
	eastl::vector<t3dRdImportedTextureGpu_t, Terrain3DEastlAllocator> &next = m_textureSyncNext;
	eastl::vector<uint8_t, Terrain3DEastlAllocator> &reused = m_textureSyncReused;
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( pSnapshot->m_textures ).size() ); iTexture++ ) {
		const t3dRdImportedTextureSource_t &source = ( pSnapshot->m_textures )[iTexture];
		bool matched = false;
		for ( size_t i = 0u; i < m_textures.size(); ++i ) {
			if ( reused[i] || m_textures[i].m_nSourceId != source.m_nSourceId || m_textures[i].m_nFingerprint != source.m_nFingerprint
			        || !m_textures[i].m_hTexture.is_valid() ) {
				continue;
			}
			if ( !t3dVectorPushBackWithinCapacity( next, m_textures[i] ) ) {
				setError( "RD instance texture sync next-state table exceeded sealed capacity while retaining a resident texture" );
				return false;
			}
			reused[i] = true;
			matched = true;
			break;
		}
		if ( matched ) {
			continue;
		}
		TypedArray<PackedByteArray> initial_data;
		initial_data.push_back( source.m_bytes );
		const String name = String( "T3D.Instance.MaterialTexture." ) + String::num_uint64( source.m_nSourceId );
		const RID texture = pManager->createTexture( source.m_layout,
		        initial_data,
		        name,
		        ( int )( Terrain3DRdContext::MEMORY_MATERIAL_TABLE ),
		        false );
		if ( !texture.is_valid() ) {
			// Material textures are optional to mesh visibility. Keep the mesh resident
			// and let material binding resolve this source ID to the fallback texture.
			++m_nTextureFallbackCount;
			LOG( WARN,
			        "RD instance material texture upload failed; retaining mesh with fallback texture. source_id=",
			        source.m_nSourceId,
			        " size=",
			        source.m_layout.m_nWidth,
			        "x",
			        source.m_layout.m_nHeight,
			        " mips=",
			        source.m_layout.m_nMipmaps );
			continue;
		}
		if ( !t3dVectorPushBackWithinCapacity(
		             next, t3dRdImportedTextureGpu_t{ source.m_nSourceId, source.m_nFingerprint, texture, source.m_layout.m_nBytes } ) ) {
			pManager->freeTexture( texture, source.m_layout.m_nBytes );
			setError( String( "RD instance texture sync next-state table exceeded sealed capacity after texture creation; source_id=" )
			        + String::num_uint64( source.m_nSourceId ) );
			return false;
		}
	}
	for ( size_t i = 0u; i < m_textures.size(); ++i ) {
		if ( !reused[i] ) {
			pManager->freeTexture( m_textures[i].m_hTexture, m_textures[i].m_nBytes );
		}
	}
	m_textures.swap( next );
	return true;
}

RID Terrain3DRdMeshCache::getTextureRid( const uint64_t nSourceId ) const {
	if ( nSourceId == 0u ) {
		return RID();
	}
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textures ).size() ); iTexture++ ) {
		const t3dRdImportedTextureGpu_t &texture = ( m_textures )[iTexture];
		if ( texture.m_nSourceId == nSourceId ) {
			return texture.m_hTexture;
		}
	}
	return RID();
}

bool Terrain3DRdMeshCache::uploadChangedChunks( const RID &rBuffer,
        const PackedByteArray &rCurrent,
        const eastl::vector<uint64_t, Terrain3DEastlAllocator> &rCurrentHashes,
        const eastl::vector<uint64_t, Terrain3DEastlAllocator> &rPreviousHashes,
        const bool bForceFull,
        uint64_t &nUploadedBytes ) {
	RenderingDevice *const pRd = m_context.getDevice();
	if ( !pRd ) {
		return false;
	}
	const uint32_t nCurrentSize = ( uint32_t )rCurrent.size();
	for ( uint32_t nChunk = 0u; nChunk < ( uint32_t )rCurrentHashes.size(); ++nChunk ) {
		if ( !bForceFull && nChunk < rPreviousHashes.size() && rPreviousHashes[nChunk] == rCurrentHashes[nChunk] ) {
			continue;
		}
		const uint32_t nOffset = nChunk * s_nDirtyUploadChunkBytes;
		const uint32_t nBytes = MIN( s_nDirtyUploadChunkBytes, nCurrentSize - nOffset );
		memcpy( m_dirtyUploadScratch.ptrw(), rCurrent.ptr() + nOffset, nBytes );
		if ( pRd->buffer_update( rBuffer, nOffset, nBytes, m_dirtyUploadScratch ) != OK ) {
			return false;
		}
		nUploadedBytes += nBytes;
	}
	return true;
}

bool Terrain3DRdMeshCache::upload( const t3dRdMeshSourceSnapshot_t *const pSnapshot ) {
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
		setError( String( "RD mesh-cache upload could not initialize shared RenderingDevice context; state=" )
		        + String::num_int64( ( int64_t )( m_context.getState() ) ) + String( ", failure=" )
		        + String::num_int64( ( int64_t )( m_context.getFailureCode() ) ) );
		return false;
	}
	if ( !m_context.validateDevice() ) {
		++m_nUploadFailureCount;
		setError( String( "RD mesh-cache upload lost the shared RenderingDevice; state=" )
		        + String::num_int64( ( int64_t )( m_context.getState() ) ) + String( ", failure=" )
		        + String::num_int64( ( int64_t )( m_context.getFailureCode() ) ) );
		return false;
	}
	if ( !ensureVertexFormat() ) {
		++m_nUploadFailureCount;
		setError( "RD mesh-cache upload failed to create/register the instance vertex format" );
		return false;
	}
#if T3D_INSTANCER_RD_PORT
	// The integrated RD instancer uses exact per-surface buffers/views rather
	// than the experimental shared arena. Material metadata and imported
	// textures are still retained because this is the production instancer
	// presentation path, not the old magenta bring-up lane.
	if ( !t3dVectorCopyCold( m_surfaces, pSnapshot->m_surfaces ) || !t3dVectorCopyCold( m_lods, pSnapshot->m_lods )
	        || !t3dVectorCopyCold( m_materials, pSnapshot->m_materials ) ) {
		++m_nUploadFailureCount;
		setError( "RD instancer port failed to retain mesh/material metadata" );
		return false;
	}
	m_context.beginResourceRebuild();
	if ( !syncTextures( pSnapshot ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD instancer port failed while synchronizing material textures" );
		}
		return false;
	}
	const bool direct_ok = rebuildDirectSurfaces( pSnapshot );
	m_context.endResourceRebuild();
	if ( !direct_ok ) {
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD instancer port failed to build standalone surface geometry" );
		}
		return false;
	}
	m_nVertexUsedBytes = ( uint32_t )( pSnapshot->m_vertexBytes.size() );
	m_nIndex16UsedBytes = ( uint32_t )( pSnapshot->m_index16Bytes.size() );
	m_nIndex32UsedBytes = ( uint32_t )( pSnapshot->m_index32Bytes.size() );
	m_nSourceGeneration = pSnapshot->m_nGeneration;
	if ( ++m_nGeneration == 0u ) {
		m_nGeneration = 1u;
	}
	m_lastError = String();
	return true;
#endif
	const bool grow_vertex = !m_hVertexArena.is_valid() || m_nVertexCapacityBytes < ( uint32_t )( pSnapshot->m_vertexBytes.size() );
	const bool grow_i16 = !pSnapshot->m_index16Bytes.is_empty()
	        && ( !m_hIndex16Arena.is_valid() || m_nIndex16CapacityBytes < ( uint32_t )( pSnapshot->m_index16Bytes.size() ) );
	const bool grow_i32 = !pSnapshot->m_index32Bytes.is_empty()
	        && ( !m_hIndex32Arena.is_valid() || m_nIndex32CapacityBytes < ( uint32_t )( pSnapshot->m_index32Bytes.size() ) );
	m_context.beginResourceRebuild();
	if ( !syncTextures( pSnapshot ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD mesh-cache upload failed while synchronizing optional material textures" );
		}
		return false;
	}
	if ( !ensureArena( m_hVertexArena,
	             m_nVertexCapacityBytes,
	             ( uint32_t )( pSnapshot->m_vertexBytes.size() ),
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
	if ( !ensureArena( m_hIndex16Arena,
	             m_nIndex16CapacityBytes,
	             ( uint32_t )( pSnapshot->m_index16Bytes.size() ),
	             "T3D.Instance.MeshIndexArena16",
	             Terrain3DRdContext::RESOURCE_INDEX_BUFFER,
	             Terrain3DRdContext::MEMORY_MESH_INDEX_ARENA ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD mesh-cache upload failed to establish the uint16 index arena" );
		}
		return false;
	}
	if ( !ensureArena( m_hIndex32Arena,
	             m_nIndex32CapacityBytes,
	             ( uint32_t )( pSnapshot->m_index32Bytes.size() ),
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
	if ( ( grow_vertex || grow_i16 || grow_i32 || !m_hVertexArray.is_valid() ) && !rebuildArrays() ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		if ( m_lastError.is_empty() ) {
			setError( "RD mesh-cache upload failed to rebuild vertex/index array views" );
		}
		return false;
	}

	if ( m_dirtyUploadScratch.size() != ( int64_t )( s_nDirtyUploadChunkBytes ) ) {
		m_dirtyUploadScratch.resize( s_nDirtyUploadChunkBytes );
		if ( m_dirtyUploadScratch.size() != ( int64_t )( s_nDirtyUploadChunkBytes ) ) {
			m_context.endResourceRebuild();
			++m_nUploadFailureCount;
			setError( "RD mesh-cache upload failed to establish dirty-upload scratch" );
			return false;
		}
	}
	uint64_t uploaded_bytes = 0u;
	const bool has_previous = m_nSourceGeneration != 0u;
	if ( !uploadChangedChunks( m_hVertexArena,
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
	if ( !uploadChangedChunks( m_hIndex16Arena,
	             pSnapshot->m_index16Bytes,
	             pSnapshot->m_index16ChunkHashes,
	             m_index16ChunkHashes,
	             grow_i16 || !has_previous,
	             uploaded_bytes ) ) {
		m_context.endResourceRebuild();
		++m_nUploadFailureCount;
		setError( "RD mesh-cache uint16 index buffer_update failed" );
		return false;
	}
	if ( !uploadChangedChunks( m_hIndex32Arena,
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
	m_context.recordBufferUpdate( uploaded_bytes );
	m_context.endResourceRebuild();
	m_nVertexUsedBytes = ( uint32_t )( pSnapshot->m_vertexBytes.size() );
	m_nIndex16UsedBytes = ( uint32_t )( pSnapshot->m_index16Bytes.size() );
	m_nIndex32UsedBytes = ( uint32_t )( pSnapshot->m_index32Bytes.size() );
	if ( !t3dVectorCopyCold( m_surfaces, pSnapshot->m_surfaces ) || !t3dVectorCopyCold( m_lods, pSnapshot->m_lods )
	        || !t3dVectorCopyCold( m_materials, pSnapshot->m_materials )
	        || !t3dVectorCopyCold( m_vertexChunkHashes, pSnapshot->m_vertexChunkHashes )
	        || !t3dVectorCopyCold( m_index16ChunkHashes, pSnapshot->m_index16ChunkHashes )
	        || !t3dVectorCopyCold( m_index32ChunkHashes, pSnapshot->m_index32ChunkHashes ) ) {
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

void Terrain3DRdMeshCache::shutdown( void ) {
	discardTextures();
	discardArrays();
	m_context.freeResource( m_hVertexArena, m_nVertexCapacityBytes );
	m_context.freeResource( m_hIndex16Arena, m_nIndex16CapacityBytes );
	m_context.freeResource( m_hIndex32Arena, m_nIndex32CapacityBytes );
	m_context.releaseVertexFormat( m_nVertexFormat );
	m_nVertexCapacityBytes = m_nIndex16CapacityBytes = m_nIndex32CapacityBytes = 0u;
	m_nVertexUsedBytes = m_nIndex16UsedBytes = m_nIndex32UsedBytes = 0u;
	m_nGeneration = 0u;
	m_nSourceGeneration = 0u;
	m_lastError = String();
	m_surfaces.clear();
	m_lods.clear();
	m_materials.clear();
	m_vertexChunkHashes.clear();
	m_index16ChunkHashes.clear();
	m_index32ChunkHashes.clear();
	m_textureSyncNext.clear();
	m_textureSyncReused.clear();
#if T3D_INSTANCER_RD_PORT
	m_directSurfaces.clear();
#endif
	m_dirtyUploadScratch.clear();
}

t3dRdMeshHandle_t Terrain3DRdMeshCache::getMeshHandle( const uint16_t nAssetId, const int nLod ) const {
	return getMeshHandleBySourceKey( t3dRdRegularMeshSourceKey( nAssetId, ( int16_t )( nLod ) ) );
}

t3dRdMeshHandle_t Terrain3DRdMeshCache::getMeshHandleBySourceKey( const uint64_t nSourceKey ) const {
	for ( uint32_t i = 0u; i < ( uint32_t )( m_lods.size() ); ++i ) {
		if ( m_lods[i].m_nSourceKey == nSourceKey ) {
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
const t3dRdMeshSourceLod_t *Terrain3DRdMeshCache::resolveMesh( const t3dRdMeshHandle_t &rHandle ) const {
	return rHandle.m_nGeneration == m_nGeneration && rHandle.m_nSlot < m_lods.size() ? &m_lods[rHandle.m_nSlot] : NULL;
}
t3dRdSurfaceHandle_t Terrain3DRdMeshCache::getSurfaceHandle( const t3dRdMeshHandle_t &rMesh, const uint32_t nSurface ) const {
	const t3dRdMeshSourceLod_t *const pLod = resolveMesh( rMesh );
	return pLod && nSurface < pLod->m_nSurfaceCount ? t3dRdSurfaceHandle_t{ pLod->m_nFirstSurface + nSurface, m_nGeneration }
	                                                : t3dRdSurfaceHandle_t{};
}
const t3dRdMeshSourceSurface_t *Terrain3DRdMeshCache::resolveSurface( const t3dRdSurfaceHandle_t &rHandle ) const {
	return rHandle.m_nGeneration == m_nGeneration && rHandle.m_nSlot < m_surfaces.size() ? &m_surfaces[rHandle.m_nSlot] : NULL;
}
