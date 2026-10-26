// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_recipe.h"
#include <godot_cpp/classes/rendering_device.hpp>

using namespace godot;

static bool t3dRdDescriptorValidDeviceKey(
	const t3dRdDescriptorDeviceKey_t &rDevice ) {
	if ( rDevice.m_nDeviceGeneration == 0u || rDevice.m_nDeviceIdentity == 0u ) {
		return false;
	}
	if ( rDevice.m_eDomain == T3D_RD_DESCRIPTOR_DEVICE_GLOBAL ) {
		return rDevice.m_nDeviceIdentity == T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID;
	}
	return rDevice.m_eDomain == T3D_RD_DESCRIPTOR_DEVICE_LOCAL;
}

static bool t3dRdDescriptorValidResourceCount(
	uint32_t nType,
	uint32_t nCount ) {
	if ( nType >= ( uint32_t ) RenderingDevice::UNIFORM_TYPE_MAX || nCount == 0u ) {
		return false;
	}
	return ( nType != ( uint32_t ) RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE &&
			 nType != ( uint32_t ) RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER ) ||
		   ( nCount >= 2u && ( nCount & 1u ) == 0u );
}

static uint64_t t3dRdDescriptorHashWord(
	uint64_t nHash,
	uint64_t nWord ) {
	// Byte-ordered FNV-1a, independent of host endianness and struct padding.
	for ( uint32_t i = 0u; i < 8u; ++i ) {
		nHash ^= nWord & UINT64_C( 0xff );
		nHash *= UINT64_C( 1099511628211 );
		nWord >>= 8u;
	}
	return nHash;
}

t3dRdDescriptorStatus_t t3dRdDescriptorRecipeBeginCold(
	t3dRdDescriptorRecipe_t			 &rRecipe,
	const t3dRdDescriptorDeviceKey_t &rDevice,
	const RID						 &rShader,
	uint32_t						  nSetIndex ) {
	if ( !t3dRdDescriptorValidDeviceKey( rDevice ) ) {
		return T3D_RD_DESCRIPTOR_WRONG_DEVICE;
	}
	if ( !rShader.is_valid() || nSetIndex >= T3D_MAX_SETS ) {
		return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	}
	rRecipe.m_hShader = rShader;
	rRecipe.m_device = rDevice;
	rRecipe.m_nSetIndex = nSetIndex;
	rRecipe.m_nBindingCount = 0u;
	rRecipe.m_nResourceCount = 0u;
	return T3D_RD_DESCRIPTOR_OK;
}

t3dRdDescriptorStatus_t t3dRdDescriptorRecipeAddBindingCold(
	t3dRdDescriptorRecipe_t &rRecipe,
	uint32_t				 nBinding,
	uint32_t				 nType,
	const RID				*pResources,
	uint32_t				 nResourceCount ) {
	if ( nBinding > UINT16_MAX || nType > UINT16_MAX || !pResources ||
		 !t3dRdDescriptorValidResourceCount( nType, nResourceCount ) ) {
		return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	}
	if ( !rRecipe.m_hShader.is_valid() || !t3dRdDescriptorValidDeviceKey( rRecipe.m_device ) ||
		 rRecipe.m_nBindingCount > T3D_RD_DESCRIPTOR_MAX_BINDINGS ||
		 rRecipe.m_nResourceCount > T3D_RD_DESCRIPTOR_MAX_RESOURCES ) {
		return T3D_RD_DESCRIPTOR_INVALID_RECIPE;
	}
	if ( rRecipe.m_nBindingCount == T3D_RD_DESCRIPTOR_MAX_BINDINGS ||
		 nResourceCount > T3D_RD_DESCRIPTOR_MAX_RESOURCES - rRecipe.m_nResourceCount ) {
		return T3D_RD_DESCRIPTOR_RECIPE_CAPACITY_EXCEEDED;
	}
	for ( uint32_t i = 0u; i < nResourceCount; ++i ) {
		if ( !pResources[i].is_valid() ) {
			return T3D_RD_DESCRIPTOR_INVALID_DEPENDENCY;
		}
	}
	uint32_t nInsert = 0u;
	while ( nInsert < rRecipe.m_nBindingCount && rRecipe.m_szBindings[nInsert].m_nBinding < nBinding ) {
		++nInsert;
	}
	if ( nInsert != rRecipe.m_nBindingCount && rRecipe.m_szBindings[nInsert].m_nBinding == nBinding ) {
		return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	}
	const uint32_t nFirst =
		nInsert < rRecipe.m_nBindingCount ? rRecipe.m_szBindings[nInsert].m_nResourceFirst : rRecipe.m_nResourceCount;
	// RID is a non-trivial Godot value type. Shift by assignment, never memcpy.
	for ( uint32_t i = rRecipe.m_nResourceCount; i > nFirst; --i ) {
		rRecipe.m_szResources[i + nResourceCount - 1u] = rRecipe.m_szResources[i - 1u];
	}
	for ( uint32_t i = rRecipe.m_nBindingCount; i > nInsert; --i ) {
		rRecipe.m_szBindings[i] = rRecipe.m_szBindings[i - 1u];
		rRecipe.m_szBindings[i].m_nResourceFirst =
			( uint16_t ) ( rRecipe.m_szBindings[i].m_nResourceFirst + nResourceCount );
	}
	for ( uint32_t i = 0u; i < nResourceCount; ++i ) {
		rRecipe.m_szResources[nFirst + i] = pResources[i];
	}
	t3dRdDescriptorBinding_t &rBinding = rRecipe.m_szBindings[nInsert];
	rBinding.m_nBinding = ( uint16_t ) nBinding;
	rBinding.m_nType = ( uint16_t ) nType;
	rBinding.m_nResourceFirst = ( uint16_t ) nFirst;
	rBinding.m_nResourceCount = ( uint16_t ) nResourceCount;
	++rRecipe.m_nBindingCount;
	rRecipe.m_nResourceCount = ( uint16_t ) ( rRecipe.m_nResourceCount + nResourceCount );
	return T3D_RD_DESCRIPTOR_OK;
}

t3dRdDescriptorStatus_t t3dRdDescriptorRecipeValidate(
	const t3dRdDescriptorRecipe_t &rRecipe,
	t3dRdDescriptorDeviceDomain_t  eRequiredDomain ) {
	if ( !t3dRdDescriptorValidDeviceKey( rRecipe.m_device ) || rRecipe.m_device.m_eDomain != eRequiredDomain ) {
		return T3D_RD_DESCRIPTOR_WRONG_DEVICE;
	}
	if ( !rRecipe.m_hShader.is_valid() || rRecipe.m_nSetIndex >= T3D_MAX_SETS || !rRecipe.m_nBindingCount ||
		 rRecipe.m_nBindingCount > T3D_RD_DESCRIPTOR_MAX_BINDINGS ||
		 rRecipe.m_nResourceCount > T3D_RD_DESCRIPTOR_MAX_RESOURCES ) {
		return T3D_RD_DESCRIPTOR_INVALID_RECIPE;
	}
	uint32_t nExpectedFirst = 0u;
	for ( uint32_t i = 0u; i < rRecipe.m_nBindingCount; ++i ) {
		const t3dRdDescriptorBinding_t &rBinding = rRecipe.m_szBindings[i];
		if ( ( i && rBinding.m_nBinding <= rRecipe.m_szBindings[i - 1u].m_nBinding ) ||
			 rBinding.m_nResourceFirst != nExpectedFirst ||
			 !t3dRdDescriptorValidResourceCount( rBinding.m_nType, rBinding.m_nResourceCount ) ||
			 rBinding.m_nResourceCount > rRecipe.m_nResourceCount - nExpectedFirst ) {
			return T3D_RD_DESCRIPTOR_INVALID_RECIPE;
		}
		for ( uint32_t j = 0u; j < rBinding.m_nResourceCount; ++j ) {
			if ( !rRecipe.m_szResources[nExpectedFirst + j].is_valid() ) {
				return T3D_RD_DESCRIPTOR_INVALID_DEPENDENCY;
			}
		}
		nExpectedFirst += rBinding.m_nResourceCount;
	}
	return nExpectedFirst == rRecipe.m_nResourceCount ? T3D_RD_DESCRIPTOR_OK : T3D_RD_DESCRIPTOR_INVALID_RECIPE;
}

uint64_t t3dRdDescriptorRecipeFingerprint(
	const t3dRdDescriptorRecipe_t &rRecipe ) {
	if ( t3dRdDescriptorRecipeValidate( rRecipe, rRecipe.m_device.m_eDomain ) != T3D_RD_DESCRIPTOR_OK ) {
		return 0u;
	}
	uint64_t nHash = UINT64_C( 14695981039346656037 );
	nHash = t3dRdDescriptorHashWord( nHash, ( uint64_t ) rRecipe.m_device.m_eDomain );
	nHash = t3dRdDescriptorHashWord( nHash, rRecipe.m_device.m_nDeviceIdentity );
	nHash = t3dRdDescriptorHashWord( nHash, rRecipe.m_device.m_nDeviceGeneration );
	nHash = t3dRdDescriptorHashWord( nHash, ( uint64_t ) rRecipe.m_hShader.get_id() );
	nHash = t3dRdDescriptorHashWord( nHash, rRecipe.m_nSetIndex );
	nHash = t3dRdDescriptorHashWord( nHash, rRecipe.m_nBindingCount );
	nHash = t3dRdDescriptorHashWord( nHash, rRecipe.m_nResourceCount );
	for ( uint32_t i = 0u; i < rRecipe.m_nBindingCount; ++i ) {
		const t3dRdDescriptorBinding_t &rBinding = rRecipe.m_szBindings[i];
		nHash = t3dRdDescriptorHashWord( nHash, rBinding.m_nBinding );
		nHash = t3dRdDescriptorHashWord( nHash, rBinding.m_nType );
		nHash = t3dRdDescriptorHashWord( nHash, rBinding.m_nResourceCount );
	}
	for ( uint32_t i = 0u; i < rRecipe.m_nResourceCount; ++i ) {
		nHash = t3dRdDescriptorHashWord( nHash, ( uint64_t ) rRecipe.m_szResources[i].get_id() );
	}
	return nHash;
}

bool t3dRdDescriptorRecipesEqual(
	const t3dRdDescriptorRecipe_t &rFirst,
	const t3dRdDescriptorRecipe_t &rSecond ) {
	if ( t3dRdDescriptorRecipeValidate( rFirst, rFirst.m_device.m_eDomain ) != T3D_RD_DESCRIPTOR_OK ||
		 t3dRdDescriptorRecipeValidate( rSecond, rSecond.m_device.m_eDomain ) != T3D_RD_DESCRIPTOR_OK ) {
		return false;
	}
	if ( rFirst.m_device.m_eDomain != rSecond.m_device.m_eDomain ||
		 rFirst.m_device.m_nDeviceIdentity != rSecond.m_device.m_nDeviceIdentity ||
		 rFirst.m_device.m_nDeviceGeneration != rSecond.m_device.m_nDeviceGeneration ||
		 rFirst.m_hShader != rSecond.m_hShader || rFirst.m_nSetIndex != rSecond.m_nSetIndex ||
		 rFirst.m_nBindingCount != rSecond.m_nBindingCount || rFirst.m_nResourceCount != rSecond.m_nResourceCount ) {
		return false;
	}
	for ( uint32_t i = 0u; i < rFirst.m_nBindingCount; ++i ) {
		const t3dRdDescriptorBinding_t &a = rFirst.m_szBindings[i];
		const t3dRdDescriptorBinding_t &b = rSecond.m_szBindings[i];
		if ( a.m_nBinding != b.m_nBinding || a.m_nType != b.m_nType || a.m_nResourceFirst != b.m_nResourceFirst ||
			 a.m_nResourceCount != b.m_nResourceCount ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < rFirst.m_nResourceCount; ++i ) {
		if ( rFirst.m_szResources[i] != rSecond.m_szResources[i] ) {
			return false;
		}
	}
	return true;
}

bool t3dRdDescriptorRecipesMatch(
	const t3dRdDescriptorRecipe_t &rFirst,
	uint64_t					   nFirstFingerprint,
	const t3dRdDescriptorRecipe_t &rSecond,
	uint64_t					   nSecondFingerprint ) {
	return nFirstFingerprint != 0u && nFirstFingerprint == nSecondFingerprint &&
		   t3dRdDescriptorRecipesEqual( rFirst, rSecond );
}

t3dRdDescriptorStatus_t t3dRdDescriptorPlanCapacityCold(
	const t3dRdDescriptorCapacityRequest_t &rRequest,
	uint32_t							   &rAllowedSlots ) {
	rAllowedSlots = 0u;
	if ( rRequest.m_nRequestedSlots == 0u || rRequest.m_nRegistrySlotsFree > T3D_RESOURCE_CAPACITY ) {
		return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	}
	if ( rRequest.m_nRegistrySlotsFree < rRequest.m_nNonDescriptorSlotsReserved ||
		 rRequest.m_nRequestedSlots > rRequest.m_nRegistrySlotsFree - rRequest.m_nNonDescriptorSlotsReserved ) {
		return T3D_RD_DESCRIPTOR_RESOURCE_REGISTRY_EXHAUSTED;
	}
	rAllowedSlots = rRequest.m_nRequestedSlots;
	return T3D_RD_DESCRIPTOR_OK;
}

bool t3dRdDescriptorLeaseMatches(
	const t3dRdDescriptorLease_t &rLease,
	uint32_t					  nSlot,
	uint32_t					  nSlotGeneration,
	uint64_t					  nDeviceGeneration ) {
	return rLease.m_nSlot != T3D_RD_DESCRIPTOR_INVALID_SLOT && rLease.m_nSlot == nSlot && nSlotGeneration != 0u &&
		   rLease.m_nSlotGeneration == nSlotGeneration && nDeviceGeneration != 0u &&
		   rLease.m_nDeviceGeneration == nDeviceGeneration;
}
