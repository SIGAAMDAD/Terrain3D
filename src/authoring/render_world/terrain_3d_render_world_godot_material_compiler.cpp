// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/render_world/terrain_3d_render_world_godot_converter.h"
#include "assets/terrain_3d_rendering_material.h"
#include "rendering/rd/vt/terrain_3d_vt_material_identity.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/material.hpp>

#include <string.h>

using namespace godot;

static uint64_t t3d_rw_material_hash_mix( uint64_t pHash, uint64_t pValue ) {
	pHash ^= pValue + 0x9e3779b97f4a7c15uLL + ( pHash << 6u ) + ( pHash >> 2u );
	return pHash;
}

static uint64_t t3d_rw_material_stable_key( const Ref<Resource> &pMaterial ) {
	return pMaterial.is_valid() ? t3d_vt_material_stable_key_from_resource_path( pMaterial->get_path() ) : 0u;
}

static bool t3d_rw_material_has_vt_sources( const Terrain3DRenderWorldMaterial *pMaterial ) {
	return pMaterial != NULL
	        && ( pMaterial->getAlbedoTexture().is_valid() || pMaterial->getNormalTexture().is_valid()
	                || pMaterial->getSurfaceTexture().is_valid() || pMaterial->getHeightTexture().is_valid() );
}

static bool t3d_rw_material_resolve_page_set( const T3DRenderWorldGodotMaterialResolver &rResolver,
        const uint64_t nStableKey,
        const bool bRequiresAllocation,
        t3dRenderingMaterialVtBindings_t &rBindings ) {
	rBindings.m_nVtAllocation = T3D_VT_INVALID_ALLOCATION;
	rBindings.m_nOverlayVtAllocation = T3D_VT_INVALID_ALLOCATION;
	if ( !bRequiresAllocation ) {
		return true;
	}
	if ( nStableKey == 0u || rResolver.pResolveMaterialVT == NULL ) {
		return false;
	}
	uint32_t nAllocation = T3D_VT_INVALID_ALLOCATION;
	if ( !rResolver.pResolveMaterialVT( rResolver.pUser, nStableKey, nAllocation ) || nAllocation == T3D_VT_INVALID_ALLOCATION ) {
		return false;
	}
	rBindings.m_nVtAllocation = nAllocation;
	return true;
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
	( void )pTier;
	return true;
}

bool Terrain3DRenderWorldGodotConverter::compileStandardMaterialCold( const Ref<Material> &pMaterial,
        const uint32_t pTier,
        t3dRenderingMaterialGpu_t &rRecord,
        bool &rSortedTransparency,
        bool &rTwoSided ) const {
	rSortedTransparency = false;
	rTwoSided = false;

	Ref<Terrain3DRenderingMaterial> Canonical;
	Canonical.instantiate();
	if ( Canonical.is_null() ) {
		return false;
	}
	if ( pMaterial.is_valid() ) {
		BaseMaterial3D *const pBase = Object::cast_to<BaseMaterial3D>( pMaterial.ptr() );
		if ( pBase == NULL ) {
			return false;
		}
		const BaseMaterial3D::Transparency eTransparency = pBase->get_transparency();
		rSortedTransparency =
		        eTransparency != BaseMaterial3D::TRANSPARENCY_DISABLED && eTransparency != BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR;
		rTwoSided = pBase->get_cull_mode() == BaseMaterial3D::CULL_DISABLED;
		if ( rSortedTransparency || !Canonical->importBaseMaterialCold( pMaterial ) ) {
			return false;
		}
	} else {
		Canonical->setCastShadows( true );
		Canonical->setReceiveShadows( true );
	}
	Canonical->setFoliage( pTier == T3D_RW_TIER_FOLIAGE );

	t3dRenderingMaterialVtBindings_t Bindings;
	const uint64_t nStableKey = t3d_rw_material_stable_key( pMaterial );
	if ( !t3d_rw_material_resolve_page_set( m_materialResolver, nStableKey, t3d_rw_material_has_vt_sources( Canonical.ptr() ), Bindings )
	        || !Canonical->compileVtRecordCold( Bindings, rRecord ) ) {
		return false;
	}
	if ( Bindings.m_nVtAllocation != T3D_VT_INVALID_ALLOCATION ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_V3_LAYOUT;
	}
	return true;
}

bool Terrain3DRenderWorldGodotConverter::findOrCompileMaterialCold( const Ref<Material> &pMaterial,
        const Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial,
        uint32_t pTier,
        uint32_t &rMaterialId,
        T3DRenderWorldConversionReason &rReason ) {
	const uint64_t nStableKey = pExplicitMaterial.is_valid()
	        ? t3d_vt_material_stable_key_from_resource_path( pExplicitMaterial->get_path() )
	        : t3d_rw_material_stable_key( pMaterial );
	uint64_t nKey = nStableKey;
	if ( nKey == 0u ) {
		nKey = pExplicitMaterial.is_valid() ? ( uint64_t )pExplicitMaterial->get_instance_id()
		                                    : ( pMaterial.is_valid() ? ( uint64_t )pMaterial->get_instance_id() : 1u );
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

	t3dRenderingMaterialGpu_t Record;
	memset( &Record, 0, sizeof( Record ) );
	if ( pExplicitMaterial.is_valid() ) {
		t3dRenderingMaterialVtBindings_t Bindings;
		if ( !t3d_rw_material_resolve_page_set(
		             m_materialResolver, nStableKey, t3d_rw_material_has_vt_sources( pExplicitMaterial.ptr() ), Bindings ) ) {
			rReason = T3D_RW_CONVERSION_UNSUPPORTED_MATERIAL;
			return false;
		}
		if ( !pExplicitMaterial->compileVtRecordCold( Bindings, Record ) ) {
			rReason = T3D_RW_CONVERSION_UNSUPPORTED_MATERIAL;
			return false;
		}
		if ( Bindings.m_nVtAllocation != T3D_VT_INVALID_ALLOCATION ) {
			Record.m_nFlags |= T3D_VT_MATERIAL_V3_LAYOUT;
		}
		if ( pTier == T3D_RW_TIER_FOLIAGE ) {
			Record.m_nFlags |= T3D_VT_MATERIAL_FOLIAGE;
		}
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
