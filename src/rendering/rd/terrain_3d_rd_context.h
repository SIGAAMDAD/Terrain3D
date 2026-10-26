// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_CONTEXT_H
#define TERRAIN3D_RD_CONTEXT_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "rendering/rd/infrastructure/terrain_3d_render_telemetry.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
using namespace godot;

class Terrain3DRdGpuBuffer;

enum Terrain3DGpuBufferPolicy : uint8_t
{
	T3D_GPU_PUBLICATION,
	T3D_GPU_DIRTY_UPLOAD,
	T3D_GPU_HOT_FRAME,
	T3D_GPU_SCRATCH,
	T3D_GPU_HISTORY,
	T3D_GPU_INDIRECT,
	T3D_GPU_NEUTRAL,
	T3D_GPU_POLICY_COUNT,
};

// Terrain3D's sole owner/coordinator for RenderingDevice objects. Resource
// creation stays in the subsystem which has the construction data, but every
// owned RID must be transferred to this registry immediately after creation.
class Terrain3DRdContext
{
public:
	enum ResourceType : uint8_t
	{
		RESOURCE_UNKNOWN = 0,
		RESOURCE_SAMPLER,
		RESOURCE_SHADER,
		RESOURCE_RASTER_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
		RESOURCE_VERTEX_ARRAY,
		RESOURCE_INDEX_ARRAY,
		RESOURCE_VERTEX_BUFFER,
		RESOURCE_INDEX_BUFFER,
		RESOURCE_STORAGE_BUFFER,
		RESOURCE_UNIFORM_BUFFER,
		RESOURCE_UNIFORM_SET,
		RESOURCE_INDIRECT_BUFFER,
		RESOURCE_CULLING_BUFFER,
		RESOURCE_SHADOW,
		RESOURCE_FRAMEBUFFER,
		RESOURCE_TEXTURE,
		RESOURCE_TYPE_COUNT,
	};

	enum MemoryCategory : uint8_t
	{
		MEMORY_NONE = 0,
		MEMORY_TOPOLOGY_VERTEX,
		MEMORY_TOPOLOGY_INDEX,
		MEMORY_MESH_VERTEX_ARENA,
		MEMORY_MESH_INDEX_ARENA,
		MEMORY_MATERIAL_TABLE,
		MEMORY_INSTANCE,
		MEMORY_HLOD,
		MEMORY_PATCH,
		MEMORY_INDIRECT,
		MEMORY_UNIFORM,
		MEMORY_STORAGE,
		MEMORY_HISTORY,
		MEMORY_SCRATCH,
		MEMORY_TERRAIN_TEXTURE,
		MEMORY_SHADOW,
		MEMORY_CATEGORY_COUNT,
	};

	enum ResourceOwner : uint8_t
	{
		OWNER_UNSPECIFIED = 0,
		OWNER_TERRAIN,
		OWNER_WORLD,
		OWNER_INSTANCES,
		OWNER_HLOD,
		OWNER_OCEAN,
		OWNER_FORWARD_LIGHTING,
		OWNER_VT,
		OWNER_COUNT,
	};
	typedef Terrain3DRenderTelemetry<OWNER_COUNT, T3D_GPU_POLICY_COUNT> RenderTelemetry;
	enum FixedIndirectSlot : uint8_t {
		FIXED_INDIRECT_WORLD,
		FIXED_INDIRECT_OCEAN,
		FIXED_INDIRECT_CLIPMAP,
		FIXED_INDIRECT_SLOT_COUNT,
	};
	static constexpr uint32_t FIXED_INDIRECT_ARENA_BYTES = 4096u;
	static constexpr uint32_t fixedIndirectOffset( FixedIndirectSlot slot ) {
		return slot == FIXED_INDIRECT_WORLD ? 0u : slot == FIXED_INDIRECT_OCEAN ? 128u : 1680u;
	}
	static constexpr uint32_t fixedIndirectCapacity( FixedIndirectSlot slot ) {
		return slot == FIXED_INDIRECT_WORLD ? 128u : slot == FIXED_INDIRECT_OCEAN ? 1536u : 1616u;
	}

	typedef struct ResourceEpoch_s
	{
		uint64_t nCreates = 0u;
		uint64_t nFrees = 0u;
		uint64_t nLive = 0u;
		uint64_t nLiveBytes = 0u;
		uint64_t nSteadyStateViolations = 0u;
	} ResourceEpoch;

	enum ContextState : uint8_t
	{
		STATE_UNINITIALIZED = 0,
		STATE_READY,
		STATE_FAILED,
		STATE_REBUILD_REQUIRED,
		STATE_REBUILDING,
		STATE_SHUTTING_DOWN,
	};

	enum FailureCode : uint8_t
	{
		FAILURE_NONE = 0,
		FAILURE_DEVICE_UNAVAILABLE,
		FAILURE_RESOURCE_CREATION,
		FAILURE_DUPLICATE_RESOURCE,
		FAILURE_UNTRACKED_RESOURCE,
		FAILURE_ACCOUNTING,
		FAILURE_DEVICE_INVALIDATED,
		FAILURE_REBUILD,
		FAILURE_RENDER_PASS,
	};

	enum SuccessfulPass : uint8_t
	{
		PASS_NONE = 0,
		PASS_TERRAIN_CLIPMAP,
		PASS_COUNT,
	};

private:
	typedef struct t3dRdResourceRecord_s
	{
		RID m_hRid;
		String m_name;
		ResourceType m_type = RESOURCE_UNKNOWN;
		MemoryCategory m_memoryCategory = MEMORY_NONE;
		ResourceOwner m_owner = OWNER_UNSPECIFIED;
		uint64_t m_nBytes = 0u;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdResourceRecord_t;

	typedef eastl::fixed_vector<t3dRdResourceRecord_t, s_nResourceCapacity, false> t3dRdResourceList_t;

	typedef struct t3dRdResourceHistory_s
	{
		String m_name;
		ResourceType m_type = RESOURCE_UNKNOWN;
	} t3dRdResourceHistory_t;

	typedef struct t3dRdFreedResource_s
	{
		uint64_t m_nRid = 0u;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdFreedResource_t;

	typedef struct t3dRdVertexFormatRecord_s
	{
		int64_t m_nFormat = RenderingDevice::INVALID_FORMAT_ID;
		String m_name;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdVertexFormatRecord_t;

	t3dAtomic_t<RenderingDevice *> m_pRd { NULL };
	t3dAtomic_t<ContextState> m_state { STATE_UNINITIALIZED };
	t3dAtomic_t<FailureCode> m_failureCode { FAILURE_NONE };
	mutable t3dMutex_t m_resourceMutex;
	// The RD registry is a render-lifetime control structure, not an unbounded
	// general-purpose container. Keep it wholly inline so resource churn cannot
	// invoke the process heap. Capacity failures are explicit and fail closed.

	t3dRdResourceList_t m_resources;
	eastl::fixed_vector<t3dRdResourceHistory_t, s_nResourceHistoryCapacity, false> m_resourceHistory;
	size_t m_nResourceHistoryCursor = 0u;
	eastl::fixed_vector<t3dRdFreedResource_t, s_nFreedHistoryCapacity, false> m_freedResources;
	eastl::fixed_vector<t3dRdVertexFormatRecord_t, s_nVertexFormatCapacity, false> m_vertexFormats;
	String m_lastFailure;
	String m_lastRecoverableFailure;
	t3dAtomic_t<SuccessfulPass> m_lastSuccessfulPass { PASS_NONE };

	t3dAtomicU64_t m_nDeviceGeneration { 0u };
	t3dAtomicU64_t m_nDeviceInitializations { 0u };
	t3dAtomicU64_t m_nDeviceRebuilds { 0u };
	t3dAtomicU64_t m_nDeviceInvalidations { 0u };
	t3dAtomicU64_t m_nCreatedResources { 0u };
	t3dAtomicU64_t m_nFreedResources { 0u };
	t3dAtomicU64_t m_nAbandonedResources { 0u };
	t3dAtomicU64_t m_nFailedResourceCreates { 0u };
	t3dAtomicU64_t m_nDoubleFreeAttempts { 0u };
	t3dAtomicU64_t m_nUntrackedFreeAttempts { 0u };
	t3dAtomicU64_t m_nAccountingErrors { 0u };
	t3dAtomicU64_t m_nRecoverableFailureCount { 0u };
	t3dAtomicU64_t m_nLiveBufferBytes { 0u };
	t3dAtomicU64_t m_nPeakBufferBytes { 0u };
	t3dAtomicU64_t m_nLiveTextureBytes { 0u };
	t3dAtomicU64_t m_nPeakTextureBytes { 0u };
	t3dAtomicU64_t m_szTypeCreates[ RESOURCE_TYPE_COUNT ] {};
	t3dAtomicU64_t m_szTypeFrees[ RESOURCE_TYPE_COUNT ] {};
	t3dAtomicU64_t m_szTypeLive[ RESOURCE_TYPE_COUNT ] {};
	t3dAtomicU64_t m_szLiveMemory[ MEMORY_CATEGORY_COUNT ] {};
	t3dAtomicU64_t m_szPeakMemory[ MEMORY_CATEGORY_COUNT ] {};
	t3dAtomicU64_t m_szOwnerCreates[ OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerFrees[ OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerLive[ OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerLiveBytes[ OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerSteadyStateViolations[ OWNER_COUNT ] {};
	t3dAtomicU64_t m_nPipelineCreates { 0u };
	t3dAtomicU64_t m_nPipelineRebuilds { 0u };
	t3dAtomicU64_t m_nUniformSetCreates { 0u };
	t3dAtomicU64_t m_nUniformSetRebuilds { 0u };
	t3dAtomicBool_t m_bWarmupComplete { false };
	t3dAtomicU32_t m_nExplicitRebuildDepth { 0u };
	t3dAtomicU64_t m_nSteadyStateAllocationViolations { 0u };

	t3dAtomicU64_t m_nGpuFrameId { 0u };
	t3dAtomicU64_t m_nSnapshotGeneration { 0u };
	t3dAtomicU64_t m_nLastSuccessfulGpuFrame { 0u };
	t3dAtomicU64_t m_nBufferUpdates { 0u };
	t3dAtomicU64_t m_nBytesUploaded { 0u };
	t3dAtomicU64_t m_nFrameBufferUpdates { 0u };
	t3dAtomicU64_t m_nFrameBytesUploaded { 0u };
	RenderTelemetry m_renderTelemetry;
	Terrain3DRdGpuBuffer *m_pFixedIndirectArena = NULL;
	uint32_t m_nFixedIndirectReferences[FIXED_INDIRECT_SLOT_COUNT]{};
	t3dAtomicU64_t m_nTerrainPatchCount { 0u };
	t3dAtomicU64_t m_nVisibleTerrainPatches { 0u };
	t3dAtomicU64_t m_nStaticInstanceCount { 0u };
	t3dAtomicU64_t m_nVisibleInstances { 0u };
	t3dAtomicU64_t m_nHlodCount { 0u };
	t3dAtomicU64_t m_nVisibleHlods { 0u };
	t3dAtomicU64_t m_nIndirectCommandCount { 0u };
	t3dAtomicU64_t m_nComputeDispatchCount { 0u };

	static ResourceType inferResourceType( const String& rName );
	static MemoryCategory inferMemoryCategory( const String& rName, ResourceType type, uint64_t nBytes );
	static ResourceOwner inferResourceOwner( const String& rName );
	static bool isPipeline( ResourceType type );
	static bool isSteadyStateAllocation( ResourceType type );
	static void updatePeak( t3dAtomicU64_t& rPeak, uint64_t nLiveBytes );
	static bool subtractSaturating( t3dAtomicU64_t& rValue, uint64_t nAmount );
	void rememberFreedResource( uint64_t nRid, uint64_t nDeviceGeneration );
	void updateBufferPeak( uint64_t nLiveBytes );
	void updateTexturePeak( uint64_t nLiveBytes );
	void accountRegistration( const t3dRdResourceRecord_t& rRecord, bool bRebuild );
	void accountRelease( const t3dRdResourceRecord_t& rRecord );
	void resetLiveAccounting( void );
	void setFailure( FailureCode code, const String& rFailure );
	bool hasResourceHistory( const String& rName, ResourceType type ) const;
	bool registerResourceInternal(
	    const RID& rRid, const String& rName, ResourceType type, MemoryCategory memoryCategory, uint64_t nBytes, ResourceOwner owner );

public:
	static Terrain3DRdContext& getShared( void );
	static void shutdownShared( void );
	// Fixed command slices share one physical indirect-capable storage buffer.
	// Each consumer retains a reference until its own shutdown.
	bool acquireFixedIndirect( FixedIndirectSlot slot, uint32_t nRequiredBytes, Terrain3DRdGpuBuffer *&rBuffer, uint32_t &rOffsetBytes );
	void releaseFixedIndirect( FixedIndirectSlot slot, Terrain3DRdGpuBuffer *&rBuffer );

	// The sole engine rendering gateway permitted in the RD branch. Godot's
	// GDExtension API exposes global/local RenderingDevice acquisition and
	// render-thread scheduling through this boundary.
	static RenderingDevice *getGlobalDevice( void );
	static RenderingDevice *createLocalDevice( void );
	// Owner/Godot-thread identity is distinct from render-thread identity. In
	// single-threaded rendering Godot legitimately reports the main thread as
	// the render thread too, so owner assertions must use this main-thread test.
	static bool isOnOwnerThread( void );
	static bool isOnRenderThread( void );
	static bool enqueueOnRenderThread( const Callable& rCallable );
	static void forceSync( void );

	Terrain3DRdContext( void ) = default;
	~Terrain3DRdContext( void );

	Terrain3DRdContext( const Terrain3DRdContext& ) = delete;
	Terrain3DRdContext& operator=( const Terrain3DRdContext& ) = delete;

	bool initialize( void );
	bool validateDevice( void );
	void shutdown( void );
	void invalidateDevice( const String& rReason );
	bool beginRebuild( void );
	bool completeRebuild( bool bSuccess, const String& rFailure = String() );
	void recordFailure( FailureCode code, const String& rFailure );
	void recordResourceCreationFailure( const String& rName, ResourceType type, const String& rFailure = String() );
	void beginRecoverableFailureScope( void );
	void endRecoverableFailureScope( void );

	// Compatibility entry point. New owners should use the typed overload so
	// accounting never depends on a debug-name convention.
	void registerResource( const RID& rRid, const String& rName, uint64_t nBufferBytes = 0u );
	bool registerResource(
	    const RID& rRid, const String& rName, ResourceType type, MemoryCategory memoryCategory, uint64_t nBytes, ResourceOwner owner );
	bool registerVertexFormat( int64_t nFormat, const String& rName );
	void releaseVertexFormat( int64_t& rFormat );
	void freeResource( RID& rRid, uint64_t nBufferBytes = 0u );
	bool ownsResource( const RID& rRid ) const;
	ResourceEpoch getResourceEpoch( ResourceOwner owner ) const;

	void markWarmupComplete( bool bComplete = true );
	void beginResourceRebuild( void );
	void endResourceRebuild( void );
	void beginFrame( uint64_t nGpuFrameId, uint64_t nSnapshotGeneration );
	void recordBufferUpdate( uint64_t nBytes,
	        ResourceOwner owner = OWNER_UNSPECIFIED,
	        Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL );
	void recordRenderEvent( ResourceOwner owner, RenderTelemetry::Event event );
	void recordRenderCapacity( ResourceOwner owner, Terrain3DGpuBufferPolicy policy, uint64_t nBytes );
	void recordRenderIndirectCommandsSubmitted( ResourceOwner owner, uint64_t nCommands );
	uint64_t getRenderEvent( ResourceOwner owner, RenderTelemetry::Event event ) const;
	uint64_t getRenderUploadCalls( ResourceOwner owner, Terrain3DGpuBufferPolicy policy ) const;
	uint64_t getRenderUploadBytes( ResourceOwner owner, Terrain3DGpuBufferPolicy policy ) const;
	uint64_t getRenderDirtyRangesUploaded( ResourceOwner owner ) const;
	void recordSuccessfulPass( SuccessfulPass pass, uint64_t nGpuFrameId );
	void recordSceneCounts( uint64_t nTerrainPatches,
	                        uint64_t nVisibleTerrainPatches,
	                        uint64_t nStaticInstances,
	                        uint64_t nVisibleInstances,
	                        uint64_t nHlods,
	                        uint64_t nVisibleHlods );
	void recordSubmissionCounts( uint64_t nIndirectCommands, uint64_t nComputeDispatches );
	Dictionary getDiagnostics( void ) const;
	EA_FORCE_INLINE uint64_t getLiveGpuBufferBytes( void ) const
	{
		return m_nLiveBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getLiveGpuTextureBytes( void ) const
	{
		return m_nLiveTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}

	EA_FORCE_INLINE RenderingDevice *getDevice( void ) const
	{
		const ContextState state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
		return ( state == STATE_READY || state == STATE_REBUILDING ) ? m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) : NULL;
	}
	EA_FORCE_INLINE bool isReady( void ) const
	{
		return m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) == STATE_READY && m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) != NULL;
	}
	EA_FORCE_INLINE bool hasFailure( void ) const
	{
		return m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE ) != FAILURE_NONE;
	}
	EA_FORCE_INLINE ContextState getState( void ) const
	{
		return m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE FailureCode getFailureCode( void ) const
	{
		return m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getDeviceGeneration( void ) const
	{
		return m_nDeviceGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getLiveBufferBytes( void ) const
	{
		return m_nLiveBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getLiveBytes( const MemoryCategory category ) const
	{
		return category < MEMORY_CATEGORY_COUNT ? m_szLiveMemory[ category ].load( T3D_MEMORY_ORDER_ACQUIRE ) : 0u;
	}
};

static_assert( Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_WORLD )
                       + Terrain3DRdContext::fixedIndirectCapacity( Terrain3DRdContext::FIXED_INDIRECT_WORLD )
                       <= Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_OCEAN ),
        "World/Ocean indirect slices overlap" );
static_assert( Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_OCEAN )
                       + Terrain3DRdContext::fixedIndirectCapacity( Terrain3DRdContext::FIXED_INDIRECT_OCEAN )
                       <= Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP ),
        "Ocean/clipmap indirect slices overlap" );
static_assert( Terrain3DRdContext::fixedIndirectOffset( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP )
                       + Terrain3DRdContext::fixedIndirectCapacity( Terrain3DRdContext::FIXED_INDIRECT_CLIPMAP )
                       <= Terrain3DRdContext::FIXED_INDIRECT_ARENA_BYTES,
        "Fixed indirect slices exceed their buffer" );

#endif // TERRAIN3D_RD_CONTEXT_H
