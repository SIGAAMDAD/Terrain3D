// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_COLLISION_SURFACE_BUILDER_H
#define TERRAIN3D_COLLISION_SURFACE_BUILDER_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"

#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <godot_cpp/variant/packed_float32_array.hpp>

class Terrain3DData;
class Terrain3DMaterial;

// Canonical final collision-surface generator shared by live collision and
// runtime-page cooking. Output is already rotated into HeightMapShape3D layout.
class T3DCollisionSurfaceBuilder {
public:
	bool build(
		const Terrain3DData *pData, const Ref<Terrain3DMaterial> &rMaterial,
		const Vector2i &rPosition, int nSize,
		PackedFloat32Array &rHeights, float &rMinHeight, float &rMaxHeight
	);
	size_t getRetainedBufferBytes( void ) const;

private:
	eastl::vector<float> m_heightRowScratch;
	eastl::vector<uint8_t> m_regionPresenceScratch;
	eastl::array<int32_t, TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES> m_szBlendRegionX0{};
	eastl::array<int32_t, TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES> m_szBlendRegionX1{};
	eastl::array<int32_t, TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES> m_szBlendRegionZ0{};
	eastl::array<int32_t, TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES> m_szBlendRegionZ1{};
	eastl::array<real_t, TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES> m_fBlendWeightX{};
	eastl::array<real_t, TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES> m_fBlendWeightZ{};
	eastl::array<real_t, TERRAIN3D_MAX_REGION_BLEND_SAMPLES> m_fRegionBlendCurve{};
	int m_nRegionBlendCurveSize = 0;
	real_t m_fRegionBlendCurveSetting = -1.0f;

	void refreshRegionBlendCurve( int nRegionSize, real_t fRegionBlend );
	real_t getRegionBlendWeight( int64_t nGridCoordinate ) const;
	real_t getRegionBlend( int iX, int iZ, int iMinRegionX, int iMinRegionZ, int nPresenceWidth ) const;
};

#endif // TERRAIN3D_COLLISION_SURFACE_BUILDER_H
