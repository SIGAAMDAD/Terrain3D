// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_CONTEXT_H
#define TERRAIN3D_RD_CONTEXT_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "rendering/rd/infrastructure/telemetry/terrain_3d_render_telemetry.h"
#include "rendering/rd/core/terrain_3d_rd_context_contract.h"
#include "rendering/rd/core/terrain_3d_rd_context_access.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
using namespace godot;



typedef Terrain3DRenderTelemetry<T3D_RD_CONTEXT_OWNER_COUNT, T3D_GPU_POLICY_COUNT> t3dRdRenderTelemetry_t;

// Terrain3D's sole owner/coordinator for RenderingDevice objects. Resource
// creation stays in the subsystem which has the construction data, but every
// owned RID must be transferred to this registry immediately after creation.
class Terrain3DRdContext : public Terrain3DRdContextAccess
{
private:
	typedef struct t3dRdResourceRecord_s
	{

		RID m_hRid;
		String m_name;
		t3dRdResourceType_t m_type = T3D_RD_CONTEXT_RESOURCE_UNKNOWN;
		t3dRdMemoryCategory_t m_memoryCategory = T3D_RD_CONTEXT_MEMORY_NONE;
		t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
		uint64_t m_nBytes = 0u;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdResourceRecord_t;


	typedef eastl::fixed_vector<t3dRdResourceRecord_t, T3D_RESOURCE_CAPACITY, false> t3dRdResourceList_t;

	typedef struct t3dRdResourceHistory_s
	{

		String m_name;
		t3dRdResourceType_t m_type = T3D_RD_CONTEXT_RESOURCE_UNKNOWN;
	} t3dRdResourceHistory_t;
typedef struct t3dRdVertexFormatRecord_s
	{

		int64_t m_nFormat = RenderingDevice::INVALID_FORMAT_ID;
		String m_name;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdVertexFormatRecord_t;


	Terrain3DAtomic<RenderingDevice *> m_pRd { NULL };
	Terrain3DAtomic<t3dRdContextState_t> m_state { T3D_RD_CONTEXT_STATE_UNINITIALIZED };
	Terrain3DAtomic<t3dRdFailureCode_t> m_failureCode { T3D_RD_CONTEXT_FAILURE_NONE };
	mutable Terrain3DMutex m_resourceMutex;
	// The RD registry is a render-lifetime control structure, not an unbounded
	// general-purpose container. Keep it wholly inline so resource churn cannot
	// invoke the process heap. Capacity failures are explicit and fail closed.

	t3dRdResourceList_t m_resources;
	eastl::fixed_vector<t3dRdResourceHistory_t, T3D_RESOURCE_HISTORY_CAPACITY, false> m_resourceHistory;
	size_t m_nResourceHistoryCursor = 0u;
	eastl::fixed_vector<t3dRdFreedResource_t, T3D_FREED_HISTORY_CAPACITY, false> m_freedResources;
	eastl::fixed_vector<t3dRdVertexFormatRecord_t, T3D_VERTEX_FORMAT_CAPACITY, false> m_vertexFormats;
	String m_lastFailure;
	String m_lastRecoverableFailure;
	Terrain3DAtomic<t3dRdSuccessfulPass_t> m_lastSuccessfulPass { T3D_RD_CONTEXT_PASS_NONE };

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
	t3dAtomicU64_t m_szTypeCreates[ T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT ] {};
	t3dAtomicU64_t m_szTypeFrees[ T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT ] {};
	t3dAtomicU64_t m_szTypeLive[ T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT ] {};
	t3dAtomicU64_t m_szLiveMemory[ T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT ] {};
	t3dAtomicU64_t m_szPeakMemory[ T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT ] {};
	t3dAtomicU64_t m_szOwnerCreates[ T3D_RD_CONTEXT_OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerFrees[ T3D_RD_CONTEXT_OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerLive[ T3D_RD_CONTEXT_OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerLiveBytes[ T3D_RD_CONTEXT_OWNER_COUNT ] {};
	t3dAtomicU64_t m_szOwnerSteadyStateViolations[ T3D_RD_CONTEXT_OWNER_COUNT ] {};
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
	t3dRdRenderTelemetry_t m_renderTelemetry;
	Terrain3DRdGpuBuffer *m_pFixedIndirectArena = NULL;
	uint32_t m_nFixedIndirectReferences[T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT]{};
	t3dAtomicU64_t m_nTerrainPatchCount { 0u };
	t3dAtomicU64_t m_nVisibleTerrainPatches { 0u };
	t3dAtomicU64_t m_nStaticInstanceCount { 0u };
	t3dAtomicU64_t m_nVisibleInstances { 0u };
	t3dAtomicU64_t m_nHlodCount { 0u };
	t3dAtomicU64_t m_nVisibleHlods { 0u };
	t3dAtomicU64_t m_nIndirectCommandCount { 0u };
	t3dAtomicU64_t m_nComputeDispatchCount { 0u };

	static t3dRdResourceType_t inferResourceType( const String& rName );
	static t3dRdMemoryCategory_t inferMemoryCategory( const String& rName, t3dRdResourceType_t type, uint64_t nBytes );
	static t3dRdResourceOwner_t inferResourceOwner( const String& rName );
	static bool isPipeline( t3dRdResourceType_t type );
	static bool isSteadyStateAllocation( t3dRdResourceType_t type );
	static void updatePeak( t3dAtomicU64_t& rPeak, uint64_t nLiveBytes );
	static bool subtractSaturating( t3dAtomicU64_t& rValue, uint64_t nAmount );
	void rememberFreedResource( uint64_t nRid, uint64_t nDeviceGeneration );
	void updateBufferPeak( uint64_t nLiveBytes );
	void updateTexturePeak( uint64_t nLiveBytes );
	void accountRegistration( const t3dRdResourceRecord_t& rRecord, bool bRebuild );
	void accountRelease( const t3dRdResourceRecord_t& rRecord );
	void resetLiveAccounting( void );
	void setFailure( t3dRdFailureCode_t code, const String& rFailure );
	bool hasResourceHistory( const String& rName, t3dRdResourceType_t type ) const;
	bool registerResourceInternal(
	    const RID& rRid, const String& rName, t3dRdResourceType_t type, t3dRdMemoryCategory_t memoryCategory, uint64_t nBytes, t3dRdResourceOwner_t owner );

public:
	static Terrain3DRdContext& getShared( void );
	static void shutdownShared( void );
	// Fixed command slices share one physical indirect-capable storage buffer.
	// Each consumer retains a reference until its own shutdown.
	bool acquireFixedIndirect( t3dRdFixedIndirectSlot_t slot, uint32_t nRequiredBytes, Terrain3DRdGpuBuffer *&rBuffer, uint32_t &rOffsetBytes );
	void releaseFixedIndirect( t3dRdFixedIndirectSlot_t slot, Terrain3DRdGpuBuffer *&rBuffer );

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
	void recordFailure( t3dRdFailureCode_t code, const String& rFailure );
	void recordResourceCreationFailure( const String& rName, t3dRdResourceType_t type, const String& rFailure = String() );
	void beginRecoverableFailureScope( void );
	void endRecoverableFailureScope( void );

	// Compatibility entry point. New owners should use the typed overload so
	// accounting never depends on a debug-name convention.
	void registerResource( const RID& rRid, const String& rName, uint64_t nBufferBytes = 0u );
	bool registerResource(
	    const RID& rRid, const String& rName, t3dRdResourceType_t type, t3dRdMemoryCategory_t memoryCategory, uint64_t nBytes, t3dRdResourceOwner_t owner ) override;
	bool registerVertexFormat( int64_t nFormat, const String& rName );
	void releaseVertexFormat( int64_t& rFormat );
	void freeResource( RID& rRid, uint64_t nBufferBytes = 0u ) override;
	bool ownsResource( const RID& rRid ) const;
	t3dRdResourceEpoch_t getResourceEpoch( t3dRdResourceOwner_t owner ) const;

	void markWarmupComplete( bool bComplete = true );
	void beginResourceRebuild( void );
	void endResourceRebuild( void );
	void beginFrame( uint64_t nGpuFrameId, uint64_t nSnapshotGeneration );
	void recordBufferUpdate( uint64_t nBytes,
	        t3dRdResourceOwner_t owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED,
	        Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) override;
	void recordRenderEvent( t3dRdResourceOwner_t owner, t3dRenderTelemetryEvent_t event ) override;
	void recordRenderCapacity( t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy, uint64_t nBytes ) override;
	void recordRenderIndirectCommandsSubmitted( t3dRdResourceOwner_t owner, uint64_t nCommands );
	uint64_t getRenderEvent( t3dRdResourceOwner_t owner, t3dRenderTelemetryEvent_t event ) const;
	uint64_t getRenderUploadCalls( t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy ) const;
	uint64_t getRenderUploadBytes( t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy ) const;
	uint64_t getRenderDirtyRangesUploaded( t3dRdResourceOwner_t owner ) const;
	void recordSuccessfulPass( t3dRdSuccessfulPass_t pass, uint64_t nGpuFrameId );
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

	EA_FORCE_INLINE RenderingDevice *getDevice( void ) const override
	{
		const t3dRdContextState_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
		return ( state == T3D_RD_CONTEXT_STATE_READY || state == T3D_RD_CONTEXT_STATE_REBUILDING ) ? m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) : NULL;
	}
	EA_FORCE_INLINE bool isReady( void ) const
	{
		return m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) == T3D_RD_CONTEXT_STATE_READY && m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) != NULL;
	}
	EA_FORCE_INLINE bool hasFailure( void ) const
	{
		return m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE ) != T3D_RD_CONTEXT_FAILURE_NONE;
	}
	EA_FORCE_INLINE t3dRdContextState_t getState( void ) const
	{
		return m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE t3dRdFailureCode_t getFailureCode( void ) const
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
	EA_FORCE_INLINE uint64_t getLiveBytes( const t3dRdMemoryCategory_t category ) const
	{
		return category < T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT ? m_szLiveMemory[ category ].load( T3D_MEMORY_ORDER_ACQUIRE ) : 0u;
	}
};

static_assert( t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD )
                       + t3d_rd_context_contract::fixedIndirectCapacity( T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD )
                       <= t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN ),
        "World/Ocean indirect slices overlap" );
static_assert( t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN )
                       + t3d_rd_context_contract::fixedIndirectCapacity( T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN )
                       <= t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP ),
        "Ocean/clipmap indirect slices overlap" );
static_assert( t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP )
                       + t3d_rd_context_contract::fixedIndirectCapacity( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP )
                       <= T3D_RD_FIXED_INDIRECT_ARENA_BYTES,
        "Fixed indirect slices exceed their buffer" );

#endif // TERRAIN3D_RD_CONTEXT_H
