#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_cache.h"
#include <new>
#include "core/utility/terrain_3d_sync.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static const char *const s_memory_keys[T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT] = {
	"none",
	"topology_vb",
	"topology_ib",
	"static_mesh_vertex_arena",
	"static_mesh_index_arena",
	"material_table",
	"instance",
	"hlod",
	"patch",
	"indirect",
	"ubo",
	"storage_buffer",
	"history",
	"scratch",
	"terrain_texture",
	"shadow",
};

static const char *const s_owner_keys[T3D_RD_CONTEXT_OWNER_COUNT] = {
	"unspecified",
	"terrain",
	"world",
	"instances",
	"hlod",
	"ocean",
	"forward_lighting",
	"vt",
	"shader_cache",
};

static const char *const s_state_names[] = {
	"uninitialized",
	"ready",
	"failed",
	"rebuild_required",
	"rebuilding",
	"shutting_down",
};

static const char *const s_failure_names[] = {
	"none",
	"device_unavailable",
	"resource_creation",
	"duplicate_resource",
	"untracked_resource",
	"accounting",
	"device_invalidated",
	"rebuild",
	"render_pass",
};

static thread_local uint32_t s_nRecoverableFailureDepth = 0u;
// Count actual RenderingServer drains, not requests made from the render thread.
static t3dAtomicU64_t s_nForceSyncOther{ 0u };
static t3dAtomicU64_t s_nForceSyncControl{ 0u };
static t3dAtomicU64_t s_nForceSyncDiagnostics{ 0u };

static const char *const s_pass_names[] = {
	"none",
	"terrain_clipmap",
};

static_assert( sizeof( s_memory_keys ) / sizeof( s_memory_keys[0] ) == T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT );
static_assert( sizeof( s_owner_keys ) / sizeof( s_owner_keys[0] ) == T3D_RD_CONTEXT_OWNER_COUNT );
static_assert( sizeof( s_state_names ) / sizeof( s_state_names[0] ) == T3D_RD_CONTEXT_STATE_SHUTTING_DOWN + 1u );
static_assert( sizeof( s_failure_names ) / sizeof( s_failure_names[0] ) == T3D_RD_CONTEXT_FAILURE_RENDER_PASS + 1u );
static_assert( sizeof( s_pass_names ) / sizeof( s_pass_names[0] ) == T3D_RD_CONTEXT_PASS_COUNT );

static RenderingServer *t3dRdRenderingServerIfAvailable(
	void ) {
	Engine *const			engine = Engine::get_singleton();
	static const StringName singleton_name( "RenderingServer" );
	// get_singleton() logs an error before server registration (and in
	// registration-only tools). Absence is an expected lifecycle state here.
	if ( !engine || !engine->has_singleton( singleton_name ) ) {
		return NULL;
	}
	return RenderingServer::get_singleton();
}

RenderingDevice *Terrain3DRdContext::getGlobalDevice(
	void ) {
	RenderingServer *const pServer = t3dRdRenderingServerIfAvailable();
	return pServer ? pServer->get_rendering_device() : NULL;
}

RenderingDevice *Terrain3DRdContext::createLocalDevice(
	void ) {
	RenderingServer *const pServer = t3dRdRenderingServerIfAvailable();
	return pServer ? pServer->create_local_rendering_device() : NULL;
}

bool Terrain3DRdContext::isOnOwnerThread(
	void ) {
	OS *const pOs = OS::get_singleton();
	return pOs && pOs->get_thread_caller_id() == pOs->get_main_thread_id();
}

bool Terrain3DRdContext::isOnRenderThread(
	void ) {
	RenderingServer *const pServer = t3dRdRenderingServerIfAvailable();
	return pServer && pServer->is_on_render_thread();
}

bool Terrain3DRdContext::enqueueOnRenderThread(
	const Callable &rCallable ) {
	RenderingServer *const pServer = t3dRdRenderingServerIfAvailable();
	if ( !pServer || !rCallable.is_valid() ) {
		return false;
	}
	pServer->call_on_render_thread( rCallable );
	return true;
}

void Terrain3DRdContext::forceSync(
	const t3dRdForceSyncReason_t eReason ) {
	RenderingServer *const pServer = t3dRdRenderingServerIfAvailable();
	if ( pServer && !pServer->is_on_render_thread() ) {
		switch ( eReason ) {
		case T3D_RD_FORCE_SYNC_CONTROL:
			s_nForceSyncControl.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			break;
		case T3D_RD_FORCE_SYNC_DIAGNOSTICS:
			s_nForceSyncDiagnostics.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			break;
		default:
			s_nForceSyncOther.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			break;
		}
		pServer->force_sync();
	}
}

Dictionary Terrain3DRdContext::getForceSyncDiagnostics(
	void ) {
	Dictionary counts;
	counts["control"] = ( int64_t ) s_nForceSyncControl.load( T3D_MEMORY_ORDER_ACQUIRE );
	counts["diagnostics"] = ( int64_t ) s_nForceSyncDiagnostics.load( T3D_MEMORY_ORDER_ACQUIRE );
	counts["other"] = ( int64_t ) s_nForceSyncOther.load( T3D_MEMORY_ORDER_ACQUIRE );
	counts["total"] = ( int64_t ) ( s_nForceSyncControl.load( T3D_MEMORY_ORDER_ACQUIRE ) +
									s_nForceSyncDiagnostics.load( T3D_MEMORY_ORDER_ACQUIRE ) +
									s_nForceSyncOther.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	return counts;
}

Terrain3DRdContext &Terrain3DRdContext::getShared(
	void ) {
	// Intentionally process-lifetime storage. Godot module shutdown explicitly
	// releases the RD resources before the rendering server disappears.
	static Terrain3DRdContext *const pContext = new Terrain3DRdContext;
	return *pContext;
}

void Terrain3DRdContext::shutdownShared(
	void ) {
	Terrain3DRdContext &rContext = getShared();
	rContext.shutdown();
	delete rContext.m_pGlobalDescriptorCache;
	rContext.m_pGlobalDescriptorCache = nullptr;
}

uint32_t Terrain3DRdContext::getAvailableResourceSlots() const {
	Terrain3DScopedLock lock( m_resourceMutex );
	return ( uint32_t ) ( T3D_RESOURCE_CAPACITY - m_resources.size() );
}

Terrain3DRdGlobalDescriptorCache *Terrain3DRdContext::getGlobalDescriptorCacheCold(
	const uint32_t			 nSlots,
	const uint32_t			 nNonDescriptorReserved,
	t3dRdDescriptorStatus_t &rStatus ) {
	rStatus = T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	if ( !isOnRenderThread() || !getDevice() ) {
		rStatus = T3D_RD_DESCRIPTOR_STALE_DEVICE;
		return nullptr;
	}
	if ( m_pGlobalDescriptorCache && m_pGlobalDescriptorCache->isInitialized() &&
		 m_pGlobalDescriptorCache->capacity() == nSlots ) {
		rStatus = m_pGlobalDescriptorCache->initializeCold( nSlots, getDeviceGeneration() );
		return rStatus == T3D_RD_DESCRIPTOR_OK ? m_pGlobalDescriptorCache : nullptr;
	}
	if ( m_pGlobalDescriptorCache ) {
		return nullptr;
	}
	const t3dRdDescriptorCapacityRequest_t request{ nSlots, getAvailableResourceSlots(), nNonDescriptorReserved };
	uint32_t							   allowed = 0u;
	rStatus = t3dRdDescriptorPlanCapacityCold( request, allowed );
	if ( rStatus != T3D_RD_DESCRIPTOR_OK ) {
		return nullptr;
	}
	Terrain3DRdGlobalDescriptorCache *const pCache = new ( std::nothrow ) Terrain3DRdGlobalDescriptorCache( *this );
	if ( !pCache ) {
		rStatus = T3D_RD_DESCRIPTOR_CACHE_CAPACITY_EXCEEDED;
		return nullptr;
	}
	rStatus = pCache->initializeCold( allowed, getDeviceGeneration() );
	if ( rStatus != T3D_RD_DESCRIPTOR_OK ) {
		delete pCache;
		return nullptr;
	}
	m_pGlobalDescriptorCache = pCache;
	return m_pGlobalDescriptorCache;
}

Terrain3DRdContext::~Terrain3DRdContext(
	void ) {
	shutdown();
}

void Terrain3DRdContext::updatePeak(
	t3dAtomicU64_t &rPeak,
	const uint64_t	nLiveBytes ) {
	uint64_t peak = rPeak.load( T3D_MEMORY_ORDER_RELAXED );
	while ( nLiveBytes > peak &&
			!rPeak.compare_exchange_weak( peak, nLiveBytes, T3D_MEMORY_ORDER_RELEASE, T3D_MEMORY_ORDER_RELAXED ) ) {
	}
}

bool Terrain3DRdContext::subtractSaturating(
	t3dAtomicU64_t &rValue,
	const uint64_t	nAmount ) {
	uint64_t value = rValue.load( T3D_MEMORY_ORDER_RELAXED );
	for ( ;; ) {
		const bool	   valid = value >= nAmount;
		const uint64_t replacement = valid ? value - nAmount : 0u;
		if ( rValue.compare_exchange_weak( value, replacement, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_RELAXED ) ) {
			return valid;
		}
	}
}

void Terrain3DRdContext::rememberFreedResource(
	const uint64_t nRid,
	const uint64_t nDeviceGeneration ) {
	if ( m_freedResources.size() >= T3D_FREED_HISTORY_CAPACITY ) {
		m_freedResources.erase( m_freedResources.begin() );
	}
	m_freedResources.push_back( { nRid, nDeviceGeneration } );
}

void Terrain3DRdContext::updateBufferPeak(
	const uint64_t nLiveBytes ) {
	updatePeak( m_nPeakBufferBytes, nLiveBytes );
}

void Terrain3DRdContext::updateTexturePeak(
	const uint64_t nLiveBytes ) {
	updatePeak( m_nPeakTextureBytes, nLiveBytes );
}

t3dRdResourceType_t Terrain3DRdContext::inferResourceType(
	const String &rName ) {
	if ( rName.find( String( "Shader" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_SHADER;
	}
	if ( rName.find( String( "Pipeline.Cull" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_COMPUTE_PIPELINE;
	}
	if ( rName.find( String( "Pipeline" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_RASTER_PIPELINE;
	}
	if ( rName.find( String( "Sampler" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_SAMPLER;
	}
	if ( rName.find( String( "UniformSet" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET;
	}
	if ( rName.find( String( "Framebuffer" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_FRAMEBUFFER;
	}
	if ( rName.find( String( ".VB" ) ) >= 0 || rName.find( String( "VertexBuffer" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER;
	}
	if ( rName.find( String( ".IB" ) ) >= 0 || rName.find( String( "IndexBuffer" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER;
	}
	if ( rName.find( String( ".VA" ) ) >= 0 || rName.find( String( "VertexArray" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_VERTEX_ARRAY;
	}
	if ( rName.find( String( ".IA" ) ) >= 0 || rName.find( String( "IndexArray" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_INDEX_ARRAY;
	}
	if ( rName.find( String( "Indirect" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER;
	}
	if ( rName.find( String( "Visible" ) ) >= 0 || rName.find( String( "Cull" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER;
	}
	if ( rName.find( String( "UBO" ) ) >= 0 || rName.find( String( "UniformBuffer" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER;
	}
	if ( rName.find( String( "Shadow" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_SHADOW;
	}
	if ( rName.find( String( "Texture" ) ) >= 0 || rName.find( String( "Atlas" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_RESOURCE_TEXTURE;
	}
	return T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
}

t3dRdMemoryCategory_t Terrain3DRdContext::inferMemoryCategory(
	const String			 &rName,
	const t3dRdResourceType_t type,
	const uint64_t			  nBytes ) {
	if ( nBytes == 0u ) {
		return T3D_RD_CONTEXT_MEMORY_NONE;
	}
	if ( rName.find( String( "MeshVertexArena" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_MESH_VERTEX_ARENA;
	}
	if ( rName.find( String( "MeshIndexArena" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_MESH_INDEX_ARENA;
	}
	if ( rName.find( String( "Material" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_MATERIAL_TABLE;
	}
	if ( rName.find( String( "HLOD" ) ) >= 0 || rName.find( String( "Hlod" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_HLOD;
	}
	if ( rName.find( String( "PreviousRecords" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_HISTORY;
	}
	if ( rName.find( String( "Instance" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_INSTANCE;
	}
	if ( rName.find( String( "Patch" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_PATCH;
	}
	if ( type == T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER && rName.find( String( "Clipmap" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_TOPOLOGY_VERTEX;
	}
	if ( type == T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER && rName.find( String( "Clipmap" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_MEMORY_TOPOLOGY_INDEX;
	}
	if ( type == T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER ) {
		return T3D_RD_CONTEXT_MEMORY_INDIRECT;
	}
	if ( type == T3D_RD_CONTEXT_RESOURCE_UNIFORM_BUFFER ) {
		return T3D_RD_CONTEXT_MEMORY_UNIFORM;
	}
	if ( type == T3D_RD_CONTEXT_RESOURCE_CULLING_BUFFER ) {
		return T3D_RD_CONTEXT_MEMORY_SCRATCH;
	}
	if ( type == T3D_RD_CONTEXT_RESOURCE_TEXTURE ) {
		return T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
	}
	if ( type == T3D_RD_CONTEXT_RESOURCE_SHADOW ) {
		return T3D_RD_CONTEXT_MEMORY_SHADOW;
	}
	return T3D_RD_CONTEXT_MEMORY_STORAGE;
}

t3dRdResourceOwner_t Terrain3DRdContext::inferResourceOwner(
	const String &rName ) {
	if ( rName.find( String( "WorldRuntime" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_OWNER_WORLD;
	}
	if ( rName.find( String( "Instance" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_OWNER_INSTANCES;
	}
	if ( rName.find( String( "HLOD" ) ) >= 0 || rName.find( String( "Hlod" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_OWNER_HLOD;
	}
	if ( rName.find( String( "Ocean" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_OWNER_OCEAN;
	}
	if ( rName.find( String( "ForwardLight" ) ) >= 0 || rName.find( String( "ForwardLighting" ) ) >= 0 ) {
		return T3D_RD_CONTEXT_OWNER_FORWARD_LIGHTING;
	}
	if ( rName.find( String( "VT " ) ) >= 0 ) {
		return T3D_RD_CONTEXT_OWNER_VT;
	}
	return T3D_RD_CONTEXT_OWNER_TERRAIN;
}

bool Terrain3DRdContext::isPipeline(
	const t3dRdResourceType_t type ) {
	return type == T3D_RD_CONTEXT_RESOURCE_RASTER_PIPELINE || type == T3D_RD_CONTEXT_RESOURCE_COMPUTE_PIPELINE;
}

bool Terrain3DRdContext::isSteadyStateAllocation(
	const t3dRdResourceType_t type ) {
	return type > T3D_RD_CONTEXT_RESOURCE_UNKNOWN && type < T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT;
}

bool Terrain3DRdContext::hasResourceHistory(
	const String			 &rName,
	const t3dRdResourceType_t type ) const {
	for ( size_t i = 0u; i < m_resourceHistory.size(); i++ ) {
		const t3dRdResourceHistory_t &rHistory = m_resourceHistory[i];
		if ( rHistory.m_type == type && rHistory.m_name == rName ) {
			return true;
		}
	}
	return false;
}

void Terrain3DRdContext::accountRegistration(
	const t3dRdResourceRecord_t &rRecord,
	const bool					 bRebuild ) {
	const int kind = bufferKind( rRecord.m_type );
	if ( kind >= 0 ) {
		++m_physicalBufferCreates[rRecord.m_owner][kind];
	}

	m_nCreatedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_szTypeCreates[rRecord.m_type].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_szTypeLive[rRecord.m_type].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	if ( rRecord.m_owner < T3D_RD_CONTEXT_OWNER_COUNT ) {
		m_szOwnerCreates[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_szOwnerLive[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( rRecord.m_nBytes > 0u ) {
			m_szOwnerLiveBytes[rRecord.m_owner].fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	if ( isPipeline( rRecord.m_type ) ) {
		( bRebuild ? m_nPipelineRebuilds : m_nPipelineCreates ).fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	} else if ( rRecord.m_type == T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET ) {
		( bRebuild ? m_nUniformSetRebuilds : m_nUniformSetCreates ).fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}

	if ( m_bWarmupComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) &&
		 m_nExplicitRebuildDepth.load( T3D_MEMORY_ORDER_ACQUIRE ) == 0u && isSteadyStateAllocation( rRecord.m_type ) ) {
		m_nSteadyStateAllocationViolations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( rRecord.m_owner < T3D_RD_CONTEXT_OWNER_COUNT ) {
			m_szOwnerSteadyStateViolations[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	if ( rRecord.m_nBytes == 0u || rRecord.m_memoryCategory == T3D_RD_CONTEXT_MEMORY_NONE ) {
		return;
	}
	const uint64_t category_live =
		m_szLiveMemory[rRecord.m_memoryCategory].fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_ACQ_REL ) +
		rRecord.m_nBytes;
	updatePeak( m_szPeakMemory[rRecord.m_memoryCategory], category_live );
	if ( rRecord.m_type == T3D_RD_CONTEXT_RESOURCE_TEXTURE ) {
		const uint64_t live =
			m_nLiveTextureBytes.fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_ACQ_REL ) + rRecord.m_nBytes;
		updateTexturePeak( live );
	} else {
		const uint64_t live =
			m_nLiveBufferBytes.fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_ACQ_REL ) + rRecord.m_nBytes;
		updateBufferPeak( live );
	}
}

void Terrain3DRdContext::accountRelease(
	const t3dRdResourceRecord_t &rRecord ) {
	const int kind = bufferKind( rRecord.m_type );
	if ( kind >= 0 ) {
		++m_physicalBufferReleases[rRecord.m_owner][kind];
	}

	m_szTypeFrees[rRecord.m_type].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	if ( !subtractSaturating( m_szTypeLive[rRecord.m_type], 1u ) ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	if ( rRecord.m_owner < T3D_RD_CONTEXT_OWNER_COUNT ) {
		m_szOwnerFrees[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( !subtractSaturating( m_szOwnerLive[rRecord.m_owner], 1u ) ) {
			m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
		if ( rRecord.m_nBytes > 0u && !subtractSaturating( m_szOwnerLiveBytes[rRecord.m_owner], rRecord.m_nBytes ) ) {
			m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
	}
	if ( rRecord.m_nBytes == 0u || rRecord.m_memoryCategory == T3D_RD_CONTEXT_MEMORY_NONE ) {
		return;
	}
	if ( !subtractSaturating( m_szLiveMemory[rRecord.m_memoryCategory], rRecord.m_nBytes ) ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	t3dAtomicU64_t &aggregate =
		rRecord.m_type == T3D_RD_CONTEXT_RESOURCE_TEXTURE ? m_nLiveTextureBytes : m_nLiveBufferBytes;
	if ( !subtractSaturating( aggregate, rRecord.m_nBytes ) ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
}

void Terrain3DRdContext::resetLiveAccounting(
	void ) {
	m_nLiveBufferBytes.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nLiveTextureBytes.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	for ( size_t iType = 0u; iType < T3D_ARRAY_COUNT( m_szTypeLive ); iType++ ) {
		t3dAtomicU64_t &value = ( m_szTypeLive )[iType];
		value.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	}
	for ( size_t iMemoryClass = 0u; iMemoryClass < T3D_ARRAY_COUNT( m_szLiveMemory ); iMemoryClass++ ) {
		t3dAtomicU64_t &value = ( m_szLiveMemory )[iMemoryClass];
		value.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	}
	for ( size_t iOwner = 0u; iOwner < T3D_ARRAY_COUNT( m_szOwnerLive ); iOwner++ ) {
		t3dAtomicU64_t &value = ( m_szOwnerLive )[iOwner];
		value.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	}
	for ( size_t iOwner = 0u; iOwner < T3D_ARRAY_COUNT( m_szOwnerLiveBytes ); iOwner++ ) {
		t3dAtomicU64_t &value = ( m_szOwnerLiveBytes )[iOwner];
		value.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	}
}

void Terrain3DRdContext::setFailure(
	const t3dRdFailureCode_t code,
	const String			&rFailure ) {
	if ( s_nRecoverableFailureDepth > 0u ) {
		{
			Terrain3DScopedLock lock( m_resourceMutex );
			m_lastRecoverableFailure = rFailure;
		}
		m_nRecoverableFailureCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return;
	}
	{
		Terrain3DScopedLock lock( m_resourceMutex );
		m_lastFailure = rFailure;
	}
	m_failureCode.store( code, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( T3D_RD_CONTEXT_STATE_FAILED, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::beginRecoverableFailureScope(
	void ) {
	++s_nRecoverableFailureDepth;
}

void Terrain3DRdContext::endRecoverableFailureScope(
	void ) {
	if ( s_nRecoverableFailureDepth == 0u ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return;
	}
	--s_nRecoverableFailureDepth;
}

bool Terrain3DRdContext::initialize(
	void ) {
	const t3dRdContextState_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( state == T3D_RD_CONTEXT_STATE_READY ) {
		return m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) != NULL;
	}
	if ( state != T3D_RD_CONTEXT_STATE_UNINITIALIZED ) {
		return false;
	}

	RenderingDevice *const rd = getGlobalDevice();
	if ( !rd ) {
		setFailure( T3D_RD_CONTEXT_FAILURE_DEVICE_UNAVAILABLE, "Global RenderingDevice is unavailable" );
		return false;
	}

	{
		Terrain3DScopedLock lock( m_resourceMutex );
		m_lastFailure = String();
	}
	m_pRd.store( rd, T3D_MEMORY_ORDER_RELEASE );
	const uint64_t generation = m_nDeviceGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	m_nDeviceInitializations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	if ( generation > 1u ) {
		m_nDeviceRebuilds.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	m_failureCode.store( T3D_RD_CONTEXT_FAILURE_NONE, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( T3D_RD_CONTEXT_STATE_READY, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

bool Terrain3DRdContext::initializeSharedArenas(
	uint32_t cold_budget,
	uint32_t scratch_budget ) {
	if ( arena( COLD ).isValid() ) {
		return arena( HOT ).isValid() && arena( SCRATCH ).isValid();
	}
	auto fail = [this]( const String &reason ) {
		setFailure( T3D_RD_CONTEXT_FAILURE_RESOURCE_CREATION, reason );
		ERR_PRINT( reason );
		return false;
	};
	RenderingDevice *rd = getDevice();
	if ( !rd || cold_budget == 0u || scratch_budget < T3D_RD_FIXED_INDIRECT_ARENA_BYTES ) {
		return fail( "Shared arena initialization requires a ready device and valid COLD/SCRATCH budgets" );
	}
	Engine *engine = Engine::get_singleton();
	m_hotBridge = engine && engine->has_singleton( s_pszEngineSingletonName )
					  ? engine->get_singleton( s_pszEngineSingletonName )
					  : NULL;
	if ( !m_hotBridge || !m_hotBridge->has_method( "complete_native_hot_frame" ) ||
		 !m_hotBridge->has_method( "get_native_device_frame" ) ) {
		return fail( "Terrain3D shared arenas require the Godot HOT companion completion hook; see the persistent HOT "
					 "engine handoff." );
	}
	const bool	  mapping_available = Terrain3DRdGpuBuffer::supportsPersistentMapping( rd );
	const int64_t persistent_abi = m_hotBridge->has_method( "get_persistent_hot_abi_version" )
									   ? int64_t( m_hotBridge->call( "get_persistent_hot_abi_version" ) )
									   : 0;
	const bool	  persistent_v1 = persistent_abi == 1 && m_hotBridge->has_method( "defer_native_buffer_flush" );
	const bool persistent_v2 = persistent_abi == 2 && m_hotBridge->has_method( "defer_native_buffer_flush_ranges" ) &&
							   m_hotBridge->has_method( "get_persistent_hot_frame_slot" );
	const bool persistent = mapping_available && m_hotBridge->has_method( "get_native_hot_backing_size" ) &&
							( persistent_v1 || persistent_v2 );
	const bool ordinary_v2 = !persistent && m_hotBridge->has_method( "get_deferred_upload_abi_version" ) &&
							 int64_t( m_hotBridge->call( "get_deferred_upload_abi_version" ) ) == 2 &&
							 m_hotBridge->has_method( "defer_native_buffer_update_ranges" );
	m_hotRangeAbi2 = persistent ? persistent_v2 : ordinary_v2;

	const uint32_t slots = rd->get_frame_delay();
	if ( slots == 0u || slots > MAX_FRAME_SLOTS ) {
		return fail( "Unsupported RD frame-slot count: " + String::num_uint64( slots ) );
	}
	const uint32_t capacities[3] = { cold_budget, HOT_FRAME_BYTES * ( persistent ? 1u : slots ), scratch_budget };
	// Discover the new engine enum through ClassDB: the pinned godot-cpp
	// headers predate this constant, but limit_get's ABI is unchanged.
	ClassDBSingleton *const class_db = ClassDBSingleton::get_singleton();
	if ( class_db && class_db->class_has_integer_constant( "RenderingDevice", "LIMIT_MAX_STORAGE_BUFFER_SIZE" ) ) {
		const auto limit_id = static_cast< RenderingDevice::Limit >(
			class_db->class_get_integer_constant( "RenderingDevice", "LIMIT_MAX_STORAGE_BUFFER_SIZE" ) );
		const uint64_t storage_limit = rd->limit_get( limit_id );
		if ( !storage_limit || cold_budget > storage_limit || capacities[HOT] > storage_limit ||
			 scratch_budget > storage_limit ) {
			return fail( String( "Shared arena capacity exceeds LIMIT_MAX_STORAGE_BUFFER_SIZE: limit_bytes=" ) +
						 String::num_uint64( storage_limit ) + " cold_bytes=" + String::num_uint64( cold_budget ) +
						 " hot_bytes=" + String::num_uint64( capacities[HOT] ) +
						 " scratch_bytes=" + String::num_uint64( scratch_budget ) );
		}
	}
	const int64_t backing =
		persistent ? int64_t( m_hotBridge->call( "get_native_hot_backing_size", int64_t( HOT_FRAME_BYTES ) ) ) : 0;
	if ( persistent && backing < HOT_FRAME_BYTES ) {
		return fail( "Godot returned an invalid persistent HOT backing size" );
	}
	if ( g_t3dMemoryAccount ) {
		const auto	   stats = g_t3dMemoryAccount->stats();
		const uint64_t required = uint64_t( cold_budget ) + scratch_budget +
								  ( persistent ? uint64_t( backing ) : uint64_t( capacities[HOT] ) ) +
								  HOT_FRAME_BYTES * uint64_t( ( m_hotRangeAbi2 ? slots + 1u
																: !persistent  ? 1u
																			   : 0u ) );
		const uint64_t available = stats.limit > stats.committed ? stats.limit - stats.committed : 0u;
		if ( required > available ) {
			g_t3dMemoryAccount->recordDenial();
			return fail( String( "Shared arena memory budget insufficient: required_bytes=" ) +
						 String::num_uint64( required ) + " available_bytes=" + String::num_uint64( available ) +
						 " committed_bytes=" + String::num_uint64( stats.committed ) +
						 " total_limit_bytes=" + String::num_uint64( stats.limit ) );
		}
	}
	static const char	*names[3] = { "T3D.Shared.ColdArena", "T3D.Shared.HotArena", "T3D.Shared.ScratchArena" };
	Terrain3DRdGpuBuffer buffers[3];
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = names[i];
		spec.m_growth = T3D_GPU_FIXED;
		spec.m_type = i == SCRATCH ? T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER : T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = i == SCRATCH ? T3D_RD_CONTEXT_MEMORY_SCRATCH : T3D_RD_CONTEXT_MEMORY_STORAGE;
		if ( i == HOT && persistent ) {
			spec.m_creationBits = Terrain3DRdGpuBuffer::persistentCreationBits();
			spec.m_nBackingBytes = uint64_t( backing );
		}
		if ( !buffers[i].configure( spec ) || !buffers[i].createTracked( *this, capacities[i] ) ) {
			for ( auto &buffer : buffers ) {
				buffer.releaseTracked( *this );
			}
			return fail( String( "Failed to allocate shared arena " ) + names[i] +
						 " capacity_bytes=" + String::num_uint64( capacities[i] ) );
		}
	}
	if ( ( !persistent || m_hotRangeAbi2 ) &&
		 ( !m_hotImageMemory.resize( HOT_FRAME_BYTES ) || m_hotImage.resize( HOT_FRAME_BYTES ) != OK ) ) {
		m_pHotImageData = NULL;
		m_hotImage.clear();
		m_hotImageMemory.reset();
		for ( auto &buffer : buffers ) {
			buffer.releaseTracked( *this );
		}
		return fail( "Failed to reserve the ordinary HOT CPU publication image" );
	}
	// Take the only writable pointer at cold setup, before any render pass.
	m_pHotImageData = ( persistent && !m_hotRangeAbi2 ) ? NULL : m_hotImage.ptrw();
	if ( ( !persistent || m_hotRangeAbi2 ) && !m_pHotImageData ) {
		m_hotImage.clear();
		m_hotImageMemory.reset();
		for ( auto &buffer : buffers ) {
			buffer.releaseTracked( *this );
		}
		return fail( "Failed to acquire the preallocated HOT CPU publication image" );
	}
	if ( m_hotRangeAbi2 &&
		 !m_hotDirty.initialize( sizeof( Terrain3DRdHotRecord ), HOT_RECORD_CAPACITY, slots, m_pHotImageData ) ) {
		m_pHotImageData = NULL;
		m_hotImage.clear();
		m_hotImageMemory.reset();
		for ( auto &buffer : buffers ) {
			buffer.releaseTracked( *this );
		}
		return fail( "Failed to allocate HOT dirty-range mirrors and comparison metadata" );
	}
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		m_sharedArenas[i].swap( buffers[i] );
	}
	if ( !arena( COLD ).isValid() || !arena( HOT ).isValid() || !arena( SCRATCH ).isValid() ) {
		return fail( "Shared arena ownership transfer produced an invalid GPU buffer RID" );
	}
	return true;
}

bool Terrain3DRdContext::reserveArenaSpan(
	SharedArena			   id,
	uint32_t			   bytes,
	uint32_t			   stride,
	t3dRdGpuBufferSlice_t &span ) {
	Terrain3DScopedLock lock( m_arenaMutex );
	auto fail = [this, id, bytes, stride]( const char *reason ) {
		++m_nArenaReservationFailures;
		const uint32_t capacity = id < SHARED_ARENA_COUNT ? arena( id ).capacityBytes() : 0u;
		const uint32_t cursor = id < SHARED_ARENA_COUNT ? m_arenaCursor[id] : 0u;
		m_lastArenaReservationFailure = String( reason ) + " arena=" + String::num_uint64( id ) +
			" requested_bytes=" + String::num_uint64( bytes ) +
			" stride=" + String::num_uint64( stride ) +
			" cursor_bytes=" + String::num_uint64( cursor ) +
			" capacity_bytes=" + String::num_uint64( capacity ) +
			" available_tail_bytes=" + String::num_uint64( capacity > cursor ? capacity - cursor : 0u );
		return false;
	};
	if ( id > SCRATCH || id == HOT || !bytes || !stride ) {
		return fail( "invalid_shared_arena_request" );
	}
	if ( !initializeSharedArenas() ) {
		return fail( "shared_arena_not_initialized" );
	}
	Terrain3DRdGpuBuffer &buffer = arena( id );
	if ( !buffer.isValid() ) {
		return fail( "arena_backing_rid_invalid" );
	}
	if ( buffer.ownsSlice( span ) ) {
		for ( const SharedArenaLease &lease : m_arenaLeases ) {
			if ( lease.m_bLive && lease.m_nArena == id && lease.m_nOffset == span.m_nOffsetBytes &&
				 lease.m_nBytes == span.m_nCapacityBytes && lease.m_nGeneration == span.m_nGeneration ) {
				return bytes <= span.m_nCapacityBytes && span.m_nOffsetBytes % stride == 0u
					 ? true : fail( "existing_lease_incompatible" );
			}
		}
		return fail( "unregistered_existing_arena_span" );
	}
	// Best-fit reuse avoids exhausting a shared process-lifetime backing after
	// Terrain3D scene destruction/recreation. A leased span is never recycled.
	SharedArenaLease *reusable = NULL;
	SharedArenaLease *vacant = NULL;
	const uint32_t generation = buffer.generation();
	for ( SharedArenaLease &lease : m_arenaLeases ) {
		if ( !lease.m_bLive ) {
			if ( lease.m_nBytes == 0u || lease.m_nGeneration != generation ) {
				if ( !vacant ) {
					vacant = &lease;
				}
			} else if ( lease.m_nArena == id && lease.m_nBytes >= bytes &&
					 lease.m_nOffset % stride == 0u &&
					 ( !reusable || lease.m_nBytes < reusable->m_nBytes ) ) {
				reusable = &lease;
			}
		}
	}
	if ( reusable ) {
		if ( !buffer.slice( reusable->m_nOffset, reusable->m_nBytes, 16u, span ) ) {
			return fail( "reusable_arena_slice_invalid" );
		}
		reusable->m_bLive = true;
		++m_nArenaLeaseReuses;
		m_lastArenaReservationFailure = String();
		return true;
	}
	if ( !vacant ) {
		return fail( "arena_lease_table_exhausted" );
	}
	uint32_t cursor = m_arenaCursor[id], offset = 0u;
	if ( !t3dRdGpuBufferSection( cursor, bytes, 16u, stride, 0u, offset ) ) {
		return fail( "arena_reservation_alignment_overflow" );
	}
	if ( offset > buffer.capacityBytes() || bytes > buffer.capacityBytes() - offset ) {
		return fail( "arena_capacity_exhausted" );
	}
	if ( !buffer.slice( offset, bytes, 16u, span ) ) {
		return fail( "arena_slice_creation_failed" );
	}
	*vacant = { id, offset, bytes, generation, true };
	m_arenaCursor[id] = cursor;
	m_lastArenaReservationFailure = String();
	return true;
}

bool Terrain3DRdContext::releaseArenaSpanFinal(
	SharedArena			   id,
	t3dRdGpuBufferSlice_t &span ) {
	Terrain3DScopedLock lock( m_arenaMutex );
	if ( id > SCRATCH || id == HOT || !arena( id ).ownsSlice( span ) ) {
		span = {};
		return false;
	}
	for ( SharedArenaLease &lease : m_arenaLeases ) {
		if ( lease.m_bLive && lease.m_nArena == id && lease.m_nOffset == span.m_nOffsetBytes &&
			 lease.m_nBytes == span.m_nCapacityBytes && lease.m_nGeneration == span.m_nGeneration ) {
			lease.m_bLive = false;
			span = {};
			return true;
		}
	}
	span = {};
	return false;
}

bool Terrain3DRdContext::acquireFixedIndirect(
	t3dRdFixedIndirectSlot_t slot,
	uint32_t				 bytes,
	Terrain3DRdGpuBuffer   *&buffer,
	uint32_t				&offset ) {
	if ( slot >= T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT || buffer || !bytes ||
		 bytes > t3d_rd_context_contract::fixedIndirectCapacity( slot ) || !initializeSharedArenas() ) {
		return false;
	}
	++m_nFixedIndirectReferences[slot];
	buffer = &arena( SCRATCH );
	offset = t3d_rd_context_contract::fixedIndirectOffset( slot );
	return true;
}

void Terrain3DRdContext::releaseFixedIndirect(
	t3dRdFixedIndirectSlot_t slot,
	Terrain3DRdGpuBuffer   *&buffer ) {
	if ( slot < T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT && buffer == &arena( SCRATCH ) &&
		 m_nFixedIndirectReferences[slot] ) {
		--m_nFixedIndirectReferences[slot];
	}
	buffer = NULL; // Shared physical lifetime belongs to the context.
}

bool Terrain3DRdContext::beginHotFrame(
	uint64_t render_frame ) {
	if ( m_hotRenderFrame == render_frame ) {
		return m_hotPending && !m_hotFailed;
	}
	if ( !getDevice() && !initialize() ) {
		return false;
	}
	if ( m_hotPending || !initializeSharedArenas() ) {
		return false;
	}
	m_hotRenderFrame = render_frame;
	m_hotAttempted = true;
	m_hotFailed = true;
	m_hotCount = 0u;
	m_hotIndex = UINT32_MAX;
	m_hotCopiedBytes = 0u;
	m_hotSections = 0u;
	m_hotRangeActive = false;
	m_hotRangeBatch = {};
	RenderingDevice *rd = getDevice();
	if ( !m_hotBridge ) {
		return false;
	}
	const Variant frame_value = m_hotBridge->call( "get_native_device_frame" );
	if ( frame_value.get_type() != Variant::INT || int64_t( frame_value ) < 0 ) {
		return false;
	}
	const uint64_t device_frame = uint64_t( int64_t( frame_value ) );
	if ( m_hotDeviceFrame == device_frame && m_hotStandalone ) {
		return false;
	}
	m_hotStandalone = false;
	m_hotDeviceFrame = device_frame;
	if ( arena( HOT ).isPersistent() ) {
		m_hotSlotBase = 0u;
		if ( m_hotRangeAbi2 ) {
			// Map first, then obtain the physical slot identity of THIS mapping.
			if ( !arena( HOT ).beginFrameWrite( rd, m_hotDeviceFrame, m_hotImage, false ) ) {
				return false;
			}
			const Variant slot_value = m_hotBridge->call( "get_persistent_hot_frame_slot", arena( HOT ).rid() );
			if ( slot_value.get_type() != Variant::INT || int64_t( slot_value ) < 0 ||
				 uint64_t( int64_t( slot_value ) ) >= rd->get_frame_delay() ||
				 !m_hotDirty.begin( uint32_t( int64_t( slot_value ) ) ) ) {
				arena( HOT ).endFrameWriteWithoutSubmit( *this );
				return false;
			}
			if ( !bool( m_hotBridge->call( "defer_native_buffer_flush_ranges",
										   arena( HOT ).rid(),
										   int64_t( HOT_FRAME_BYTES ),
										   int64_t( uintptr_t( &Terrain3DRdContext::prepareHotRanges ) ),
										   int64_t( uintptr_t( &Terrain3DRdContext::completeHotRanges ) ),
										   int64_t( uintptr_t( this ) ) ) ) ) {
				m_hotDirty.abandon();
				arena( HOT ).endFrameWriteWithoutSubmit( *this );
				return false;
			}
			m_hotRangeActive = true;
		} else {
			// Map first. Registering the deferred callback before a failing map
			// used to leave m_hotPending latched and block subsequent frames.
			if ( !arena( HOT ).beginFrameWrite( rd, m_hotDeviceFrame, m_hotImage, false ) ) {
				return false;
			}
			if ( !bool( m_hotBridge->call( "defer_native_buffer_flush",
										   arena( HOT ).rid(),
										   int64_t( uintptr_t( &Terrain3DRdContext::sealHotFrame ) ),
										   int64_t( uintptr_t( this ) ) ) ) ) {
				arena( HOT ).endFrameWriteWithoutSubmit( *this );
				return false;
			}
		}
	} else {
		if ( !rd->has_method( "get_frame_slot" ) ) {
			return false;
		}
		const int64_t slot = rd->call( "get_frame_slot" );
		if ( slot < 0 || uint64_t( slot ) >= arena( HOT ).capacityBytes() / HOT_FRAME_BYTES ) {
			return false;
		}
		m_hotSlotBase = uint32_t( slot ) * HOT_RECORD_CAPACITY;
		if ( m_hotRangeAbi2 ) {
			if ( !m_hotDirty.begin( uint32_t( slot ) ) ) {
				return false;
			}
			if ( !bool( m_hotBridge->call( "defer_native_buffer_update_ranges",
										   arena( HOT ).rid(),
										   int64_t( uint32_t( slot ) * HOT_FRAME_BYTES ),
										   int64_t( HOT_FRAME_BYTES ),
										   int64_t( uintptr_t( &Terrain3DRdContext::prepareHotRanges ) ),
										   int64_t( uintptr_t( &Terrain3DRdContext::completeHotRanges ) ),
										   int64_t( uintptr_t( this ) ) ) ) ) {
				m_hotDirty.abandon();
				return false;
			}
			m_hotRangeActive = true;
		} else {
			if ( !m_hotBridge->has_method( "get_deferred_upload_abi_version" ) ||
				 int64_t( m_hotBridge->call( "get_deferred_upload_abi_version" ) ) != 1 ) {
				return false;
			}
			if ( !bool( m_hotBridge->call( "defer_native_buffer_update",
										   arena( HOT ).rid(),
										   int64_t( uint32_t( slot ) * HOT_FRAME_BYTES ),
										   int64_t( HOT_FRAME_BYTES ),
										   int64_t( uintptr_t( &Terrain3DRdContext::fillHotFrame ) ),
										   int64_t( uintptr_t( this ) ) ) ) ) {
				return false;
			}
		}
	}
	m_hotPending = true;
	m_hotFailed = false;
	return true;
}

void Terrain3DRdContext::invalidateHotMirrors() {
	if ( !m_hotRangeAbi2 ) {
		return;
	}
	m_hotDirty.invalidateAllSlots();
	++m_hotRecoveries;
}

void Terrain3DRdContext::recordHotDirtyOutcome(
	bool	 success,
	bool	 persistent,
	uint32_t upload_bytes,
	uint32_t flush_bytes ) {
	m_hotLastDirtyRanges = m_hotDirty.rangeCount();
	m_hotLastDirtyBytes = m_hotDirty.dirtyBytes();
	m_hotLastDirtyBlocks = m_hotLastDirtyBytes / Terrain3DHotDirtyImage::BLOCK_BYTES;
	m_hotLastInitializedBlocks = m_hotDirty.initializedBlocks();
	m_hotLastUploadBytes = success ? upload_bytes : 0u;
	m_hotLastFlushBytes = success ? flush_bytes : 0u;
	m_hotLastSlot = m_hotDirty.physicalSlot();
	if ( success ) {
		m_hotDirtyBytesTotal += m_hotLastDirtyBytes;
		m_hotUploadBytesTotal += upload_bytes;
		m_hotFlushBytesTotal += flush_bytes;
		if ( !m_hotLastDirtyRanges ) {
			++m_hotNoopFrames;
		}
	} else {
		++m_hotFailedPublications;
		++m_hotRecoveries;
	}
	( void ) persistent;
}

bool Terrain3DRdContext::beginStandaloneHotFrame() {
	if ( m_hotPending ) {
		return !m_hotFailed;
	} // Append to the native finalizer.
	if ( !initializeSharedArenas() || !m_hotBridge ) {
		return false;
	}
	const Variant frame_value = m_hotBridge->call( "get_native_device_frame" );
	if ( frame_value.get_type() != Variant::INT || int64_t( frame_value ) < 0 ) {
		return false;
	}
	const uint64_t frame = uint64_t( int64_t( frame_value ) );
	if ( m_hotAttempted && m_hotDeviceFrame == frame ) {
		return false;
	}
	RenderingDevice *rd = getDevice();
	m_hotDeviceFrame = frame;
	m_hotAttempted = true;
	m_hotFailed = true;
	m_hotStandalone = true;
	m_hotRangeActive = false;
	m_hotRangeBatch = {};
	m_hotCount = 0u;
	m_hotIndex = UINT32_MAX;
	m_hotCopiedBytes = 0u;
	m_hotSections = 0u;
	if ( arena( HOT ).isPersistent() ) {
		m_hotSlotBase = 0u;
		if ( !arena( HOT ).beginFrameWrite( rd, frame, m_hotImage, false ) ) {
			return false;
		}
		if ( m_hotRangeAbi2 ) {
			const Variant slot_value = m_hotBridge->call( "get_persistent_hot_frame_slot", arena( HOT ).rid() );
			if ( slot_value.get_type() != Variant::INT || int64_t( slot_value ) < 0 ||
				 uint64_t( int64_t( slot_value ) ) >= rd->get_frame_delay() ||
				 !m_hotDirty.begin( uint32_t( int64_t( slot_value ) ) ) ) {
				arena( HOT ).endFrameWriteWithoutSubmit( *this );
				return false;
			}
			m_hotRangeActive = true;
		}
	} else {
		if ( !rd->has_method( "get_frame_slot" ) ) {
			return false;
		}
		const int64_t slot = rd->call( "get_frame_slot" );
		if ( slot < 0 || uint64_t( slot ) >= arena( HOT ).capacityBytes() / HOT_FRAME_BYTES ) {
			return false;
		}
		m_hotSlotBase = uint32_t( slot ) * HOT_RECORD_CAPACITY;
		if ( m_hotRangeAbi2 ) {
			if ( !m_hotDirty.begin( uint32_t( slot ) ) ) {
				return false;
			}
			m_hotRangeActive = true;
		}
	}
	m_hotFailed = false;
	m_hotPending = true;
	return true;
}

bool Terrain3DRdContext::publishStandaloneHotFrame() {
	if ( !m_hotStandalone ) {
		return m_hotPending && !m_hotFailed;
	}
	if ( !m_hotPending ) {
		return false;
	}
	const bool persistent = arena( HOT ).isPersistent();
	bool	   success = !m_hotFailed;
	uint32_t   upload_bytes = 0u, flush_bytes = 0u;
	if ( m_hotRangeActive ) {
		if ( success ) {
			success = m_hotDirty.finalize();
		}
		if ( success && persistent ) {
			const t3dHotRange_t *ranges = m_hotDirty.ranges();
			for ( uint32_t i = 0u; i < m_hotDirty.rangeCount(); ++i ) {
				const t3dHotRange_t &range = ranges[i];
				if ( !arena( HOT ).writeFrameSpan( range.offset_bytes, range.source, range.size_bytes ) ) {
					success = false;
					break;
				}
			}
			// Standalone is synchronous and outside the native Forward+ finalizer.
			// Its legacy persistent flush is whole-buffer, but no data is cleared.
			if ( success && m_hotDirty.rangeCount() ) {
				success = arena( HOT ).endFrameWrite( *this );
				if ( success ) {
					flush_bytes = HOT_FRAME_BYTES;
				}
			} else {
				success = arena( HOT ).endFrameWriteWithoutSubmit( *this ) && success;
			}
		} else if ( persistent ) {
			arena( HOT ).endFrameWriteWithoutSubmit( *this );
		} else if ( success ) {
			RenderingDevice *rd = getDevice();
			if ( !rd ) {
				success = false;
			}
			for ( uint32_t i = 0u; success && i < m_hotDirty.rangeCount(); ++i ) {
				const t3dHotRange_t &range = m_hotDirty.ranges()[i];
				// Godot's public buffer_update accepts PackedByteArray; the
				// standalone-only copy does not affect the native Forward+ path.
				const PackedByteArray bytes =
					m_hotImage.slice( range.offset_bytes, range.offset_bytes + range.size_bytes );
				success = rd->buffer_update( arena( HOT ).rid(),
											 m_hotSlotBase * sizeof( Terrain3DRdHotRecord ) + range.offset_bytes,
											 range.size_bytes,
											 bytes ) == OK;
				if ( success ) {
					upload_bytes += range.size_bytes;
				}
			}
		}
		if ( success && !persistent && upload_bytes ) {
			recordBufferUpdate( upload_bytes, T3D_RD_CONTEXT_OWNER_UNSPECIFIED, T3D_GPU_HOT_FRAME );
		}
		recordHotDirtyOutcome( success, persistent, upload_bytes, flush_bytes );
		if ( m_hotDirty.isOpen() ) {
			m_hotDirty.finish( success );
			if ( m_hotDirty.isOpen() ) {
				m_hotDirty.abandon();
			}
		}
		m_hotRangeActive = false;
	} else {
		if ( persistent ) {
			success = arena( HOT ).endFrameWrite( *this ) && success;
		} else if ( success ) {
			RenderingDevice *rd = getDevice();
			success = rd && rd->buffer_update( arena( HOT ).rid(),
											   m_hotSlotBase * sizeof( Terrain3DRdHotRecord ),
											   HOT_FRAME_BYTES,
											   m_hotImage ) == OK;
			if ( success ) {
				recordBufferUpdate( HOT_FRAME_BYTES, T3D_RD_CONTEXT_OWNER_UNSPECIFIED, T3D_GPU_HOT_FRAME );
			}
		}
		if ( success ) {
			++m_hotLegacyPublications;
		}
		// A legacy standalone publication changes GPU backing without advancing
		// the ABI-2 committed image. Force a full logical replay next time.
		if ( m_hotRangeAbi2 ) {
			invalidateHotMirrors();
		}
	}
	m_hotFailed = !success;
	m_hotPending = false;
	m_hotIndex = UINT32_MAX;
	return success;
}

bool Terrain3DRdContext::beginHotPass() {
	if ( !m_hotPending || m_hotFailed || m_hotCount == HOT_RECORD_CAPACITY ) {
		return false;
	}
	// ABI 2 tracks an allocated record's complete logical contents, including
	// implicit zero/default sections. ABI 1 retains its old per-record clear.
	const uint32_t offset = m_hotCount * sizeof( Terrain3DRdHotRecord );
	if ( m_hotRangeActive ) {
		if ( !m_hotDirty.reserveRecord() ) {
			m_hotFailed = true;
			return false;
		}
	} else if ( arena( HOT ).isPersistent() ) {
		if ( !arena( HOT ).clearFrameSpan( offset, sizeof( Terrain3DRdHotRecord ) ) ) {
			m_hotFailed = true;
			return false;
		}
	} else {
		if ( !m_pHotImageData ) {
			m_hotFailed = true;
			return false;
		}
		memset( m_pHotImageData + offset, 0, sizeof( Terrain3DRdHotRecord ) );
	}
	m_hotIndex = m_hotSlotBase + m_hotCount++;
	if ( m_hotCount > m_nHotPeakRecords ) {
		m_nHotPeakRecords = m_hotCount;
	}
	return true;
}

bool Terrain3DRdContext::writeHotRecord(
	uint32_t	offset,
	const void *data,
	uint32_t	bytes ) {
	if ( !m_hotPending || m_hotFailed || m_hotIndex == UINT32_MAX || offset > sizeof( Terrain3DRdHotRecord ) ||
		 bytes > sizeof( Terrain3DRdHotRecord ) - offset || ( bytes && !data ) ) {
		return false;
	}
	const uint32_t destination = ( m_hotIndex - m_hotSlotBase ) * sizeof( Terrain3DRdHotRecord ) + offset;
	if ( m_hotRangeActive ) {
		if ( !m_hotDirty.write( offset, data, bytes ) ) {
			m_hotFailed = true;
			return false;
		}
	} else if ( arena( HOT ).isPersistent() ) {
		if ( !arena( HOT ).writeFrameSpan( destination, data, bytes ) ) {
			m_hotFailed = true;
			return false;
		}
	} else if ( bytes ) {
		if ( !m_pHotImageData ) {
			m_hotFailed = true;
			return false;
		}
		memcpy( m_pHotImageData + destination, data, bytes );
	}
	m_hotCopiedBytes += bytes;
	++m_hotSections;
	return true;
}

const Terrain3DRdContext::HotRangeBatch *Terrain3DRdContext::prepareHotRanges(
	void					*userdata,
	const t3dHotRangeInfo_t *info ) noexcept {
	auto	  &context = *static_cast< Terrain3DRdContext * >( userdata );
	const bool valid = context.m_hotPending && context.m_hotRangeActive && !context.m_hotFailed && info &&
					   info->abi == 2u && info->size == sizeof( t3dHotRangeInfo_t ) && !info->reserved &&
					   info->render_frame == context.m_hotRenderFrame &&
					   info->device_frame == context.m_hotDeviceFrame && info->byte_count == HOT_FRAME_BYTES &&
					   context.m_hotDirty.isOpen() && info->frame_slot == context.m_hotDirty.physicalSlot() &&
					   context.getDevice() && info->frame_slot_count == context.getDevice()->get_frame_delay();
	if ( !valid || !context.m_hotDirty.finalize() ) {
		context.m_hotFailed = true;
		return NULL;
	}
	context.m_hotRangeBatch = { 2u,
								sizeof( HotRangeBatch ),
								info->frame_slot,
								context.m_hotDirty.rangeCount(),
								context.m_hotDirty.ranges() };
	if ( context.arena( HOT ).isPersistent() ) {
		for ( uint32_t i = 0u; i < context.m_hotRangeBatch.range_count; ++i ) {
			const auto &range = context.m_hotRangeBatch.ranges[i];
			if ( !context.arena( HOT ).writeFrameSpan( range.offset_bytes, range.source, range.size_bytes ) ) {
				context.m_hotFailed = true;
				return NULL;
			}
		}
	}
	return &context.m_hotRangeBatch;
}

bool Terrain3DRdContext::completeHotRanges(
	void					*userdata,
	const t3dHotRangeInfo_t *info,
	bool					 success ) noexcept {
	auto	  &context = *static_cast< Terrain3DRdContext * >( userdata );
	const bool valid = success && !context.m_hotFailed && context.m_hotPending && context.m_hotRangeActive && info &&
					   info->abi == 2u && info->size == sizeof( t3dHotRangeInfo_t ) && !info->reserved &&
					   info->byte_count == HOT_FRAME_BYTES && info->device_frame == context.m_hotDeviceFrame &&
					   info->render_frame == context.m_hotRenderFrame &&
					   info->frame_slot == context.m_hotDirty.physicalSlot() && context.m_hotRangeBatch.abi == 2u &&
					   context.m_hotRangeBatch.size == sizeof( HotRangeBatch ) &&
					   context.m_hotRangeBatch.frame_slot == info->frame_slot &&
					   context.m_hotRangeBatch.range_count == context.m_hotDirty.rangeCount() && context.getDevice() &&
					   info->frame_slot_count == context.getDevice()->get_frame_delay();
	bool closed = true;
	if ( context.arena( HOT ).isPersistent() ) {
		closed =
			context.arena( HOT ).endFrameWriteWithoutSubmit( *static_cast< Terrain3DRdContextAccess * >( &context ) );
	}
	const bool prepared = context.m_hotDirty.rangeCount() || context.m_hotDirty.isOpen();
	bool	   committed = false;
	if ( prepared && context.m_hotDirty.isOpen() ) {
		const bool accepted =
			valid && closed && context.m_hotDirty.rangeCount() <= HOT_FRAME_BYTES / Terrain3DHotDirtyImage::BLOCK_BYTES;
		const uint32_t dirty_bytes = context.m_hotDirty.dirtyBytes();
		context.recordHotDirtyOutcome( accepted,
									   context.arena( HOT ).isPersistent(),
									   accepted && !context.arena( HOT ).isPersistent() ? dirty_bytes : 0u,
									   accepted && context.arena( HOT ).isPersistent() ? dirty_bytes : 0u );
		committed = context.m_hotDirty.finish( accepted );
		if ( context.m_hotDirty.isOpen() ) {
			context.m_hotDirty.abandon();
		}
	}
	if ( committed ) {
		uint32_t bytes = 0u;
		for ( uint32_t i = 0u; i < context.m_hotRangeBatch.range_count; ++i ) {
			bytes += context.m_hotRangeBatch.ranges[i].size_bytes;
		}
		if ( bytes ) {
			context.recordBufferUpdate( bytes, T3D_RD_CONTEXT_OWNER_UNSPECIFIED, T3D_GPU_HOT_FRAME );
		}
	}
	context.m_hotFailed = !committed;
	context.m_hotPending = false;
	context.m_hotRangeActive = false;
	context.m_hotIndex = UINT32_MAX;
	return committed;
}

void Terrain3DRdContext::fillHotFrame(
	void			   *userdata,
	const HotFrameInfo *info,
	uint8_t			   *destination ) noexcept {
	auto	  &context = *static_cast< Terrain3DRdContext * >( userdata );
	const bool valid = context.m_hotPending && !context.m_hotFailed && info && destination && info->abi == 1u &&
					   info->size == sizeof( HotFrameInfo ) && !info->reserved &&
					   info->render_frame == context.m_hotRenderFrame && info->byte_count == HOT_FRAME_BYTES &&
					   info->device_frame == context.m_hotDeviceFrame &&
					   info->frame_slot_count == context.arena( HOT ).capacityBytes() / HOT_FRAME_BYTES &&
					   info->frame_slot < info->frame_slot_count &&
					   info->frame_slot * HOT_RECORD_CAPACITY == context.m_hotSlotBase && context.m_pHotImageData;
	if ( destination ) {
		if ( valid && context.m_pHotImageData ) {
			memcpy( destination, context.m_pHotImageData, HOT_FRAME_BYTES );
		} else {
			memset( destination, 0, HOT_FRAME_BYTES );
		}
	}
	context.m_hotDeviceFrame = info ? info->device_frame : 0u;
	context.m_hotFailed = !valid;
	context.m_hotPending = false;
	context.m_hotIndex = UINT32_MAX;
	if ( valid ) {
		context.recordBufferUpdate( HOT_FRAME_BYTES, T3D_RD_CONTEXT_OWNER_UNSPECIFIED, T3D_GPU_HOT_FRAME );
	}
}

bool Terrain3DRdContext::sealHotFrame(
	void			   *userdata,
	const HotFrameInfo *info ) noexcept {
	auto	  &context = *static_cast< Terrain3DRdContext * >( userdata );
	const bool valid = context.m_hotPending && !context.m_hotFailed && info && info->abi == 1u && context.getDevice() &&
					   info->size == sizeof( HotFrameInfo ) && info->render_frame == context.m_hotRenderFrame &&
					   info->device_frame == context.m_hotDeviceFrame && info->byte_count == HOT_FRAME_BYTES &&
					   info->frame_slot_count == context.getDevice()->get_frame_delay() &&
					   info->frame_slot < info->frame_slot_count && !info->reserved;
	const bool published = context.arena( HOT ).endFrameWrite( context );
	context.m_hotFailed = !valid || !published;
	context.m_hotPending = false;
	context.m_hotIndex = UINT32_MAX;
	return !context.m_hotFailed; // Engine must discard dependent work on failure.
}

void Terrain3DRdContext::markHotShader(
	const Ref< RDShaderSPIRV > &shader,
	int							binding ) {
	if ( shader.is_null() || !shader->has_method( "set_dynamic_buffers" ) ) {
		return;
	}
	PackedInt64Array dynamic;
	if ( arena( HOT ).isPersistent() ) {
		dynamic.push_back( binding );
	}
	shader->call( "set_dynamic_buffers", dynamic );
}

Dictionary Terrain3DRdContext::getSharedArenaDiagnostics() const {
	Terrain3DScopedLock lock( m_arenaMutex );
	Dictionary result;
	result["owned_live_buffers"] =
		int64_t( arena( COLD ).isValid() + arena( HOT ).isValid() + arena( SCRATCH ).isValid() );
	result["cold_capacity_bytes"] = arena( COLD ).capacityBytes();
	result["cold_arena_cursor_bytes"] = m_arenaCursor[COLD];
	result["scratch_arena_cursor_bytes"] = m_arenaCursor[SCRATCH];
	result["arena_reservation_failures"] = int64_t( m_nArenaReservationFailures );
	result["arena_lease_reuses"] = int64_t( m_nArenaLeaseReuses );
	result["last_arena_reservation_failure"] = m_lastArenaReservationFailure;
	uint32_t active = 0u, reusable = 0u;
	for ( const SharedArenaLease &lease : m_arenaLeases ) {
		if ( lease.m_nBytes ) {
			lease.m_bLive ? ++active : ++reusable;
		}
	}
	result["arena_active_leases"] = active;
	result["arena_reusable_leases"] = reusable;
	result["scratch_capacity_bytes"] = arena( SCRATCH ).capacityBytes();
	result["hot_capacity_bytes"] = arena( HOT ).capacityBytes();
	result["hot_record_capacity"] = HOT_RECORD_CAPACITY;
	result["hot_peak_records"] = m_nHotPeakRecords;
	result["hot_records_written"] = m_hotCount;
	result["hot_render_frame"] = int64_t( m_hotRenderFrame );
	result["hot_device_frame"] = int64_t( m_hotDeviceFrame );
	result["persistent_active"] = arena( HOT ).isPersistent();
	result["hot_dirty_range_abi2"] = m_hotRangeAbi2;
	result["hot_dirty_range_active"] = m_hotRangeActive;
	result["hot_dirty_blocks"] = m_hotLastDirtyBlocks;
	result["hot_dirty_ranges"] = m_hotLastDirtyRanges;
	result["hot_dirty_bytes"] = m_hotLastDirtyBytes;
	result["hot_bytes_written_cpu"] = int64_t( m_hotCopiedBytes );
	result["hot_bytes_uploaded"] = m_hotLastUploadBytes;
	result["hot_flush_bytes"] = m_hotLastFlushBytes;
	result["hot_noop_frames"] = int64_t( m_hotNoopFrames );
	result["hot_full_initializations"] = int64_t( m_hotFullInitializations ); // Never a full clear in ABI 2.
	result["hot_initialized_blocks"] = m_hotLastInitializedBlocks;
	result["hot_physical_slot"] = m_hotLastSlot == UINT32_MAX ? -1 : int64_t( m_hotLastSlot );
	result["hot_recoveries"] = int64_t( m_hotRecoveries );
	result["hot_failed_publications"] = int64_t( m_hotFailedPublications );
	result["hot_legacy_publications"] = int64_t( m_hotLegacyPublications );
	result["hot_dirty_bytes_total"] = int64_t( m_hotDirtyBytesTotal );
	result["hot_uploaded_bytes_total"] = int64_t( m_hotUploadBytesTotal );
	result["hot_flushed_bytes_total"] = int64_t( m_hotFlushBytesTotal );
	result["publication_pending"] = m_hotPending;
	result["publication_failed"] = m_hotFailed;
	const auto &frame = arena( HOT ).framePublication();
	result["map_advances"] = frame.map_advances;
	result["hot_flushes"] = frame.flushes;
	result["hot_buffer_updates"] = arena( HOT ).isPersistent() ? 0
								   : m_hotRangeAbi2
									   ? int( !m_hotPending && !m_hotFailed ? m_hotRangeBatch.range_count : 0u )
									   : int( m_hotAttempted && !m_hotPending && !m_hotFailed );
	result["hot_bytes_copied"] = int64_t( m_hotCopiedBytes );
	result["hot_sections_written"] = m_hotSections;
	result["hot_backing_bytes"] = int64_t( arena( HOT ).spec().m_nBackingBytes ? arena( HOT ).spec().m_nBackingBytes
																			   : arena( HOT ).capacityBytes() );
	return result;
}

bool Terrain3DRdContext::validateDevice(
	void ) {
	if ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) != T3D_RD_CONTEXT_STATE_READY ) {
		return false;
	}
	RenderingDevice *const owned = m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE );
	RenderingDevice *const current = getGlobalDevice();
	if ( owned && owned == current ) {
		return true;
	}
	invalidateDevice( current ? "Global RenderingDevice identity changed"
							  : "Global RenderingDevice became unavailable" );
	return false;
}

void Terrain3DRdContext::shutdown(
	void ) {
	if ( m_hotPending && m_hotBridge && m_hotBridge->has_method( "complete_native_hot_frame" ) ) {
		m_hotBridge->call( "complete_native_hot_frame" );
	}
	ERR_FAIL_COND_MSG( m_hotPending,
					   "Godot must complete the pending HOT publication before releasing its callback or buffers." );
	m_hotDirty.release();
	m_hotRangeAbi2 = false;
	m_hotRangeActive = false;
	m_hotLastDirtyBytes = m_hotLastDirtyRanges = m_hotLastDirtyBlocks = 0u;
	m_hotLastUploadBytes = m_hotLastFlushBytes = m_hotLastInitializedBlocks = 0u;
	m_hotLastSlot = UINT32_MAX;
	m_pHotImageData = NULL;
	m_hotImage.clear();
	m_hotImageMemory.reset();
	{
		Terrain3DScopedLock lock( m_arenaMutex );
		m_arenaCursor[COLD] = 0u;
		m_arenaCursor[SCRATCH] = T3D_RD_FIXED_INDIRECT_ARENA_BYTES;
		for ( SharedArenaLease &lease : m_arenaLeases ) {
			lease = {};
		}
		m_lastArenaReservationFailure = String();
		m_nArenaReservationFailures = 0u;
		m_nArenaLeaseReuses = 0u;
	}
	m_hotRenderFrame = UINT64_MAX;
	m_hotAttempted = false;
	m_hotDeviceFrame = 0u;
	m_hotStandalone = false;
	m_hotFailed = false;
	m_hotIndex = UINT32_MAX;
	m_hotCount = 0u;
	m_nHotPeakRecords = 0u;
	m_state.exchange( T3D_RD_CONTEXT_STATE_SHUTTING_DOWN, T3D_MEMORY_ORDER_ACQ_REL );
	if ( m_pGlobalDescriptorCache ) {
		m_pGlobalDescriptorCache->shutdownCold();
	}
	RenderingDevice *const rd = m_pRd.exchange( NULL, T3D_MEMORY_ORDER_ACQ_REL );
	{
		Terrain3DScopedLock lock( m_resourceMutex );
		// Swap-removal keeps ordinary release O(1) after the hash lookup. Restore
		// creation order only during cold shutdown, where reverse destruction
		// remains important for dependent RenderingDevice resources.
		eastl::sort( m_resources.begin(), m_resources.end(),
			[]( const t3dRdResourceRecord_t &rLeft, const t3dRdResourceRecord_t &rRight ) {
				return rLeft.m_nCreationOrder < rRight.m_nCreationOrder;
			} );
		for ( t3dRdResourceList_t::reverse_iterator iterator = m_resources.rbegin(); iterator != m_resources.rend();
			  ++iterator ) {
			const t3dRdResourceRecord_t &record = *iterator;
			accountRelease( record );
			rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
			if ( !record.m_hRid.is_valid() ) {
				continue;
			}
			if ( rd ) {
				if ( record.m_type != T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET ||
					 rd->uniform_set_is_valid( record.m_hRid ) ) {
					rd->free_rid( record.m_hRid );
				}
				m_nFreedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			} else {
				m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			}
		}
		m_resources.clear();
		m_ridIndex.clear();
		m_nResourceCreationOrder = 0u;
		for ( auto &arena : m_sharedArenas ) {
			arena.abandon();
		}
		memset( m_nFixedIndirectReferences, 0, sizeof( m_nFixedIndirectReferences ) );
		m_vertexFormats.clear();
		resetLiveAccounting();
	}
	m_state.store( T3D_RD_CONTEXT_STATE_UNINITIALIZED, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::invalidateDevice(
	const String &rReason ) {
	if ( m_pGlobalDescriptorCache ) {
		m_pGlobalDescriptorCache->abandonDeviceCold();
	}
	m_pRd.store( NULL, T3D_MEMORY_ORDER_RELEASE );
	Terrain3DScopedLock lock( m_resourceMutex );
	for ( size_t iResource = 0u; iResource < ( size_t ) ( ( m_resources ).size() ); iResource++ ) {
		const t3dRdResourceRecord_t &record = ( m_resources )[iResource];
		accountRelease( record );
		rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
		m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	m_resources.clear();
	m_ridIndex.clear();
	m_nResourceCreationOrder = 0u;
	for ( auto &arena : m_sharedArenas ) {
		arena.abandon();
	}
	invalidateHotMirrors();
	if ( m_hotPending ) {
		m_hotFailed = true;
	} // A deferred callback must reject draws on a lost device.
	memset( m_nFixedIndirectReferences, 0, sizeof( m_nFixedIndirectReferences ) );
	m_vertexFormats.clear();
	resetLiveAccounting();
	m_lastFailure = rReason.is_empty() ? String( "RenderingDevice resources were invalidated" ) : rReason;
	m_nDeviceInvalidations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_failureCode.store( T3D_RD_CONTEXT_FAILURE_DEVICE_INVALIDATED, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( T3D_RD_CONTEXT_STATE_REBUILD_REQUIRED, T3D_MEMORY_ORDER_RELEASE );
}

bool Terrain3DRdContext::beginRebuild(
	void ) {
	const t3dRdContextState_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( state == T3D_RD_CONTEXT_STATE_REBUILDING ) {
		return m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) != NULL;
	}
	if ( state == T3D_RD_CONTEXT_STATE_READY || state == T3D_RD_CONTEXT_STATE_SHUTTING_DOWN ) {
		return false;
	}
	shutdown();

	RenderingDevice *const rd = getGlobalDevice();
	if ( !rd ) {
		setFailure( T3D_RD_CONTEXT_FAILURE_REBUILD, "RenderingDevice is unavailable during reconstruction" );
		return false;
	}
	m_pRd.store( rd, T3D_MEMORY_ORDER_RELEASE );
	m_nDeviceGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
	m_nDeviceInitializations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nDeviceRebuilds.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_failureCode.store( T3D_RD_CONTEXT_FAILURE_NONE, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( T3D_RD_CONTEXT_STATE_REBUILDING, T3D_MEMORY_ORDER_RELEASE );
	beginResourceRebuild();
	return true;
}

bool Terrain3DRdContext::completeRebuild(
	const bool	  bSuccess,
	const String &rFailure ) {
	if ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) != T3D_RD_CONTEXT_STATE_REBUILDING ) {
		return false;
	}
	endResourceRebuild();
	if ( !bSuccess || !m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		setFailure( T3D_RD_CONTEXT_FAILURE_REBUILD,
					rFailure.is_empty() ? String( "RenderingDevice reconstruction failed" ) : rFailure );
		return false;
	}
	{
		Terrain3DScopedLock lock( m_resourceMutex );
		m_lastFailure = String();
	}
	m_failureCode.store( T3D_RD_CONTEXT_FAILURE_NONE, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( T3D_RD_CONTEXT_STATE_READY, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

void Terrain3DRdContext::recordFailure(
	const t3dRdFailureCode_t code,
	const String			&rFailure ) {
	setFailure( code == T3D_RD_CONTEXT_FAILURE_NONE ? T3D_RD_CONTEXT_FAILURE_RESOURCE_CREATION : code,
				rFailure.is_empty() ? String( "Terrain3D RenderingDevice failure" ) : rFailure );
}

void Terrain3DRdContext::recordResourceCreationFailure(
	const String			 &rName,
	const t3dRdResourceType_t type,
	const String			 &rFailure ) {
	( void ) type;
	m_nFailedResourceCreates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	const String failure = rFailure.is_empty() ? String( "Failed to create RD resource: " ) + rName : rFailure;
	setFailure( T3D_RD_CONTEXT_FAILURE_RESOURCE_CREATION, failure );
}

bool Terrain3DRdContext::registerResourceInternal(
	const RID				   &rRid,
	const String			   &rName,
	const t3dRdResourceType_t	type,
	const t3dRdMemoryCategory_t memoryCategory,
	const uint64_t				nBytes,
	const t3dRdResourceOwner_t	owner ) {
	RenderingDevice *const rd = getDevice();
	if ( !rd || !rRid.is_valid() ) {
		recordResourceCreationFailure( rName,
									   type,
									   rRid.is_valid() ? String( "RD context is not accepting resources: " ) + rName
													   : String( "Invalid RID returned for: " ) + rName );
		return false;
	}
	if ( type >= T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT || memoryCategory >= T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT ||
		 owner >= T3D_RD_CONTEXT_OWNER_COUNT ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		setFailure( T3D_RD_CONTEXT_FAILURE_ACCOUNTING, "Invalid RD resource accounting category" );
		return false;
	}

	const t3dRdResourceOwner_t resolved_owner =
		owner == T3D_RD_CONTEXT_OWNER_UNSPECIFIED ? inferResourceOwner( rName ) : owner;
	bool duplicate = false;
	bool capacity_exhausted = false;
	{
		Terrain3DScopedLock lock( m_resourceMutex );
		const uint64_t		rid_id = rRid.get_id();
		duplicate = m_ridIndex.find( rid_id ) != UINT32_MAX;
		if ( !duplicate ) {
			const bool rebuild = hasResourceHistory( rName, type );
			if ( m_resources.size() >= T3D_RESOURCE_CAPACITY ) {
				capacity_exhausted = true;
			} else {
				if ( !rebuild ) {
					const t3dRdResourceHistory_t history{ rName, type };
					if ( m_resourceHistory.size() < T3D_RESOURCE_HISTORY_CAPACITY ) {
						m_resourceHistory.push_back( history );
					} else {
						m_resourceHistory[m_nResourceHistoryCursor] = history;
						m_nResourceHistoryCursor = ( m_nResourceHistoryCursor + 1u ) % T3D_RESOURCE_HISTORY_CAPACITY;
					}
				}
				for ( size_t i = 0u; i < m_freedResources.size(); ) {
					if ( m_freedResources[i].m_nRid == rid_id ) {
						m_freedResources.erase( m_freedResources.begin() + i );
					} else {
						i++;
					}
				}
				const uint32_t nRecordIndex = ( uint32_t ) m_resources.size();
				if ( !m_ridIndex.insert( rid_id, nRecordIndex ) ) {
					capacity_exhausted = true;
				} else {
					const t3dRdResourceRecord_t record{ rRid, rName, type, memoryCategory,
						resolved_owner, nBytes, getDeviceGeneration(), ++m_nResourceCreationOrder };
					m_resources.push_back( record );
					accountRegistration( record, rebuild );
				}
			}
		}
	}
	if ( duplicate ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		setFailure( T3D_RD_CONTEXT_FAILURE_DUPLICATE_RESOURCE, String( "RD resource registered twice: " ) + rName );
		return false;
	}
	if ( capacity_exhausted ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		setFailure( T3D_RD_CONTEXT_FAILURE_ACCOUNTING, "Terrain3D fixed RD resource registry capacity exhausted" );
		return false;
	}
	// Vertex and index arrays are RenderingDevice-side views without a driver
	// object. Godot's set_resource_name() deliberately does not accept those
	// RID owners, so keep their names in Terrain3D's resource records only.
	if ( type != T3D_RD_CONTEXT_RESOURCE_VERTEX_ARRAY && type != T3D_RD_CONTEXT_RESOURCE_INDEX_ARRAY ) {
		rd->set_resource_name( rRid, rName );
	}
	return true;
}

void Terrain3DRdContext::registerResource(
	const RID	  &rRid,
	const String  &rName,
	const uint64_t nBufferBytes ) {
	const t3dRdResourceType_t type = inferResourceType( rName );
	registerResourceInternal( rRid,
							  rName,
							  type,
							  inferMemoryCategory( rName, type, nBufferBytes ),
							  nBufferBytes,
							  inferResourceOwner( rName ) );
}

bool Terrain3DRdContext::registerResource(
	const RID				   &rRid,
	const String			   &rName,
	const t3dRdResourceType_t	type,
	const t3dRdMemoryCategory_t memoryCategory,
	const uint64_t				nBytes,
	const t3dRdResourceOwner_t	owner ) {
	return registerResourceInternal( rRid, rName, type, memoryCategory, nBytes, owner );
}

bool Terrain3DRdContext::registerVertexFormat(
	const int64_t nFormat,
	const String &rName ) {
	if ( !getDevice() || nFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		recordResourceCreationFailure( rName,
									   T3D_RD_CONTEXT_RESOURCE_VERTEX_ARRAY,
									   "Invalid Terrain3D RD vertex format" );
		return false;
	}
	Terrain3DScopedLock lock( m_resourceMutex );
	for ( size_t i = 0u; i < m_vertexFormats.size(); i++ ) {
		if ( m_vertexFormats[i].m_nFormat == nFormat ) {
			return true;
		}
	}
	if ( m_vertexFormats.size() >= T3D_VERTEX_FORMAT_CAPACITY ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return false;
	}
	m_vertexFormats.push_back( { nFormat, rName, getDeviceGeneration() } );
	return true;
}

void Terrain3DRdContext::releaseVertexFormat(
	int64_t &rFormat ) {
	if ( rFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		return;
	}
	Terrain3DScopedLock lock( m_resourceMutex );
	for ( size_t i = 0u; i < m_vertexFormats.size(); ) {
		if ( m_vertexFormats[i].m_nFormat == rFormat ) {
			m_vertexFormats.erase( m_vertexFormats.begin() + i );
		} else {
			i++;
		}
	}
	rFormat = RenderingDevice::INVALID_FORMAT_ID;
}

void Terrain3DRdContext::freeResource(
	RID			  &rRid,
	const uint64_t nBufferBytes ) {
	if ( !rRid.is_valid() ) {
		return;
	}
	// Mark dependent global descriptors stale before the physical RID is
	// destroyed. This runs outside the registry lock: descriptor retirement
	// calls back into freeResource for uniform sets.
	if ( m_pGlobalDescriptorCache && isOnRenderThread() ) {
		const uint64_t nDeviceGeneration = getDeviceGeneration();
		m_pGlobalDescriptorCache->invalidateDependencyCold( rRid, nDeviceGeneration );
		m_pGlobalDescriptorCache->invalidateShaderCold( rRid, nDeviceGeneration );
	}
	const uint64_t		  rid_id = rRid.get_id();
	t3dRdResourceRecord_t record;
	bool				  found = false;
	bool				  already_freed = false;
	bool				  byte_mismatch = false;
	{
		Terrain3DScopedLock			  lock( m_resourceMutex );
		const uint32_t nRecordIndex = m_ridIndex.find( rid_id );
		if ( nRecordIndex != UINT32_MAX ) {
			record = m_resources[nRecordIndex];
			found = true;
			byte_mismatch = nBufferBytes > 0u && record.m_nBytes != nBufferBytes;
			accountRelease( record );
			rememberFreedResource( rid_id, record.m_nDeviceGeneration );
			const uint32_t nLastIndex = ( uint32_t ) m_resources.size() - 1u;
			( void ) m_ridIndex.erase( rid_id );
			if ( nRecordIndex != nLastIndex ) {
				m_resources[nRecordIndex] = m_resources[nLastIndex];
				( void ) m_ridIndex.updateIndex( m_resources[nRecordIndex].m_hRid.get_id(), nRecordIndex );
			}
			m_resources.pop_back();
		} else {
			for ( size_t i = 0u; i < m_freedResources.size(); i++ ) {
				if ( m_freedResources[i].m_nRid == rid_id ) {
					already_freed = true;
					break;
				}
			}
		}
	}

	if ( byte_mismatch ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	if ( already_freed ) {
		m_nDoubleFreeAttempts.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		rRid = RID();
		return;
	}
	if ( !found ) {
		m_nUntrackedFreeAttempts.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		setFailure( T3D_RD_CONTEXT_FAILURE_UNTRACKED_RESOURCE, "Attempted to free an untracked Terrain3D RD resource" );
		// The registry does not own this RID. Leave it valid so its real owner can
		// release it, and never turn an accounting bug into destruction of an
		// external framebuffer attachment or texture.
		return;
	}
	RenderingDevice *const rd = m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( rd ) {
		// RenderingDevice invalidates uniform sets automatically when a borrowed
		// shader/buffer/texture dependency is destroyed. Their registry records are
		// still ours and must be retired for accounting, but calling free_rid() on
		// an already-invalidated set emits "Attempted to free invalid ID". This is
		// the only RD resource class with a public validity query and dependency-
		// driven invalidation semantics.
		if ( record.m_type != T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET || rd->uniform_set_is_valid( rRid ) ) {
			rd->free_rid( rRid );
		}
		m_nFreedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	} else {
		m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	rRid = RID();
}

bool Terrain3DRdContext::ownsResource(
	const RID &rRid ) const {
	if ( !rRid.is_valid() ) {
		return false;
	}
	const uint64_t		rid_id = rRid.get_id();
	Terrain3DScopedLock lock( m_resourceMutex );
	return m_ridIndex.find( rid_id ) != UINT32_MAX;
}

t3dRdResourceEpoch_t Terrain3DRdContext::getResourceEpoch(
	const t3dRdResourceOwner_t owner ) const {
	t3dRdResourceEpoch_t epoch;
	if ( owner >= T3D_RD_CONTEXT_OWNER_COUNT ) {
		return epoch;
	}
	epoch.nCreates = m_szOwnerCreates[owner].load( T3D_MEMORY_ORDER_ACQUIRE );
	epoch.nFrees = m_szOwnerFrees[owner].load( T3D_MEMORY_ORDER_ACQUIRE );
	epoch.nLive = m_szOwnerLive[owner].load( T3D_MEMORY_ORDER_ACQUIRE );
	epoch.nLiveBytes = m_szOwnerLiveBytes[owner].load( T3D_MEMORY_ORDER_ACQUIRE );
	epoch.nSteadyStateViolations = m_szOwnerSteadyStateViolations[owner].load( T3D_MEMORY_ORDER_ACQUIRE );
	return epoch;
}

void Terrain3DRdContext::markWarmupComplete(
	const bool bComplete ) {
	m_bWarmupComplete.store( bComplete, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::beginResourceRebuild(
	void ) {
	m_nExplicitRebuildDepth.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
}

void Terrain3DRdContext::endResourceRebuild(
	void ) {
	uint32_t depth = m_nExplicitRebuildDepth.load( T3D_MEMORY_ORDER_RELAXED );
	while ( depth > 0u && !m_nExplicitRebuildDepth.compare_exchange_weak( depth,
																		  depth - 1u,
																		  T3D_MEMORY_ORDER_ACQ_REL,
																		  T3D_MEMORY_ORDER_RELAXED ) ) {
	}
}

void Terrain3DRdContext::beginFrame(
	const uint64_t nGpuFrameId,
	const uint64_t nSnapshotGeneration ) {
	m_nGpuFrameId.store( nGpuFrameId, T3D_MEMORY_ORDER_RELEASE );
	m_nSnapshotGeneration.store( nSnapshotGeneration, T3D_MEMORY_ORDER_RELEASE );
	if ( !m_renderTelemetry.beginFrame( nGpuFrameId ) ) {
		return;
	}
	m_nFrameBufferUpdates.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nFrameBytesUploaded.store( 0u, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::recordBufferUpdate(
	const uint64_t				   nBytes,
	const t3dRdResourceOwner_t	   owner,
	const Terrain3DGpuBufferPolicy policy ) {
	if ( nBytes == 0u ) {
		return;
	}
	m_nBufferUpdates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nBytesUploaded.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
	m_nFrameBufferUpdates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nFrameBytesUploaded.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
	m_renderTelemetry.recordUpload( owner, policy, nBytes );
	if ( policy == T3D_GPU_DIRTY_UPLOAD && nBytes > 0u ) {
		m_renderTelemetry.recordDirtyRangeUploaded( owner );
	}
}

void Terrain3DRdContext::recordTextureUpload(
	const uint64_t				   nBytes,
	const t3dRdResourceOwner_t	   owner,
	const Terrain3DGpuBufferPolicy policy ) {
	if ( nBytes == 0u ) {
		return;
	}
	m_nTextureUploads.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nTextureBytesUploaded.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
	m_renderTelemetry.recordUpload( owner, policy, nBytes );
}

Dictionary Terrain3DRdContext::getGpuUploadDiagnostics(
	void ) const {
	// Atomic CPU counters only: polling this must never synchronize the GPU.
	Dictionary				 result, current, completed;
	static const char *const policies[T3D_GPU_POLICY_COUNT] =
		{ "publication", "dirty_upload", "hot_frame", "scratch", "history", "indirect", "neutral" };
	uint64_t bytes = 0u, calls = 0u, previous_bytes = 0u, previous_calls = 0u;
	for ( uint32_t owner = 0; owner < T3D_RD_CONTEXT_OWNER_COUNT; ++owner ) {
		for ( uint32_t policy = 0; policy < T3D_GPU_POLICY_COUNT; ++policy ) {
			const uint64_t frame_bytes = m_renderTelemetry.getFrameUploadBytes( owner, policy );
			const uint64_t frame_calls = m_renderTelemetry.getFrameUploadCalls( owner, policy );
			const uint64_t last_bytes = m_renderTelemetry.getFrameUploadBytes( owner, policy, true );
			const uint64_t last_calls = m_renderTelemetry.getFrameUploadCalls( owner, policy, true );
			bytes += frame_bytes;
			calls += frame_calls;
			previous_bytes += last_bytes;
			previous_calls += last_calls;
			const String key = String( s_owner_keys[owner] ) + String( "_" ) + String( policies[policy] );
			if ( frame_calls ) {
				current[key + String( "_bytes" )] = ( int64_t ) frame_bytes;
				current[key + String( "_calls" )] = ( int64_t ) frame_calls;
			}
			if ( last_calls ) {
				completed[key + String( "_bytes" )] = ( int64_t ) last_bytes;
				completed[key + String( "_calls" )] = ( int64_t ) last_calls;
			}
		}
	}
	result["gpu_frame_id"] = ( int64_t ) m_nGpuFrameId.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["completed_frame_id"] = ( int64_t ) m_renderTelemetry.getCompletedFrameId();
	result["bytes_uploaded_frame"] = ( int64_t ) bytes;
	result["upload_calls_frame"] = ( int64_t ) calls;
	result["bytes_uploaded_completed_frame"] = ( int64_t ) previous_bytes;
	result["upload_calls_completed_frame"] = ( int64_t ) previous_calls;
	result["peak_completed_frame_bytes"] = ( int64_t ) m_renderTelemetry.getPeakFrameBytes();
	result["buffer_upload_bytes"] = ( int64_t ) m_nBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["texture_upload_bytes"] = ( int64_t ) m_nTextureBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["texture_upload_calls"] = ( int64_t ) m_nTextureUploads.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["bytes_uploaded"] = ( int64_t ) ( m_nBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE ) +
											 m_nTextureBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["frame_by_owner_and_policy"] = current;
	result["completed_frame_by_owner_and_policy"] = completed;
	result["scope"] = "shared RenderingDevice; successful host uploads; excludes GPU copies/readbacks";
	return result;
}

Dictionary Terrain3DRdContext::getPublishedResourceDiagnostics(
	void ) const {
	// These counters are atomic. Sampling must not inspect shared RD maps,
	// descriptor tables, or engine resources from the owner thread.
	Dictionary counters;
	counters["resource_creates"] = ( int64_t ) m_nCreatedResources.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["resource_frees"] = ( int64_t ) m_nFreedResources.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["shader_creates"] =
		( int64_t ) m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_SHADER].load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["pipeline_creates"] = ( int64_t ) m_nPipelineCreates.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["pipeline_rebuilds"] = ( int64_t ) m_nPipelineRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["uniform_set_creates"] = ( int64_t ) m_nUniformSetCreates.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["uniform_set_rebuilds"] = ( int64_t ) m_nUniformSetRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["live_buffer_bytes"] = ( int64_t ) m_nLiveBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["peak_buffer_bytes"] = ( int64_t ) m_nPeakBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["live_texture_bytes"] = ( int64_t ) m_nLiveTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["peak_texture_bytes"] = ( int64_t ) m_nPeakTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["buffer_updates"] = ( int64_t ) m_nBufferUpdates.load( T3D_MEMORY_ORDER_ACQUIRE );
	counters["buffer_updates_frame"] = ( int64_t ) m_nFrameBufferUpdates.load( T3D_MEMORY_ORDER_ACQUIRE );
	return counters;
}

void Terrain3DRdContext::recordRenderEvent(
	const t3dRdResourceOwner_t		owner,
	const t3dRenderTelemetryEvent_t event ) {
	m_renderTelemetry.recordEvent( owner, event );
}

void Terrain3DRdContext::recordRenderCapacity(
	const t3dRdResourceOwner_t	   owner,
	const Terrain3DGpuBufferPolicy policy,
	const uint64_t				   nBytes ) {
	m_renderTelemetry.recordCapacity( owner, policy, nBytes );
}

void Terrain3DRdContext::recordRenderIndirectCommandsSubmitted(
	const t3dRdResourceOwner_t owner,
	const uint64_t			   nCommands ) {
	m_renderTelemetry.recordIndirectCommandsSubmitted( owner, nCommands );
}

uint64_t Terrain3DRdContext::getRenderEvent(
	const t3dRdResourceOwner_t		owner,
	const t3dRenderTelemetryEvent_t event ) const {
	return m_renderTelemetry.getEvent( owner, event );
}

uint64_t Terrain3DRdContext::getRenderUploadCalls(
	const t3dRdResourceOwner_t	   owner,
	const Terrain3DGpuBufferPolicy policy ) const {
	return m_renderTelemetry.getUploadCalls( owner, policy );
}

uint64_t Terrain3DRdContext::getRenderUploadBytes(
	const t3dRdResourceOwner_t	   owner,
	const Terrain3DGpuBufferPolicy policy ) const {
	return m_renderTelemetry.getUploadBytes( owner, policy );
}

uint64_t Terrain3DRdContext::getRenderDirtyRangesUploaded(
	const t3dRdResourceOwner_t owner ) const {
	return m_renderTelemetry.getDirtyRangesUploaded( owner );
}

void Terrain3DRdContext::recordSuccessfulPass(
	const t3dRdSuccessfulPass_t pass,
	const uint64_t				nGpuFrameId ) {
	m_lastSuccessfulPass.store( pass < T3D_RD_CONTEXT_PASS_COUNT ? pass : T3D_RD_CONTEXT_PASS_NONE,
								T3D_MEMORY_ORDER_RELEASE );
	m_nLastSuccessfulGpuFrame.store( nGpuFrameId, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::recordSceneCounts(
	const uint64_t nTerrainPatches,
	const uint64_t nVisibleTerrainPatches,
	const uint64_t nStaticInstances,
	const uint64_t nVisibleInstances,
	const uint64_t nHlods,
	const uint64_t nVisibleHlods ) {
	m_nTerrainPatchCount.store( nTerrainPatches, T3D_MEMORY_ORDER_RELEASE );
	m_nVisibleTerrainPatches.store( nVisibleTerrainPatches, T3D_MEMORY_ORDER_RELEASE );
	m_nStaticInstanceCount.store( nStaticInstances, T3D_MEMORY_ORDER_RELEASE );
	m_nVisibleInstances.store( nVisibleInstances, T3D_MEMORY_ORDER_RELEASE );
	m_nHlodCount.store( nHlods, T3D_MEMORY_ORDER_RELEASE );
	m_nVisibleHlods.store( nVisibleHlods, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::recordSubmissionCounts(
	const uint64_t nIndirectCommands,
	const uint64_t nComputeDispatches ) {
	m_nIndirectCommandCount.store( nIndirectCommands, T3D_MEMORY_ORDER_RELEASE );
	m_nComputeDispatchCount.store( nComputeDispatches, T3D_MEMORY_ORDER_RELEASE );
}

Dictionary Terrain3DRdContext::getDiagnostics(
	void ) const {
	Dictionary				  result;
	const t3dRdContextState_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	const t3dRdFailureCode_t  failure = m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["state"] = s_state_names[state];
	result["failure_state"] = s_failure_names[failure];
	result["shared_arenas"] = getSharedArenaDiagnostics();
	result["ready"] = isReady();
	result["device_generation"] = ( int64_t ) ( m_nDeviceGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["device_initializations"] = ( int64_t ) ( m_nDeviceInitializations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["device_rebuilds"] = ( int64_t ) ( m_nDeviceRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["device_invalidations"] = ( int64_t ) ( m_nDeviceInvalidations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["resource_creates"] = ( int64_t ) ( m_nCreatedResources.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["resource_frees"] = ( int64_t ) ( m_nFreedResources.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["resource_abandons"] = ( int64_t ) ( m_nAbandonedResources.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["failed_resource_creates"] = ( int64_t ) ( m_nFailedResourceCreates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["double_free_attempts"] = ( int64_t ) ( m_nDoubleFreeAttempts.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["untracked_free_attempts"] = ( int64_t ) ( m_nUntrackedFreeAttempts.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["accounting_errors"] = ( int64_t ) ( m_nAccountingErrors.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["recoverable_failure_count"] = ( int64_t ) ( m_nRecoverableFailureCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["live_buffer_bytes"] = ( int64_t ) ( m_nLiveBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["peak_buffer_bytes"] = ( int64_t ) ( m_nPeakBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["live_texture_bytes"] = ( int64_t ) ( m_nLiveTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["peak_texture_bytes"] = ( int64_t ) ( m_nPeakTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["buffer_updates"] = ( int64_t ) ( m_nBufferUpdates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["bytes_uploaded"] = ( int64_t ) ( m_nBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["buffer_updates_frame"] = ( int64_t ) ( m_nFrameBufferUpdates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["bytes_uploaded_frame"] = ( int64_t ) ( m_nFrameBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["gpu_uploads"] = getGpuUploadDiagnostics();
	result["gpu_frame_id"] = ( int64_t ) ( m_nGpuFrameId.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["snapshot_generation"] = ( int64_t ) ( m_nSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["last_successful_gpu_frame"] = ( int64_t ) ( m_nLastSuccessfulGpuFrame.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["vb_creates"] =
		( int64_t ) ( m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["vb_frees"] =
		( int64_t ) ( m_szTypeFrees[T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["ib_creates"] =
		( int64_t ) ( m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["ib_frees"] =
		( int64_t ) ( m_szTypeFrees[T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["shader_creates"] =
		( int64_t ) ( m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_SHADER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["pipeline_creates"] = ( int64_t ) ( m_nPipelineCreates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["pipeline_rebuilds"] = ( int64_t ) ( m_nPipelineRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["uniform_set_creates"] = ( int64_t ) ( m_nUniformSetCreates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["uniform_set_rebuilds"] = ( int64_t ) ( m_nUniformSetRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["steady_state_allocation_violations"] =
		( int64_t ) ( m_nSteadyStateAllocationViolations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["terrain_patch_count"] = ( int64_t ) ( m_nTerrainPatchCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["visible_terrain_patches"] = ( int64_t ) ( m_nVisibleTerrainPatches.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["static_instance_count"] = ( int64_t ) ( m_nStaticInstanceCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["visible_instances"] = ( int64_t ) ( m_nVisibleInstances.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["hlod_count"] = ( int64_t ) ( m_nHlodCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["visible_hlods"] = ( int64_t ) ( m_nVisibleHlods.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["indirect_command_count"] = ( int64_t ) ( m_nIndirectCommandCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["compute_dispatch_count"] = ( int64_t ) ( m_nComputeDispatchCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );

	uint64_t owned_bytes = 0u;
	for ( uint32_t category = T3D_RD_CONTEXT_MEMORY_NONE + 1u; category < T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT;
		  ++category ) {
		const uint64_t live = m_szLiveMemory[category].load( T3D_MEMORY_ORDER_ACQUIRE );
		const uint64_t peak = m_szPeakMemory[category].load( T3D_MEMORY_ORDER_ACQUIRE );
		result[String( s_memory_keys[category] ) + "_bytes"] = ( int64_t ) ( live );
		result[String( "peak_" ) + s_memory_keys[category] + "_bytes"] = ( int64_t ) ( peak );
		owned_bytes += live;
	}
	result["topology_memory"] =
		( int64_t ) ( m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_TOPOLOGY_VERTEX].load( T3D_MEMORY_ORDER_ACQUIRE ) +
					  m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_TOPOLOGY_INDEX].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["mesh_arena_memory"] =
		( int64_t ) ( m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_MESH_VERTEX_ARENA].load( T3D_MEMORY_ORDER_ACQUIRE ) +
					  m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_MESH_INDEX_ARENA].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["texture_memory"] =
		( int64_t ) ( m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["owned_gpu_bytes"] = ( int64_t ) ( owned_bytes );
	for ( uint32_t owner = 0u; owner < T3D_RD_CONTEXT_OWNER_COUNT; ++owner ) {
		const String prefix = String( "owner_" ) + String( s_owner_keys[owner] );
		result[prefix + String( "_creates" )] =
			( int64_t ) ( m_szOwnerCreates[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_frees" )] = ( int64_t ) ( m_szOwnerFrees[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_live" )] = ( int64_t ) ( m_szOwnerLive[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_live_bytes" )] =
			( int64_t ) ( m_szOwnerLiveBytes[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_steady_state_violations" )] =
			( int64_t ) ( m_szOwnerSteadyStateViolations[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	}
	static const char *const policy_names[T3D_GPU_POLICY_COUNT] =
		{ "publication", "dirty_upload", "hot_frame", "scratch", "history", "indirect", "neutral" };
	static_assert( T3D_GPU_POLICY_COUNT == 7u, "Update render upload telemetry policy names" );
	static const char *const event_names[EVENT_COUNT] = { "buffer_creates",
														  "buffer_rebuilds",
														  "descriptor_creates",
														  "pipeline_creates",
														  "descriptor_evictions",
														  "pipeline_evictions",
														  "target_creates",
														  "target_reuses",
														  "hiz_builds",
														  "hiz_rebinds",
														  "resource_failures" };
	static_assert( EVENT_COUNT == 11u, "Update render telemetry event names" );
	Dictionary upload_telemetry;
	Dictionary event_telemetry;
	for ( uint32_t owner = 0u; owner < T3D_RD_CONTEXT_OWNER_COUNT; ++owner ) {
		const uint64_t dirty_ranges = m_renderTelemetry.getDirtyRangesUploaded( owner );
		if ( dirty_ranges > 0u ) {
			upload_telemetry[String( s_owner_keys[owner] ) + String( "_dirty_ranges" )] = ( int64_t ) dirty_ranges;
		}
		const uint64_t indirect_commands = m_renderTelemetry.getIndirectCommandsSubmitted( owner );
		if ( indirect_commands > 0u ) {
			event_telemetry[String( s_owner_keys[owner] ) + String( "_indirect_commands_submitted" )] =
				( int64_t ) indirect_commands;
		}
		for ( uint32_t policy = 0u; policy < T3D_GPU_POLICY_COUNT; ++policy ) {
			const uint64_t capacity = m_renderTelemetry.getCapacityHighWater( owner, policy );
			if ( capacity > 0u ) {
				const String capacity_key = String( s_owner_keys[owner] ) + String( "_" ) + policy_names[policy];
				upload_telemetry[capacity_key + String( "_capacity_high_water_bytes" )] = ( int64_t ) capacity;
			}
			const uint64_t calls = m_renderTelemetry.getUploadCalls( owner, policy );
			if ( calls == 0u ) {
				continue;
			}
			const String prefix = String( s_owner_keys[owner] ) + String( "_" ) + policy_names[policy];
			upload_telemetry[prefix + String( "_calls" )] = ( int64_t ) calls;
			upload_telemetry[prefix + String( "_bytes" )] =
				( int64_t ) m_renderTelemetry.getUploadBytes( owner, policy );
		}
		for ( uint32_t event = 0u; event < EVENT_COUNT; ++event ) {
			const uint64_t count = m_renderTelemetry.getEvent( owner, ( t3dRenderTelemetryEvent_t ) event );
			if ( count > 0u ) {
				event_telemetry[String( s_owner_keys[owner] ) + String( "_" ) + event_names[event]] = ( int64_t ) count;
			}
		}
	}
	result["render_upload_telemetry"] = upload_telemetry;
	result["render_event_telemetry"] = event_telemetry;

	{
		Terrain3DScopedLock lock( m_resourceMutex );
		Dictionary			physical;
		static const char  *kinds[] = { "ubo", "ssbo", "indirect" };
		uint64_t			live[T3D_RD_CONTEXT_OWNER_COUNT][3]{}, bytes[T3D_RD_CONTEXT_OWNER_COUNT][3]{};
		for ( const auto &record : m_resources ) {
			const int kind = bufferKind( record.m_type );
			if ( kind >= 0 ) {
				++live[record.m_owner][kind];
				bytes[record.m_owner][kind] += record.m_nBytes;
			}
		}
		for ( uint32_t owner = 0u; owner < T3D_RD_CONTEXT_OWNER_COUNT; ++owner ) {
			for ( uint32_t kind = 0u; kind < 3u; ++kind ) {
				Dictionary row;
				row["live"] = int64_t( live[owner][kind] );
				row["capacity_bytes"] = int64_t( bytes[owner][kind] );
				row["created"] = int64_t( m_physicalBufferCreates[owner][kind] );
				row["released_or_abandoned"] = int64_t( m_physicalBufferReleases[owner][kind] );
				physical[String( s_owner_keys[owner] ) + "_" + kinds[kind]] = row;
			}
		}
		result["physical_buffers"] = physical;
		result["physical_buffer_counter_scope"] =
			"Registered RID lifetime; driver deferred destruction and in-flight retired bytes are not observable here";
		result["live_resource_count"] = ( int64_t ) ( m_resources.size() );
		result["vertex_format_count"] = ( int64_t ) ( m_vertexFormats.size() );
		result["last_failure"] = m_lastFailure;
		result["last_recoverable_failure"] = m_lastRecoverableFailure;
	}
	result["last_successful_pass"] = s_pass_names[m_lastSuccessfulPass.load( T3D_MEMORY_ORDER_ACQUIRE )];
	RenderingDevice *const rd = m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( rd ) {
		result["device_name"] = rd->get_device_name();
		result["device_vendor"] = rd->get_device_vendor_name();
		result["device_memory"] = ( int64_t ) ( rd->get_device_total_memory() );
	}
	return result;
}
