// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_RD_BACKEND_H
#define TERRAIN3D_RENDER_WORLD_RD_BACKEND_H

#include "authoring/render_world/terrain_3d_render_world_controller.h"
#include "rendering/rd/render_world/terrain_3d_render_world.h"
#include "rendering/rd/render_world/terrain_3d_render_world_submission.h"

class Terrain3DRenderCoordinator;

typedef struct t3dRenderWorldRdAuthoringBackendContext_s {
	Terrain3DRenderCoordinator *m_pCoordinator = NULL;
	const Terrain3DRenderWorldResourceImage *m_pResources = NULL;
	Terrain3DRenderWorld *m_pRenderWorld = NULL;
	T3DThreeTierUploadStats m_lastUploadStats{};
	uint64_t m_nPublicationGeneration = 0u;
	uint32_t m_nPublishedCellCount = 0u;
	uint32_t m_nPublishedInstanceCount = 0u;
	uint32_t m_nPublishedMeshCount = 0u;
	uint32_t m_nPublishedLodCount = 0u;
	uint32_t m_nPublishedClusterCount = 0u;
	uint32_t m_nPublishedMaterialCount = 0u;
	bool m_bScenePublished = false;
} t3dRenderWorldRdAuthoringBackendContext_t;

bool t3dRenderWorldRdAuthoringBackendInitializeCold( t3dRenderWorldRdAuthoringBackendContext_t &rContext,
        const Terrain3DRenderWorldResourceImage *pResources,
        Terrain3DRenderWorld *pRenderWorld );

bool t3dRenderWorldRdAuthoringBackendInitializeCoordinatorCold(
        t3dRenderWorldRdAuthoringBackendContext_t &rContext, Terrain3DRenderCoordinator *pCoordinator );

T3DRenderWorldAuthoringBackend t3dRenderWorldRdAuthoringBackendMake( t3dRenderWorldRdAuthoringBackendContext_t &rContext );

#endif // TERRAIN3D_RENDER_WORLD_RD_BACKEND_H
