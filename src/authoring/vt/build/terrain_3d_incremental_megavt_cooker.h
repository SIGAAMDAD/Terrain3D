// Copyright ? 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INCREMENTAL_MEGAVT_COOKER_H
#define TERRAIN3D_INCREMENTAL_MEGAVT_COOKER_H

#include "Terrain3DTypes.h"
#include "memory/policy/terrain_3d_memory_account.h"
#include "data/model/terrain_3d_data_access.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h"
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3DMegaVTCookHost {
protected:
	~Terrain3DMegaVTCookHost( void ) = default;
public:
	virtual void requestMegaVTPackageRecomposeCold( void ) = 0;
	virtual uint64_t publishedMegaVTGpuFingerprintCold( void ) const = 0;
	virtual void persistMegaVTEditorStateCold( void ) = 0;
};

struct t3dMegaVTAuthoringInputs_t {
	Terrain3DRegionSource &m_source;
	String m_dataDirectory;
	uint64_t m_nCpuByteLimit = 512ull * 1024ull * 1024ull;
};

// Live Images are accessed only during owner-thread capture. Workers own bytes.
struct t3dMegaVTColorSource_t {
	PackedByteArray m_bytes;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
};
struct t3dMegaVTColorCapture_t {
	t3dMegaVTColorSource_t m_neighbors[9];
	uint32_t m_nExtent = 0u;
	uint32_t m_nRegionIndex = 0u;
	uint32_t m_nGeneration = 0u;
	uint32_t m_nDirtyMip0Mask = 0u;
};
struct t3dMegaVTRegionPixels_t {
	uint32_t m_nRegionIndex = 0u;
	uint32_t m_nExtent = 0u;
	PackedByteArray m_albedo;
	PackedByteArray m_pbr;
};

// One worker and one pending publication bound concurrent authoring work.
// Eight region pixel caches allow dirty-page updates; other regions rebuild
// their pixels when revisited. Unchanged encoded region fragments are retained.
class Terrain3DIncrementalMegaVTCooker {
private:
	Terrain3DMemoryReservation m_cpuMemory;
	Terrain3DThread m_worker;
	t3dAtomicBool_t m_done{ false };
	t3dAtomicBool_t m_cancel{ false };
	t3dMegaVTColorCapture_t m_capture;
	t3dMegaVTRegionPixels_t m_pixels;
	t3dTerrainMegaVTCookTicket_t m_ticket{};
	PackedByteArray m_workerFragment;
	PackedByteArray m_pendingFragment;
	PackedByteArray m_fragments[T3D_REGION_SLOTS];
	PackedByteArray m_forceFragments[T3D_REGION_SLOTS];
	t3dTerrainMegaVTCookTicket_t m_forceTickets[T3D_REGION_SLOTS]{};
	uint32_t m_forceSourceGenerations[T3D_REGION_SLOTS]{};
	bool m_bForceRequested = false;
	bool m_bForceActive = false;
	bool m_bForceFailed = false;
	uint32_t m_nForceRegionCount = 0u;
	uint32_t m_nForceCookedCount = 0u;
	bool m_bCacheRestoreAttempted[T3D_REGION_SLOTS]{};
	eastl::vector<t3dMegaVTRegionPixels_t, Terrain3DEastlAllocator> m_pixelCache;
	uint64_t m_nPendingFingerprint = 0u;
	uint64_t m_nIdlePublicationGeneration = UINT64_MAX;
	uint64_t m_nObservedTopologyGeneration = UINT64_MAX;
	uint32_t m_nFailedSourceGenerations[T3D_REGION_SLOTS]{};
	uint64_t m_nPublicationDeadlineUsec = 0u;
	uint64_t m_nSubmitted = 0u;
	uint64_t m_nAccepted = 0u;
	uint64_t m_nCacheRestored = 0u;
	uint64_t m_nCacheRejected = 0u;
	uint64_t m_nRejected = 0u;
	uint64_t m_nFailures = 0u;
	uint32_t m_nNextRegion = 0u;
	uint32_t m_nTexelsPerMeter = 0u;
	uint64_t m_nCpuByteLimit = 512ull * 1024ull * 1024ull;
	uint64_t m_nPeakCpuByteEstimate = 0u;
	uint64_t m_nMergeUsec = 0u;
	uint64_t m_nPayloadComparisons = 0u;
	uint64_t m_nCompositionWorkingBytes = 0u;
	uint64_t m_nCompositionWorkingLimit = 0u;
	uint64_t m_nWorkerByteLimit = 32ull * 1024ull * 1024ull;
	bool m_bWorkerSucceeded = false;
	String m_lastError;

	bool captureRegionCold( const t3dMegaVTAuthoringInputs_t &rInputs, const t3dTerrainMegaVTIncrementalCookRequest_t &rRequest );
	size_t getRetainedPayloadBytesCold( void ) const;
	void trimCpuReservationCold( void );
	void cookCapturedRegion( void );
	void retireClaimCold( Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost, const String &rError );
	bool publishFragmentFileCold( const t3dMegaVTAuthoringInputs_t &rInputs );
	bool restoreCachedRegionCold( const t3dMegaVTAuthoringInputs_t &rInputs, uint32_t nRegionIndex,
	        uint32_t nSourceGeneration, uint32_t nDirtyMip0Mask, uint64_t &rRetainedBytes );
	void finishWorkerCold( Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost );
	void finishPublicationCold( const t3dMegaVTAuthoringInputs_t &rInputs, Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost );
	void queueRegionCold( const t3dMegaVTAuthoringInputs_t &rInputs, Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost );
	static void workerEntry( Terrain3DIncrementalMegaVTCooker *pCooker );
public:
	~Terrain3DIncrementalMegaVTCooker( void );
	// Restore clean region fragments before the first GPU package is published.
	void restoreCachedRegionsCold( const t3dMegaVTAuthoringInputs_t &rInputs, uint32_t nTexelsPerMeter );
	void updateCold( const t3dMegaVTAuthoringInputs_t &rInputs, Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost );
	void retryDirtyRegionsCold( void );
	void requestForceRecookAllRegionsCold( void ) { m_bForceRequested = true; }
	bool isForceRecookReadyCold() const { return m_bForceActive && !m_bForceFailed && m_nForceCookedCount == m_nForceRegionCount; }
	bool isForceRecookFailedCold() const { return m_bForceFailed; }
	uint32_t getForceRegionCountCold() const { return m_nForceRegionCount; }
	uint32_t getForceCookedCountCold() const { return m_nForceCookedCount; }
	bool validateForceRecookSnapshotCold( Terrain3DTerrainMegaVT &rMegaVT ) const;
	bool finishForceRecookAllRegionsCold( const t3dMegaVTAuthoringInputs_t &rInputs,
	        Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost, bool bCommit );
	void shutdownCold( Terrain3DTerrainMegaVT &rMegaVT );
	bool composePackageCold( PackedByteArray &rPackage, uint32_t &rGeneration, uint64_t &rFingerprint,
	        Terrain3DSharedMemoryReservation *pPackageMemory = NULL );
	Dictionary getDiagnosticsCold( void ) const;
	size_t getRetainedCpuBytesCold( void ) const;
	uint64_t getReservedCpuBytesCold() const { return m_cpuMemory.bytes(); }
	String getLastErrorCold() const { return m_lastError; }
	// Explicit export is synchronous authoring. It uses the same capture/cook
	// contract and emits all regions, independently of preview/GPU readiness.
	static bool appendRuntimeRegionsCold( const t3dMegaVTAuthoringInputs_t &rInputs, PackedByteArray &rPackage, uint32_t &rGeneration, String &rError );
};
#endif // TERRAIN3D_INCREMENTAL_MEGAVT_COOKER_H
