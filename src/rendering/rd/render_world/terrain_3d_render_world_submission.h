// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_SUBMISSION_H
#define TERRAIN3D_RENDER_WORLD_SUBMISSION_H

#include "rendering/rd/render_world/terrain_3d_render_world_resources.h"
#include "rendering/rd/render_world/terrain_3d_render_world_visibility.h"

#include <godot_cpp/variant/rid.hpp>

struct T3DThreeTierUploadStats {
	uint64_t nUploadBytes;
	uint32_t nUploadCallCount;
	uint32_t nDrawCount;
};

struct T3DThreeTierRasterBindings {
	godot::RID hPipeline[T3D_RW_TIER_COUNT];
	godot::RID hUniformSet[T3D_RW_TIER_COUNT];
	godot::RID hLightingSet;
	uint32_t nUniformSetIndex;
	uint32_t nLightingSetIndex;
	uint32_t nFlags;
	uint32_t nReserved;
};

struct T3DThreeTierDrawPlan {
	uint32_t nIndirectOffset[T3D_RW_TIER_COUNT];
	uint32_t nEnabled[T3D_RW_TIER_COUNT];
	uint32_t nDrawCount;
};

T3DThreeTierDrawPlan t3d_render_world_build_three_tier_draw_plan_reference_debug( const T3DVisibilityReferenceResult &pResult );

bool t3d_render_world_upload_scene_database_warm( const Terrain3DRenderWorldResourceImage &pResources,
        const T3DGeometryDatabase &pGeometry,
        const T3DSceneCell *pCells,
        uint32_t pCellCount,
        const T3DInstanceRecord *pInstances,
        uint32_t pInstanceCount,
        const T3DMaterialRecord *pMaterials,
        uint32_t pMaterialCount,
        T3DThreeTierUploadStats &rStats );

bool t3d_render_world_upload_reference_visibility_debug( const Terrain3DRenderWorldResourceImage &pResources,
        const T3DVisibilityOutputBuffers &pBuffers,
        uint32_t pInstanceCount,
        const T3DVisibilityReferenceResult &pResult,
        T3DThreeTierUploadStats &rStats );

bool t3d_render_world_submit_three_tier_indirect_hot( int64_t pDrawList,
        const Terrain3DRenderWorldResourceImage &pResources,
        const T3DThreeTierRasterBindings &pBindings,
        T3DThreeTierUploadStats &rStats );

#endif // TERRAIN3D_RENDER_WORLD_SUBMISSION_H
