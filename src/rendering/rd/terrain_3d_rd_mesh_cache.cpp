// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_mesh_cache.h"

#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "data/terrain_3d_half_buffer.h"

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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr uint64_t s_nFnvOffset = 14695981039346656037ull;
constexpr uint64_t s_nFnvPrime = 1099511628211ull;
constexpr uint32_t s_nMinVertexArenaBytes = 4u * 1024u * 1024u;
constexpr uint32_t s_nMinIndexArenaBytes = 2u * 1024u * 1024u;

uint64_t t3dRdHashU64( uint64_t nHash, const uint64_t nValue ) {
	for ( uint32_t i = 0u; i < 8u; ++i ) {
		nHash ^= ( nValue >> ( i * 8u ) ) & 0xffu;
		nHash *= s_nFnvPrime;
	}
	return nHash;
}

uint64_t t3dRdObjectId( const Object *const pObject ) {
	return pObject ? ( uint64_t )( pObject->get_instance_id() ) : 0u;
}

uint64_t t3dRdHashF32( uint64_t nHash, const float fValue ) {
	uint32_t bits = 0u;
	static_assert( sizeof( bits ) == sizeof( fValue ), "float hash ABI changed" );
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	return t3dRdHashU64( nHash, bits );
}

uint64_t t3dRdHashVector3( uint64_t nHash, const Vector3 &rValue ) {
	nHash = t3dRdHashF32( nHash, ( float )( rValue.x ) );
	nHash = t3dRdHashF32( nHash, ( float )( rValue.y ) );
	return t3dRdHashF32( nHash, ( float )( rValue.z ) );
}

uint64_t t3dRdHashColor( uint64_t nHash, const Color &rValue ) {
	nHash = t3dRdHashF32( nHash, ( float )( rValue.r ) );
	nHash = t3dRdHashF32( nHash, ( float )( rValue.g ) );
	nHash = t3dRdHashF32( nHash, ( float )( rValue.b ) );
	return t3dRdHashF32( nHash, ( float )( rValue.a ) );
}

uint64_t t3dRdHashTexture( uint64_t nHash, const Ref<Texture2D> &rTexture ) {
	nHash = t3dRdHashU64( nHash, t3dRdObjectId( rTexture.ptr() ) );
	if ( rTexture.is_null() ) return nHash;
	const RID texture_rid = rTexture->get_rid();
	nHash = t3dRdHashU64( nHash, ( uint64_t )( texture_rid.get_id() ) );
	nHash = t3dRdHashU64( nHash, ( uint64_t )( rTexture->get_width() ) );
	return t3dRdHashU64( nHash, ( uint64_t )( rTexture->get_height() ) );
}

uint64_t t3dRdHashMaterial( uint64_t nHash, const Ref<Material> &rMaterial ) {
	nHash = t3dRdHashU64( nHash, t3dRdObjectId( rMaterial.ptr() ) );
	BaseMaterial3D *const pBase = rMaterial.is_valid() ? Object::cast_to<BaseMaterial3D>( rMaterial.ptr() ) : nullptr;
	if ( !pBase ) return nHash;
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
	for ( const BaseMaterial3D::TextureParam slot : texture_slots ) {
		nHash = t3dRdHashTexture( nHash, pBase->get_texture( slot ) );
	}
	nHash = t3dRdHashU64( nHash, ( uint64_t )( pBase->get_roughness_texture_channel() ) );
	return nHash;
}

int16_t t3dRdPackSnorm16( const float fValue ) {
	return ( int16_t )( std::lround( CLAMP( fValue, -1.0f, 1.0f ) * 32767.0f ) );
}

uint64_t t3dRdHashBytes( uint64_t nHash, const PackedByteArray &rBytes ) {
	const uint8_t *const pBytes = rBytes.ptr();
	for ( int64_t i = 0; i < rBytes.size(); ++i ) {
		nHash ^= pBytes[i];
		nHash *= s_nFnvPrime;
	}
	return nHash;
}

bool t3dRdAddTextureSource( const Ref<Texture2D> &rTexture, std::vector<t3dRdImportedTextureSource_t> &rTextures ) {
	if ( rTexture.is_null() ) return false;
	const uint64_t source_id = t3dRdObjectId( rTexture.ptr() );
	for ( const t3dRdImportedTextureSource_t &existing : rTextures )
		if ( existing.m_nSourceId == source_id ) return true;
	const Ref<Image> source = rTexture->get_image();
	if ( source.is_null() || source->is_empty() ) return false;
	Ref<Image> upload;
	upload.instantiate();
	upload->copy_from( source );
	PackedByteArray bytes = upload->get_data();
	t3dRdTextureLayout_t layout;
	if ( bytes.is_empty() || !Terrain3DRdTextureManager::buildLayout( upload, 1u, false, ( uint64_t )( bytes.size() ), layout ) ) {
		if ( upload->is_compressed() && upload->decompress() != OK ) return false;
		upload->convert( Image::FORMAT_RGBA8 );
		bytes = upload->get_data();
		if ( bytes.is_empty() || !Terrain3DRdTextureManager::buildLayout( upload, 1u, false, ( uint64_t )( bytes.size() ), layout ) ) return false;
	}
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
	rTextures.push_back( std::move( record ) );
	return true;
}

uint16_t t3dRdAddMaterial( const Ref<Material> &rMaterial,
		std::vector<t3dRdInstanceMaterial_t> &rMaterials,
		std::vector<t3dRdImportedTextureSource_t> &rTextures ) {
	if ( rMaterial.is_null() ) return UINT16_MAX;
	if ( rMaterials.size() >= ( size_t )( UINT16_MAX ) ) return UINT16_MAX;
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
		} else if ( transparency == BaseMaterial3D::TRANSPARENCY_ALPHA ||
				transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS ) {
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND;
			if ( transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS ) {
				packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS;
			}
		}
		if ( pBase->get_cull_mode() == BaseMaterial3D::CULL_DISABLED )
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED;
		else if ( pBase->get_cull_mode() == BaseMaterial3D::CULL_FRONT )
			packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_CULL_FRONT;
		if ( pBase->get_feature( BaseMaterial3D::FEATURE_EMISSION ) ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_EMISSIVE;
		if ( pBase->get_feature( BaseMaterial3D::FEATURE_NORMAL_MAPPING ) ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_NORMAL_MAPPING;
		if ( pBase->get_feature( BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION ) ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_AO_ENABLED;
		if ( pBase->get_shading_mode() == BaseMaterial3D::SHADING_MODE_UNSHADED ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_UNSHADED;
		if ( pBase->get_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR ) ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR;
		if ( pBase->get_flag( BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR ) ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR_SRGB;
		if ( pBase->get_flag( BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT ) ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT;
		if ( pBase->get_flag( BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS ) ) packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_DONT_RECEIVE_SHADOWS;
		const uint32_t filter = ( uint32_t )( CLAMP( ( int )( pBase->get_texture_filter() ),
				( int )( BaseMaterial3D::TEXTURE_FILTER_NEAREST ),
				( int )( BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC ) ) );
		packed.m_nFlags |= ( filter << T3D_RD_INSTANCE_MATERIAL_FILTER_SHIFT ) & T3D_RD_INSTANCE_MATERIAL_FILTER_MASK;
		const struct TextureSlot { BaseMaterial3D::TextureParam m_param; uint32_t m_flag; uint32_t *m_pLow; uint32_t *m_pHigh; } slots[] = {
			{ BaseMaterial3D::TEXTURE_ALBEDO, T3D_RD_INSTANCE_MATERIAL_HAS_ALBEDO_TEXTURE, &packed.m_nAlbedoTextureIdLow, &packed.m_nAlbedoTextureIdHigh },
			{ BaseMaterial3D::TEXTURE_NORMAL, T3D_RD_INSTANCE_MATERIAL_HAS_NORMAL_TEXTURE, &packed.m_nNormalTextureIdLow, &packed.m_nNormalTextureIdHigh },
			{ BaseMaterial3D::TEXTURE_ORM, T3D_RD_INSTANCE_MATERIAL_HAS_ORM_TEXTURE, &packed.m_nOrmTextureIdLow, &packed.m_nOrmTextureIdHigh },
		};
		for ( const TextureSlot &slot : slots ) {
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
				packed.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_HAS_ORM_TEXTURE |
						T3D_RD_INSTANCE_MATERIAL_ROUGHNESS_TEXTURE;
			}
		}
	}
	for ( uint16_t i = 0u; i < ( uint16_t )( rMaterials.size() ); ++i )
		if ( std::memcmp( &rMaterials[i], &packed, sizeof( packed ) ) == 0 ) return i;
	rMaterials.push_back( packed );
	return ( uint16_t )( rMaterials.size() - 1u );
}

bool t3dRdAppendBytes( PackedByteArray &rDestination, const void *const pSource, const uint32_t nBytes ) {
	if ( nBytes == 0u ) return true;
	const int64_t old_size = rDestination.size();
	if ( old_size > ( int64_t )( UINT32_MAX ) - nBytes ) return false;
	rDestination.resize( old_size + nBytes );
	if ( rDestination.size() != old_size + nBytes ) return false;
	std::memcpy( rDestination.ptrw() + old_size, pSource, nBytes );
	return true;
}

void t3dRdSetError( String *const pError, const String &rError ) {
	if ( pError ) *pError = rError;
}


bool t3dRdAppendMeshSource( const Ref<Mesh> &rMesh,
		const uint64_t nSourceKey,
		const uint16_t nAssetId,
		const int16_t nLod,
		const Ref<Material> &rOverrideMaterial,
		const uint16_t nOverlayMaterial,
		t3dRdMeshSourceSnapshot_t &rResult,
		String *const pError ) {
	if ( rMesh.is_null() || rMesh->get_surface_count() <= 0 ) return true;
	if ( nSourceKey == 0u ) {
		t3dRdSetError( pError, "RD static mesh source key is invalid" );
		return false;
	}
	t3dRdMeshSourceLod_t lod_record;
	lod_record.m_nSourceKey = nSourceKey;
	lod_record.m_nAssetId = nAssetId;
	lod_record.m_nLod = nLod;
	lod_record.m_nFirstSurface = ( uint32_t )( rResult.m_surfaces.size() );
	lod_record.m_bounds = rMesh->get_aabb();
	for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
		if ( rMesh->_surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) continue;
		const Array arrays = rMesh->surface_get_arrays( surface );
		if ( arrays.size() <= Mesh::ARRAY_VERTEX || arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY ) continue;
		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		if ( positions.is_empty() || positions.size() > ( int64_t )( UINT32_MAX ) ) continue;
		const PackedVector3Array normals = arrays.size() > Mesh::ARRAY_NORMAL && arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY
				? ( PackedVector3Array )( arrays[Mesh::ARRAY_NORMAL] ) : PackedVector3Array();
		const PackedFloat32Array tangents = arrays.size() > Mesh::ARRAY_TANGENT && arrays[Mesh::ARRAY_TANGENT].get_type() == Variant::PACKED_FLOAT32_ARRAY
				? ( PackedFloat32Array )( arrays[Mesh::ARRAY_TANGENT] ) : PackedFloat32Array();
		const PackedColorArray colors = arrays.size() > Mesh::ARRAY_COLOR && arrays[Mesh::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY
				? ( PackedColorArray )( arrays[Mesh::ARRAY_COLOR] ) : PackedColorArray();
		const PackedVector2Array uvs = arrays.size() > Mesh::ARRAY_TEX_UV && arrays[Mesh::ARRAY_TEX_UV].get_type() == Variant::PACKED_VECTOR2_ARRAY
				? ( PackedVector2Array )( arrays[Mesh::ARRAY_TEX_UV] ) : PackedVector2Array();
		const PackedVector2Array uv2s = arrays.size() > Mesh::ARRAY_TEX_UV2 && arrays[Mesh::ARRAY_TEX_UV2].get_type() == Variant::PACKED_VECTOR2_ARRAY
				? ( PackedVector2Array )( arrays[Mesh::ARRAY_TEX_UV2] ) : PackedVector2Array();
		const PackedInt32Array indices = arrays.size() > Mesh::ARRAY_INDEX && arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY
				? ( PackedInt32Array )( arrays[Mesh::ARRAY_INDEX] ) : PackedInt32Array();
		const uint32_t vertex_count = ( uint32_t )( positions.size() );
		const uint32_t vertex_offset = ( uint32_t )( rResult.m_vertexBytes.size() / sizeof( t3dRdMeshVertex_t ) );
		for ( uint32_t vertex = 0u; vertex < vertex_count; ++vertex ) {
			t3dRdMeshVertex_t packed;
			const Vector3 position = positions[( int64_t )( vertex )];
			packed.m_szPosition[0] = ( float )( position.x );
			packed.m_szPosition[1] = ( float )( position.y );
			packed.m_szPosition[2] = ( float )( position.z );
			packed.m_nColorRgba8 = vertex < ( uint32_t )( colors.size() )
					? t3dRdPackGodotRgba32ForGpu( colors[( int64_t )( vertex )].to_rgba32() ) : 0xffffffffu;
			const Vector3 normal = vertex < ( uint32_t )( normals.size() ) ? normals[( int64_t )( vertex )] : Vector3( 0, 1, 0 );
			packed.m_szNormal[0] = t3dRdPackSnorm16( ( float )( normal.x ) );
			packed.m_szNormal[1] = t3dRdPackSnorm16( ( float )( normal.y ) );
			packed.m_szNormal[2] = t3dRdPackSnorm16( ( float )( normal.z ) );
			packed.m_szNormal[3] = 0;
			if ( tangents.size() >= ( int64_t )( vertex + 1u ) * 4 ) {
				for ( uint32_t component = 0u; component < 4u; ++component )
					packed.m_szTangent[component] = t3dRdPackSnorm16( tangents[( int64_t )( vertex * 4u + component )] );
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
		const uint32_t overlay_flags = record.m_nOverlayMaterial == UINT16_MAX ? base_flags : rResult.m_materials[record.m_nOverlayMaterial].m_nFlags;
		const bool base_double = ( base_flags & T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED ) != 0u;
		const bool overlay_double = ( overlay_flags & T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED ) != 0u;
		const bool base_front = ( base_flags & T3D_RD_INSTANCE_MATERIAL_CULL_FRONT ) != 0u;
		const bool overlay_front = ( overlay_flags & T3D_RD_INSTANCE_MATERIAL_CULL_FRONT ) != 0u;
		if ( base_double || overlay_double || ( record.m_nOverlayMaterial != UINT16_MAX && base_front != overlay_front ) ) {
			record.m_nFlags |= T3D_RD_INSTANCE_DOUBLE_SIDED;
		} else if ( base_front ) {
			record.m_nFlags |= T3D_RD_INSTANCE_CULL_FRONT;
		}
		if ( ( ( base_flags | overlay_flags ) & T3D_RD_INSTANCE_MATERIAL_ALPHA_BLEND ) != 0u )
			record.m_nFlags |= T3D_RD_INSTANCE_ALPHA_BLEND;
		if ( ( ( base_flags | overlay_flags ) & T3D_RD_INSTANCE_MATERIAL_ALPHA_DEPTH_PREPASS ) != 0u )
			record.m_nFlags |= T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS;
		if ( max_index <= 65535u ) {
			record.m_nIndexFormat = ( uint32_t )( RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 );
			record.m_nIndexOffset = ( uint32_t )( rResult.m_index16Bytes.size() / sizeof( uint16_t ) );
			for ( uint32_t i = 0u; i < index_count; ++i ) {
				const uint16_t packed = ( uint16_t )( indices.is_empty() ? i : ( uint32_t )( indices[( int64_t )( i )] ) );
				if ( !t3dRdAppendBytes( rResult.m_index16Bytes, &packed, sizeof( packed ) ) ) return false;
			}
		} else {
			record.m_nIndexFormat = ( uint32_t )( RenderingDevice::INDEX_BUFFER_FORMAT_UINT32 );
			record.m_nIndexOffset = ( uint32_t )( rResult.m_index32Bytes.size() / sizeof( uint32_t ) );
			for ( uint32_t i = 0u; i < index_count; ++i ) {
				const uint32_t packed = indices.is_empty() ? i : ( uint32_t )( indices[( int64_t )( i )] );
				if ( !t3dRdAppendBytes( rResult.m_index32Bytes, &packed, sizeof( packed ) ) ) return false;
			}
		}
		rResult.m_surfaces.push_back( record );
	}
	lod_record.m_nSurfaceCount = ( uint32_t )( rResult.m_surfaces.size() ) - lod_record.m_nFirstSurface;
	if ( lod_record.m_nSurfaceCount > 0u ) rResult.m_lods.push_back( lod_record );
	return true;
}
} // namespace

Terrain3DRdMeshCache::Terrain3DRdMeshCache( void ) : m_context( Terrain3DRdContext::getShared() ) {}
Terrain3DRdMeshCache::~Terrain3DRdMeshCache( void ) { shutdown(); }

uint32_t Terrain3DRdMeshCache::nextArenaCapacity( const uint32_t nRequiredBytes, const uint32_t nMinimumBytes ) {
	if ( nRequiredBytes == 0u ) return 0u;
	uint64_t capacity = MAX( ( uint64_t )( nMinimumBytes ), 4096ull );
	while ( capacity < nRequiredBytes ) capacity <<= 1u;
	return capacity > UINT32_MAX ? 0u : ( uint32_t )( capacity );
}

uint64_t Terrain3DRdMeshCache::computeSourceFingerprint( const Ref<Terrain3DAssets> &rAssets ) {
	const std::vector<t3dRdExternalMeshSource_t> empty;
	return computeSourceFingerprint( rAssets, empty );
}

uint64_t Terrain3DRdMeshCache::computeSourceFingerprint( const Ref<Terrain3DAssets> &rAssets,
		const std::vector<t3dRdExternalMeshSource_t> &rExternalMeshes ) {
	if ( rAssets.is_null() && rExternalMeshes.empty() ) return 0u;
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
			if ( mesh.is_null() ) continue;
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
	hash = t3dRdHashU64( hash, ( uint64_t )( rExternalMeshes.size() ) );
	for ( const t3dRdExternalMeshSource_t &source : rExternalMeshes ) {
		hash = t3dRdHashU64( hash, source.m_nSourceKey );
		hash = t3dRdHashU64( hash, t3dRdObjectId( source.m_mesh.ptr() ) );
		if ( source.m_mesh.is_null() ) continue;
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

std::shared_ptr<const t3dRdMeshSourceSnapshot_t> Terrain3DRdMeshCache::buildSourceSnapshot(
		const Ref<Terrain3DAssets> &rAssets,
		const uint64_t nFingerprint,
		const uint64_t nGeneration,
		String *const pError ) {
	const std::vector<t3dRdExternalMeshSource_t> empty;
	return buildSourceSnapshot( rAssets, empty, nFingerprint, nGeneration, pError );
}

std::shared_ptr<const t3dRdMeshSourceSnapshot_t> Terrain3DRdMeshCache::buildSourceSnapshot(
		const Ref<Terrain3DAssets> &rAssets,
		const std::vector<t3dRdExternalMeshSource_t> &rExternalMeshes,
		const uint64_t nFingerprint,
		const uint64_t nGeneration,
		String *const pError ) {
	if ( rAssets.is_null() && rExternalMeshes.empty() ) {
		t3dRdSetError( pError, "Terrain3D mesh sources unavailable" );
		return nullptr;
	}
	std::shared_ptr<t3dRdMeshSourceSnapshot_t> result = std::make_shared<t3dRdMeshSourceSnapshot_t>();
	result->m_nFingerprint = nFingerprint;
	result->m_nGeneration = nGeneration == 0u ? 1u : nGeneration;
	const int mesh_count = rAssets.is_valid() ? rAssets->getMeshCount() : 0;
	for ( int asset_id = 0; asset_id < mesh_count; ++asset_id ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( asset_id );
		if ( asset.is_null() || !asset->isEnabled() ) continue;
		const Ref<Material> override_material = asset->isHighlighted() && asset->getHighlightMaterial().is_valid()
				? asset->getHighlightMaterial() : asset->getMaterialOverride();
		const uint16_t overlay_material = asset->isHighlighted() ? UINT16_MAX
				: t3dRdAddMaterial( asset->getMaterialOverlay(), result->m_materials, result->m_textures );
		for ( int lod = 0; lod < asset->getLodCount(); ++lod ) {
			const Ref<Mesh> mesh = asset->getMesh( lod );
			if ( !t3dRdAppendMeshSource( mesh,
						t3dRdRegularMeshSourceKey( ( uint16_t )( asset_id ), ( int16_t )( lod ) ),
						( uint16_t )( asset_id ),
						( int16_t )( lod ),
						override_material,
						overlay_material,
						*result,
						pError ) ) return nullptr;
		}
	}
	std::vector<uint64_t> external_keys;
	external_keys.reserve( rExternalMeshes.size() );
	for ( const t3dRdExternalMeshSource_t &source : rExternalMeshes ) {
		if ( source.m_nSourceKey == 0u || source.m_mesh.is_null() ) continue;
		if ( std::find( external_keys.begin(), external_keys.end(), source.m_nSourceKey ) != external_keys.end() ) {
			t3dRdSetError( pError, "Duplicate HLOD mesh source key in RD publication" );
			return nullptr;
		}
		for ( const t3dRdMeshSourceLod_t &existing : result->m_lods ) {
			if ( existing.m_nSourceKey == source.m_nSourceKey ) {
				t3dRdSetError( pError, "HLOD mesh source key collides with an existing RD mesh source" );
				return nullptr;
			}
		}
		external_keys.push_back( source.m_nSourceKey );
		if ( !t3dRdAppendMeshSource( source.m_mesh,
					source.m_nSourceKey,
					UINT16_MAX,
					0,
					Ref<Material>(),
					UINT16_MAX,
					*result,
					pError ) ) return nullptr;
	}
	if ( pError ) *pError = String();
	return result;
}

bool Terrain3DRdMeshCache::ensureVertexFormat( void ) {
	if ( m_nVertexFormat != RenderingDevice::INVALID_FORMAT_ID ) return true;
	RenderingDevice *const rd = m_context.getDevice(); if ( !rd ) return false;
	TypedArray<Ref<RDVertexAttribute>> attributes;
	const struct AttributeDesc { uint32_t location; RenderingDevice::DataFormat format; uint32_t offset; } desc[] = {
		{ 0u, RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT, 0u },
		{ 1u, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, 12u },
		{ 2u, RenderingDevice::DATA_FORMAT_R16G16B16A16_SNORM, 16u },
		{ 3u, RenderingDevice::DATA_FORMAT_R16G16B16A16_SNORM, 24u },
		{ 4u, RenderingDevice::DATA_FORMAT_R16G16_SFLOAT, 32u },
		{ 5u, RenderingDevice::DATA_FORMAT_R16G16_SFLOAT, 36u },
	};
	for ( const AttributeDesc &item : desc ) {
		Ref<RDVertexAttribute> attribute; attribute.instantiate(); attribute->set_location( item.location ); attribute->set_binding( 0u ); attribute->set_format( item.format ); attribute->set_offset( item.offset ); attribute->set_stride( sizeof( t3dRdMeshVertex_t ) ); attribute->set_frequency( RenderingDevice::VERTEX_FREQUENCY_VERTEX ); attributes.push_back( attribute );
	}
	m_nVertexFormat = rd->vertex_format_create( attributes );
	return m_nVertexFormat != RenderingDevice::INVALID_FORMAT_ID && m_context.registerVertexFormat( m_nVertexFormat, "T3D.VertexFormat.InstanceMesh48" );
}

bool Terrain3DRdMeshCache::ensureArena( RID &rBuffer, uint32_t &rCapacityBytes, const uint32_t nRequiredBytes,
		const String &rName, const Terrain3DRdContext::ResourceType resourceType, const Terrain3DRdContext::MemoryCategory memoryCategory ) {
	if ( nRequiredBytes == 0u ) return true;
	const uint32_t minimum = resourceType == Terrain3DRdContext::RESOURCE_VERTEX_BUFFER ? s_nMinVertexArenaBytes : s_nMinIndexArenaBytes;
	if ( rBuffer.is_valid() && rCapacityBytes >= nRequiredBytes ) return true;
	const uint32_t capacity = nextArenaCapacity( nRequiredBytes, minimum ); if ( capacity == 0u ) return false;
	RenderingDevice *const rd = m_context.getDevice(); if ( !rd ) return false;
	if ( rBuffer.is_valid() ) m_context.freeResource( rBuffer, rCapacityBytes );
	PackedByteArray empty; empty.resize( capacity ); std::memset( empty.ptrw(), 0, capacity );
	if ( resourceType == Terrain3DRdContext::RESOURCE_VERTEX_BUFFER ) rBuffer = rd->vertex_buffer_create( capacity, empty );
	else {
		const RenderingDevice::IndexBufferFormat format = rName.find( "16" ) >= 0 ? RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 : RenderingDevice::INDEX_BUFFER_FORMAT_UINT32;
		rBuffer = rd->index_buffer_create( capacity / ( format == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 2u : 4u ), format, empty );
	}
	if ( !rBuffer.is_valid() || !m_context.registerResource( rBuffer, rName, resourceType, memoryCategory, capacity ) ) return false;
	rCapacityBytes = capacity; return true;
}

void Terrain3DRdMeshCache::discardArrays( void ) {
	m_context.freeResource( m_hVertexArray ); m_context.freeResource( m_hIndex16Array ); m_context.freeResource( m_hIndex32Array );
}

bool Terrain3DRdMeshCache::rebuildArrays( void ) {
	discardArrays(); RenderingDevice *const rd = m_context.getDevice(); if ( !rd ) return false;
	if ( m_hVertexArena.is_valid() ) { TypedArray<RID> buffers; buffers.push_back( m_hVertexArena ); m_hVertexArray = rd->vertex_array_create( m_nVertexCapacityBytes / sizeof( t3dRdMeshVertex_t ), m_nVertexFormat, buffers ); if ( !m_hVertexArray.is_valid() || !m_context.registerResource( m_hVertexArray, "T3D.Instance.MeshVertexArray", Terrain3DRdContext::RESOURCE_VERTEX_ARRAY, Terrain3DRdContext::MEMORY_NONE, 0u ) ) return false; }
	if ( m_hIndex16Arena.is_valid() ) { m_hIndex16Array = rd->index_array_create( m_hIndex16Arena, 0u, m_nIndex16CapacityBytes / sizeof( uint16_t ) ); if ( !m_hIndex16Array.is_valid() || !m_context.registerResource( m_hIndex16Array, "T3D.Instance.MeshIndexArray16", Terrain3DRdContext::RESOURCE_INDEX_ARRAY, Terrain3DRdContext::MEMORY_NONE, 0u ) ) return false; }
	if ( m_hIndex32Arena.is_valid() ) { m_hIndex32Array = rd->index_array_create( m_hIndex32Arena, 0u, m_nIndex32CapacityBytes / sizeof( uint32_t ) ); if ( !m_hIndex32Array.is_valid() || !m_context.registerResource( m_hIndex32Array, "T3D.Instance.MeshIndexArray32", Terrain3DRdContext::RESOURCE_INDEX_ARRAY, Terrain3DRdContext::MEMORY_NONE, 0u ) ) return false; }
	return m_hVertexArray.is_valid();
}

void Terrain3DRdMeshCache::discardTextures( void ) {
	Terrain3DRdTextureManager *const pManager = Terrain3DRdTextureManager::getSingleton();
	if ( pManager ) for ( const t3dRdImportedTextureGpu_t &texture : m_textures )
		pManager->freeTexture( texture.m_hTexture, texture.m_nBytes );
	m_textures.clear();
}

bool Terrain3DRdMeshCache::syncTextures( const std::shared_ptr<const t3dRdMeshSourceSnapshot_t> &pSnapshot ) {
	if ( !pSnapshot ) return false;
	Terrain3DRdTextureManager *const pManager = Terrain3DRdTextureManager::getSingleton();
	if ( !pManager ) return pSnapshot->m_textures.empty();
	std::vector<t3dRdImportedTextureGpu_t> next;
	std::vector<bool> reused( m_textures.size(), false );
	next.reserve( pSnapshot->m_textures.size() );
	for ( const t3dRdImportedTextureSource_t &source : pSnapshot->m_textures ) {
		bool matched = false;
		for ( size_t i = 0u; i < m_textures.size(); ++i ) {
			if ( reused[i] || m_textures[i].m_nSourceId != source.m_nSourceId ||
				m_textures[i].m_nFingerprint != source.m_nFingerprint || !m_textures[i].m_hTexture.is_valid() ) continue;
			next.push_back( m_textures[i] );
			reused[i] = true;
			matched = true;
			break;
		}
		if ( matched ) continue;
		TypedArray<PackedByteArray> initial_data;
		initial_data.push_back( source.m_bytes );
		const String name = String( "T3D.Instance.MaterialTexture." ) + String::num_uint64( source.m_nSourceId );
		const RID texture = pManager->createTexture( source.m_layout, initial_data, name,
				( int )( Terrain3DRdContext::MEMORY_MATERIAL_TABLE ) );
		if ( !texture.is_valid() ) {
			for ( const t3dRdImportedTextureGpu_t &created : next ) {
				bool was_old = false;
				for ( const t3dRdImportedTextureGpu_t &old : m_textures ) if ( old.m_hTexture == created.m_hTexture ) { was_old = true; break; }
				if ( !was_old ) pManager->freeTexture( created.m_hTexture, created.m_nBytes );
			}
			return false;
		}
		next.push_back( t3dRdImportedTextureGpu_t{ source.m_nSourceId, source.m_nFingerprint, texture, source.m_layout.m_nBytes } );
	}
	for ( size_t i = 0u; i < m_textures.size(); ++i ) if ( !reused[i] )
		pManager->freeTexture( m_textures[i].m_hTexture, m_textures[i].m_nBytes );
	m_textures.swap( next );
	return true;
}

RID Terrain3DRdMeshCache::getTextureRid( const uint64_t nSourceId ) const {
	if ( nSourceId == 0u ) return RID();
	for ( const t3dRdImportedTextureGpu_t &texture : m_textures )
		if ( texture.m_nSourceId == nSourceId ) return texture.m_hTexture;
	return RID();
}

bool Terrain3DRdMeshCache::upload( const std::shared_ptr<const t3dRdMeshSourceSnapshot_t> &pSnapshot ) {
	if ( !pSnapshot ) return false;
	if ( pSnapshot->m_nGeneration == m_nSourceGeneration && isReady() ) return true;
	if ( !m_context.initialize() || !m_context.validateDevice() || !ensureVertexFormat() ) return false;
	const bool grow_vertex = !m_hVertexArena.is_valid() || m_nVertexCapacityBytes < ( uint32_t )( pSnapshot->m_vertexBytes.size() );
	const bool grow_i16 = !pSnapshot->m_index16Bytes.is_empty() &&
			( !m_hIndex16Arena.is_valid() || m_nIndex16CapacityBytes < ( uint32_t )( pSnapshot->m_index16Bytes.size() ) );
	const bool grow_i32 = !pSnapshot->m_index32Bytes.is_empty() &&
			( !m_hIndex32Arena.is_valid() || m_nIndex32CapacityBytes < ( uint32_t )( pSnapshot->m_index32Bytes.size() ) );
	m_context.beginResourceRebuild();
	if ( !syncTextures( pSnapshot ) ) {
		m_context.endResourceRebuild();
		return false;
	}
	const bool arenas_ok = ensureArena( m_hVertexArena, m_nVertexCapacityBytes,
			( uint32_t )( pSnapshot->m_vertexBytes.size() ), "T3D.Instance.MeshVertexArena",
			Terrain3DRdContext::RESOURCE_VERTEX_BUFFER, Terrain3DRdContext::MEMORY_MESH_VERTEX_ARENA ) &&
			ensureArena( m_hIndex16Arena, m_nIndex16CapacityBytes,
					( uint32_t )( pSnapshot->m_index16Bytes.size() ), "T3D.Instance.MeshIndexArena16",
					Terrain3DRdContext::RESOURCE_INDEX_BUFFER, Terrain3DRdContext::MEMORY_MESH_INDEX_ARENA ) &&
			ensureArena( m_hIndex32Arena, m_nIndex32CapacityBytes,
					( uint32_t )( pSnapshot->m_index32Bytes.size() ), "T3D.Instance.MeshIndexArena32",
					Terrain3DRdContext::RESOURCE_INDEX_BUFFER, Terrain3DRdContext::MEMORY_MESH_INDEX_ARENA );
	if ( !arenas_ok || ( ( grow_vertex || grow_i16 || grow_i32 || !m_hVertexArray.is_valid() ) && !rebuildArrays() ) ) {
		m_context.endResourceRebuild();
		return false;
	}

	RenderingDevice *const rd = m_context.getDevice();
	uint64_t uploaded_bytes = 0u;
	auto upload_changed_span = [&]( const RID &rBuffer,
			const PackedByteArray &rCurrent,
			const PackedByteArray *pPrevious,
			const bool bForceFull ) -> bool {
		const uint32_t current_size = ( uint32_t )( rCurrent.size() );
		if ( current_size == 0u ) return true;
		uint32_t first = 0u;
		uint32_t last = current_size;
		if ( !bForceFull && pPrevious ) {
			const uint32_t previous_size = ( uint32_t )( pPrevious->size() );
			const uint32_t common = MIN( current_size, previous_size );
			const uint8_t *const current = rCurrent.ptr();
			const uint8_t *const previous = pPrevious->ptr();
			while ( first + 32u <= common && std::memcmp( current + first, previous + first, 32u ) == 0 ) first += 32u;
			while ( first < common && current[first] == previous[first] ) ++first;
			if ( first == current_size ) return true;
			while ( last > first && last <= previous_size && current[last - 1u] == previous[last - 1u] ) --last;
		}
		if ( last <= first ) return true;
		const uint32_t bytes = last - first;
		PackedByteArray dirty;
		dirty.resize( bytes );
		std::memcpy( dirty.ptrw(), rCurrent.ptr() + first, bytes );
		if ( rd->buffer_update( rBuffer, first, bytes, dirty ) != OK ) return false;
		uploaded_bytes += bytes;
		return true;
	};

	const t3dRdMeshSourceSnapshot_t *const previous = m_lastUploadedSnapshot.get();
	if ( !upload_changed_span( m_hVertexArena, pSnapshot->m_vertexBytes,
			previous ? &previous->m_vertexBytes : nullptr, grow_vertex || !previous ) ||
			!upload_changed_span( m_hIndex16Arena, pSnapshot->m_index16Bytes,
					previous ? &previous->m_index16Bytes : nullptr, grow_i16 || !previous ) ||
			!upload_changed_span( m_hIndex32Arena, pSnapshot->m_index32Bytes,
					previous ? &previous->m_index32Bytes : nullptr, grow_i32 || !previous ) ) {
		m_context.endResourceRebuild();
		return false;
	}
	m_context.recordBufferUpdate( uploaded_bytes );
	m_context.endResourceRebuild();
	m_nVertexUsedBytes = ( uint32_t )( pSnapshot->m_vertexBytes.size() );
	m_nIndex16UsedBytes = ( uint32_t )( pSnapshot->m_index16Bytes.size() );
	m_nIndex32UsedBytes = ( uint32_t )( pSnapshot->m_index32Bytes.size() );
	m_surfaces = pSnapshot->m_surfaces;
	m_lods = pSnapshot->m_lods;
	m_materials = pSnapshot->m_materials;
	m_lastUploadedSnapshot = pSnapshot;
	m_nSourceGeneration = pSnapshot->m_nGeneration;
	if ( ++m_nGeneration == 0u ) m_nGeneration = 1u;
	return true;
}

void Terrain3DRdMeshCache::shutdown( void ) {
	discardTextures();
	discardArrays(); m_context.freeResource( m_hVertexArena, m_nVertexCapacityBytes ); m_context.freeResource( m_hIndex16Arena, m_nIndex16CapacityBytes ); m_context.freeResource( m_hIndex32Arena, m_nIndex32CapacityBytes ); m_context.releaseVertexFormat( m_nVertexFormat );
	m_nVertexCapacityBytes = m_nIndex16CapacityBytes = m_nIndex32CapacityBytes = 0u; m_nVertexUsedBytes = m_nIndex16UsedBytes = m_nIndex32UsedBytes = 0u; m_nGeneration = 0u; m_nSourceGeneration = 0u; m_surfaces.clear(); m_lods.clear(); m_materials.clear(); m_lastUploadedSnapshot.reset();
}

t3dRdMeshHandle_t Terrain3DRdMeshCache::getMeshHandle( const uint16_t nAssetId, const int nLod ) const {
	return getMeshHandleBySourceKey( t3dRdRegularMeshSourceKey( nAssetId, ( int16_t )( nLod ) ) );
}

t3dRdMeshHandle_t Terrain3DRdMeshCache::getMeshHandleBySourceKey( const uint64_t nSourceKey ) const {
	for ( uint32_t i = 0u; i < ( uint32_t )( m_lods.size() ); ++i )
		if ( m_lods[i].m_nSourceKey == nSourceKey ) return t3dRdMeshHandle_t{ i, m_nGeneration };
	return t3dRdMeshHandle_t{};
}
const t3dRdMeshSourceLod_t *Terrain3DRdMeshCache::resolveMesh( const t3dRdMeshHandle_t &rHandle ) const { return rHandle.m_nGeneration == m_nGeneration && rHandle.m_nSlot < m_lods.size() ? &m_lods[rHandle.m_nSlot] : nullptr; }
t3dRdSurfaceHandle_t Terrain3DRdMeshCache::getSurfaceHandle( const t3dRdMeshHandle_t &rMesh, const uint32_t nSurface ) const { const t3dRdMeshSourceLod_t *const pLod = resolveMesh( rMesh ); return pLod && nSurface < pLod->m_nSurfaceCount ? t3dRdSurfaceHandle_t{ pLod->m_nFirstSurface + nSurface, m_nGeneration } : t3dRdSurfaceHandle_t{}; }
const t3dRdMeshSourceSurface_t *Terrain3DRdMeshCache::resolveSurface( const t3dRdSurfaceHandle_t &rHandle ) const { return rHandle.m_nGeneration == m_nGeneration && rHandle.m_nSlot < m_surfaces.size() ? &m_surfaces[rHandle.m_nSlot] : nullptr; }
