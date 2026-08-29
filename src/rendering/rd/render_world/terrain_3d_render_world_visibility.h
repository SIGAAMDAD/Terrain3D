// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_VISIBILITY_H
#define TERRAIN3D_RENDER_WORLD_VISIBILITY_H

#include "rendering/rd/render_world/terrain_3d_render_world_geometry.h"

#include <stdint.h>

static const uint32_t T3D_RW_VISIBILITY_FLAG_FRUSTUM = 1u << 0u;
static const uint32_t T3D_RW_VISIBILITY_FLAG_DISTANCE = 1u << 1u;
static const uint32_t T3D_RW_VISIBILITY_FLAG_CONE = 1u << 2u;
static const uint32_t T3D_RW_VISIBILITY_FLAG_HIZ = 1u << 3u;
static const uint32_t T3D_RW_VISIBILITY_FLAG_CAMERA_CUT = 1u << 4u;
static const uint32_t T3D_RW_VISIBILITY_FLAG_REFERENCE_NO_OCCLUSION = 1u << 5u;
static const uint32_t T3D_RW_INSTANCE_FLAG_CONE_SAFE = 1u << 0u;

static const uint32_t T3D_RW_VISIBILITY_OVERFLOW_CELLS = 1u << 0u;
static const uint32_t T3D_RW_VISIBILITY_OVERFLOW_INSTANCES = 1u << 1u;
static const uint32_t T3D_RW_VISIBILITY_OVERFLOW_WORLD = 1u << 2u;
static const uint32_t T3D_RW_VISIBILITY_OVERFLOW_CLUTTER = 1u << 3u;
static const uint32_t T3D_RW_VISIBILITY_OVERFLOW_FOLIAGE = 1u << 4u;

typedef bool ( *T3DVisibilityOcclusionReferenceFn )( const void *pContext,
        const T3DVisibilityFrame &pFrame,
        const float pCenter[3],
        float pRadius,
        uint32_t pClusterId,
        uint32_t pInstanceId );

struct T3DVisibilityScene {
	const T3DSceneCell *pCells;
	uint32_t nCellCount;
	const T3DInstanceRecord *pInstances;
	uint32_t nInstanceCount;
	const T3DGeometryDatabase *pGeometry;
};

struct T3DVisibilityOutputBuffers {
	uint32_t *pVisibleCells;
	uint32_t nVisibleCellCapacity;
	uint32_t *pVisibleInstances;
	uint32_t nVisibleInstanceCapacity;
	uint32_t *pSelectedLodByInstance;
	uint32_t nSelectedLodCapacity;
	T3DVisibleCluster *pVisibleClusters[T3D_RW_TIER_COUNT];
	uint32_t nVisibleClusterCapacity[T3D_RW_TIER_COUNT];
};

struct T3DVisibilityReferenceConfig {
	T3DVisibilityOcclusionReferenceFn pOcclusionTest;
	const void *pOcclusionContext;
};

struct T3DVisibilityReferenceResult {
	T3DVisibilityCounters Counters;
	T3DDrawIndirectArgs DrawArgs[T3D_RW_TIER_COUNT];
};

bool t3d_render_world_visibility_build_reference_debug( const T3DVisibilityScene &pScene,
        const T3DVisibilityFrame &pFrame,
        const T3DVisibilityReferenceConfig &pConfig,
        const T3DVisibilityOutputBuffers &pBuffers,
        T3DVisibilityReferenceResult &rResult );

bool t3d_render_world_visibility_validate_reference_debug(
        const T3DVisibilityScene &pScene, const T3DVisibilityOutputBuffers &pBuffers, const T3DVisibilityReferenceResult &pResult );

uint32_t t3d_render_world_visibility_select_lod_reference_debug( const T3DGeometryDatabase &pGeometry, uint32_t pMeshId, float pDistance );

#endif // TERRAIN3D_RENDER_WORLD_VISIBILITY_H
