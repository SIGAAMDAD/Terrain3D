// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RUNTIME_PAGE_STORE_H
#define TERRAIN3D_RUNTIME_PAGE_STORE_H

#include "Terrain3DConstants.h"
#include "instances/terrain_3d_instance_store.h"

#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "eastl_config.h"
#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>

class T3DRegion;
class T3DRegionPersistence;
class T3DRegionRepository;

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

typedef struct t3dRuntimePageConfig_s {
	String dataDirectory;
	uint64_t materialSourceSignature = 0u;
	uint64_t collisionSurfaceSignature = 0u;
	int colorDivisor = 1;
	int derivedDivisor = 1;
	int middleHeightDivisor = 1;
	int prepareThreads = 2;
	bool bCookedPagesEnabled = false;
	bool bEditorAuthoring = false;
} t3dRuntimePageConfig_t;

typedef struct t3dRuntimePageCookPayload_s {
	Ref<Image> height;
	Ref<Image> pControl;
	Ref<Image> pColor;
	Ref<Image> pDerived;
	Ref<Image> pMiddleHeight;
	Ref<Image> pMiddleComposite;
	Ref<Image> pTerrainShadowMask;
	PackedByteArray holeMask;
	PackedByteArray collisionHeight;
	PackedByteArray collisionBounds;
	PackedByteArray navigation;
	PackedByteArray minMaxHeight;
	PackedByteArray nodeDescriptors;
	PackedByteArray instanceMetadata;
	PackedByteArray lightingReceivers;
	PackedByteArray multimeshFastStart;
	uint32_t materialMask = 1u;
} t3dRuntimePageCookPayload_t;

typedef struct t3dRuntimePageStats_s {
	int nPreparedPages = 0;
	int nPreparedLoadedPages = 0;
	int nPendingPages = 0;
	int nQueuedPages = 0;
	int nPrepareWorkers = 0;
	bool bPrepareJobActive = false;
	uint64_t nPrepareLatencyUsecLast = 0u;
	uint64_t nPrepareQueueWaitUsecLast = 0u;
	uint64_t nPrepareFileOpenUsecLast = 0u;
	uint64_t nPrepareFileReadUsecLast = 0u;
	uint64_t nPrepareValidationUsecLast = 0u;
	uint64_t nPrepareMaterializeUsecLast = 0u;
	uint64_t nPrepareWorkerUsecLast = 0u;
	uint64_t nPrepareReadyWaitUsecLast = 0u;
} t3dRuntimePageStats_t;

typedef struct t3dRuntimePageServiceState_s {
	using IsWantedFn = bool ( * )( void *pContext, const Vector2i &rLocation );

	t3dRuntimePageConfig_t config;
	Vector2i predictedCenter = V2I_MAX;
	uint64_t streamingGeneration = 0u;
	IsWantedFn pIsWanted = nullptr;
	void *pWantedContext = nullptr;
} t3dRuntimePageServiceState_t;

// Owns cooked runtime-page I/O, validation/materialization, merged consumer
// requests, worker preparation, and prepared-page lifetime. Residency consumers
// decide which locations they want; this store performs one physical page read
// that can satisfy all consumers of that location.
typedef struct t3dRuntimePreparedPage_s {
	Vector2i location = V2I_MAX;
	String pagePath;
	String sourcePath;
	uint32_t expectedChannelFlags = 0u;
	uint64_t expectedSourceSignature = 0u;
	int nRegionSize = 0;
	int colorDivisor = 1;
	int derivedDivisor = 1;
	int middleHeightDivisor = 1;
	uint32_t materialMask = 1u;
	uint16_t consumerMask = 0u;
	PackedByteArray heightBytes;
	Ref<Image> height;
	Ref<Image> pControl;
	Ref<Image> pColor;
	Ref<Image> pDerived;
	Ref<Image> pMiddleHeight;
	Ref<Image> pMiddleComposite;
	Ref<Image> pTerrainShadowMask;
	PackedByteArray instancePage;
	PackedByteArray holeMask;
	PackedByteArray collisionHeight;
	PackedByteArray collisionBounds;
	PackedByteArray navigation;
	PackedByteArray minMaxHeight;
	PackedByteArray nodeDescriptors;
	PackedByteArray instanceMetadata;
	PackedByteArray lightingReceivers;
	PackedByteArray multimeshFastStart;
	T3DInstanceStore preparedInstanceStore;
	uint64_t nRequestedUsec = 0u;
	uint64_t nWorkerStartedUsec = 0u;
	uint64_t nFileOpenUsec = 0u;
	uint64_t nFileReadUsec = 0u;
	uint64_t nValidationUsec = 0u;
	uint64_t nMaterializeUsec = 0u;
	uint64_t nWorkerFinishedUsec = 0u;
	bool bAttempted = false;
	bool bLoaded = false;
	bool bInstanceAttempted = false;
	bool bInstanceLoaded = false;
	bool bLoadMinMax = false;
	bool bEditorAuthoring = false;
} t3dRuntimePreparedPage_t;

typedef struct t3dRuntimePrepareJobContext_s {
	t3dRuntimePreparedPage_t *pPages = nullptr;
	int nPageCount = 0;
} t3dRuntimePrepareJobContext_t;

class T3DRuntimePageStore {
public:
	explicit T3DRuntimePageStore(
		T3DRegionRepository *pRepository,
		const T3DRegionPersistence *pPersistence
	);
	~T3DRuntimePageStore( void );
	T3DRuntimePageStore( const T3DRuntimePageStore & ) = delete;
	T3DRuntimePageStore &operator=( const T3DRuntimePageStore & ) = delete;

	void reset( void );
	void resetFrameStats( void );
	void poll( bool bWait );

	String getPagePath( const T3DRegion *pRegion, const String &rDataDirectory ) const;
	uint64_t computeSourceSignature( const T3DRegion *pRegion, const t3dRuntimePageConfig_t &rConfig ) const;
	bool readSourceSignature( const String &rPagePath, uint64_t &nSignature ) const;
	uint32_t readMaterialMask( const String &rPagePath ) const;
	bool removePageForSource( const String &rSourcePath ) const;
	bool removePage( const String &rPagePath ) const;
	bool validatePageForRegion( const T3DRegion *pRegion, const t3dRuntimePageConfig_t &rConfig ) const;
	bool loadCpuMapsNow( const Vector2i &rLocation, const t3dRuntimePageConfig_t &rConfig,
		Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived ) const;
	bool loadCollisionNow( const Vector2i &rLocation, const t3dRuntimePageConfig_t &rConfig,
		PackedByteArray &rHeight, PackedByteArray &rBounds, PackedByteArray &rHoleMask ) const;
	bool loadNavigationNow( const Vector2i &rLocation, const t3dRuntimePageConfig_t &rConfig,
		PackedByteArray &rNavigation, PackedByteArray *pCollisionHeight = nullptr,
		PackedByteArray *pMinMaxHeight = nullptr ) const;
	bool loadLightingReceiversNow( const Vector2i &rLocation, const t3dRuntimePageConfig_t &rConfig,
		PackedByteArray &rReceivers ) const;
	bool writePage(
		T3DRegion *pRegion, const t3dRuntimePageConfig_t &rConfig,
		const t3dRuntimePageCookPayload_t &rPayload, String *pError = nullptr
	) const;

	bool collectPageSources(
		const String &rDataDirectory, PackedStringArray &rSourceFiles,
		eastl::vector<Vector2i> &rLocations, PackedStringArray &rErrors
	) const;

	void request( const Vector2i &rLocation, uint16_t nConsumers, uint8_t nPriority, bool bEnabled );
	void service( const t3dRuntimePageServiceState_t &rState );
	bool isPending( const Vector2i &rLocation ) const;
	bool isPrepared( const Vector2i &rLocation ) const;

	bool takeHeight(
		const Vector2i &rLocation, uint16_t nConsumer, Ref<Image> &rHeight, bool bMiddle,
		PackedByteArray *pNativeHeightBytes = nullptr
	);
	bool takeInstance( const Vector2i &rLocation, T3DInstanceStore &rStore,
		PackedByteArray *pInstanceMetadata = nullptr, PackedByteArray *pMultimeshFastStart = nullptr );
	bool takeCpuMaps( const Vector2i &rLocation, Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived );
	bool takeNodeDescriptors( const Vector2i &rLocation, PackedByteArray &rDescriptors );
	bool takeCollision( const Vector2i &rLocation, PackedByteArray &rHeight, PackedByteArray &rBounds, PackedByteArray &rHoleMask );
	bool takeNavigation( const Vector2i &rLocation, PackedByteArray &rNavigation );
	bool takeTerrainShadowMask( const Vector2i &rLocation, Ref<Image> &rShadowMask );
	bool takeGpuPage(
		const Vector2i &rLocation, Ref<Image> &rHeight,
		Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived,
		Ref<Image> &rMiddleComposite, uint32_t &rMaterialMask, bool &bAttempted,
		uint64_t *pRequestedUsec = nullptr
	);

	t3dRuntimePageStats_t getStats( void ) const;
	EA_FORCE_INLINE int getPreparedPageCount( void ) const { return m_nPreparedPageCount; }
	EA_FORCE_INLINE int getPendingPageCount( void ) const { return m_bPrepareJobActive ? m_nPrepareWorkPageCount : 0; }
	EA_FORCE_INLINE int getQueuedPageCount( void ) const { return m_nQueuedCount; }
	EA_FORCE_INLINE int getPrepareWorkerCount( void ) const { return m_bPrepareJobActive ? m_nPrepareJobWorkers : 0; }
	EA_FORCE_INLINE bool isPrepareJobActive( void ) const { return m_bPrepareJobActive; }
	EA_FORCE_INLINE int getTransientPageCount( void ) const { return m_nPreparedPageCount + m_nPrepareWorkPageCount; }

private:




	T3DRegionRepository *m_pRepository = nullptr;
	const T3DRegionPersistence *m_pPersistence = nullptr;
	eastl::array<t3dRuntimePreparedPage_t, TERRAIN3D_GPU_CACHE_MAX_LAYERS> m_preparedPages{};
	eastl::array<t3dRuntimePreparedPage_t, TERRAIN3D_GPU_CACHE_MAX_LAYERS> m_workPages{};
	eastl::array<int16_t, TERRAIN3D_MAX_REGION_SLOTS> m_preparedByRegion{};
	eastl::array<int16_t, TERRAIN3D_MAX_REGION_SLOTS> m_workByRegion{};
	t3dRuntimePrepareJobContext_t m_prepareContext;
	int m_nPreparedPageCount = 0;
	int m_nPrepareWorkPageCount = 0;
	int64_t m_nPrepareGroupId = godot::WorkerThreadPool::INVALID_TASK_ID;
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
	eastl::array<uint16_t, TERRAIN3D_MAX_REGION_SLOTS> m_queuedConsumers{};
	eastl::array<uint8_t, TERRAIN3D_MAX_REGION_SLOTS> m_queuedPriorities{};
	int m_nQueuedCount = 0;
	uint64_t m_nStreamingGenerationLast = 0u;

	int getPreparedIndex( const Vector2i &rLocation ) const;
	int getWorkIndex( const Vector2i &rLocation ) const;
	bool loadSourceRegion( const Vector2i &rLocation, Ref<T3DRegion> &rSource ) const;
	static bool loadSnapshot( t3dRuntimePreparedPage_t &rPage, bool bPreserveMaterialized = false );
	static void prepareTask( void *pContext, uint32_t iIndex );
	void consumePreparedIndex( int nIndex );
};

#endif // TERRAIN3D_RUNTIME_PAGE_STORE_H
