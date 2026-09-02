// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RUNTIME_PROFILE_H
#define TERRAIN3D_RUNTIME_PROFILE_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

typedef enum t3dRuntimeProfileEvent_e : uint32_t {
	TERRAIN3D_PROFILE_EVENT_NONE = 0u,
	TERRAIN3D_PROFILE_EVENT_CAMERA_MOVED = 1u << 0u,
	TERRAIN3D_PROFILE_EVENT_GPU_UPLOAD = 1u << 1u,
	TERRAIN3D_PROFILE_EVENT_GPU_EVICTION = 1u << 2u,
	TERRAIN3D_PROFILE_EVENT_GPU_MISSING_REQUIRED = 1u << 3u,
	TERRAIN3D_PROFILE_EVENT_COOKED_PAGE_MISS = 1u << 4u,
	TERRAIN3D_PROFILE_EVENT_RUNTIME_PAGE_PREPARE = 1u << 5u,
	TERRAIN3D_PROFILE_EVENT_CLIPMAP_TELEPORT = 1u << 6u,
	TERRAIN3D_PROFILE_EVENT_NODE_LOAD_FAILURE = 1u << 7u,
	TERRAIN3D_PROFILE_EVENT_FAR_ATLAS_WORK = 1u << 8u,
	TERRAIN3D_PROFILE_EVENT_CPU_DEMAND_LOAD = 1u << 9u,
	TERRAIN3D_PROFILE_EVENT_INSTANCE_BACKLOG = 1u << 10u,
	TERRAIN3D_PROFILE_EVENT_SHADER_UPDATE = 1u << 11u,
	TERRAIN3D_PROFILE_EVENT_SHADER_VARIANT_CREATED = 1u << 12u,
} t3dRuntimeProfileEvent_t;

typedef enum t3dRuntimeProfileStall_e : uint32_t {
	TERRAIN3D_PROFILE_STALL_NONE = 0u,
	TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET = 1u << 0u,
	TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING = 1u << 1u,
	TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE = 1u << 2u,
	TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY = 1u << 3u,
	TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET = 1u << 4u,
	TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET = 1u << 5u,
	TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET = 1u << 6u,
	TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET = 1u << 7u,
	TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET = 1u << 8u,
	TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET = 1u << 9u,
	TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET = 1u << 10u,
	TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY = 1u << 11u,
	TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET = 1u << 12u,
	TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY = 1u << 13u,
	TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET = 1u << 14u,
} t3dRuntimeProfileStall_t;

typedef struct t3dRuntimeProfileSample_s {
	uint64_t m_nFrameSequence = 0u;
	uint64_t m_nPhysicsTotalUsec = 0u;
	uint64_t m_nDataResidencyUsec = 0u;
	uint64_t m_nHlodStreamingUsec = 0u;
	uint64_t m_nNodeStreamingUsec = 0u;
	uint64_t m_nClipmapUsec = 0u;
	uint64_t m_nOceanUsec = 0u;
	uint64_t m_nInstancerUsec = 0u;
	uint64_t m_nLightSyncUsec = 0u;
	uint64_t m_nCollisionUsec = 0u;
	uint64_t m_nStreamingUsec = 0u;
	float m_fTargetX = 0.0f;
	float m_fTargetZ = 0.0f;
	float m_fMovementX = 0.0f;
	float m_fMovementZ = 0.0f;
	int m_nTargetRegionX = 0;
	int m_nTargetRegionZ = 0;
	uint32_t m_nEventFlags = TERRAIN3D_PROFILE_EVENT_NONE;
	uint32_t m_nStallFlags = TERRAIN3D_PROFILE_STALL_NONE;
	int m_nGpuResident = 0;
	int m_nGpuCapacity = 0;
	int m_nGpuUploads = 0;
	int m_nGpuEvictions = 0;
	int m_nGpuMissingRequired = 0;
	uint64_t m_nGpuUploadBytes = 0u;
	int m_nGpuPreparedPages = 0;
	int m_nGpuPrepareWorkers = 0;
	int m_nGpuEffectiveUploadLimit = 0;
	int m_nGpuCameraMoved = 0;
	int m_nGpuRequestedPages = 0;
	int m_nGpuRequiredPages = 0;
	int m_nGpuGuardPages = 0;
	int m_nGpuPrefetchPages = 0;
	int m_nGpuHysteresisPages = 0;
	int m_nGpuPreparedQueue = 0;
	int m_nGpuPreparePendingPages = 0;
	int m_nGpuPrepareJobActive = 0;
	int m_nGpuCookedHits = 0;
	int m_nGpuCookedMisses = 0;
	int m_nGpuRuntimePrepares = 0;
	int m_nGpuFarDirtyRegions = 0;
	int m_nGpuFarRegionsProcessed = 0;
	uint64_t m_nGpuPrepareLatencyUsec = 0u;
	uint64_t m_nGpuPrepareQueueWaitUsec = 0u;
	uint64_t m_nGpuPrepareFileOpenUsec = 0u;
	uint64_t m_nGpuPrepareFileReadUsec = 0u;
	uint64_t m_nGpuPrepareValidationUsec = 0u;
	uint64_t m_nGpuPrepareMaterializeUsec = 0u;
	uint64_t m_nGpuPrepareWorkerUsec = 0u;
	uint64_t m_nGpuPrepareReadyWaitUsec = 0u;
	uint64_t m_nGpuPreparePublicationUsec = 0u;
	uint64_t m_nGpuPrepareEndToEndUsec = 0u;
	uint64_t m_nGpuCacheServiceUsec = 0u;
	int m_nClipmapNearUpdates = 0;
	int m_nClipmapMiddleUpdates = 0;
	int m_nClipmapFarUpdates = 0;
	int m_nClipmapTeleports = 0;
	int m_nClipmapBufferUploads = 0;
	int m_nCpuResident = 0;
	int m_nCpuLoads = 0;
	int m_nCpuEvictions = 0;
	uint64_t m_nCpuLoadBytes = 0u;
	uint64_t m_nCpuCacheServiceUsec = 0u;
	int m_nCpuDesired = 0;
	int m_nCpuSelectionPending = 0;
	int m_nCpuPinned = 0;
	int m_nInstanceResident = 0;
	int m_nInstanceLoads = 0;
	int m_nInstanceEvictions = 0;
	uint64_t m_nInstanceLoadBytes = 0u;
	uint64_t m_nInstanceCacheServiceUsec = 0u;
	int m_nInstanceDesired = 0;
	int m_nInstanceSelectionPending = 0;
	int m_nRenderCells = 0;
	int m_nPendingCellUpdates = 0;
	int m_nNodeActiveRegions = 0;
	int m_nNodeActiveNodes = 0;
	int m_nNodeLoads = 0;
	int m_nNodeUnloads = 0;
	int m_nNodeLoadFailures = 0;
	uint64_t m_nLightmapStreamingUsec = 0u;
	int m_nShaderUpdates = 0;
	int m_nShaderVariantCreations = 0;
	uint64_t m_nShaderSourceSubmissionUsec = 0u;
	uint64_t m_nShaderUpdateUsec = 0u;
} t3dRuntimeProfileSample_t;

typedef struct t3dRuntimeProfileHistory_s {
	t3dRuntimeProfileSample_t m_szSamples[s_nRuntimeProfileHistoryCapacity]{};
	size_t m_iWriteIndex = 0u;
	size_t m_nCount = 0u;

	EA_FORCE_INLINE void reset( void ) noexcept {
		m_iWriteIndex = 0u;
		m_nCount = 0u;
	}

	EA_FORCE_INLINE void push( const t3dRuntimeProfileSample_t &rSample ) noexcept {
		m_szSamples[m_iWriteIndex] = rSample;
		m_iWriteIndex = ( m_iWriteIndex + 1u ) % s_nRuntimeProfileHistoryCapacity;
		m_nCount = MIN( m_nCount + 1u, s_nRuntimeProfileHistoryCapacity );
	}

	EA_FORCE_INLINE const t3dRuntimeProfileSample_t &getOldest( const size_t iIndex ) const noexcept {
		const size_t oldest = ( m_iWriteIndex + s_nRuntimeProfileHistoryCapacity - m_nCount ) % s_nRuntimeProfileHistoryCapacity;
		return m_szSamples[( oldest + iIndex ) % s_nRuntimeProfileHistoryCapacity];
	}
} t3dRuntimeProfileHistory_t;

typedef struct t3dRuntimeProfileAccumulator_s {
	uint64_t m_nSampledTicks = 0u;
	uint64_t m_nPhysicsTotalUsec = 0u;
	uint64_t m_nPhysicsPeakUsec = 0u;
	uint64_t m_nDataResidencyTotalUsec = 0u;
	uint64_t m_nDataResidencyPeakUsec = 0u;
	uint64_t m_nHlodStreamingTotalUsec = 0u;
	uint64_t m_nHlodStreamingPeakUsec = 0u;
	uint64_t m_nNodeStreamingTotalUsec = 0u;
	uint64_t m_nNodeStreamingPeakUsec = 0u;
	uint64_t m_nClipmapTotalUsec = 0u;
	uint64_t m_nClipmapPeakUsec = 0u;
	uint64_t m_nOceanTotalUsec = 0u;
	uint64_t m_nOceanPeakUsec = 0u;
	uint64_t m_nInstancerTotalUsec = 0u;
	uint64_t m_nInstancerPeakUsec = 0u;
	uint64_t m_nLightSyncTotalUsec = 0u;
	uint64_t m_nLightSyncPeakUsec = 0u;
	uint64_t m_nCollisionTotalUsec = 0u;
	uint64_t m_nCollisionPeakUsec = 0u;
	uint64_t m_nStreamingTotalUsec = 0u;
	uint64_t m_nStreamingPeakUsec = 0u;
	uint64_t m_nGpuUploadsTotal = 0u;
	uint64_t m_nGpuEvictionsTotal = 0u;
	uint64_t m_nGpuUploadBytesTotal = 0u;
	uint64_t m_nGpuMissingRequiredTotal = 0u;
	int m_nGpuResidentPeak = 0;
	uint64_t m_nGpuPreparedPagesTotal = 0u;
	int m_nGpuPrepareWorkersPeak = 0;
	int m_nGpuEffectiveUploadLimitPeak = 0;
	uint64_t m_nGpuCameraMoveTicks = 0u;
	double m_fDistanceTravelledM = 0.0;
	uint64_t m_nGpuCookedHitsTotal = 0u;
	uint64_t m_nGpuCookedMissesTotal = 0u;
	uint64_t m_nGpuRuntimePreparesTotal = 0u;
	uint64_t m_nClipmapNearUpdatesTotal = 0u;
	uint64_t m_nClipmapMiddleUpdatesTotal = 0u;
	uint64_t m_nClipmapFarUpdatesTotal = 0u;
	uint64_t m_nClipmapTeleportsTotal = 0u;
	int m_nClipmapTeleportsPeak = 0;
	uint64_t m_nClipmapBufferUploadsTotal = 0u;
	int m_nClipmapBufferUploadsPeak = 0;
	uint64_t m_nCpuLoadsTotal = 0u;
	uint64_t m_nCpuEvictionsTotal = 0u;
	uint64_t m_nCpuLoadBytesTotal = 0u;
	int m_nCpuResidentPeak = 0;
	uint64_t m_nInstanceLoadsTotal = 0u;
	uint64_t m_nInstanceEvictionsTotal = 0u;
	uint64_t m_nInstanceLoadBytesTotal = 0u;
	int m_nInstanceResidentPeak = 0;
	int m_nRenderCellsPeak = 0;
	int m_nPendingCellUpdatesPeak = 0;
	int m_nNodeActiveRegionsPeak = 0;
	int m_nNodeActiveNodesPeak = 0;
	uint64_t m_nNodeLoadsTotal = 0u;
	uint64_t m_nNodeUnloadsTotal = 0u;
	uint64_t m_nNodeLoadFailuresTotal = 0u;

	void reset( void ) noexcept {
		*this = t3dRuntimeProfileAccumulator_s{};
	}

	void addSample( const t3dRuntimeProfileSample_t &rSample ) noexcept {
		++m_nSampledTicks;
		m_nPhysicsTotalUsec += rSample.m_nPhysicsTotalUsec;
		m_nPhysicsPeakUsec = MAX( m_nPhysicsPeakUsec, rSample.m_nPhysicsTotalUsec );
		m_nDataResidencyTotalUsec += rSample.m_nDataResidencyUsec;
		m_nDataResidencyPeakUsec = MAX( m_nDataResidencyPeakUsec, rSample.m_nDataResidencyUsec );
		m_nHlodStreamingTotalUsec += rSample.m_nHlodStreamingUsec;
		m_nHlodStreamingPeakUsec = MAX( m_nHlodStreamingPeakUsec, rSample.m_nHlodStreamingUsec );
		m_nNodeStreamingTotalUsec += rSample.m_nNodeStreamingUsec;
		m_nNodeStreamingPeakUsec = MAX( m_nNodeStreamingPeakUsec, rSample.m_nNodeStreamingUsec );
		m_nClipmapTotalUsec += rSample.m_nClipmapUsec;
		m_nClipmapPeakUsec = MAX( m_nClipmapPeakUsec, rSample.m_nClipmapUsec );
		m_nOceanTotalUsec += rSample.m_nOceanUsec;
		m_nOceanPeakUsec = MAX( m_nOceanPeakUsec, rSample.m_nOceanUsec );
		m_nInstancerTotalUsec += rSample.m_nInstancerUsec;
		m_nInstancerPeakUsec = MAX( m_nInstancerPeakUsec, rSample.m_nInstancerUsec );
		m_nLightSyncTotalUsec += rSample.m_nLightSyncUsec;
		m_nLightSyncPeakUsec = MAX( m_nLightSyncPeakUsec, rSample.m_nLightSyncUsec );
		m_nCollisionTotalUsec += rSample.m_nCollisionUsec;
		m_nCollisionPeakUsec = MAX( m_nCollisionPeakUsec, rSample.m_nCollisionUsec );
		m_nStreamingTotalUsec += rSample.m_nStreamingUsec;
		m_nStreamingPeakUsec = MAX( m_nStreamingPeakUsec, rSample.m_nStreamingUsec );
		m_nGpuUploadsTotal += ( uint64_t )( MAX( rSample.m_nGpuUploads, 0 ) );
		m_nGpuEvictionsTotal += ( uint64_t )( MAX( rSample.m_nGpuEvictions, 0 ) );
		m_nGpuUploadBytesTotal += rSample.m_nGpuUploadBytes;
		m_nGpuMissingRequiredTotal += ( uint64_t )( MAX( rSample.m_nGpuMissingRequired, 0 ) );
		m_nGpuResidentPeak = MAX( m_nGpuResidentPeak, rSample.m_nGpuResident );
		m_nGpuPreparedPagesTotal += ( uint64_t )( MAX( rSample.m_nGpuPreparedPages, 0 ) );
		m_nGpuPrepareWorkersPeak = MAX( m_nGpuPrepareWorkersPeak, rSample.m_nGpuPrepareWorkers );
		m_nGpuEffectiveUploadLimitPeak = MAX( m_nGpuEffectiveUploadLimitPeak, rSample.m_nGpuEffectiveUploadLimit );
		m_nGpuCameraMoveTicks += rSample.m_nGpuCameraMoved != 0 ? 1u : 0u;
		m_fDistanceTravelledM += sqrt( ( double )( rSample.m_fMovementX ) * ( double )( rSample.m_fMovementX )
		        + ( double )( rSample.m_fMovementZ ) * ( double )( rSample.m_fMovementZ ) );
		m_nGpuCookedHitsTotal += ( uint64_t )( MAX( rSample.m_nGpuCookedHits, 0 ) );
		m_nGpuCookedMissesTotal += ( uint64_t )( MAX( rSample.m_nGpuCookedMisses, 0 ) );
		m_nGpuRuntimePreparesTotal += ( uint64_t )( MAX( rSample.m_nGpuRuntimePrepares, 0 ) );
		m_nClipmapNearUpdatesTotal += ( uint64_t )( MAX( rSample.m_nClipmapNearUpdates, 0 ) );
		m_nClipmapMiddleUpdatesTotal += ( uint64_t )( MAX( rSample.m_nClipmapMiddleUpdates, 0 ) );
		m_nClipmapFarUpdatesTotal += ( uint64_t )( MAX( rSample.m_nClipmapFarUpdates, 0 ) );
		m_nClipmapTeleportsTotal += ( uint64_t )( MAX( rSample.m_nClipmapTeleports, 0 ) );
		m_nClipmapTeleportsPeak = MAX( m_nClipmapTeleportsPeak, rSample.m_nClipmapTeleports );
		m_nClipmapBufferUploadsTotal += ( uint64_t )( MAX( rSample.m_nClipmapBufferUploads, 0 ) );
		m_nClipmapBufferUploadsPeak = MAX( m_nClipmapBufferUploadsPeak, rSample.m_nClipmapBufferUploads );
		m_nCpuLoadsTotal += ( uint64_t )( MAX( rSample.m_nCpuLoads, 0 ) );
		m_nCpuEvictionsTotal += ( uint64_t )( MAX( rSample.m_nCpuEvictions, 0 ) );
		m_nCpuLoadBytesTotal += rSample.m_nCpuLoadBytes;
		m_nCpuResidentPeak = MAX( m_nCpuResidentPeak, rSample.m_nCpuResident );
		m_nInstanceLoadsTotal += ( uint64_t )( MAX( rSample.m_nInstanceLoads, 0 ) );
		m_nInstanceEvictionsTotal += ( uint64_t )( MAX( rSample.m_nInstanceEvictions, 0 ) );
		m_nInstanceLoadBytesTotal += rSample.m_nInstanceLoadBytes;
		m_nInstanceResidentPeak = MAX( m_nInstanceResidentPeak, rSample.m_nInstanceResident );
		m_nRenderCellsPeak = MAX( m_nRenderCellsPeak, rSample.m_nRenderCells );
		m_nPendingCellUpdatesPeak = MAX( m_nPendingCellUpdatesPeak, rSample.m_nPendingCellUpdates );
		m_nNodeActiveRegionsPeak = MAX( m_nNodeActiveRegionsPeak, rSample.m_nNodeActiveRegions );
		m_nNodeActiveNodesPeak = MAX( m_nNodeActiveNodesPeak, rSample.m_nNodeActiveNodes );
		m_nNodeLoadsTotal += ( uint64_t )( MAX( rSample.m_nNodeLoads, 0 ) );
		m_nNodeUnloadsTotal += ( uint64_t )( MAX( rSample.m_nNodeUnloads, 0 ) );
		m_nNodeLoadFailuresTotal += ( uint64_t )( MAX( rSample.m_nNodeLoadFailures, 0 ) );
	}
} t3dRuntimeProfileAccumulator_t;
#endif // TERRAIN3D_RUNTIME_PROFILE_H
