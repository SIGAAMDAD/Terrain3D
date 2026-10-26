// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_CULLING_H
#define TERRAIN3D_RD_CULLING_H

#include "Terrain3DTypes.h"

#include "rendering/rd/core/terrain_3d_rd_types.h"

#include <stddef.h>
#include <string.h>

// t3dRdCpuCullResult_s centralized in Terrain3DTypes.h.


EA_FORCE_INLINE void t3dRdCpuCullResultReset( t3dRdCpuCullResult_t *pResult ) {
	memset( pResult, 0, sizeof( *pResult ) );
}

class Terrain3DRdCpuCuller {
public:
	static uint32_t getDrawGroup( const t3dRdPatch_t &rPatch );
	static bool isPatchValid( const t3dRdPatch_t &rPatch );
	static bool isPatchVisible( const t3dRdPatch_t &rPatch, const Projection &rViewProjection );
	static bool isCommandRangeValid(
	        uint32_t nGroup, uint32_t nBufferBytes, uint32_t nStride = sizeof( t3dRdIndexedIndirectCommand_t ), uint32_t nDrawCount = 1u );
	static bool build( const t3dRdFrameSnapshot_t &rSnapshot,
	        const Projection &rViewProjection,
	        uint32_t nLod0PartCount,
	        uint32_t nOuterPartCount,
	        t3dRdCpuCullResult_t &rResult );
	static bool buildMultiview( const t3dRdFrameSnapshot_t &rSnapshot,
	        const Projection *pViewProjections,
	        uint32_t nViewCount,
	        uint32_t nLod0PartCount,
	        uint32_t nOuterPartCount,
	        t3dRdCpuCullResult_t &rResult );
	static bool validate( const t3dRdFrameSnapshot_t &rSnapshot,
	        const Projection &rViewProjection,
	        uint32_t nLod0PartCount,
	        uint32_t nOuterPartCount,
	        const t3dRdCpuCullResult_t &rResult );
};

#endif // TERRAIN3D_RD_CULLING_H
