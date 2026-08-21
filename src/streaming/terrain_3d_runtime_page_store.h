// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RUNTIME_PAGE_STORE_H
#define TERRAIN3D_RUNTIME_PAGE_STORE_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"
#include "instances/terrain_3d_instance_store.h"

#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

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
} t3dRuntimePageStats_t;

typedef struct t3dRuntimePageServiceState_s {
	using IsWantedFn = bool ( * )( void *pContext, const Vector2i &rLocation );

	t3dRuntimePageConfig_t m_config;
	Vector2i m_predictedCenter = s_v2iMax;
	uint64_t m_nStreamingGeneration = 0u;
	IsWantedFn m_isWanted = nullptr;
	void *m_pWantedContext = nullptr;
} t3dRuntimePageServiceState_t;

// Owns cooked runtime-page I/O, validation/materialization, merged consumer
// requests, worker preparation, and prepared-page lifetime. Residency consumers
// decide which locations they want; this store performs one physical page read
// that can satisfy all consumers of that location.
typedef struct t3dRuntimePreparedPage_s {
	Vector2i m_location = s_v2iMax;
	String m_pagePath;
	String m_sourcePath;
	uint32_t m_nExpectedChannelFlags = 0u;
	uint64_t m_nExpectedSourceSignature = 0u;
	int m_nRegionSize = 0;
	int m_nColorDivisor = 1;
	int m_nDerivedDivisor = 1;
	int m_nMiddleHeightDivisor = 1;
	uint32_t m_nMaterialMask = 1u;
	uint16_t m_nConsumerMask = 0u;
	PackedByteArray m_heightBytes;
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
	T3DInstanceStore m_preparedInstanceStore;
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
} t3dRuntimePreparedPage_t;

typedef struct t3dRuntimePrepareJobContext_s {
	t3dRuntimePreparedPage_t *m_pPages = nullptr;
	int m_nPageCount = 0;
} t3dRuntimePrepareJobContext_t;

class T3DRuntimePageStore {
public:
	explicit T3DRuntimePageStore( T3DRegionRepository *pRepository, const T3DRegionPersistence *pPersistence );
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
			PackedByteArray *pCollisionHeight = nullptr,
			PackedByteArray *pMinMaxHeight = nullptr ) const;
	bool loadLightingReceiversNow( const Vector2i &rLocation,
			const t3dRuntimePageConfig_t &rConfig,
			PackedByteArray &rReceivers ) const;
	bool writePage( T3DRegion *pRegion,
			const t3dRuntimePageConfig_t &rConfig,
			const t3dRuntimePageCookPayload_t &rPayload,
			String *pError = nullptr ) const;

	bool collectPageSources( const String &rDataDirectory,
			PackedStringArray &rSourceFiles,
			eastl::vector<Vector2i> &rLocations,
			PackedStringArray &rErrors ) const;

	void request( const Vector2i &rLocation, uint16_t nConsumers, uint8_t nPriority, bool bEnabled );
	void service( const t3dRuntimePageServiceState_t &rState );
	bool isPending( const Vector2i &rLocation ) const;
	bool isPrepared( const Vector2i &rLocation ) const;

	bool takeHeight( const Vector2i &rLocation,
			uint16_t nConsumer,
			Ref<Image> &rHeight,
			bool bMiddle,
			PackedByteArray *pNativeHeightBytes = nullptr );
	bool takeInstance( const Vector2i &rLocation,
			T3DInstanceStore &rStore,
			PackedByteArray *pInstanceMetadata = nullptr,
			PackedByteArray *pMultimeshFastStart = nullptr );
	bool takeCpuMaps( const Vector2i &rLocation,
			Ref<Image> &rHeight,
			Ref<Image> &rControl,
			Ref<Image> &rColor,
			Ref<Image> &rDerived );
	bool takeNodeDescriptors( const Vector2i &rLocation, PackedByteArray &rDescriptors );
	bool takeCollision( const Vector2i &rLocation,
			PackedByteArray &rHeight,
			PackedByteArray &rBounds,
			PackedByteArray &rHoleMask );
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
			uint64_t *pRequestedUsec = nullptr );

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
	T3DRegionRepository *m_pRepository = nullptr;
	const T3DRegionPersistence *m_pPersistence = nullptr;
	eastl::array<t3dRuntimePreparedPage_t, s_nGpuCacheMaxLayers> m_preparedPages{};
	eastl::array<t3dRuntimePreparedPage_t, s_nGpuCacheMaxLayers> m_workPages{};
	eastl::array<int16_t, s_nMaxRegionSlots> m_preparedByRegion{};
	eastl::array<int16_t, s_nMaxRegionSlots> m_workByRegion{};
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
	eastl::array<uint16_t, s_nMaxRegionSlots> m_queuedConsumers{};
	eastl::array<uint8_t, s_nMaxRegionSlots> m_queuedPriorities{};
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
