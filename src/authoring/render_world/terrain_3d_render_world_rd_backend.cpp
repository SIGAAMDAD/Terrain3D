// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/render_world/terrain_3d_render_world_rd_backend.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"

#include <string.h>

static bool t3dRwPublishConvertedSceneCold(
        void *const pUser, const T3DRenderWorldSceneBuildBuffers &rBuffers, const T3DRenderWorldSceneBuildStats &rStats ) {
	( void )( rStats );
	t3dRenderWorldRdAuthoringBackendContext_t *const pContext = ( t3dRenderWorldRdAuthoringBackendContext_t * )( pUser );
	if ( !pContext || !rBuffers.pGeometry ) {
		return false;
	}

	T3DThreeTierUploadStats stats{};
	bool published = false;
	if ( pContext->m_pCoordinator ) {
		published = pContext->m_pCoordinator->publishRenderWorldDatabaseCold( *rBuffers.pGeometry,
		        rBuffers.pCells,
		        rBuffers.nCellCount,
		        rBuffers.pInstances,
		        rBuffers.nInstanceCount,
		        rBuffers.pMaterials,
		        rBuffers.nMaterialCount );
	} else if ( pContext->m_pResources && pContext->m_pRenderWorld && Terrain3DRdContext::isOnRenderThread() ) {
		pContext->m_pRenderWorld->set_submission_enabled_cold( false );
		published = t3d_render_world_upload_scene_database_warm( *pContext->m_pResources,
		        *rBuffers.pGeometry,
		        rBuffers.pCells,
		        rBuffers.nCellCount,
		        rBuffers.pInstances,
		        rBuffers.nInstanceCount,
		        rBuffers.pMaterials,
		        rBuffers.nMaterialCount,
		        stats );
		if ( published ) {
			pContext->m_pRenderWorld->set_submission_enabled_cold( true );
		}
	}
	if ( !published ) {
		return false;
	}

	pContext->m_lastUploadStats = stats;
	pContext->m_nPublishedCellCount = rBuffers.nCellCount;
	pContext->m_nPublishedInstanceCount = rBuffers.nInstanceCount;
	pContext->m_nPublishedMeshCount = rBuffers.pGeometry->nMeshCount;
	pContext->m_nPublishedLodCount = rBuffers.pGeometry->nLodCount;
	pContext->m_nPublishedClusterCount = rBuffers.pGeometry->nClusterCount;
	pContext->m_nPublishedMaterialCount = rBuffers.nMaterialCount;
	pContext->m_nPublicationGeneration = pContext->m_nPublicationGeneration == UINT64_MAX ? 1u : pContext->m_nPublicationGeneration + 1u;
	pContext->m_bScenePublished = true;
	return true;
}

static void t3dRwReleaseConvertedSceneCold( void *const pUser ) {
	t3dRenderWorldRdAuthoringBackendContext_t *const pContext = ( t3dRenderWorldRdAuthoringBackendContext_t * )( pUser );
	if ( !pContext ) {
		return;
	}
	if ( pContext->m_pCoordinator ) {
		pContext->m_pCoordinator->releaseRenderWorldDatabaseCold();
	} else if ( pContext->m_pRenderWorld ) {
		pContext->m_pRenderWorld->set_submission_enabled_cold( false );
	}
	pContext->m_bScenePublished = false;
	pContext->m_nPublishedCellCount = 0u;
	pContext->m_nPublishedInstanceCount = 0u;
	pContext->m_nPublishedMeshCount = 0u;
	pContext->m_nPublishedLodCount = 0u;
	pContext->m_nPublishedClusterCount = 0u;
	pContext->m_nPublishedMaterialCount = 0u;
}

bool t3dRenderWorldRdAuthoringBackendInitializeCold( t3dRenderWorldRdAuthoringBackendContext_t &rContext,
        const Terrain3DRenderWorldResourceImage *const pResources,
        Terrain3DRenderWorld *const pRenderWorld ) {
	rContext = t3dRenderWorldRdAuthoringBackendContext_t{};
	if ( !pResources || !pResources->is_initialized() || !pRenderWorld ) {
		return false;
	}

	Terrain3DRdContext *const pRdContext = pResources->get_rd_context();
	if ( !pRdContext || !pRdContext->getDevice() ) {
		return false;
	}
	rContext.m_pResources = pResources;
	rContext.m_pRenderWorld = pRenderWorld;
	rContext.m_nPublicationGeneration = 1u;
	pRenderWorld->set_submission_enabled_cold( false );
	return true;
}

bool t3dRenderWorldRdAuthoringBackendInitializeCoordinatorCold(
        t3dRenderWorldRdAuthoringBackendContext_t &rContext, Terrain3DRenderCoordinator *const pCoordinator ) {
	rContext = t3dRenderWorldRdAuthoringBackendContext_t{};
	if ( !pCoordinator ) {
		return false;
	}
	rContext.m_pCoordinator = pCoordinator;
	rContext.m_nPublicationGeneration = 1u;
	return true;
}

T3DRenderWorldAuthoringBackend t3dRenderWorldRdAuthoringBackendMake( t3dRenderWorldRdAuthoringBackendContext_t &rContext ) {
	T3DRenderWorldAuthoringBackend backend;
	backend.pPublishSceneCold = &t3dRwPublishConvertedSceneCold;
	backend.pReleaseSceneCold = &t3dRwReleaseConvertedSceneCold;
	backend.pUser = &rContext;
	return backend;
}
