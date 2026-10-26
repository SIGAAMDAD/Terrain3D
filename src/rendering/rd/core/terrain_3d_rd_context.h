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
#include "rendering/rd/core/terrain_3d_rd_rid_index.h"
#include "rendering/rd/core/terrain_3d_hot_dirty_image.h"
#include "rendering/rd/core/terrain_3d_hot_range_abi.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "Terrain3DTypes.h"
#include <stddef.h>

// One immutable record per object/native pass. All offsets select elements
// relative to the backing, including Godot's internally selected HOT region.
struct alignas(
	16 ) Terrain3DRdHotRecord
{
	t3dRdFrameUniforms_t		   terrain{};
	t3dWorldFrame_t				   world{};
	t3dRdEditorOverlayConfig_t	   editor{};
	t3dRdPatch_t				   patches[T3D_MAX_CLIPMAP_LEVELS]{};
	t3dWorldTerrainClusterBounds_t bounds[T3D_MAX_CLIPMAP_LEVELS]{};
	uint32_t					   world_bases[20]{};
	uint32_t					   terrain_bases[4]{};
	uint32_t					   scratch_bases[8]{};
	uint32_t					   cpu_visible[64]{};
	uint32_t					   cpu_indexed[20]{};
	uint32_t					   cpu_strip[384]{};
};
static_assert( offsetof( Terrain3DRdHotRecord, world ) == 528u, "HOT terrain ABI" );
static_assert( offsetof( Terrain3DRdHotRecord, editor ) == 864u, "HOT world ABI" );
static_assert( offsetof( Terrain3DRdHotRecord, cpu_visible ) == 2464u, "HOT CPU cull ABI" );
static_assert( sizeof( Terrain3DRdHotRecord ) == 4336u, "HOT std430 stride" );

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
using namespace godot;

typedef Terrain3DRenderTelemetry< T3D_RD_CONTEXT_OWNER_COUNT, T3D_GPU_POLICY_COUNT > t3dRdRenderTelemetry_t;

enum t3dRdForceSyncReason_t : uint8_t
{
	T3D_RD_FORCE_SYNC_OTHER = 0,
	T3D_RD_FORCE_SYNC_CONTROL = 1,
	T3D_RD_FORCE_SYNC_DIAGNOSTICS = 2
};

class Terrain3DRdGlobalDescriptorCache;
enum t3dRdDescriptorStatus_t : uint8_t;

// Terrain3D's sole owner/coordinator for RenderingDevice objects. Resource
// creation stays in the subsystem which has the construction data, but every
// owned RID must be transferred to this registry immediately after creation.
class Terrain3DRdContext : public Terrain3DRdContextAccess
{
private:
	typedef struct t3dRdResourceRecord_s
	{
		RID					  m_hRid;
		String				  m_name;
		t3dRdResourceType_t	  m_type = T3D_RD_CONTEXT_RESOURCE_UNKNOWN;
		t3dRdMemoryCategory_t m_memoryCategory = T3D_RD_CONTEXT_MEMORY_NONE;
		t3dRdResourceOwner_t  m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
		uint64_t			  m_nBytes = 0u;
		uint64_t			  m_nDeviceGeneration = 0u;
		uint64_t			  m_nCreationOrder = 0u;
	} t3dRdResourceRecord_t;

	typedef eastl::fixed_vector< t3dRdResourceRecord_t, T3D_RESOURCE_CAPACITY, false > t3dRdResourceList_t;

	typedef struct t3dRdResourceHistory_s
	{
		String				m_name;
		t3dRdResourceType_t m_type = T3D_RD_CONTEXT_RESOURCE_UNKNOWN;
	} t3dRdResourceHistory_t;
	typedef struct t3dRdVertexFormatRecord_s
	{
		int64_t	 m_nFormat = RenderingDevice::INVALID_FORMAT_ID;
		String	 m_name;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdVertexFormatRecord_t;

	Terrain3DRdGlobalDescriptorCache	  *m_pGlobalDescriptorCache = nullptr;
	Terrain3DAtomic< RenderingDevice * >   m_pRd{ NULL };
	Terrain3DAtomic< t3dRdContextState_t > m_state{ T3D_RD_CONTEXT_STATE_UNINITIALIZED };
	Terrain3DAtomic< t3dRdFailureCode_t >  m_failureCode{ T3D_RD_CONTEXT_FAILURE_NONE };
	mutable Terrain3DMutex				   m_resourceMutex;
	// The RD registry is a render-lifetime control structure, not an unbounded
	// general-purpose container. Keep it wholly inline so resource churn cannot
	// invoke the process heap. Capacity failures are explicit and fail closed.

	t3dRdResourceList_t m_resources;
	Terrain3DRdRidIndex< T3D_RESOURCE_CAPACITY > m_ridIndex;
	uint64_t m_nResourceCreationOrder = 0u;
	uint64_t			m_physicalBufferCreates[T3D_RD_CONTEXT_OWNER_COUNT][3]{};
	uint64_t			m_physicalBufferReleases[T3D_RD_CONTEXT_OWNER_COUNT][3]{};
	static int			bufferKind(
				 t3dRdResourceType_t type ) {
		return type == T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER													  ? 0
			   : type == T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER												  ? 2
			   : type == T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER || type == T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER ? 1
																												  : -1;
	}
	eastl::fixed_vector< t3dRdResourceHistory_t, T3D_RESOURCE_HISTORY_CAPACITY, false > m_resourceHistory;
	size_t																				m_nResourceHistoryCursor = 0u;
	eastl::fixed_vector< t3dRdFreedResource_t, T3D_FREED_HISTORY_CAPACITY, false >		m_freedResources;
	eastl::fixed_vector< t3dRdVertexFormatRecord_t, T3D_VERTEX_FORMAT_CAPACITY, false > m_vertexFormats;
	String																				m_lastFailure;
	String																				m_lastRecoverableFailure;
	Terrain3DAtomic< t3dRdSuccessfulPass_t > m_lastSuccessfulPass{ T3D_RD_CONTEXT_PASS_NONE };

	t3dAtomicU64_t	m_nDeviceGeneration{ 0u };
	t3dAtomicU64_t	m_nDeviceInitializations{ 0u };
	t3dAtomicU64_t	m_nDeviceRebuilds{ 0u };
	t3dAtomicU64_t	m_nDeviceInvalidations{ 0u };
	t3dAtomicU64_t	m_nCreatedResources{ 0u };
	t3dAtomicU64_t	m_nFreedResources{ 0u };
	t3dAtomicU64_t	m_nAbandonedResources{ 0u };
	t3dAtomicU64_t	m_nFailedResourceCreates{ 0u };
	t3dAtomicU64_t	m_nDoubleFreeAttempts{ 0u };
	t3dAtomicU64_t	m_nUntrackedFreeAttempts{ 0u };
	t3dAtomicU64_t	m_nAccountingErrors{ 0u };
	t3dAtomicU64_t	m_nRecoverableFailureCount{ 0u };
	t3dAtomicU64_t	m_nLiveBufferBytes{ 0u };
	t3dAtomicU64_t	m_nPeakBufferBytes{ 0u };
	t3dAtomicU64_t	m_nLiveTextureBytes{ 0u };
	t3dAtomicU64_t	m_nPeakTextureBytes{ 0u };
	t3dAtomicU64_t	m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT]{};
	t3dAtomicU64_t	m_szTypeFrees[T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT]{};
	t3dAtomicU64_t	m_szTypeLive[T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT]{};
	t3dAtomicU64_t	m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT]{};
	t3dAtomicU64_t	m_szPeakMemory[T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT]{};
	t3dAtomicU64_t	m_szOwnerCreates[T3D_RD_CONTEXT_OWNER_COUNT]{};
	t3dAtomicU64_t	m_szOwnerFrees[T3D_RD_CONTEXT_OWNER_COUNT]{};
	t3dAtomicU64_t	m_szOwnerLive[T3D_RD_CONTEXT_OWNER_COUNT]{};
	t3dAtomicU64_t	m_szOwnerLiveBytes[T3D_RD_CONTEXT_OWNER_COUNT]{};
	t3dAtomicU64_t	m_szOwnerSteadyStateViolations[T3D_RD_CONTEXT_OWNER_COUNT]{};
	t3dAtomicU64_t	m_nPipelineCreates{ 0u };
	t3dAtomicU64_t	m_nPipelineRebuilds{ 0u };
	t3dAtomicU64_t	m_nUniformSetCreates{ 0u };
	t3dAtomicU64_t	m_nUniformSetRebuilds{ 0u };
	t3dAtomicBool_t m_bWarmupComplete{ false };
	t3dAtomicU32_t	m_nExplicitRebuildDepth{ 0u };
	t3dAtomicU64_t	m_nSteadyStateAllocationViolations{ 0u };

	t3dAtomicU64_t		   m_nGpuFrameId{ 0u };
	t3dAtomicU64_t		   m_nSnapshotGeneration{ 0u };
	t3dAtomicU64_t		   m_nLastSuccessfulGpuFrame{ 0u };
	t3dAtomicU64_t		   m_nBufferUpdates{ 0u };
	t3dAtomicU64_t		   m_nBytesUploaded{ 0u };
	t3dAtomicU64_t		   m_nFrameBufferUpdates{ 0u };
	t3dAtomicU64_t		   m_nFrameBytesUploaded{ 0u };
	t3dAtomicU64_t		   m_nTextureUploads{ 0u };
	t3dAtomicU64_t		   m_nTextureBytesUploaded{ 0u };
	t3dRdRenderTelemetry_t m_renderTelemetry;
	Terrain3DRdGpuBuffer   m_sharedArenas[3];
	uint32_t			   m_arenaCursor[3] = { 0u, 0u, T3D_RD_FIXED_INDIRECT_ARENA_BYTES };
	// COLD control plane: keep each live span leased, and recycle its exact
	// physical extent only after final render-thread owner teardown. A lane
	// recovery retains the existing lease; it must not return it to the pool.
	static constexpr uint32_t MAX_SHARED_ARENA_LEASES = 128u;
	struct SharedArenaLease
	{
		uint32_t m_nArena = 0u;
		uint32_t m_nOffset = 0u;
		uint32_t m_nBytes = 0u;
		uint32_t m_nGeneration = 0u;
		bool m_bLive = false;
	};
	mutable Terrain3DMutex m_arenaMutex;
	SharedArenaLease m_arenaLeases[MAX_SHARED_ARENA_LEASES]{};
	String m_lastArenaReservationFailure;
	uint64_t m_nArenaReservationFailures = 0u;
	uint64_t m_nArenaLeaseReuses = 0u;
	PackedByteArray		   m_hotImage;
	Terrain3DHotDirtyImage m_hotDirty;
	bool				   m_hotRangeAbi2 = false, m_hotRangeActive = false;
	// Last completed transaction (never lifetime-scaled as frame cost), plus lifetime
	// event counters. All writes occur on the RenderingDevice render thread.
	uint64_t m_hotDirtyBytesTotal = 0u, m_hotUploadBytesTotal = 0u, m_hotFlushBytesTotal = 0u;
	uint64_t m_hotNoopFrames = 0u, m_hotFullInitializations = 0u, m_hotRecoveries = 0u;
	uint64_t m_hotFailedPublications = 0u, m_hotLegacyPublications = 0u;
	uint32_t m_hotLastDirtyBytes = 0u, m_hotLastDirtyRanges = 0u, m_hotLastDirtyBlocks = 0u;
	uint32_t m_hotLastUploadBytes = 0u, m_hotLastFlushBytes = 0u, m_hotLastInitializedBlocks = 0u;
	uint32_t m_hotLastSlot = UINT32_MAX;
	void	 recordHotDirtyOutcome( bool success, bool persistent, uint32_t upload_bytes, uint32_t flush_bytes );
	void	 invalidateHotMirrors();
	// Cached after cold allocation: HOT record writes never call ptrw() or resize.
	uint8_t					  *m_pHotImageData = NULL;
	Terrain3DMemoryReservation m_hotImageMemory;
	Object					  *m_hotBridge = NULL;
	uint64_t				   m_hotRenderFrame = UINT64_MAX, m_hotDeviceFrame = 0u;
	uint32_t				   m_hotCount = 0u, m_hotIndex = UINT32_MAX, m_hotSlotBase = 0u;
	uint32_t				   m_nHotPeakRecords = 0u;
	uint64_t				   m_hotCopiedBytes = 0u;
	uint32_t				   m_hotSections = 0u;
	bool					   m_hotPending = false, m_hotAttempted = false, m_hotFailed = false;
	bool					   m_hotStandalone = false;
	struct HotFrameInfo
	{
		uint32_t abi, size;
		uint64_t render_frame, device_frame;
		uint32_t frame_slot, frame_slot_count, byte_count, reserved;
	};
	static_assert( sizeof( HotFrameInfo ) == 40u && offsetof( HotFrameInfo, render_frame ) == 8u,
				   "HOT companion ABI 1" );
	// Companion ABI 2: callback pointers are only consumed by the matching native
	// Godot bridge. The engine must copy the returned range array before invoking completion.
	using HotRangeBatch = t3dHotRangeBatch_t;
	HotRangeBatch				m_hotRangeBatch{};
	static const HotRangeBatch *prepareHotRanges( void *userdata, const t3dHotRangeInfo_t *info ) noexcept;
	static bool	   completeHotRanges( void *userdata, const t3dHotRangeInfo_t *info, bool success ) noexcept;
	static void	   fillHotFrame( void *userdata, const HotFrameInfo *info, uint8_t *destination ) noexcept;
	static bool	   sealHotFrame( void *userdata, const HotFrameInfo *info ) noexcept;
	uint32_t	   m_nFixedIndirectReferences[T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT]{};
	t3dAtomicU64_t m_nTerrainPatchCount{ 0u };
	t3dAtomicU64_t m_nVisibleTerrainPatches{ 0u };
	t3dAtomicU64_t m_nStaticInstanceCount{ 0u };
	t3dAtomicU64_t m_nVisibleInstances{ 0u };
	t3dAtomicU64_t m_nHlodCount{ 0u };
	t3dAtomicU64_t m_nVisibleHlods{ 0u };
	t3dAtomicU64_t m_nIndirectCommandCount{ 0u };
	t3dAtomicU64_t m_nComputeDispatchCount{ 0u };

	static t3dRdResourceType_t	 inferResourceType( const String &rName );
	static t3dRdMemoryCategory_t inferMemoryCategory( const String &rName, t3dRdResourceType_t type, uint64_t nBytes );
	static t3dRdResourceOwner_t	 inferResourceOwner( const String &rName );
	static bool					 isPipeline( t3dRdResourceType_t type );
	static bool					 isSteadyStateAllocation( t3dRdResourceType_t type );
	static void					 updatePeak( t3dAtomicU64_t &rPeak, uint64_t nLiveBytes );
	static bool					 subtractSaturating( t3dAtomicU64_t &rValue, uint64_t nAmount );
	void						 rememberFreedResource( uint64_t nRid, uint64_t nDeviceGeneration );
	void						 updateBufferPeak( uint64_t nLiveBytes );
	void						 updateTexturePeak( uint64_t nLiveBytes );
	void						 accountRegistration( const t3dRdResourceRecord_t &rRecord, bool bRebuild );
	void						 accountRelease( const t3dRdResourceRecord_t &rRecord );
	void						 resetLiveAccounting( void );
	void						 setFailure( t3dRdFailureCode_t code, const String &rFailure );
	bool						 hasResourceHistory( const String &rName, t3dRdResourceType_t type ) const;
	bool						 registerResourceInternal( const RID			&rRid,
														   const String			&rName,
														   t3dRdResourceType_t	 type,
														   t3dRdMemoryCategory_t memoryCategory,
														   uint64_t				 nBytes,
														   t3dRdResourceOwner_t	 owner );

public:
	enum SharedArena : uint32_t
	{
		COLD,
		HOT,
		SCRATCH,
		SHARED_ARENA_COUNT
	};
	// 192 MiB is a legacy fallback; normal startup sizes COLD from the world profile.
	static constexpr uint32_t COLD_ARENA_BYTES = 192u * 1024u * 1024u;
	static constexpr uint32_t SCRATCH_ARENA_BYTES = 8u * 1024u * 1024u;
	static constexpr uint32_t MAX_FRAME_SLOTS = 8u;
	static constexpr uint32_t HOT_RECORD_CAPACITY = T3D_RD_HOT_RECORD_CAPACITY;
	static constexpr uint32_t HOT_FRAME_BYTES = HOT_RECORD_CAPACITY * sizeof( Terrain3DRdHotRecord );
	bool					  initializeSharedArenas( uint32_t cold_budget = COLD_ARENA_BYTES,
													  uint32_t scratch_budget = SCRATCH_ARENA_BYTES );
	Terrain3DRdGpuBuffer	 &arena(
			SharedArena id ) {
		return m_sharedArenas[id];
	}
	const Terrain3DRdGpuBuffer &arena(
		SharedArena id ) const {
		return m_sharedArenas[id];
	}
	bool reserveArenaSpan( SharedArena id, uint32_t bytes, uint32_t stride, t3dRdGpuBufferSlice_t &span );
	// Called after GPU work for a departing owner has been drained and its
	// pipelines/descriptors released. Never call during ordinary lane recovery.
	bool releaseArenaSpanFinal( SharedArena id, t3dRdGpuBufferSlice_t &span );
	String getLastArenaReservationFailure() const {
		Terrain3DScopedLock lock( m_arenaMutex );
		return m_lastArenaReservationFailure;
	}
	bool beginHotFrame( uint64_t render_frame );
	bool beginStandaloneHotFrame();
	bool publishStandaloneHotFrame();
	void cancelStandaloneHotFrame() {
		if ( m_hotStandalone && m_hotPending ) {
			m_hotFailed = true;
			publishStandaloneHotFrame();
		}
	}
	bool	 beginHotPass();
	uint32_t hotPassIndex() const {
		return m_hotIndex;
	}
	bool writeHotRecord( uint32_t relative_offset, const void *data, uint32_t bytes );
	bool hotPending() const {
		return m_hotPending;
	}
	void					   markHotShader( const Ref< RDShaderSPIRV > &shader, int binding = 0 );
	Dictionary				   getSharedArenaDiagnostics() const;
	static Terrain3DRdContext &getShared( void );
	static void				   shutdownShared( void );
	// One fixed-budget global descriptor service per RenderingDevice context.
	// Configure once, on the render thread, against actual RD registry headroom.
	Terrain3DRdGlobalDescriptorCache *
	getGlobalDescriptorCacheCold( uint32_t nSlots, uint32_t nNonDescriptorReserved, t3dRdDescriptorStatus_t &rStatus );
	Terrain3DRdGlobalDescriptorCache *tryGetGlobalDescriptorCacheHot() const {
		return m_pGlobalDescriptorCache;
	}
	uint32_t getAvailableResourceSlots() const;
	// Fixed command slices share one physical indirect-capable storage buffer.
	// Each consumer retains a reference until its own shutdown.
	bool acquireFixedIndirect( t3dRdFixedIndirectSlot_t slot,
							   uint32_t					nRequiredBytes,
							   Terrain3DRdGpuBuffer	  *&rBuffer,
							   uint32_t				   &rOffsetBytes );
	void releaseFixedIndirect( t3dRdFixedIndirectSlot_t slot, Terrain3DRdGpuBuffer *&rBuffer );

	// The sole engine rendering gateway permitted in the RD branch. Godot's
	// GDExtension API exposes global/local RenderingDevice acquisition and
	// render-thread scheduling through this boundary.
	static RenderingDevice *getGlobalDevice( void );
	static RenderingDevice *createLocalDevice( void );
	// Owner/Godot-thread identity is distinct from render-thread identity. In
	// single-threaded rendering Godot legitimately reports the main thread as
	// the render thread too, so owner assertions must use this main-thread test.
	static bool		  isOnOwnerThread( void );
	static bool		  isOnRenderThread( void );
	static bool		  enqueueOnRenderThread( const Callable &rCallable );
	static void		  forceSync( t3dRdForceSyncReason_t eReason = T3D_RD_FORCE_SYNC_OTHER );
	static Dictionary getForceSyncDiagnostics( void );

	Terrain3DRdContext( void ) = default;
	~Terrain3DRdContext( void );

	Terrain3DRdContext( const Terrain3DRdContext & ) = delete;
	Terrain3DRdContext &operator=( const Terrain3DRdContext & ) = delete;

	bool initialize( void );
	bool validateDevice( void );
	void shutdown( void );
	void invalidateDevice( const String &rReason );
	bool beginRebuild( void );
	bool completeRebuild( bool bSuccess, const String &rFailure = String() );
	void recordFailure( t3dRdFailureCode_t code, const String &rFailure );
	void
	recordResourceCreationFailure( const String &rName, t3dRdResourceType_t type, const String &rFailure = String() );
	void beginRecoverableFailureScope( void );
	void endRecoverableFailureScope( void );

	// Compatibility entry point. New owners should use the typed overload so
	// accounting never depends on a debug-name convention.
	void				 registerResource( const RID &rRid, const String &rName, uint64_t nBufferBytes = 0u );
	bool				 registerResource( const RID			&rRid,
										   const String			&rName,
										   t3dRdResourceType_t	 type,
										   t3dRdMemoryCategory_t memoryCategory,
										   uint64_t				 nBytes,
										   t3dRdResourceOwner_t	 owner ) override;
	bool				 registerVertexFormat( int64_t nFormat, const String &rName );
	void				 releaseVertexFormat( int64_t &rFormat );
	void				 freeResource( RID &rRid, uint64_t nBufferBytes = 0u ) override;
	bool				 ownsResource( const RID &rRid ) const;
	t3dRdResourceEpoch_t getResourceEpoch( t3dRdResourceOwner_t owner ) const;

	void	   markWarmupComplete( bool bComplete = true );
	void	   beginResourceRebuild( void );
	void	   endResourceRebuild( void );
	void	   beginFrame( uint64_t nGpuFrameId, uint64_t nSnapshotGeneration );
	void	   recordTextureUpload( uint64_t				 nBytes,
									t3dRdResourceOwner_t	 owner,
									Terrain3DGpuBufferPolicy policy = T3D_GPU_DIRTY_UPLOAD );
	Dictionary getGpuUploadDiagnostics( void ) const;
	Dictionary getPublishedResourceDiagnostics( void ) const;
	void	   recordBufferUpdate( uint64_t					nBytes,
								   t3dRdResourceOwner_t		owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED,
								   Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) override;
	void	   recordRenderEvent( t3dRdResourceOwner_t owner, t3dRenderTelemetryEvent_t event ) override;
	void recordRenderCapacity( t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy, uint64_t nBytes ) override;
	void recordRenderIndirectCommandsSubmitted( t3dRdResourceOwner_t owner, uint64_t nCommands );
	uint64_t				 getRenderEvent( t3dRdResourceOwner_t owner, t3dRenderTelemetryEvent_t event ) const;
	uint64_t				 getRenderUploadCalls( t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy ) const;
	uint64_t				 getRenderUploadBytes( t3dRdResourceOwner_t owner, Terrain3DGpuBufferPolicy policy ) const;
	uint64_t				 getRenderDirtyRangesUploaded( t3dRdResourceOwner_t owner ) const;
	void					 recordSuccessfulPass( t3dRdSuccessfulPass_t pass, uint64_t nGpuFrameId );
	void					 recordSceneCounts( uint64_t nTerrainPatches,
												uint64_t nVisibleTerrainPatches,
												uint64_t nStaticInstances,
												uint64_t nVisibleInstances,
												uint64_t nHlods,
												uint64_t nVisibleHlods );
	void					 recordSubmissionCounts( uint64_t nIndirectCommands, uint64_t nComputeDispatches );
	Dictionary				 getDiagnostics( void ) const;
	EA_FORCE_INLINE uint64_t getLiveGpuBufferBytes(
		void ) const {
		return m_nLiveBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getLiveGpuTextureBytes(
		void ) const {
		return m_nLiveTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}

	EA_FORCE_INLINE RenderingDevice *getDevice(
		void ) const override {
		const t3dRdContextState_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
		return ( state == T3D_RD_CONTEXT_STATE_READY || state == T3D_RD_CONTEXT_STATE_REBUILDING )
				   ? m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE )
				   : NULL;
	}
	EA_FORCE_INLINE bool isReady(
		void ) const {
		return m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) == T3D_RD_CONTEXT_STATE_READY &&
			   m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) != NULL;
	}
	EA_FORCE_INLINE bool hasFailure(
		void ) const {
		return m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE ) != T3D_RD_CONTEXT_FAILURE_NONE;
	}
	String getLastFailure(
		void ) const {
		Terrain3DScopedLock lock( m_resourceMutex );
		return m_lastFailure;
	}
	EA_FORCE_INLINE t3dRdContextState_t getState(
		void ) const {
		return m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE t3dRdFailureCode_t getFailureCode(
		void ) const {
		return m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getDeviceGeneration(
		void ) const {
		return m_nDeviceGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getLiveBufferBytes(
		void ) const {
		return m_nLiveBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getLiveBytes(
		const t3dRdMemoryCategory_t category ) const {
		return category < T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT
				   ? m_szLiveMemory[category].load( T3D_MEMORY_ORDER_ACQUIRE )
				   : 0u;
	}
};

static_assert( t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD ) +
					   t3d_rd_context_contract::fixedIndirectCapacity( T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD ) <=
				   t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN ),
			   "World/Ocean indirect slices overlap" );
static_assert( t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN ) +
					   t3d_rd_context_contract::fixedIndirectCapacity( T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN ) <=
				   t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP ),
			   "Ocean/clipmap indirect slices overlap" );
static_assert( t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP ) +
					   t3d_rd_context_contract::fixedIndirectCapacity( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP ) <=
				   T3D_RD_FIXED_INDIRECT_ARENA_BYTES,
			   "Fixed indirect slices exceed their buffer" );

#endif // TERRAIN3D_RD_CONTEXT_H
