// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/render_world/terrain_3d_render_world_godot_converter.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/material.hpp>

#include <string.h>

using namespace godot;

static uint64_t t3d_rw_material_hash_mix( uint64_t pHash, uint64_t pValue ) {
	pHash ^= pValue + 0x9e3779b97f4a7c15uLL + ( pHash << 6u ) + ( pHash >> 2u );
	return pHash;
}

static uint16_t t3d_rw_material_float_to_half_local( float pValue ) {
	uint32_t nBits = 0u;
	memcpy( &nBits, &pValue, sizeof( nBits ) );
	const uint32_t nSign = ( nBits >> 16u ) & 0x8000u;
	int32_t nExponent = ( int32_t )( ( nBits >> 23u ) & 0xffu ) - 127 + 15;
	uint32_t nMantissa = nBits & 0x7fffffu;
	if ( nExponent <= 0 ) {
		if ( nExponent < -10 ) {
			return ( uint16_t )nSign;
		}
		nMantissa = ( nMantissa | 0x800000u ) >> ( uint32_t )( 1 - nExponent );
		return ( uint16_t )( nSign | ( ( nMantissa + 0x1000u ) >> 13u ) );
	}
	if ( nExponent >= 31 ) {
		return ( uint16_t )( nSign | 0x7c00u );
	}
	return ( uint16_t )( nSign | ( ( uint32_t )nExponent << 10u ) | ( ( nMantissa + 0x1000u ) >> 13u ) );
}

static uint32_t t3d_rw_material_pack_half2_local( float pX, float pY ) {
	return ( uint32_t )t3d_rw_material_float_to_half_local( pX ) | ( ( uint32_t )t3d_rw_material_float_to_half_local( pY ) << 16u );
}

static uint32_t t3d_rw_material_resolve_vt(
        const T3DRenderWorldGodotMaterialResolver &pResolver, const Ref<Texture2D> &pTexture, T3DRenderWorldVTMaterialChannel pChannel ) {
	if ( pTexture.is_null() || pResolver.pResolveTextureVT == NULL ) {
		return T3D_RW_INVALID_ID;
	}
	return pResolver.pResolveTextureVT( pResolver.pUser, pTexture, pChannel );
}

bool Terrain3DRenderWorldGodotConverter::analyzeMaterialsCold( MeshInstance3D *pMeshInstance,
        uint32_t pTier,
        const Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial,
        bool &rSupported,
        bool &rSortedTransparency,
        bool &rTwoSided ) const {
	rSupported = true;
	rSortedTransparency = false;
	rTwoSided = false;
	if ( pExplicitMaterial.is_valid() ) {
		rTwoSided = pExplicitMaterial->isTwoSided();
		return true;
	}

	Ref<ArrayMesh> Mesh = pMeshInstance->get_mesh();
	if ( Mesh.is_null() ) {
		rSupported = false;
		return false;
	}

	for ( int32_t s = 0; s < Mesh->get_surface_count(); ++s ) {
		Ref<Material> Material = pMeshInstance->get_surface_override_material( s );
		if ( Material.is_null() ) {
			Material = Mesh->surface_get_material( s );
		}
		if ( Material.is_null() ) {
			continue;
		}
		BaseMaterial3D *pBase = Object::cast_to<BaseMaterial3D>( *Material );
		if ( pBase == NULL ) {
			rSupported = false;
			return false;
		}
		const BaseMaterial3D::Transparency eTransparency = pBase->get_transparency();
		if ( eTransparency != BaseMaterial3D::TRANSPARENCY_DISABLED && eTransparency != BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR ) {
			rSortedTransparency = true;
		}
		if ( pBase->get_cull_mode() == BaseMaterial3D::CULL_DISABLED ) {
			rTwoSided = true;
		}
	}
	if ( rTwoSided && pTier != T3D_RW_TIER_FOLIAGE ) {
		return true;
	}
	return true;
}

bool Terrain3DRenderWorldGodotConverter::compileStandardMaterialCold(
        const Ref<Material> &pMaterial, uint32_t pTier, T3DMaterialRecord &rRecord, bool &rSortedTransparency, bool &rTwoSided ) const {
	memset( &rRecord, 0, sizeof( rRecord ) );
	rRecord.nAlbedoVT = T3D_RW_INVALID_ID;
	rRecord.nNormalVT = T3D_RW_INVALID_ID;
	rRecord.nSurfaceVT = T3D_RW_INVALID_ID;
	rRecord.nHeightVT = T3D_RW_INVALID_ID;
	rRecord.vBaseColor[0] = 1.0f;
	rRecord.vBaseColor[1] = 1.0f;
	rRecord.vBaseColor[2] = 1.0f;
	rRecord.vBaseColor[3] = 1.0f;
	rRecord.vUvScaleBias[0] = 1.0f;
	rRecord.vUvScaleBias[1] = 1.0f;

	rSortedTransparency = false;
	rTwoSided = false;
	if ( pMaterial.is_null() ) {
		rRecord.nFlags = T3D_RW_MATERIAL_FLAG_CAST_SHADOWS | T3D_RW_MATERIAL_FLAG_RECEIVE_SHADOWS;
		return true;
	}

	BaseMaterial3D *pBase = Object::cast_to<BaseMaterial3D>( *pMaterial );
	if ( pBase == NULL ) {
		return false;
	}

	const BaseMaterial3D::Transparency eTransparency = pBase->get_transparency();
	rSortedTransparency =
	        eTransparency != BaseMaterial3D::TRANSPARENCY_DISABLED && eTransparency != BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR;
	rTwoSided = pBase->get_cull_mode() == BaseMaterial3D::CULL_DISABLED;
	if ( rSortedTransparency || ( rTwoSided && pTier != T3D_RW_TIER_FOLIAGE ) ) {
		return false;
	}

	const Color Albedo = pBase->get_albedo();
	const Vector3 UvScale = pBase->get_uv1_scale();
	const Vector3 UvOffset = pBase->get_uv1_offset();
	rRecord.vBaseColor[0] = Albedo.r;
	rRecord.vBaseColor[1] = Albedo.g;
	rRecord.vBaseColor[2] = Albedo.b;
	rRecord.vBaseColor[3] = Albedo.a;
	rRecord.vUvScaleBias[0] = UvScale.x;
	rRecord.vUvScaleBias[1] = UvScale.y;
	rRecord.vUvScaleBias[2] = UvOffset.x;
	rRecord.vUvScaleBias[3] = UvOffset.y;
	rRecord.nFlags = T3D_RW_MATERIAL_FLAG_CAST_SHADOWS | T3D_RW_MATERIAL_FLAG_RECEIVE_SHADOWS;
	if ( eTransparency == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR ) {
		rRecord.nFlags |= T3D_RW_MATERIAL_FLAG_ALPHA_SCISSOR;
	}
	if ( pTier == T3D_RW_TIER_FOLIAGE ) {
		rRecord.nFlags |= T3D_RW_MATERIAL_FLAG_FOLIAGE;
	}
	if ( rTwoSided ) {
		rRecord.nFlags |= T3D_RW_MATERIAL_FLAG_TWO_SIDED;
	}
	rRecord.nSamplerClass = T3D_RW_SAMPLER_REPEAT;
	rRecord.nPackedScalars0 = t3d_rw_material_pack_half2_local( pBase->get_roughness(), pBase->get_metallic() );
	rRecord.nPackedScalars1 = t3d_rw_material_pack_half2_local( pBase->get_normal_scale(), pBase->get_alpha_scissor_threshold() );
	const Ref<Texture2D> AlbedoTexture = pBase->get_texture( BaseMaterial3D::TEXTURE_ALBEDO );
	const Ref<Texture2D> NormalTexture = pBase->get_texture( BaseMaterial3D::TEXTURE_NORMAL );
	const Ref<Texture2D> HeightTexture = pBase->get_texture( BaseMaterial3D::TEXTURE_HEIGHTMAP );
	rRecord.nAlbedoVT = t3d_rw_material_resolve_vt( m_materialResolver, AlbedoTexture, T3D_RW_VT_MATERIAL_ALBEDO );
	rRecord.nNormalVT = t3d_rw_material_resolve_vt( m_materialResolver, NormalTexture, T3D_RW_VT_MATERIAL_NORMAL );
	rRecord.nHeightVT = t3d_rw_material_resolve_vt( m_materialResolver, HeightTexture, T3D_RW_VT_MATERIAL_HEIGHT );
	// A source texture may not silently collapse to a neutral/default VT page.
	// Until the resolver has a cooked allocation, keep the source visual native.
	if ( ( AlbedoTexture.is_valid() && rRecord.nAlbedoVT == T3D_RW_INVALID_ID )
	        || ( NormalTexture.is_valid() && rRecord.nNormalVT == T3D_RW_INVALID_ID )
	        || ( HeightTexture.is_valid() && rRecord.nHeightVT == T3D_RW_INVALID_ID ) ) {
		return false;
	}
	return true;
}

bool Terrain3DRenderWorldGodotConverter::findOrCompileMaterialCold( const Ref<Material> &pMaterial,
        const Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial,
        uint32_t pTier,
        uint32_t &rMaterialId,
        T3DRenderWorldConversionReason &rReason ) {
	uint64_t nKey = 0u;
	if ( pExplicitMaterial.is_valid() ) {
		nKey = pExplicitMaterial->get_instance_id();
	} else if ( pMaterial.is_valid() ) {
		nKey = ( uint64_t )pMaterial->get_rid().get_id();
	} else {
		nKey = 1u;
	}
	nKey = t3d_rw_material_hash_mix( nKey, pTier );

	for ( uint32_t i = 0u; i < m_pBuffers->nMaterialEntryCount; ++i ) {
		if ( m_pBuffers->pMaterialEntries[i].nKey == nKey ) {
			rMaterialId = m_pBuffers->pMaterialEntries[i].nMaterialId;
			return true;
		}
	}
	if ( m_pBuffers->nMaterialCount >= m_pBuffers->nMaterialCapacity
	        || m_pBuffers->nMaterialEntryCount >= m_pBuffers->nMaterialEntryCapacity ) {
		rReason = T3D_RW_CONVERSION_CAPACITY_EXCEEDED;
		return false;
	}

	T3DMaterialRecord Record;
	if ( pExplicitMaterial.is_valid() ) {
		if ( pExplicitMaterial->isTwoSided() && pTier != T3D_RW_TIER_FOLIAGE ) {
			rReason = T3D_RW_CONVERSION_TWO_SIDED_NON_FOLIAGE;
			return false;
		}
		t3dRenderWorldMaterialVtBindings_t VT;
		const Ref<Texture2D> AlbedoTexture = pExplicitMaterial->getAlbedoTexture();
		const Ref<Texture2D> NormalTexture = pExplicitMaterial->getNormalTexture();
		const Ref<Texture2D> SurfaceTexture = pExplicitMaterial->getSurfaceTexture();
		const Ref<Texture2D> HeightTexture = pExplicitMaterial->getHeightTexture();
		VT.m_nAlbedoVt = t3d_rw_material_resolve_vt( m_materialResolver, AlbedoTexture, T3D_RW_VT_MATERIAL_ALBEDO );
		VT.m_nNormalVt = t3d_rw_material_resolve_vt( m_materialResolver, NormalTexture, T3D_RW_VT_MATERIAL_NORMAL );
		VT.m_nSurfaceVt = t3d_rw_material_resolve_vt( m_materialResolver, SurfaceTexture, T3D_RW_VT_MATERIAL_SURFACE );
		VT.m_nHeightVt = t3d_rw_material_resolve_vt( m_materialResolver, HeightTexture, T3D_RW_VT_MATERIAL_HEIGHT );
		if ( ( AlbedoTexture.is_valid() && VT.m_nAlbedoVt == T3D_RW_INVALID_ID )
		        || ( NormalTexture.is_valid() && VT.m_nNormalVt == T3D_RW_INVALID_ID )
		        || ( SurfaceTexture.is_valid() && VT.m_nSurfaceVt == T3D_RW_INVALID_ID )
		        || ( HeightTexture.is_valid() && VT.m_nHeightVt == T3D_RW_INVALID_ID ) ) {
			rReason = T3D_RW_CONVERSION_UNSUPPORTED_MATERIAL;
			return false;
		}
		pExplicitMaterial->compileRecordCold( VT, Record );
	} else {
		bool bSorted = false;
		bool bTwoSided = false;
		if ( !compileStandardMaterialCold( pMaterial, pTier, Record, bSorted, bTwoSided ) ) {
			rReason = bSorted ? T3D_RW_CONVERSION_SORTED_TRANSPARENCY : T3D_RW_CONVERSION_UNSUPPORTED_MATERIAL;
			return false;
		}
	}

	rMaterialId = m_pBuffers->nMaterialCount++;
	m_pBuffers->pMaterials[rMaterialId] = Record;
	T3DRenderWorldMaterialEntry &Entry = m_pBuffers->pMaterialEntries[m_pBuffers->nMaterialEntryCount++];
	Entry.nKey = nKey;
	Entry.nMaterialId = rMaterialId;
	Entry.nGeneration = m_pBuffers->pOwnership->nGeneration;
	return true;
}
