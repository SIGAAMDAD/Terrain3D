// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_INSTANCE_PUBLICATION_BUILDER_H
#define TERRAIN3D_INSTANCE_PUBLICATION_BUILDER_H

#include "Terrain3DConstants.h"

#include <stdint.h>

class Terrain3D;
class Terrain3DHLODStreamer;
struct t3dRdInstanceSceneSnapshot_s;

typedef struct t3dInstancePublicationCategoryDiagnostics_s {
	uint64_t m_nPublishedLod0Cells = 0u;
	uint64_t m_nPublishedLod1Cells = 0u;
	uint64_t m_nPublishedHigherLodCells = 0u;
	uint64_t m_nLodFallbacks = 0u;
	uint64_t m_nLodMissingSourceSkips = 0u;
	uint64_t m_nVisibilityLayerFilteredCells = 0u;
	uint64_t m_nFadeActiveCells = 0u;
	uint64_t m_nHlodEligibleSources = 0u;
	uint64_t m_nHlodActiveReplacements = 0u;
	uint64_t m_nHlodDetailSuppressedCells = 0u;
	uint64_t m_nHlodDetailRetainedMissingProxy = 0u;
	uint64_t m_nHlodTransitionOverlapCells = 0u;
	uint64_t m_nHlodTransitionGapErrors = 0u;
} t3dInstancePublicationCategoryDiagnostics_t;

typedef struct t3dInstancePublicationDiagnostics_s {
	t3dInstancePublicationCategoryDiagnostics_t m_categories[3]{};
	uint64_t m_nResidentSourceRecords = 0u;
	uint64_t m_nPublishedRecords = 0u;
	uint64_t m_nPublishedCells = 0u;
	uint64_t m_nRenderableAssetSourceRecords = 0u;
	uint64_t m_nSkippedMissingAssetRecords = 0u;
	uint64_t m_nInvalidAssetIdRecords = 0u;
	uint64_t m_nMissingAssetRecords = 0u;
	uint64_t m_nDisabledAssetRecords = 0u;
	uint64_t m_nNoLodAssetRecords = 0u;
	uint64_t m_nAssetRegistryDeferredBuilds = 0u;
	int32_t m_nFirstInvalidAssetId = -1;
	int32_t m_nFirstMissingAssetId = -1;
	int32_t m_nFirstDisabledAssetId = -1;
	int32_t m_nFirstNoLodAssetId = -1;
	uint64_t m_nRegionResidencyFilteredCells = 0u;
	uint64_t m_nPackedRecordsReused = 0u;
	uint64_t m_nPackedRecordsRebuilt = 0u;
	uint64_t m_nBoundsReused = 0u;
	uint64_t m_nBoundsRebuilt = 0u;
	uint64_t m_nPolicyOnlyCellUpdates = 0u;
	uint64_t m_nDirtyRecordRanges = 0u;
	uint64_t m_nInstanceLayoutChanges = 0u;

	void reset( void );
} t3dInstancePublicationDiagnostics_t;

typedef struct t3dInstancePublicationBuildContext_s {
	Terrain3D *m_pTerrain = NULL;
	const Terrain3DHLODStreamer *m_pHlodStreamer = NULL;
	Vector3 m_targetPosition;
	int m_nCellSize = s_nInstanceDefaultCellSize;
	uint64_t m_nGeneration = 1u;
	const uint32_t *m_pMeshSettingsRevision = NULL;
	bool m_bRespectRegionResidency = false;
} t3dInstancePublicationBuildContext_t;

class Terrain3DInstancePublicationBuilder {
public:
	static bool build( const t3dInstancePublicationBuildContext_t &rContext,
	        t3dRdInstanceSceneSnapshot_s &rSnapshot,
	        t3dInstancePublicationDiagnostics_t &rDiagnostics );
};

#endif // TERRAIN3D_INSTANCE_PUBLICATION_BUILDER_H
