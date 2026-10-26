// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_DESCRIPTOR_RECIPE_H
#define TERRAIN3D_RD_DESCRIPTOR_RECIPE_H

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_contract.h"

// Called on the cold prepare / resource-rebuild path, before creating any
// RDUniform or TypedArray. Failed operations leave the recipe unchanged.
t3dRdDescriptorStatus_t t3dRdDescriptorRecipeBeginCold( t3dRdDescriptorRecipe_t			 &rRecipe,
														const t3dRdDescriptorDeviceKey_t &rDevice,
														const godot::RID				 &rShader,
														uint32_t						  nSetIndex );
t3dRdDescriptorStatus_t t3dRdDescriptorRecipeAddBindingCold( t3dRdDescriptorRecipe_t &rRecipe,
															 uint32_t				  nBinding,
															 uint32_t				  nType,
															 const godot::RID		 *pResources,
															 uint32_t				  nResourceCount );
t3dRdDescriptorStatus_t t3dRdDescriptorRecipeValidate( const t3dRdDescriptorRecipe_t &rRecipe,
													   t3dRdDescriptorDeviceDomain_t  eRequiredDomain );

// A fingerprint only screens candidates. A matching fingerprint NEVER
// bypasses exact comparison, including all bound resource RIDs.
uint64_t t3dRdDescriptorRecipeFingerprint( const t3dRdDescriptorRecipe_t &rRecipe );
bool	 t3dRdDescriptorRecipesEqual( const t3dRdDescriptorRecipe_t &rFirst, const t3dRdDescriptorRecipe_t &rSecond );
bool	 t3dRdDescriptorRecipesMatch( const t3dRdDescriptorRecipe_t &rFirst,
									  uint64_t						 nFirstFingerprint,
									  const t3dRdDescriptorRecipe_t &rSecond,
									  uint64_t						 nSecondFingerprint );

// Never allocate entries on behalf of the cache unless sufficient tracked
// resource-registry headroom remains after other reserved resources.
t3dRdDescriptorStatus_t t3dRdDescriptorPlanCapacityCold( const t3dRdDescriptorCapacityRequest_t &rRequest,
														 uint32_t								&rAllowedSlots );

bool t3dRdDescriptorLeaseMatches( const t3dRdDescriptorLease_t &rLease,
								  uint32_t						nSlot,
								  uint32_t						nSlotGeneration,
								  uint64_t						nDeviceGeneration );

#endif // TERRAIN3D_RD_DESCRIPTOR_RECIPE_H
