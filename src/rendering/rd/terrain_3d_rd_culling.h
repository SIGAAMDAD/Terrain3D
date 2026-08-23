// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_CULLING_H
#define TERRAIN3D_RD_CULLING_H

#include "rendering/rd/terrain_3d_rd_types.h"

#include <cstddef>

typedef struct t3dRdCpuCullResult_s {
	uint32_t m_szVisibleIndices[s_nRdDrawGroupCount * s_nMaxClipmapLevels]{};
	t3dRdIndexedIndirectCommand_t m_szCommands[s_nRdDrawGroupCount]{};
	uint32_t m_nVisibleCount = 0u;
	uint32_t m_nInvalidPatchCount = 0u;
} t3dRdCpuCullResult_t;

class Terrain3DRdCpuCuller {
public:
	static uint32_t getDrawGroup( const t3dRdPatch_t &rPatch );
	static bool isPatchValid( const t3dRdPatch_t &rPatch );
	static bool isPatchVisible( const t3dRdPatch_t &rPatch, const Projection &rViewProjection );
	static bool isCommandRangeValid( uint32_t nGroup,
			uint32_t nBufferBytes,
			uint32_t nStride = sizeof( t3dRdIndexedIndirectCommand_t ),
			uint32_t nDrawCount = 1u );
	static bool build( const t3dRdFrameSnapshot_t &rSnapshot,
			const Projection &rViewProjection,
			uint32_t nLod0IndexCount,
			uint32_t nOuterIndexCount,
			t3dRdCpuCullResult_t &rResult );
	static bool buildMultiview( const t3dRdFrameSnapshot_t &rSnapshot, const Projection *pViewProjections, uint32_t nViewCount,
			uint32_t nLod0IndexCount, uint32_t nOuterIndexCount, t3dRdCpuCullResult_t &rResult );
	static bool validate( const t3dRdFrameSnapshot_t &rSnapshot,
			const Projection &rViewProjection,
			uint32_t nLod0IndexCount,
			uint32_t nOuterIndexCount,
			const t3dRdCpuCullResult_t &rResult );
};

#endif // TERRAIN3D_RD_CULLING_H
