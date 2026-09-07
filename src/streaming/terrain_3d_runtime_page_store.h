// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RUNTIME_PAGE_STORE_H
#define TERRAIN3D_RUNTIME_PAGE_STORE_H

#include "core/terrain_3d_eastl_helpers.h"
#include "core/terrain_3d_sync.h"
#include "Terrain3DConstants.h"
#include "eastl_config.h"
#include "data/terrain_3d_runtime_page_constants.h"
#include "instances/terrain_3d_instance_store.h"

#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

class Terrain3DRegion;
class Terrain3DRegionPersistence;
class Terrain3DRegionRepository;

typedef enum t3dRuntimePageConsumer_e : uint16_t {
	RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT = 1u << 0,
	RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT = 1u << 1,
	RUNTIME_PAGE_CONSUMER_GPU_DETAIL = 1u << 2,
	RUNTIME_PAGE_CONSUMER_INSTANCE_STORE = 1u << 3,
	RUNTIME_PAGE_CONSUMER_CPU_MAPS = 1u << 4,
	RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS = 1u << 5,
	RUNTIME_PAGE_CONSUMER_COLLISION = 1u << 6,
	RUNTIME_PAGE_CONSUMER_NAVIGATION = 1u << 7,
	RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW = 1u << 8,
} t3dRuntimePageConsumer_t;

typedef struct t3dRuntimePageNumericConfig_s {
	int m_nColorDivisor = 1;
	int m_nDerivedDivisor = 1;
	int m_nMiddleHeightDivisor = 1;
	int m_nPrepareThreads = 2;
} t3dRuntimePageNumericConfig_t;

EA_FORCE_INLINE t3dRuntimePageNumericConfig_t t3dCompileRuntimePageNumericConfig(
        const int nColorDivisor, const int nDerivedDivisor, const int nMiddleHeightDivisor, const int nPrepareThreads ) {
	t3dRuntimePageNumericConfig_t config;
	config.m_nColorDivisor = nColorDivisor > 0 ? nColorDivisor : 1;
	config.m_nDerivedDivisor = nDerivedDivisor > 0 ? nDerivedDivisor : 1;
	config.m_nMiddleHeightDivisor = nMiddleHeightDivisor > 0 ? nMiddleHeightDivisor : 1;
	config.m_nPrepareThreads = nPrepareThreads < 1 ? 1 : ( nPrepareThreads > 32 ? 32 : nPrepareThreads );
	return config;
}

typedef struct t3dRuntimePageConfig_s {
	String m_dataDirectory;
	uint64_t m_nMaterialSourceSignature = 0u;
	uint64_t m_nCollisionSurfaceSignature = 0u;
	int m_nColorDivisor = 1;
	int m_nDerivedDivisor = 1;
	int m_nMiddleHeightDivisor = 1;
	int m_nPrepareThreads = 2;
	bool m_bCookedPagesEnabled = false;
	bool m_bEditorAuthoring = false;
} t3dRuntimePageConfig_t;

typedef struct t3dRuntimePageCookPayload_s {
	Ref<Image> m_height;
	Ref<Image> m_control;
	Ref<Image> m_color;
	Ref<Image> m_derived;
	Ref<Image> m_middleHeight;
	Ref<Image> m_middleComposite;
	Ref<Image> m_terrainShadowMask;
	PackedByteArray m_holeMask;
	PackedByteArray m_collisionHeight;
	PackedByteArray m_collisionBounds;
	PackedByteArray m_navigation;
	PackedByteArray m_minMaxHeight;
	PackedByteArray m_nodeDescriptors;
	PackedByteArray m_instanceMetadata;
	PackedByteArray m_lightingReceivers;
	PackedByteArray m_multimeshFastStart;
	uint32_t m_nMaterialMask = 1u;
} t3dRuntimePageCookPayload_t;

typedef enum t3dRuntimePageProbeState_e : uint8_t {
	RUNTIME_PAGE_PROBE_MISSING = 0u,
	RUNTIME_PAGE_PROBE_CURRENT = 1u,
	RUNTIME_PAGE_PROBE_LEGACY = 2u,
	RUNTIME_PAGE_PROBE_FUTURE = 3u,
	RUNTIME_PAGE_PROBE_INVALID = 4u,
} t3dRuntimePageProbeState_t;

typedef struct t3dRuntimePageProbe_s {
	t3dRuntimePageProbeState_t m_state = RUNTIME_PAGE_PROBE_MISSING;
	uint32_t m_nVersion = 0u;
	uint32_t m_nHeaderBytes = 0u;
	uint32_t m_nTotalBytes = 0u;
	uint32_t m_nChannelCount = 0u;
	uint64_t m_nFileBytes = 0u;
} t3dRuntimePageProbe_t;

typedef struct t3dRuntimePageStats_s {
	int m_nPreparedPages = 0;
	int m_nPreparedLoadedPages = 0;
	int m_nPendingPages = 0;
	int m_nQueuedPages = 0;
	int m_nPrepareWorkers = 0;
	bool m_bPrepareJobActive = false;
	uint64_t m_nPrepareLatencyUsecLast = 0u;
	uint64_t m_nPrepareQueueWaitUsecLast = 0u;
	uint64_t m_nPrepareFileOpenUsecLast = 0u;
	uint64_t m_nPrepareFileReadUsecLast = 0u;
	uint64_t m_nPrepareValidationUsecLast = 0u;
	uint64_t m_nPrepareMaterializeUsecLast = 0u;
	uint64_t m_nPrepareWorkerUsecLast = 0u;
	uint64_t m_nPrepareReadyWaitUsecLast = 0u;
	uint64_t m_nPreparedByteBudget = 0u;
	uint64_t m_nPreparedReservedBytes = 0u;
	uint64_t m_nPayloadBytesPerSlot = 0u;
	uint64_t m_nWorkerScratchReservedBytes = 0u;
	int m_nConfiguredPrepareWorkers = 0;
	int m_nEffectivePrepareWorkers = 0;
	bool m_bCapacityFailureLatched = false;
} t3dRuntimePageStats_t;

typedef struct t3dRuntimePageServiceState_s {
	typedef bool ( *IsWantedFn )( void *pContext, const Vector2i &rLocation );

	t3dRuntimePageConfig_t m_config;
	Vector2i m_predictedCenter = s_v2iMax;
	uint64_t m_nStreamingGeneration = 0u;
	IsWantedFn m_isWanted = NULL;
	void *m_pWantedContext = NULL;
} t3dRuntimePageServiceState_t;

// Owns cooked runtime-page I/O, validation/materialization, merged consumer
// requests, worker preparation, and prepared-page lifetime. Residency consumers
// decide which locations they want; this store performs one physical page read
// that can satisfy all consumers of that location.
typedef struct t3dRuntimePageMeta_s {
	Vector2i m_location = s_v2iMax;
	int16_t m_iSourceIndex = -1;
	int16_t m_iPayloadSlot = -1;
	uint32_t m_nExpectedChannelFlags = 0u;
	uint64_t m_nExpectedSourceSignature = 0u;
	int m_nColorDivisor = 1;
	int m_nDerivedDivisor = 1;
	int m_nMiddleHeightDivisor = 1;
	uint32_t m_nMaterialMask = 1u;
	uint16_t m_nConsumerMask = 0u;
	uint16_t m_nAttemptedConsumerMask = 0u;
	uint16_t m_nMaterializedConsumerMask = 0u;
	uint16_t m_nLoadedConsumerMask = 0u;
	uint64_t m_nRequestedUsec = 0u;
	uint64_t m_nWorkerStartedUsec = 0u;
	uint64_t m_nFileOpenUsec = 0u;
	uint64_t m_nFileReadUsec = 0u;
	uint64_t m_nValidationUsec = 0u;
	uint64_t m_nMaterializeUsec = 0u;
	uint64_t m_nWorkerFinishedUsec = 0u;
	bool m_bAttempted = false;
	bool m_bLoaded = false;
	bool m_bInstanceAttempted = false;
	bool m_bInstanceLoaded = false;
	bool m_bLoadMinMax = false;
	bool m_bEditorAuthoring = false;
} t3dRuntimePageMeta_t;

typedef struct t3dRuntimePagePayload_s {
	uint32_t m_szLogicalBytes[s_nRuntimePageChannelCount + 1u] = {};
	PackedByteArray m_heightBytes;
	PackedByteArray m_controlBytes;
	PackedByteArray m_colorBytes;
	PackedByteArray m_derivedBytes;
	PackedByteArray m_middleHeightBytes;
	PackedByteArray m_middleCompositeBytes;
	PackedByteArray m_terrainShadowMaskBytes;
	Ref<Image> m_height;
	Ref<Image> m_control;
	Ref<Image> m_color;
	Ref<Image> m_derived;
	Ref<Image> m_middleHeight;
	Ref<Image> m_middleComposite;
	Ref<Image> m_terrainShadowMask;
	PackedByteArray m_instancePage;
	PackedByteArray m_holeMask;
	PackedByteArray m_collisionHeight;
	PackedByteArray m_collisionBounds;
	PackedByteArray m_navigation;
	PackedByteArray m_minMaxHeight;
	PackedByteArray m_nodeDescriptors;
	PackedByteArray m_instanceMetadata;
	PackedByteArray m_lightingReceivers;
	PackedByteArray m_multimeshFastStart;
	Terrain3DInstanceStore m_preparedInstanceStore{ TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED };
} t3dRuntimePagePayload_t;

typedef struct t3dRuntimePageSource_s {
	String m_pagePath;
	String m_sourcePath;
	uint32_t m_nExpectedChannelFlags = 0u;
	uint64_t m_nExpectedSourceSignature = 0u;
	bool m_bPresent = false;
} t3dRuntimePageSource_t;

typedef struct t3dRuntimePageWorkerScratch_s {
	PackedByteArray m_ioBytes;
} t3dRuntimePageWorkerScratch_t;

class Terrain3DRuntimePageStore {
public:
	explicit Terrain3DRuntimePageStore( Terrain3DRegionRepository *pRepository, const Terrain3DRegionPersistence *pPersistence );
	~Terrain3DRuntimePageStore( void );
	Terrain3DRuntimePageStore( const Terrain3DRuntimePageStore & ) = delete;
	Terrain3DRuntimePageStore &operator=( const Terrain3DRuntimePageStore & ) = delete;

	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT );
	void setPreparedByteBudget( uint64_t nBytes );
	EA_FORCE_INLINE uint64_t getPreparedByteBudget( void ) const {
		return m_nPreparedByteBudget;
	}
	EA_FORCE_INLINE uint64_t getPreparedReservedBytes( void ) const {
		return m_nPreparedReservedBytes;
	}
	void reset( void );
	void resetFrameStats( void );
	void poll( bool bWait );

	String getPagePath( const Terrain3DRegion *pRegion, const String &rDataDirectory ) const;
	uint64_t computeSourceSignature( const Terrain3DRegion *pRegion, const t3dRuntimePageConfig_t &rConfig ) const;
	bool readSourceSignature( const String &rPagePath, uint64_t &nSignature ) const;
	bool probePage( const String &rPagePath, t3dRuntimePageProbe_t &rProbe ) const;
	uint32_t readMaterialMask( const String &rPagePath ) const;
	bool removePageForSource( const String &rSourcePath ) const;
	bool removePage( const String &rPagePath ) const;
	bool validatePageForRegion( const Terrain3DRegion *pRegion, const t3dRuntimePageConfig_t &rConfig ) const;
	bool loadCpuMapsNow( const Vector2i &rLocation,
	        const t3dRuntimePageConfig_t &rConfig,
	        Ref<Image> &rHeight,
	        Ref<Image> &rControl,
	        Ref<Image> &rColor,
	        Ref<Image> &rDerived ) const;
	bool loadCollisionNow( const Vector2i &rLocation,
	        const t3dRuntimePageConfig_t &rConfig,
	        PackedByteArray &rHeight,
	        PackedByteArray &rBounds,
	        PackedByteArray &rHoleMask ) const;
	bool loadNavigationNow( const Vector2i &rLocation,
	        const t3dRuntimePageConfig_t &rConfig,
	        PackedByteArray &rNavigation,
	        PackedByteArray *pCollisionHeight = NULL,
	        PackedByteArray *pMinMaxHeight = NULL ) const;
	bool loadLightingReceiversNow( const Vector2i &rLocation, const t3dRuntimePageConfig_t &rConfig, PackedByteArray &rReceivers ) const;
	bool writePage( Terrain3DRegion *pRegion,
	        const t3dRuntimePageConfig_t &rConfig,
	        const t3dRuntimePageCookPayload_t &rPayload,
	        String *pError = NULL );

	bool collectPageSources( const String &rDataDirectory,
	        PackedStringArray &rSourceFiles,
	        eastl::vector<Vector2i, Terrain3DEastlAllocator> &rLocations,
	        PackedStringArray &rErrors ) const;

	void request( const Vector2i &rLocation, uint16_t nConsumers, uint8_t nPriority, bool bEnabled );
	void service( const t3dRuntimePageServiceState_t &rState );
	bool isPending( const Vector2i &rLocation ) const;
	bool isPrepared( const Vector2i &rLocation ) const;

	bool takeHeight(
	        const Vector2i &rLocation, uint16_t nConsumer, Ref<Image> &rHeight, bool bMiddle, PackedByteArray *pNativeHeightBytes = NULL );
	bool takeInstance( const Vector2i &rLocation,
	        Terrain3DInstanceStore &rStore,
	        PackedByteArray *pInstanceMetadata = NULL,
	        PackedByteArray *pMultimeshFastStart = NULL );
	bool takeCpuMaps( const Vector2i &rLocation, Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived );
	bool takeNodeDescriptors( const Vector2i &rLocation, PackedByteArray &rDescriptors );
	bool takeCollision( const Vector2i &rLocation, PackedByteArray &rHeight, PackedByteArray &rBounds, PackedByteArray &rHoleMask );
	bool takeNavigation( const Vector2i &rLocation, PackedByteArray &rNavigation );
	bool takeTerrainShadowMask( const Vector2i &rLocation, Ref<Image> &rShadowMask );
	bool takeGpuPage( const Vector2i &rLocation,
	        Ref<Image> &rHeight,
	        Ref<Image> &rControl,
	        Ref<Image> &rColor,
	        Ref<Image> &rDerived,
	        Ref<Image> &rMiddleComposite,
	        uint32_t &rMaterialMask,
	        bool &bAttempted,
	        uint64_t *pRequestedUsec = NULL );

	t3dRuntimePageStats_t getStats( void ) const;
	EA_FORCE_INLINE int getPreparedPageCount( void ) const {
		return m_nPreparedPageCount;
	}
	EA_FORCE_INLINE int getPendingPageCount( void ) const {
		return m_bPrepareJobActive ? m_nPrepareWorkPageCount : 0;
	}
	EA_FORCE_INLINE int getQueuedPageCount( void ) const {
		return m_nQueuedCount;
	}
	EA_FORCE_INLINE int getPrepareWorkerCount( void ) const {
		return m_bPrepareJobActive ? m_nPrepareJobWorkers : 0;
	}
	EA_FORCE_INLINE bool isPrepareJobActive( void ) const {
		return m_bPrepareJobActive;
	}
	EA_FORCE_INLINE int getTransientPageCount( void ) const {
		return m_nPreparedPageCount + m_nPrepareWorkPageCount;
	}

private:
	Terrain3DRegionRepository *m_pRepository = NULL;
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
	const Terrain3DRegionPersistence *m_pPersistence = NULL;
	eastl::array<t3dRuntimePageMeta_t, s_nGpuCacheMaxLayers> m_preparedPages{};
	eastl::array<t3dRuntimePageMeta_t, s_nGpuCacheMaxLayers> m_workPages{};
	eastl::array<t3dRuntimePagePayload_t, s_nGpuCacheMaxLayers> m_payloadSlots{};
	eastl::array<int16_t, s_nGpuCacheMaxLayers> m_freePayloadSlots{};
	eastl::array<t3dRuntimePageSource_t, s_nRegionSlots> m_pageSources{};
	eastl::array<uint32_t, s_nRuntimePageChannelCount + 1u> m_runtimeChannelCapacities{};
	eastl::array<int16_t, s_nRegionSlots> m_preparedByRegion{};
	eastl::array<int16_t, s_nRegionSlots> m_workByRegion{};
	int m_nPreparedPageCount = 0;
	int m_nPrepareWorkPageCount = 0;
	int m_nFreePayloadCount = 0;
	int m_nRuntimePayloadLimit = s_nGpuCacheRuntimeMaxLayers;
	bool m_bPrepareJobActive = false;
	uint64_t m_nPrepareJobStartedUsec = 0u;
	uint64_t m_nPrepareLatencyUsecLast = 0u;
	uint64_t m_nPrepareQueueWaitUsecLast = 0u;
	uint64_t m_nPrepareFileOpenUsecLast = 0u;
	uint64_t m_nPrepareFileReadUsecLast = 0u;
	uint64_t m_nPrepareValidationUsecLast = 0u;
	uint64_t m_nPrepareMaterializeUsecLast = 0u;
	uint64_t m_nPrepareWorkerUsecLast = 0u;
	uint64_t m_nPrepareReadyWaitUsecLast = 0u;
	int m_nPrepareJobWorkers = 0;
	eastl::array<uint16_t, s_nRegionSlots> m_queuedConsumers{};
	eastl::array<uint8_t, s_nRegionSlots> m_queuedPriorities{};
	eastl::array<int16_t, s_nRegionSlots> m_queuedNext{};
	eastl::array<int16_t, s_nRegionSlots> m_queuedPrev{};
	eastl::array<int16_t, 256> m_priorityHeads{};
	eastl::array<int16_t, 256> m_priorityTails{};
	int m_nQueuedCount = 0;
	uint64_t m_nStreamingGenerationLast = 0u;
	uint64_t m_nSourceTopologyGeneration = 0u;
	uint64_t m_nSourceMaterialSignature = 0u;
	uint64_t m_nSourceCollisionSignature = 0u;
	String m_sourceDataDirectory;
	t3dRuntimePageNumericConfig_t m_sourceNumericConfig;
	bool m_bSourceEditorAuthoring = false;
	bool m_bRuntimeSourcesDirty = true;
	int m_nEffectivePrepareThreads = 0;
	bool m_bCapacityFailureLatched = false;
	uint64_t m_nCapacityFailureTopologyGeneration = 0u;
	uint64_t m_nCapacityFailurePreparedByteBudget = 0u;
	uint64_t m_nCapacityFailureMaterialSignature = 0u;
	uint64_t m_nCapacityFailureCollisionSignature = 0u;
	t3dRuntimePageNumericConfig_t m_capacityFailureNumericConfig;
	String m_capacityFailureDataDirectory;
	bool m_bCapacityFailureEditorAuthoring = false;
	uint32_t m_nWorkerScratchBytes = s_nRuntimePageHeaderBytes;
	uint64_t m_nPreparedByteBudget = 0u;
	uint64_t m_nPreparedReservedBytes = 0u;
	uint64_t m_nPayloadBytesPerSlot = 0u;
	uint64_t m_nWorkerScratchReservedBytes = 0u;

	eastl::array<t3dThread_t, s_nRuntimeWorkerLimit> m_workers;
	eastl::array<t3dRuntimePageWorkerScratch_t, s_nRuntimeWorkerLimit> m_workerScratch{};
	eastl::array<int16_t, s_nGpuCacheMaxLayers> m_workRing{};
	eastl::array<int16_t, s_nGpuCacheMaxLayers> m_completionRing{};
	int m_iWorkRead = 0;
	int m_iWorkWrite = 0;
	int m_nWorkRingCount = 0;
	int m_iCompletionRead = 0;
	int m_iCompletionWrite = 0;
	int m_nCompletionRingCount = 0;
	int m_nWorkerThreadCount = 0;
	bool m_bWorkersStop = false;
	mutable t3dMutex_t m_workerMutex;
	t3dCondition_t m_workerWake;
	t3dCondition_t m_completionWake;

	int getPreparedIndex( const Vector2i &rLocation ) const;
	int getWorkIndex( const Vector2i &rLocation ) const;
	bool loadSourceRegion( const Vector2i &rLocation, Ref<Terrain3DRegion> &rSource ) const;
	uint64_t resolveExpectedSourceSignature( const Terrain3DRegion *pRegion, const t3dRuntimePageConfig_t &rConfig ) const;
	bool loadSnapshot( t3dRuntimePageMeta_t &rMeta,
	        t3dRuntimePagePayload_t &rPayload,
	        t3dRuntimePageWorkerScratch_t &rScratch,
	        const t3dRuntimePageSource_t &rSource,
	        bool bPreserveMaterialized = false ) const;
	void prepareWorkIndex( int nWorkIndex, int nWorkerIndex );
	void workerMain( int nWorkerIndex );
	void startWorkers( int nWorkerCount );
	void stopWorkers( void );
	bool rebuildRuntimeSources( const t3dRuntimePageServiceState_t &rState );
	void queueLink( int nMapIndex, uint8_t nPriority );
	void queueUnlink( int nMapIndex );
	int popBestQueued( const Vector2i &rPredictedCenter );
	int acquirePayloadSlot( void );
	void releasePayloadSlot( int nPayloadSlot );
	void mergePreparedPayload( t3dRuntimePageMeta_t &rDestinationMeta,
	        t3dRuntimePagePayload_t &rDestinationPayload,
	        const t3dRuntimePageMeta_t &rSourceMeta,
	        t3dRuntimePagePayload_t &rSourcePayload );
	void consumePreparedIndex( int nIndex );
};

#endif // TERRAIN3D_RUNTIME_PAGE_STORE_H
