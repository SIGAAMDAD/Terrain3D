// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_COLLISION_SURFACE_BUILDER_H
#define TERRAIN3D_COLLISION_SURFACE_BUILDER_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_fixed_buffer.h"
#include "eastl_config.h"
#include "memory/terrain_3d_memory_arena.h"

#include <godot_cpp/variant/packed_float32_array.hpp>

class Terrain3DData;
class Terrain3DMaterial;

// Canonical final collision-surface generator shared by live collision and
// runtime-page cooking. Output is already rotated into HeightMapShape3D layout.
class Terrain3DCollisionSurfaceBuilder {
public:
	bool prepareRuntimeScratch( int nAxisSamples );
	bool build( const Terrain3DData *pData,
	        const Ref<Terrain3DMaterial> &rMaterial,
	        const Vector2i &rPosition,
	        int nSize,
	        PackedFloat32Array &rHeights,
	        float &rMinHeight,
	        float &rMaxHeight );
	size_t getRetainedBufferBytes( void ) const;

private:
	float m_dynamicHeightScratch[s_nDynamicScratchSamples]{};
	Terrain3DMemoryArena m_fullHeightScratchArena;
	Terrain3DFixedBuffer<float> m_fullHeightRowScratch;
	uint8_t m_regionPresenceScratch[s_nRegionPresenceSamples]{};
	int32_t m_szBlendRegionX0[s_nMaxCollisionAxisSamples]{};
	int32_t m_szBlendRegionX1[s_nMaxCollisionAxisSamples]{};
	int32_t m_szBlendRegionZ0[s_nMaxCollisionAxisSamples]{};
	int32_t m_szBlendRegionZ1[s_nMaxCollisionAxisSamples]{};
	real_t m_fBlendWeightX[s_nMaxCollisionAxisSamples]{};
	real_t m_fBlendWeightZ[s_nMaxCollisionAxisSamples]{};
	real_t m_fRegionBlendCurve[s_nMaxRegionBlendSamples]{};
	real_t m_fRegionBlendCurveSetting = -1.0f;

	void refreshRegionBlendCurve( real_t fRegionBlend );
	real_t getRegionBlendWeight( int64_t nGridCoordinate ) const;
	real_t getRegionBlend( int iX, int iZ, int iMinRegionX, int iMinRegionZ, int nPresenceWidth ) const;
};

#endif // TERRAIN3D_COLLISION_SURFACE_BUILDER_H
