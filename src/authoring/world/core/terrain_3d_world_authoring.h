// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_AUTHORING_H
#define TERRAIN3D_WORLD_AUTHORING_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/world/geometry/terrain_3d_world_geometry.h"

#include <stdint.h>

t3dWorldConversionPolicy_t t3d_world_authoring_default_policy();
t3dWorldClassification_t   t3d_world_authoring_classify_reference_debug( const t3dWorldConversionPolicy_t &pPolicy,
																		 const t3dWorldCandidateDesc_t	  &pCandidate );

bool t3d_world_ownership_registry_initialize_cold( t3dWorldOwnershipRegistry_t &rRegistry,
												   t3dWorldOwnershipEntry_t	   *pEntries,
												   uint32_t						pCapacity );
void t3d_world_ownership_registry_clear_cold( t3dWorldOwnershipRegistry_t &rRegistry );
bool t3d_world_ownership_registry_publish_cold( t3dWorldOwnershipRegistry_t	   &rRegistry,
												const t3dWorldOwnershipEntry_t &pEntry );
const t3dWorldOwnershipEntry_t *
t3d_world_ownership_registry_find_reference_debug( const t3dWorldOwnershipRegistry_t &pRegistry, uint64_t pSourceId );

#endif // TERRAIN3D_WORLD_AUTHORING_H
