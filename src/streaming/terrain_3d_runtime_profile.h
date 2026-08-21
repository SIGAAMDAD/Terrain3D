// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RUNTIME_PROFILE_H
#define TERRAIN3D_RUNTIME_PROFILE_H

#include "Terrain3DConstants.h"

#include <algorithm>
#include "eastl_config.h"
#include <EASTL/array.h>
#include <cmath>
#include <cstddef>
#include <cstdint>



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
	uint64_t frameSequence = 0u;
	uint64_t nPhysicsTotalUsec = 0u;
	uint64_t nDataResidencyUsec = 0u;
	uint64_t nHlodStreamingUsec = 0u;
	uint64_t nNodeStreamingUsec = 0u;
	uint64_t nClipmapUsec = 0u;
	uint64_t nOceanUsec = 0u;
	uint64_t nInstancerUsec = 0u;
	uint64_t nLightSyncUsec = 0u;
	uint64_t nCollisionUsec = 0u;
	uint64_t nStreamingUsec = 0u;
	float fTargetX = 0.0f;
	float fTargetZ = 0.0f;
	float fMovementX = 0.0f;
	float fMovementZ = 0.0f;
	int targetRegionX = 0;
	int targetRegionZ = 0;
	uint32_t eventFlags = TERRAIN3D_PROFILE_EVENT_NONE;
	uint32_t stallFlags = TERRAIN3D_PROFILE_STALL_NONE;
	int gpuResident = 0;
	int nGpuCapacity = 0;
	int nGpuUploads = 0;
	int nGpuEvictions = 0;
	int gpuMissingRequired = 0;
	uint64_t nGpuUploadBytes = 0u;
	int nGpuPreparedPages = 0;
	int nGpuPrepareWorkers = 0;
	int gpuEffectiveUploadLimit = 0;
	int gpuCameraMoved = 0;
	int nGpuRequestedPages = 0;
	int nGpuRequiredPages = 0;
	int nGpuGuardPages = 0;
	int nGpuPrefetchPages = 0;
	int nGpuHysteresisPages = 0;
	int gpuPreparedQueue = 0;
	int nGpuPreparePendingPages = 0;
	int gpuPrepareJobActive = 0;
	int nGpuCookedHits = 0;
	int nGpuCookedMisses = 0;
	int gpuRuntimePrepares = 0;
	int nGpuFarDirtyRegions = 0;
	int gpuFarRegionsProcessed = 0;
	uint64_t nGpuPrepareLatencyUsec = 0u;
	uint64_t nGpuPrepareQueueWaitUsec = 0u;
	uint64_t nGpuPrepareFileOpenUsec = 0u;
	uint64_t nGpuPrepareFileReadUsec = 0u;
	uint64_t nGpuPrepareValidationUsec = 0u;
	uint64_t nGpuPrepareMaterializeUsec = 0u;
	uint64_t nGpuPrepareWorkerUsec = 0u;
	uint64_t nGpuPrepareReadyWaitUsec = 0u;
	uint64_t nGpuPreparePublicationUsec = 0u;
	uint64_t nGpuPrepareEndToEndUsec = 0u;
	uint64_t nGpuCacheServiceUsec = 0u;
	int clipmapNearUpdates = 0;
	int clipmapMiddleUpdates = 0;
	int clipmapFarUpdates = 0;
	int clipmapTeleports = 0;
	int nClipmapBufferUploads = 0;
	int cpuResident = 0;
	int cpuLoads = 0;
	int nCpuEvictions = 0;
	uint64_t nCpuLoadBytes = 0u;
	uint64_t nCpuCacheServiceUsec = 0u;
	int cpuDesired = 0;
	int cpuSelectionPending = 0;
	int cpuPinned = 0;
	int instanceResident = 0;
	int instanceLoads = 0;
	int nInstanceEvictions = 0;
	uint64_t nInstanceLoadBytes = 0u;
	uint64_t nInstanceCacheServiceUsec = 0u;
	int instanceDesired = 0;
	int instanceSelectionPending = 0;
	int renderCells = 0;
	int pendingCellUpdates = 0;
	int nNodeActiveRegions = 0;
	int nodeActiveNodes = 0;
	int nodeLoads = 0;
	int nodeUnloads = 0;
	int nNodeLoadFailures = 0;
	uint64_t nLightmapStreamingUsec = 0u;
	int shaderUpdates = 0;
	int shaderVariantCreations = 0;
	uint64_t nShaderSourceSubmissionUsec = 0u;
	uint64_t nShaderUpdateUsec = 0u;
} t3dRuntimeProfileSample_t;

typedef struct t3dRuntimeProfileHistory_s {
	eastl::array<t3dRuntimeProfileSample_t, TERRAIN3D_RUNTIME_PROFILE_HISTORY_CAPACITY> samples{};
	size_t iWriteIndex = 0u;
	size_t nCount = 0u;

	EA_FORCE_INLINE void reset( void ) noexcept { iWriteIndex = 0u; nCount = 0u; }

	EA_FORCE_INLINE void push( const t3dRuntimeProfileSample_t &rSample ) noexcept { samples[iWriteIndex] = rSample; iWriteIndex = ( iWriteIndex + 1u ) % samples.size(); nCount = std::min( nCount + 1u, samples.size() ); }

	EA_FORCE_INLINE const t3dRuntimeProfileSample_t &getOldest( const size_t iIndex ) const noexcept { const size_t oldest = ( iWriteIndex + samples.size() - nCount ) % samples.size(); return samples[( oldest + iIndex ) % samples.size()]; }
} t3dRuntimeProfileHistory_t;

typedef struct t3dRuntimeProfileAccumulator_s {
	uint64_t nSampledTicks = 0u;
	uint64_t nPhysicsTotalUsec = 0u;
	uint64_t nPhysicsPeakUsec = 0u;
	uint64_t nDataResidencyTotalUsec = 0u;
	uint64_t nDataResidencyPeakUsec = 0u;
	uint64_t nHlodStreamingTotalUsec = 0u;
	uint64_t nHlodStreamingPeakUsec = 0u;
	uint64_t nNodeStreamingTotalUsec = 0u;
	uint64_t nNodeStreamingPeakUsec = 0u;
	uint64_t nClipmapTotalUsec = 0u;
	uint64_t nClipmapPeakUsec = 0u;
	uint64_t nOceanTotalUsec = 0u;
	uint64_t nOceanPeakUsec = 0u;
	uint64_t nInstancerTotalUsec = 0u;
	uint64_t nInstancerPeakUsec = 0u;
	uint64_t nLightSyncTotalUsec = 0u;
	uint64_t nLightSyncPeakUsec = 0u;
	uint64_t nCollisionTotalUsec = 0u;
	uint64_t nCollisionPeakUsec = 0u;
	uint64_t nStreamingTotalUsec = 0u;
	uint64_t nStreamingPeakUsec = 0u;
	uint64_t nGpuUploadsTotal = 0u;
	uint64_t nGpuEvictionsTotal = 0u;
	uint64_t nGpuUploadBytesTotal = 0u;
	uint64_t nGpuMissingRequiredTotal = 0u;
	int gpuResidentPeak = 0;
	uint64_t nGpuPreparedPagesTotal = 0u;
	int gpuPrepareWorkersPeak = 0;
	int gpuEffectiveUploadLimitPeak = 0;
	uint64_t nGpuCameraMoveTicks = 0u;
	double fDistanceTravelledM = 0.0;
	uint64_t nGpuCookedHitsTotal = 0u;
	uint64_t nGpuCookedMissesTotal = 0u;
	uint64_t nGpuRuntimePreparesTotal = 0u;
	uint64_t nClipmapNearUpdatesTotal = 0u;
	uint64_t nClipmapMiddleUpdatesTotal = 0u;
	uint64_t nClipmapFarUpdatesTotal = 0u;
	uint64_t nClipmapTeleportsTotal = 0u;
	int clipmapTeleportsPeak = 0;
	uint64_t nClipmapBufferUploadsTotal = 0u;
	int clipmapBufferUploadsPeak = 0;
	uint64_t nCpuLoadsTotal = 0u;
	uint64_t nCpuEvictionsTotal = 0u;
	uint64_t nCpuLoadBytesTotal = 0u;
	int cpuResidentPeak = 0;
	uint64_t nInstanceLoadsTotal = 0u;
	uint64_t nInstanceEvictionsTotal = 0u;
	uint64_t nInstanceLoadBytesTotal = 0u;
	int instanceResidentPeak = 0;
	int renderCellsPeak = 0;
	int pendingCellUpdatesPeak = 0;
	int nodeActiveRegionsPeak = 0;
	int nodeActiveNodesPeak = 0;
	uint64_t nNodeLoadsTotal = 0u;
	uint64_t nNodeUnloadsTotal = 0u;
	uint64_t nNodeLoadFailuresTotal = 0u;

	void reset( void ) noexcept {
		*this = t3dRuntimeProfileAccumulator_s{};
	}

	void addSample( const t3dRuntimeProfileSample_t &rSample ) noexcept {
		++nSampledTicks;
		nPhysicsTotalUsec += rSample.nPhysicsTotalUsec;
		nPhysicsPeakUsec = std::max( nPhysicsPeakUsec, rSample.nPhysicsTotalUsec );
		nDataResidencyTotalUsec += rSample.nDataResidencyUsec;
		nDataResidencyPeakUsec = std::max( nDataResidencyPeakUsec, rSample.nDataResidencyUsec );
		nHlodStreamingTotalUsec += rSample.nHlodStreamingUsec;
		nHlodStreamingPeakUsec = std::max( nHlodStreamingPeakUsec, rSample.nHlodStreamingUsec );
		nNodeStreamingTotalUsec += rSample.nNodeStreamingUsec;
		nNodeStreamingPeakUsec = std::max( nNodeStreamingPeakUsec, rSample.nNodeStreamingUsec );
		nClipmapTotalUsec += rSample.nClipmapUsec;
		nClipmapPeakUsec = std::max( nClipmapPeakUsec, rSample.nClipmapUsec );
		nOceanTotalUsec += rSample.nOceanUsec;
		nOceanPeakUsec = std::max( nOceanPeakUsec, rSample.nOceanUsec );
		nInstancerTotalUsec += rSample.nInstancerUsec;
		nInstancerPeakUsec = std::max( nInstancerPeakUsec, rSample.nInstancerUsec );
		nLightSyncTotalUsec += rSample.nLightSyncUsec;
		nLightSyncPeakUsec = std::max( nLightSyncPeakUsec, rSample.nLightSyncUsec );
		nCollisionTotalUsec += rSample.nCollisionUsec;
		nCollisionPeakUsec = std::max( nCollisionPeakUsec, rSample.nCollisionUsec );
		nStreamingTotalUsec += rSample.nStreamingUsec;
		nStreamingPeakUsec = std::max( nStreamingPeakUsec, rSample.nStreamingUsec );
		nGpuUploadsTotal += ( uint64_t ) ( std::max( rSample.nGpuUploads, 0 ) );
		nGpuEvictionsTotal += ( uint64_t ) ( std::max( rSample.nGpuEvictions, 0 ) );
		nGpuUploadBytesTotal += rSample.nGpuUploadBytes;
		nGpuMissingRequiredTotal += ( uint64_t ) ( std::max( rSample.gpuMissingRequired, 0 ) );
		gpuResidentPeak = std::max( gpuResidentPeak, rSample.gpuResident );
		nGpuPreparedPagesTotal += ( uint64_t ) ( std::max( rSample.nGpuPreparedPages, 0 ) );
		gpuPrepareWorkersPeak = std::max( gpuPrepareWorkersPeak, rSample.nGpuPrepareWorkers );
		gpuEffectiveUploadLimitPeak = std::max( gpuEffectiveUploadLimitPeak, rSample.gpuEffectiveUploadLimit );
		nGpuCameraMoveTicks += rSample.gpuCameraMoved != 0 ? 1u : 0u;
		fDistanceTravelledM += std::sqrt( ( double ) ( rSample.fMovementX ) * ( double ) ( rSample.fMovementX ) +
			( double ) ( rSample.fMovementZ ) * ( double ) ( rSample.fMovementZ ) );
		nGpuCookedHitsTotal += ( uint64_t ) ( std::max( rSample.nGpuCookedHits, 0 ) );
		nGpuCookedMissesTotal += ( uint64_t ) ( std::max( rSample.nGpuCookedMisses, 0 ) );
		nGpuRuntimePreparesTotal += ( uint64_t ) ( std::max( rSample.gpuRuntimePrepares, 0 ) );
		nClipmapNearUpdatesTotal += ( uint64_t ) ( std::max( rSample.clipmapNearUpdates, 0 ) );
		nClipmapMiddleUpdatesTotal += ( uint64_t ) ( std::max( rSample.clipmapMiddleUpdates, 0 ) );
		nClipmapFarUpdatesTotal += ( uint64_t ) ( std::max( rSample.clipmapFarUpdates, 0 ) );
		nClipmapTeleportsTotal += ( uint64_t ) ( std::max( rSample.clipmapTeleports, 0 ) );
		clipmapTeleportsPeak = std::max( clipmapTeleportsPeak, rSample.clipmapTeleports );
		nClipmapBufferUploadsTotal += ( uint64_t ) ( std::max( rSample.nClipmapBufferUploads, 0 ) );
		clipmapBufferUploadsPeak = std::max( clipmapBufferUploadsPeak, rSample.nClipmapBufferUploads );
		nCpuLoadsTotal += ( uint64_t ) ( std::max( rSample.cpuLoads, 0 ) );
		nCpuEvictionsTotal += ( uint64_t ) ( std::max( rSample.nCpuEvictions, 0 ) );
		nCpuLoadBytesTotal += rSample.nCpuLoadBytes;
		cpuResidentPeak = std::max( cpuResidentPeak, rSample.cpuResident );
		nInstanceLoadsTotal += ( uint64_t ) ( std::max( rSample.instanceLoads, 0 ) );
		nInstanceEvictionsTotal += ( uint64_t ) ( std::max( rSample.nInstanceEvictions, 0 ) );
		nInstanceLoadBytesTotal += rSample.nInstanceLoadBytes;
		instanceResidentPeak = std::max( instanceResidentPeak, rSample.instanceResident );
		renderCellsPeak = std::max( renderCellsPeak, rSample.renderCells );
		pendingCellUpdatesPeak = std::max( pendingCellUpdatesPeak, rSample.pendingCellUpdates );
		nodeActiveRegionsPeak = std::max( nodeActiveRegionsPeak, rSample.nNodeActiveRegions );
		nodeActiveNodesPeak = std::max( nodeActiveNodesPeak, rSample.nodeActiveNodes );
		nNodeLoadsTotal += ( uint64_t ) ( std::max( rSample.nodeLoads, 0 ) );
		nNodeUnloadsTotal += ( uint64_t ) ( std::max( rSample.nodeUnloads, 0 ) );
		nNodeLoadFailuresTotal += ( uint64_t ) ( std::max( rSample.nNodeLoadFailures, 0 ) );
	}
} t3dRuntimeProfileAccumulator_t;
#endif // TERRAIN3D_RUNTIME_PROFILE_H
