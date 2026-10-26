// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/world/core/terrain_3d_world_authoring.h"

#include <string.h>

static uint32_t t3d_world_authoring_hash_source_id( uint64_t pSourceId ) {
	uint64_t nValue = pSourceId;
	nValue ^= nValue >> 33u;
	nValue *= 0xff51afd7ed558ccduLL;
	nValue ^= nValue >> 33u;
	nValue *= 0xc4ceb9fe1a85ec53uLL;
	nValue ^= nValue >> 33u;
	return ( uint32_t )nValue;
}

static bool t3d_world_authoring_kind_enabled( const t3dWorldConversionPolicy_t &pPolicy, t3dWorldCandidateKind_t pKind ) {
	switch ( pKind ) {
		case T3D_WORLD_CANDIDATE_WORLD_INSTANCE:
			return true;
		case T3D_WORLD_CANDIDATE_MESH_INSTANCE:
			return pPolicy.bConvertMeshInstances;
		case T3D_WORLD_CANDIDATE_MULTIMESH_INSTANCE:
			return pPolicy.bConvertMultiMeshInstances;
		default:
			return false;
	}
}

t3dWorldConversionPolicy_t t3d_world_authoring_default_policy() {
	t3dWorldConversionPolicy_t Policy;
	Policy.eMode = T3D_WORLD_MODE_EXPLICIT;
	Policy.nDefaultTier = T3D_WORLD_TIER_WORLD;
	Policy.eMaterialPolicy = T3D_WORLD_MATERIAL_AUTO_CONVERT;
	Policy.eTransparencyPolicy = T3D_WORLD_TRANSPARENCY_NATIVE_SORTED;
	Policy.bConvertMeshInstances = true;
	Policy.bConvertMultiMeshInstances = false;
	Policy.bConvertDynamicObjects = false;
	Policy.bConvertHiddenObjects = false;
	Policy.fCellSize = 32.0f;
	return Policy;
}

t3dWorldClassification_t t3d_world_authoring_classify_reference_debug(
        const t3dWorldConversionPolicy_t &pPolicy, const t3dWorldCandidateDesc_t &pCandidate ) {
	t3dWorldClassification_t Result;
	Result.eOwnership = T3D_WORLD_OWNERSHIP_NATIVE_GODOT;
	Result.eReason = T3D_WORLD_CONVERSION_OK;
	Result.nTier = pCandidate.nTier < T3D_WORLD_TIER_COUNT ? pCandidate.nTier : pPolicy.nDefaultTier;

	if ( pPolicy.eMode == T3D_WORLD_MODE_DISABLED ) {
		Result.eReason = T3D_WORLD_CONVERSION_DISABLED;
		return Result;
	}
	if ( pCandidate.bExplicitExclude ) {
		Result.eReason = T3D_WORLD_CONVERSION_EXCLUDED;
		return Result;
	}
	if ( !pCandidate.bVisible && !pPolicy.bConvertHiddenObjects ) {
		Result.eReason = T3D_WORLD_CONVERSION_HIDDEN;
		return Result;
	}
	if ( !pCandidate.bStatic ) {
		// World compatibility ingress is currently static-only. The normal
		// Forward+ motion output is correct for camera motion, but object motion
		// requires a published previous-transform bank before ownership can move.
		Result.eReason = T3D_WORLD_CONVERSION_DYNAMIC_DISABLED;
		return Result;
	}
	if ( !pCandidate.bHasMesh ) {
		Result.eReason = T3D_WORLD_CONVERSION_MISSING_MESH;
		return Result;
	}
	if ( !t3d_world_authoring_kind_enabled( pPolicy, pCandidate.eKind ) ) {
		Result.eReason = T3D_WORLD_CONVERSION_UNSUPPORTED_KIND;
		return Result;
	}
	if ( pPolicy.eMode == T3D_WORLD_MODE_EXPLICIT && pCandidate.eKind != T3D_WORLD_CANDIDATE_WORLD_INSTANCE ) {
		Result.eReason = T3D_WORLD_CONVERSION_NOT_EXPLICIT;
		return Result;
	}
	if ( pPolicy.eMode == T3D_WORLD_MODE_ELIGIBLE_SCENE && pCandidate.eKind != T3D_WORLD_CANDIDATE_WORLD_INSTANCE
	        && !pCandidate.bExplicitInclude ) {
		Result.eReason = T3D_WORLD_CONVERSION_NOT_EXPLICIT;
		return Result;
	}
	if ( pCandidate.bSortedTransparency ) {
		Result.eOwnership = T3D_WORLD_OWNERSHIP_COMPATIBILITY;
		Result.eReason = T3D_WORLD_CONVERSION_SORTED_TRANSPARENCY;
		return Result;
	}
	if ( !pCandidate.bMaterialSupported ) {
		Result.eOwnership = T3D_WORLD_OWNERSHIP_COMPATIBILITY;
		Result.eReason = T3D_WORLD_CONVERSION_UNSUPPORTED_MATERIAL;
		return Result;
	}
	Result.eOwnership = T3D_WORLD_OWNERSHIP_WORLD;
	Result.eReason = T3D_WORLD_CONVERSION_OK;
	return Result;
}

bool t3d_world_ownership_registry_initialize_cold(
        t3dWorldOwnershipRegistry_t &rRegistry, t3dWorldOwnershipEntry_t *pEntries, uint32_t pCapacity ) {
	if ( pEntries == NULL || pCapacity < 8u || ( pCapacity & ( pCapacity - 1u ) ) != 0u ) {
		return false;
	}

	rRegistry.pEntries = pEntries;
	rRegistry.nCapacity = pCapacity;
	rRegistry.nCount = 0u;
	rRegistry.nGeneration = 1u;
	for ( uint32_t i = 0u; i < pCapacity; ++i ) {
		memset( &pEntries[i], 0, sizeof( t3dWorldOwnershipEntry_t ) );
		pEntries[i].nSourceId = 0u;
		pEntries[i].nPrototypeId = T3D_WORLD_AUTHORING_INVALID_SLOT;
		pEntries[i].nInstanceId = T3D_WORLD_AUTHORING_INVALID_SLOT;
		pEntries[i].eOwnership = T3D_WORLD_OWNERSHIP_NATIVE_GODOT;
		pEntries[i].eReason = T3D_WORLD_CONVERSION_OK;
	}
	return true;
}

void t3d_world_ownership_registry_clear_cold( t3dWorldOwnershipRegistry_t &rRegistry ) {
	if ( rRegistry.pEntries == NULL || rRegistry.nCapacity == 0u ) {
		return;
	}

	++rRegistry.nGeneration;
	if ( rRegistry.nGeneration == 0u ) {
		rRegistry.nGeneration = 1u;
	}
	rRegistry.nCount = 0u;
	for ( uint32_t i = 0u; i < rRegistry.nCapacity; ++i ) {
		rRegistry.pEntries[i].nSourceId = 0u;
		rRegistry.pEntries[i].nPrototypeId = T3D_WORLD_AUTHORING_INVALID_SLOT;
		rRegistry.pEntries[i].nInstanceId = T3D_WORLD_AUTHORING_INVALID_SLOT;
		rRegistry.pEntries[i].nGeneration = 0u;
		rRegistry.pEntries[i].eOwnership = T3D_WORLD_OWNERSHIP_NATIVE_GODOT;
		rRegistry.pEntries[i].eReason = T3D_WORLD_CONVERSION_OK;
	}
}

bool t3d_world_ownership_registry_publish_cold(
        t3dWorldOwnershipRegistry_t &rRegistry, const t3dWorldOwnershipEntry_t &pEntry ) {
	if ( rRegistry.pEntries == NULL || rRegistry.nCapacity == 0u || pEntry.nSourceId == 0u ) {
		return false;
	}

	const uint32_t nMask = rRegistry.nCapacity - 1u;
	uint32_t nSlot = t3d_world_authoring_hash_source_id( pEntry.nSourceId ) & nMask;
	for ( uint32_t i = 0u; i < rRegistry.nCapacity; ++i ) {
		t3dWorldOwnershipEntry_t &Entry = rRegistry.pEntries[nSlot];
		if ( Entry.nSourceId == 0u || Entry.nSourceId == pEntry.nSourceId ) {
			if ( Entry.nSourceId == 0u ) {
				++rRegistry.nCount;
			}
			Entry = pEntry;
			Entry.nGeneration = rRegistry.nGeneration;
			return true;
		}
		nSlot = ( nSlot + 1u ) & nMask;
	}
	return false;
}

const t3dWorldOwnershipEntry_t *t3d_world_ownership_registry_find_reference_debug(
        const t3dWorldOwnershipRegistry_t &pRegistry, uint64_t pSourceId ) {
	if ( pRegistry.pEntries == NULL || pRegistry.nCapacity == 0u || pSourceId == 0u ) {
		return NULL;
	}

	const uint32_t nMask = pRegistry.nCapacity - 1u;
	uint32_t nSlot = t3d_world_authoring_hash_source_id( pSourceId ) & nMask;
	for ( uint32_t i = 0u; i < pRegistry.nCapacity; ++i ) {
		const t3dWorldOwnershipEntry_t &Entry = pRegistry.pEntries[nSlot];
		if ( Entry.nSourceId == pSourceId ) {
			return &Entry;
		}
		if ( Entry.nSourceId == 0u ) {
			return NULL;
		}
		nSlot = ( nSlot + 1u ) & nMask;
	}
	return NULL;
}
