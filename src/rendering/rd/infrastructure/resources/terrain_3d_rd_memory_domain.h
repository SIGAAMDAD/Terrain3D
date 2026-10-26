#ifndef TERRAIN3D_RD_MEMORY_DOMAIN_H
#define TERRAIN3D_RD_MEMORY_DOMAIN_H

#include "Terrain3DTypes.h"
#include "memory/policy/terrain_3d_memory_account.h"

// Unified world buffers containing instance transforms/cells and HLOD records
// belong to those consumers. Shared mesh geometry stays in the world domain.
inline Terrain3DMemoryAccount::Domain t3dRdMemoryDomain(
	t3dRdResourceOwner_t  owner,
	t3dRdMemoryCategory_t memory ) {
	using Account = Terrain3DMemoryAccount;
	if ( memory == T3D_RD_CONTEXT_MEMORY_COMPOSED_SURFACE ) {
		return Account::COMPOSED_SURFACE;
	}
	if ( owner == T3D_RD_CONTEXT_OWNER_VT ) {
		return Account::VIRTUAL_TEXTURE;
	}
	if ( memory == T3D_RD_CONTEXT_MEMORY_INSTANCE || owner == T3D_RD_CONTEXT_OWNER_INSTANCES ) {
		return Account::INSTANCE_RENDER;
	}
	if ( memory == T3D_RD_CONTEXT_MEMORY_HLOD || owner == T3D_RD_CONTEXT_OWNER_HLOD ) {
		return Account::HLOD_RENDER;
	}
	switch ( owner ) {
	case T3D_RD_CONTEXT_OWNER_TERRAIN:
		return Account::TERRAIN_RENDER;
	case T3D_RD_CONTEXT_OWNER_WORLD:
		return Account::WORLD_RENDER;
	case T3D_RD_CONTEXT_OWNER_OCEAN:
		return Account::OCEAN_RENDER;
	case T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING:
		return Account::FORWARD_LIGHTING;
	default:
		return Account::GENERAL;
	}
}

#endif
