// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_INSTANCE_PUBLICATION_BUILDER_H
#define TERRAIN3D_INSTANCE_PUBLICATION_BUILDER_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"

#include <stdint.h>
#include "hlod/runtime/terrain_3d_hlod_streamer.h"

typedef struct t3dInstancePublicationDiagnostics_s
{
	t3dInstancePublicationCategoryDiagnostics_t m_categories[3]{};
	uint64_t									m_nResidentSourceRecords = 0u;
	uint64_t									m_nPublishedRecords = 0u;
	uint64_t									m_nPublishedCells = 0u;
	uint64_t									m_nRenderableAssetSourceRecords = 0u;
	uint64_t									m_nSkippedMissingAssetRecords = 0u;
	uint64_t									m_nInvalidAssetIdRecords = 0u;
	uint64_t									m_nMissingAssetRecords = 0u;
	uint64_t									m_nDisabledAssetRecords = 0u;
	uint64_t									m_nNoLodAssetRecords = 0u;
	uint64_t									m_nAssetRegistryDeferredBuilds = 0u;
	int32_t										m_nFirstInvalidAssetId = -1;
	int32_t										m_nFirstMissingAssetId = -1;
	int32_t										m_nFirstDisabledAssetId = -1;
	int32_t										m_nFirstNoLodAssetId = -1;
	uint64_t									m_nRegionResidencyFilteredCells = 0u;
	uint64_t									m_nPackedRecordsReused = 0u;
	uint64_t									m_nPackedRecordsRebuilt = 0u;
	uint64_t									m_nBoundsReused = 0u;
	uint64_t									m_nBoundsRebuilt = 0u;
	uint64_t									m_nPolicyOnlyCellUpdates = 0u;
	uint64_t									m_nDirtyRecordRanges = 0u;
	uint64_t									m_nInstanceLayoutChanges = 0u;

	void reset( void );
} t3dInstancePublicationDiagnostics_t;

typedef struct t3dInstancePublicationBuildContext_s
{
	Terrain3DOwner				*m_pTerrain = NULL;
	const Terrain3DHLODStreamer *m_pHlodStreamer = NULL;
	Vector3						 m_targetPosition;
	int							 m_nCellSize = T3D_INSTANCE_DEFAULT_CELL_SIZE;
	uint64_t					 m_nGeneration = 1u;
	const uint32_t				*m_pMeshSettingsRevision = NULL;
	bool						 m_bRespectRegionResidency = false;
} t3dInstancePublicationBuildContext_t;

class Terrain3DInstancePublicationBuilder
{
public:
	static bool build( const t3dInstancePublicationBuildContext_t &rContext,
					   t3dRdInstanceSceneSnapshot_s				  &rSnapshot,
					   t3dInstancePublicationDiagnostics_t		  &rDiagnostics );
};

#endif // TERRAIN3D_INSTANCE_PUBLICATION_BUILDER_H
