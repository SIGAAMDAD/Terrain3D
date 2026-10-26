// Public resource and lifecycle value contracts for the RD context.
#ifndef TERRAIN3D_RD_CONTEXT_CONTRACT_H
#define TERRAIN3D_RD_CONTEXT_CONTRACT_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"
#include <stdint.h>
namespace t3d_rd_context_contract
{
static constexpr uint32_t fixedIndirectOffset(
	const t3dRdFixedIndirectSlot_t slot ) {
	return slot == T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD	 ? 0u
		   : slot == T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN ? 128u
														 : 1680u;
}

static constexpr uint32_t fixedIndirectCapacity(
	const t3dRdFixedIndirectSlot_t slot ) {
	return slot == T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD	 ? 128u
		   : slot == T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN ? 1536u
														 : 1616u;
}
}

#endif // TERRAIN3D_RD_CONTEXT_CONTRACT_H
