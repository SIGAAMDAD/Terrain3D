// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
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

static const char *const s_pass_names[] = {
	"none",
	"terrain_clipmap",
};

static_assert( sizeof( s_memory_keys ) / sizeof( s_memory_keys[0] ) == T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT );
static_assert( sizeof( s_owner_keys ) / sizeof( s_owner_keys[0] ) == T3D_RD_CONTEXT_OWNER_COUNT );
static_assert( sizeof( s_state_names ) / sizeof( s_state_names[0] ) == T3D_RD_CONTEXT_STATE_SHUTTING_DOWN + 1u );
static_assert( sizeof( s_failure_names ) / sizeof( s_failure_names[0] ) == T3D_RD_CONTEXT_FAILURE_RENDER_PASS + 1u );
static_assert( sizeof( s_pass_names ) / sizeof( s_pass_names[0] ) == T3D_RD_CONTEXT_PASS_COUNT );


RenderingDevice *Terrain3DRdContext::getGlobalDevice( void ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	return pServer ? pServer->get_rendering_device() : NULL;
}

RenderingDevice *Terrain3DRdContext::createLocalDevice( void ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	return pServer ? pServer->create_local_rendering_device() : NULL;
}

bool Terrain3DRdContext::isOnOwnerThread( void ) {
	OS *const pOs = OS::get_singleton();
	return pOs && pOs->get_thread_caller_id() == pOs->get_main_thread_id();
}

bool Terrain3DRdContext::isOnRenderThread( void ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	return pServer && pServer->is_on_render_thread();
}

bool Terrain3DRdContext::enqueueOnRenderThread( const Callable &rCallable ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer || !rCallable.is_valid() ) {
		return false;
	}
	pServer->call_on_render_thread( rCallable );
	return true;
}

void Terrain3DRdContext::forceSync( void ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && !pServer->is_on_render_thread() ) {
		pServer->force_sync();
	}
}

Terrain3DRdContext &Terrain3DRdContext::getShared( void ) {
	// Intentionally process-lifetime storage. Godot module shutdown explicitly
	// releases the RD resources before the rendering server disappears.
	static Terrain3DRdContext *const pContext = new Terrain3DRdContext;
	return *pContext;
}

void Terrain3DRdContext::shutdownShared( void ) {
	getShared().shutdown();
}

Terrain3DRdContext::~Terrain3DRdContext( void ) {
	shutdown();
	delete m_pFixedIndirectArena;
	m_pFixedIndirectArena = NULL;
}

void Terrain3DRdContext::updatePeak( t3dAtomicU64_t &rPeak, const uint64_t nLiveBytes ) {
	uint64_t peak = rPeak.load( T3D_MEMORY_ORDER_RELAXED );
	while ( nLiveBytes > peak && !rPeak.compare_exchange_weak( peak, nLiveBytes, T3D_MEMORY_ORDER_RELEASE, T3D_MEMORY_ORDER_RELAXED ) ) {
	}
}

bool Terrain3DRdContext::subtractSaturating( t3dAtomicU64_t &rValue, const uint64_t nAmount ) {
	uint64_t value = rValue.load( T3D_MEMORY_ORDER_RELAXED );
	for ( ;; ) {
		const bool valid = value >= nAmount;
		const uint64_t replacement = valid ? value - nAmount : 0u;
		if ( rValue.compare_exchange_weak( value, replacement, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_RELAXED ) ) {
			return valid;
		}
	}
}

void Terrain3DRdContext::rememberFreedResource( const uint64_t nRid, const uint64_t nDeviceGeneration ) {
	if ( m_freedResources.size() >= T3D_FREED_HISTORY_CAPACITY ) {
		m_freedResources.erase( m_freedResources.begin() );
	}
	m_freedResources.push_back( { nRid, nDeviceGeneration } );
}

void Terrain3DRdContext::updateBufferPeak( const uint64_t nLiveBytes ) {
	updatePeak( m_nPeakBufferBytes, nLiveBytes );
}

void Terrain3DRdContext::updateTexturePeak( const uint64_t nLiveBytes ) {
	updatePeak( m_nPeakTextureBytes, nLiveBytes );
}

t3dRdResourceType_t Terrain3DRdContext::inferResourceType( const String &rName ) {
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
        const String &rName, const t3dRdResourceType_t type, const uint64_t nBytes ) {
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

t3dRdResourceOwner_t Terrain3DRdContext::inferResourceOwner( const String &rName ) {
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

bool Terrain3DRdContext::isPipeline( const t3dRdResourceType_t type ) {
	return type == T3D_RD_CONTEXT_RESOURCE_RASTER_PIPELINE || type == T3D_RD_CONTEXT_RESOURCE_COMPUTE_PIPELINE;
}

bool Terrain3DRdContext::isSteadyStateAllocation( const t3dRdResourceType_t type ) {
	return type > T3D_RD_CONTEXT_RESOURCE_UNKNOWN && type < T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT;
}

bool Terrain3DRdContext::hasResourceHistory( const String &rName, const t3dRdResourceType_t type ) const {
	for ( size_t i = 0u; i < m_resourceHistory.size(); i++ ) {
		const t3dRdResourceHistory_t &rHistory = m_resourceHistory[i];
		if ( rHistory.m_type == type && rHistory.m_name == rName ) {
			return true;
		}
	}
	return false;
}

void Terrain3DRdContext::accountRegistration( const t3dRdResourceRecord_t &rRecord, const bool bRebuild ) {
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

	if ( m_bWarmupComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) && m_nExplicitRebuildDepth.load( T3D_MEMORY_ORDER_ACQUIRE ) == 0u
	        && isSteadyStateAllocation( rRecord.m_type ) ) {
		m_nSteadyStateAllocationViolations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( rRecord.m_owner < T3D_RD_CONTEXT_OWNER_COUNT ) {
			m_szOwnerSteadyStateViolations[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	if ( rRecord.m_nBytes == 0u || rRecord.m_memoryCategory == T3D_RD_CONTEXT_MEMORY_NONE ) {
		return;
	}
	const uint64_t category_live =
	        m_szLiveMemory[rRecord.m_memoryCategory].fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_ACQ_REL ) + rRecord.m_nBytes;
	updatePeak( m_szPeakMemory[rRecord.m_memoryCategory], category_live );
	if ( rRecord.m_type == T3D_RD_CONTEXT_RESOURCE_TEXTURE ) {
		const uint64_t live = m_nLiveTextureBytes.fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_ACQ_REL ) + rRecord.m_nBytes;
		updateTexturePeak( live );
	} else {
		const uint64_t live = m_nLiveBufferBytes.fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_ACQ_REL ) + rRecord.m_nBytes;
		updateBufferPeak( live );
	}
}

void Terrain3DRdContext::accountRelease( const t3dRdResourceRecord_t &rRecord ) {
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
	t3dAtomicU64_t &aggregate = rRecord.m_type == T3D_RD_CONTEXT_RESOURCE_TEXTURE ? m_nLiveTextureBytes : m_nLiveBufferBytes;
	if ( !subtractSaturating( aggregate, rRecord.m_nBytes ) ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
}

void Terrain3DRdContext::resetLiveAccounting( void ) {
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

void Terrain3DRdContext::setFailure( const t3dRdFailureCode_t code, const String &rFailure ) {
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

void Terrain3DRdContext::beginRecoverableFailureScope( void ) {
	++s_nRecoverableFailureDepth;
}

void Terrain3DRdContext::endRecoverableFailureScope( void ) {
	if ( s_nRecoverableFailureDepth == 0u ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return;
	}
	--s_nRecoverableFailureDepth;
}

bool Terrain3DRdContext::initialize( void ) {
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

bool Terrain3DRdContext::acquireFixedIndirect( const t3dRdFixedIndirectSlot_t slot,
        const uint32_t nRequiredBytes, Terrain3DRdGpuBuffer *&rBuffer, uint32_t &rOffsetBytes ) {
	if ( slot >= T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT || nRequiredBytes == 0u || nRequiredBytes > t3d_rd_context_contract::fixedIndirectCapacity( slot )
	        || rBuffer != NULL || t3d_rd_context_contract::fixedIndirectOffset( slot ) + t3d_rd_context_contract::fixedIndirectCapacity( slot ) > T3D_RD_FIXED_INDIRECT_ARENA_BYTES
	        || !getDevice() ) {
		return false;
	}
	if ( !m_pFixedIndirectArena ) {
		m_pFixedIndirectArena = new Terrain3DRdGpuBuffer;
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.FixedIndirectArena";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_INDIRECT_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_INDIRECT;
		spec.m_growth = T3D_GPU_FIXED;
		spec.m_nMinimumCapacityBytes = T3D_RD_FIXED_INDIRECT_ARENA_BYTES;
		if ( !m_pFixedIndirectArena->configure( spec ) ) {
			delete m_pFixedIndirectArena;
			m_pFixedIndirectArena = NULL;
			return false;
		}
	}
	if ( !m_pFixedIndirectArena->isValid() && !m_pFixedIndirectArena->createTracked( *this, T3D_RD_FIXED_INDIRECT_ARENA_BYTES ) ) {
		return false;
	}
	++m_nFixedIndirectReferences[slot];
	const t3dRdResourceOwner_t owner = slot == T3D_RD_CONTEXT_FIXED_INDIRECT_WORLD ? T3D_RD_CONTEXT_OWNER_WORLD : slot == T3D_RD_CONTEXT_FIXED_INDIRECT_OCEAN ? T3D_RD_CONTEXT_OWNER_OCEAN : T3D_RD_CONTEXT_OWNER_TERRAIN;
	recordRenderCapacity( owner, T3D_GPU_INDIRECT, t3d_rd_context_contract::fixedIndirectCapacity( slot ) );
	rBuffer = m_pFixedIndirectArena;
	rOffsetBytes = t3d_rd_context_contract::fixedIndirectOffset( slot );
	return true;
}

void Terrain3DRdContext::releaseFixedIndirect( const t3dRdFixedIndirectSlot_t slot, Terrain3DRdGpuBuffer *&rBuffer ) {
	if ( slot >= T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT || !rBuffer ) {
		rBuffer = NULL;
		return;
	}
	if ( rBuffer == m_pFixedIndirectArena && m_nFixedIndirectReferences[slot] > 0u ) {
		--m_nFixedIndirectReferences[slot];
	}
	rBuffer = NULL;
	for ( uint32_t i = 0u; i < T3D_RD_CONTEXT_FIXED_INDIRECT_SLOT_COUNT; ++i ) {
		if ( m_nFixedIndirectReferences[i] != 0u ) {
			return;
		}
	}
	if ( m_pFixedIndirectArena && m_pFixedIndirectArena->isValid() ) {
		m_pFixedIndirectArena->releaseTracked( *this );
	}
}

bool Terrain3DRdContext::validateDevice( void ) {
	if ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) != T3D_RD_CONTEXT_STATE_READY ) {
		return false;
	}
	RenderingDevice *const owned = m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE );
	RenderingDevice *const current = getGlobalDevice();
	if ( owned && owned == current ) {
		return true;
	}
	invalidateDevice( current ? "Global RenderingDevice identity changed" : "Global RenderingDevice became unavailable" );
	return false;
}

void Terrain3DRdContext::shutdown( void ) {
	m_state.exchange( T3D_RD_CONTEXT_STATE_SHUTTING_DOWN, T3D_MEMORY_ORDER_ACQ_REL );
	RenderingDevice *const rd = m_pRd.exchange( NULL, T3D_MEMORY_ORDER_ACQ_REL );
	{
		Terrain3DScopedLock lock( m_resourceMutex );
		for ( t3dRdResourceList_t::reverse_iterator iterator = m_resources.rbegin(); iterator != m_resources.rend(); ++iterator ) {
			const t3dRdResourceRecord_t &record = *iterator;
			accountRelease( record );
			rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
			if ( !record.m_hRid.is_valid() ) {
				continue;
			}
			if ( rd ) {
				if ( record.m_type != T3D_RD_CONTEXT_RESOURCE_UNIFORM_SET || rd->uniform_set_is_valid( record.m_hRid ) ) {
					rd->free_rid( record.m_hRid );
				}
				m_nFreedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			} else {
				m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			}
		}
		m_resources.clear();
		if ( m_pFixedIndirectArena ) {
			m_pFixedIndirectArena->abandon();
		}
		memset( m_nFixedIndirectReferences, 0, sizeof( m_nFixedIndirectReferences ) );
		m_vertexFormats.clear();
		resetLiveAccounting();
	}
	m_state.store( T3D_RD_CONTEXT_STATE_UNINITIALIZED, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::invalidateDevice( const String &rReason ) {
	m_pRd.store( NULL, T3D_MEMORY_ORDER_RELEASE );
	Terrain3DScopedLock lock( m_resourceMutex );
	for ( size_t iResource = 0u; iResource < ( size_t )( ( m_resources ).size() ); iResource++ ) {
		const t3dRdResourceRecord_t &record = ( m_resources )[iResource];
		accountRelease( record );
		rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
		m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	m_resources.clear();
	if ( m_pFixedIndirectArena ) {
		m_pFixedIndirectArena->abandon();
	}
	memset( m_nFixedIndirectReferences, 0, sizeof( m_nFixedIndirectReferences ) );
	m_vertexFormats.clear();
	resetLiveAccounting();
	m_lastFailure = rReason.is_empty() ? String( "RenderingDevice resources were invalidated" ) : rReason;
	m_nDeviceInvalidations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_failureCode.store( T3D_RD_CONTEXT_FAILURE_DEVICE_INVALIDATED, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( T3D_RD_CONTEXT_STATE_REBUILD_REQUIRED, T3D_MEMORY_ORDER_RELEASE );
}

bool Terrain3DRdContext::beginRebuild( void ) {
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

bool Terrain3DRdContext::completeRebuild( const bool bSuccess, const String &rFailure ) {
	if ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) != T3D_RD_CONTEXT_STATE_REBUILDING ) {
		return false;
	}
	endResourceRebuild();
	if ( !bSuccess || !m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		setFailure( T3D_RD_CONTEXT_FAILURE_REBUILD, rFailure.is_empty() ? String( "RenderingDevice reconstruction failed" ) : rFailure );
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

void Terrain3DRdContext::recordFailure( const t3dRdFailureCode_t code, const String &rFailure ) {
	setFailure( code == T3D_RD_CONTEXT_FAILURE_NONE ? T3D_RD_CONTEXT_FAILURE_RESOURCE_CREATION : code,
	        rFailure.is_empty() ? String( "Terrain3D RenderingDevice failure" ) : rFailure );
}

void Terrain3DRdContext::recordResourceCreationFailure( const String &rName, const t3dRdResourceType_t type, const String &rFailure ) {
	( void )type;
	m_nFailedResourceCreates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	const String failure = rFailure.is_empty() ? String( "Failed to create RD resource: " ) + rName : rFailure;
	setFailure( T3D_RD_CONTEXT_FAILURE_RESOURCE_CREATION, failure );
}

bool Terrain3DRdContext::registerResourceInternal( const RID &rRid,
        const String &rName,
        const t3dRdResourceType_t type,
        const t3dRdMemoryCategory_t memoryCategory,
        const uint64_t nBytes,
        const t3dRdResourceOwner_t owner ) {
	RenderingDevice *const rd = getDevice();
	if ( !rd || !rRid.is_valid() ) {
		recordResourceCreationFailure( rName,
		        type,
		        rRid.is_valid() ? String( "RD context is not accepting resources: " ) + rName
		                        : String( "Invalid RID returned for: " ) + rName );
		return false;
	}
	if ( type >= T3D_RD_CONTEXT_RESOURCE_TYPE_COUNT || memoryCategory >= T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT || owner >= T3D_RD_CONTEXT_OWNER_COUNT ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		setFailure( T3D_RD_CONTEXT_FAILURE_ACCOUNTING, "Invalid RD resource accounting category" );
		return false;
	}

	const t3dRdResourceOwner_t resolved_owner = owner == T3D_RD_CONTEXT_OWNER_UNSPECIFIED ? inferResourceOwner( rName ) : owner;
	bool duplicate = false;
	bool capacity_exhausted = false;
	{
		Terrain3DScopedLock lock( m_resourceMutex );
		const uint64_t rid_id = rRid.get_id();
		for ( size_t i = 0u; i < m_resources.size(); i++ ) {
			if ( ( uint64_t )( m_resources[i].m_hRid.get_id() ) == rid_id ) {
				duplicate = true;
				break;
			}
		}
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
				const t3dRdResourceRecord_t record{ rRid, rName, type, memoryCategory, resolved_owner, nBytes, getDeviceGeneration() };
				m_resources.push_back( record );
				accountRegistration( record, rebuild );
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

void Terrain3DRdContext::registerResource( const RID &rRid, const String &rName, const uint64_t nBufferBytes ) {
	const t3dRdResourceType_t type = inferResourceType( rName );
	registerResourceInternal(
	        rRid, rName, type, inferMemoryCategory( rName, type, nBufferBytes ), nBufferBytes, inferResourceOwner( rName ) );
}

bool Terrain3DRdContext::registerResource( const RID &rRid,
        const String &rName,
        const t3dRdResourceType_t type,
        const t3dRdMemoryCategory_t memoryCategory,
        const uint64_t nBytes,
        const t3dRdResourceOwner_t owner ) {
	return registerResourceInternal( rRid, rName, type, memoryCategory, nBytes, owner );
}

bool Terrain3DRdContext::registerVertexFormat( const int64_t nFormat, const String &rName ) {
	if ( !getDevice() || nFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		recordResourceCreationFailure( rName, T3D_RD_CONTEXT_RESOURCE_VERTEX_ARRAY, "Invalid Terrain3D RD vertex format" );
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

void Terrain3DRdContext::releaseVertexFormat( int64_t &rFormat ) {
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

void Terrain3DRdContext::freeResource( RID &rRid, const uint64_t nBufferBytes ) {
	if ( !rRid.is_valid() ) {
		return;
	}
	const uint64_t rid_id = rRid.get_id();
	t3dRdResourceRecord_t record;
	bool found = false;
	bool already_freed = false;
	bool byte_mismatch = false;
	{
		Terrain3DScopedLock lock( m_resourceMutex );
		t3dRdResourceList_t::iterator iterator = m_resources.begin();
		for ( ; iterator != m_resources.end(); ++iterator ) {
			if ( ( uint64_t )( iterator->m_hRid.get_id() ) == rid_id ) {
				break;
			}
		}
		if ( iterator != m_resources.end() ) {
			record = *iterator;
			found = true;
			byte_mismatch = nBufferBytes > 0u && record.m_nBytes != nBufferBytes;
			accountRelease( record );
			rememberFreedResource( rid_id, record.m_nDeviceGeneration );
			m_resources.erase( iterator );
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

bool Terrain3DRdContext::ownsResource( const RID &rRid ) const {
	if ( !rRid.is_valid() ) {
		return false;
	}
	const uint64_t rid_id = rRid.get_id();
	Terrain3DScopedLock lock( m_resourceMutex );
	for ( size_t i = 0u; i < m_resources.size(); i++ ) {
		if ( ( uint64_t )( m_resources[i].m_hRid.get_id() ) == rid_id ) {
			return true;
		}
	}
	return false;
}

t3dRdResourceEpoch_t Terrain3DRdContext::getResourceEpoch( const t3dRdResourceOwner_t owner ) const {
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

void Terrain3DRdContext::markWarmupComplete( const bool bComplete ) {
	m_bWarmupComplete.store( bComplete, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::beginResourceRebuild( void ) {
	m_nExplicitRebuildDepth.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
}

void Terrain3DRdContext::endResourceRebuild( void ) {
	uint32_t depth = m_nExplicitRebuildDepth.load( T3D_MEMORY_ORDER_RELAXED );
	while ( depth > 0u
	        && !m_nExplicitRebuildDepth.compare_exchange_weak( depth, depth - 1u, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_RELAXED ) ) {
	}
}

void Terrain3DRdContext::beginFrame( const uint64_t nGpuFrameId, const uint64_t nSnapshotGeneration ) {
	m_nGpuFrameId.store( nGpuFrameId, T3D_MEMORY_ORDER_RELEASE );
	m_nSnapshotGeneration.store( nSnapshotGeneration, T3D_MEMORY_ORDER_RELEASE );
	m_nFrameBufferUpdates.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nFrameBytesUploaded.store( 0u, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::recordBufferUpdate( const uint64_t nBytes,
        const t3dRdResourceOwner_t owner,
        const Terrain3DGpuBufferPolicy policy ) {
	m_nBufferUpdates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nBytesUploaded.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
	m_nFrameBufferUpdates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nFrameBytesUploaded.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
	m_renderTelemetry.recordUpload( owner, policy, nBytes );
	if ( policy == T3D_GPU_DIRTY_UPLOAD && nBytes > 0u ) {
		m_renderTelemetry.recordDirtyRangeUploaded( owner );
	}
}

void Terrain3DRdContext::recordRenderEvent( const t3dRdResourceOwner_t owner, const t3dRenderTelemetryEvent_t event ) {
	m_renderTelemetry.recordEvent( owner, event );
}

void Terrain3DRdContext::recordRenderCapacity(
        const t3dRdResourceOwner_t owner, const Terrain3DGpuBufferPolicy policy, const uint64_t nBytes ) {
	m_renderTelemetry.recordCapacity( owner, policy, nBytes );
}

void Terrain3DRdContext::recordRenderIndirectCommandsSubmitted( const t3dRdResourceOwner_t owner, const uint64_t nCommands ) {
	m_renderTelemetry.recordIndirectCommandsSubmitted( owner, nCommands );
}

uint64_t Terrain3DRdContext::getRenderEvent( const t3dRdResourceOwner_t owner, const t3dRenderTelemetryEvent_t event ) const {
	return m_renderTelemetry.getEvent( owner, event );
}

uint64_t Terrain3DRdContext::getRenderUploadCalls( const t3dRdResourceOwner_t owner, const Terrain3DGpuBufferPolicy policy ) const {
	return m_renderTelemetry.getUploadCalls( owner, policy );
}

uint64_t Terrain3DRdContext::getRenderUploadBytes( const t3dRdResourceOwner_t owner, const Terrain3DGpuBufferPolicy policy ) const {
	return m_renderTelemetry.getUploadBytes( owner, policy );
}

uint64_t Terrain3DRdContext::getRenderDirtyRangesUploaded( const t3dRdResourceOwner_t owner ) const {
	return m_renderTelemetry.getDirtyRangesUploaded( owner );
}

void Terrain3DRdContext::recordSuccessfulPass( const t3dRdSuccessfulPass_t pass, const uint64_t nGpuFrameId ) {
	m_lastSuccessfulPass.store( pass < T3D_RD_CONTEXT_PASS_COUNT ? pass : T3D_RD_CONTEXT_PASS_NONE, T3D_MEMORY_ORDER_RELEASE );
	m_nLastSuccessfulGpuFrame.store( nGpuFrameId, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::recordSceneCounts( const uint64_t nTerrainPatches,
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

void Terrain3DRdContext::recordSubmissionCounts( const uint64_t nIndirectCommands, const uint64_t nComputeDispatches ) {
	m_nIndirectCommandCount.store( nIndirectCommands, T3D_MEMORY_ORDER_RELEASE );
	m_nComputeDispatchCount.store( nComputeDispatches, T3D_MEMORY_ORDER_RELEASE );
}

Dictionary Terrain3DRdContext::getDiagnostics( void ) const {
	Dictionary result;
	const t3dRdContextState_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	const t3dRdFailureCode_t failure = m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["state"] = s_state_names[state];
	result["failure_state"] = s_failure_names[failure];
	result["ready"] = isReady();
	result["device_generation"] = ( int64_t )( m_nDeviceGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["device_initializations"] = ( int64_t )( m_nDeviceInitializations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["device_rebuilds"] = ( int64_t )( m_nDeviceRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["device_invalidations"] = ( int64_t )( m_nDeviceInvalidations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["resource_creates"] = ( int64_t )( m_nCreatedResources.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["resource_frees"] = ( int64_t )( m_nFreedResources.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["resource_abandons"] = ( int64_t )( m_nAbandonedResources.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["failed_resource_creates"] = ( int64_t )( m_nFailedResourceCreates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["double_free_attempts"] = ( int64_t )( m_nDoubleFreeAttempts.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["untracked_free_attempts"] = ( int64_t )( m_nUntrackedFreeAttempts.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["accounting_errors"] = ( int64_t )( m_nAccountingErrors.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["recoverable_failure_count"] = ( int64_t )( m_nRecoverableFailureCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["live_buffer_bytes"] = ( int64_t )( m_nLiveBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["peak_buffer_bytes"] = ( int64_t )( m_nPeakBufferBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["live_texture_bytes"] = ( int64_t )( m_nLiveTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["peak_texture_bytes"] = ( int64_t )( m_nPeakTextureBytes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["buffer_updates"] = ( int64_t )( m_nBufferUpdates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["bytes_uploaded"] = ( int64_t )( m_nBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["buffer_updates_frame"] = ( int64_t )( m_nFrameBufferUpdates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["bytes_uploaded_frame"] = ( int64_t )( m_nFrameBytesUploaded.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["gpu_frame_id"] = ( int64_t )( m_nGpuFrameId.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["snapshot_generation"] = ( int64_t )( m_nSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["last_successful_gpu_frame"] = ( int64_t )( m_nLastSuccessfulGpuFrame.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["vb_creates"] = ( int64_t )( m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["vb_frees"] = ( int64_t )( m_szTypeFrees[T3D_RD_CONTEXT_RESOURCE_VERTEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["ib_creates"] = ( int64_t )( m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["ib_frees"] = ( int64_t )( m_szTypeFrees[T3D_RD_CONTEXT_RESOURCE_INDEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["shader_creates"] = ( int64_t )( m_szTypeCreates[T3D_RD_CONTEXT_RESOURCE_SHADER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["pipeline_creates"] = ( int64_t )( m_nPipelineCreates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["pipeline_rebuilds"] = ( int64_t )( m_nPipelineRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["uniform_set_creates"] = ( int64_t )( m_nUniformSetCreates.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["uniform_set_rebuilds"] = ( int64_t )( m_nUniformSetRebuilds.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["steady_state_allocation_violations"] = ( int64_t )( m_nSteadyStateAllocationViolations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["terrain_patch_count"] = ( int64_t )( m_nTerrainPatchCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["visible_terrain_patches"] = ( int64_t )( m_nVisibleTerrainPatches.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["static_instance_count"] = ( int64_t )( m_nStaticInstanceCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["visible_instances"] = ( int64_t )( m_nVisibleInstances.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["hlod_count"] = ( int64_t )( m_nHlodCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["visible_hlods"] = ( int64_t )( m_nVisibleHlods.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["indirect_command_count"] = ( int64_t )( m_nIndirectCommandCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["compute_dispatch_count"] = ( int64_t )( m_nComputeDispatchCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );

	uint64_t owned_bytes = 0u;
	for ( uint32_t category = T3D_RD_CONTEXT_MEMORY_NONE + 1u; category < T3D_RD_CONTEXT_MEMORY_CATEGORY_COUNT; ++category ) {
		const uint64_t live = m_szLiveMemory[category].load( T3D_MEMORY_ORDER_ACQUIRE );
		const uint64_t peak = m_szPeakMemory[category].load( T3D_MEMORY_ORDER_ACQUIRE );
		result[String( s_memory_keys[category] ) + "_bytes"] = ( int64_t )( live );
		result[String( "peak_" ) + s_memory_keys[category] + "_bytes"] = ( int64_t )( peak );
		owned_bytes += live;
	}
	result["topology_memory"] = ( int64_t )( m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_TOPOLOGY_VERTEX].load( T3D_MEMORY_ORDER_ACQUIRE )
	        + m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_TOPOLOGY_INDEX].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["mesh_arena_memory"] = ( int64_t )( m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_MESH_VERTEX_ARENA].load( T3D_MEMORY_ORDER_ACQUIRE )
	        + m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_MESH_INDEX_ARENA].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["texture_memory"] = ( int64_t )( m_szLiveMemory[T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["owned_gpu_bytes"] = ( int64_t )( owned_bytes );
	for ( uint32_t owner = 0u; owner < T3D_RD_CONTEXT_OWNER_COUNT; ++owner ) {
		const String prefix = String( "owner_" ) + String( s_owner_keys[owner] );
		result[prefix + String( "_creates" )] = ( int64_t )( m_szOwnerCreates[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_frees" )] = ( int64_t )( m_szOwnerFrees[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_live" )] = ( int64_t )( m_szOwnerLive[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_live_bytes" )] = ( int64_t )( m_szOwnerLiveBytes[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_steady_state_violations" )] =
		        ( int64_t )( m_szOwnerSteadyStateViolations[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	}
	static const char *const policy_names[T3D_GPU_POLICY_COUNT] = {
		"publication", "dirty_upload", "hot_frame", "scratch", "history", "indirect", "neutral"
	};
	static_assert( T3D_GPU_POLICY_COUNT == 7u, "Update render upload telemetry policy names" );
	static const char *const event_names[EVENT_COUNT] = {
		"buffer_creates", "buffer_rebuilds", "descriptor_creates", "pipeline_creates", "descriptor_evictions",
		"pipeline_evictions", "target_creates", "target_reuses", "hiz_builds", "hiz_rebinds", "resource_failures"
	};
	static_assert( EVENT_COUNT == 11u, "Update render telemetry event names" );
	Dictionary upload_telemetry;
	Dictionary event_telemetry;
	for ( uint32_t owner = 0u; owner < T3D_RD_CONTEXT_OWNER_COUNT; ++owner ) {
		const uint64_t dirty_ranges = m_renderTelemetry.getDirtyRangesUploaded( owner );
		if ( dirty_ranges > 0u ) {
			upload_telemetry[String( s_owner_keys[owner] ) + String( "_dirty_ranges" )] = ( int64_t )dirty_ranges;
		}
		const uint64_t indirect_commands = m_renderTelemetry.getIndirectCommandsSubmitted( owner );
		if ( indirect_commands > 0u ) {
			event_telemetry[String( s_owner_keys[owner] ) + String( "_indirect_commands_submitted" )] = ( int64_t )indirect_commands;
		}
		for ( uint32_t policy = 0u; policy < T3D_GPU_POLICY_COUNT; ++policy ) {
			const uint64_t capacity = m_renderTelemetry.getCapacityHighWater( owner, policy );
			if ( capacity > 0u ) {
				const String capacity_key = String( s_owner_keys[owner] ) + String( "_" ) + policy_names[policy];
				upload_telemetry[capacity_key + String( "_capacity_high_water_bytes" )] = ( int64_t )capacity;
			}
			const uint64_t calls = m_renderTelemetry.getUploadCalls( owner, policy );
			if ( calls == 0u ) {
				continue;
			}
			const String prefix = String( s_owner_keys[owner] ) + String( "_" ) + policy_names[policy];
			upload_telemetry[prefix + String( "_calls" )] = ( int64_t )calls;
			upload_telemetry[prefix + String( "_bytes" )] = ( int64_t )m_renderTelemetry.getUploadBytes( owner, policy );
		}
		for ( uint32_t event = 0u; event < EVENT_COUNT; ++event ) {
			const uint64_t count = m_renderTelemetry.getEvent( owner, ( t3dRenderTelemetryEvent_t )event );
			if ( count > 0u ) {
				event_telemetry[String( s_owner_keys[owner] ) + String( "_" ) + event_names[event]] = ( int64_t )count;
			}
		}
	}
	result["render_upload_telemetry"] = upload_telemetry;
	result["render_event_telemetry"] = event_telemetry;

	{
		Terrain3DScopedLock lock( m_resourceMutex );
		result["live_resource_count"] = ( int64_t )( m_resources.size() );
		result["vertex_format_count"] = ( int64_t )( m_vertexFormats.size() );
		result["last_failure"] = m_lastFailure;
		result["last_recoverable_failure"] = m_lastRecoverableFailure;
	}
	result["last_successful_pass"] = s_pass_names[m_lastSuccessfulPass.load( T3D_MEMORY_ORDER_ACQUIRE )];
	RenderingDevice *const rd = m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( rd ) {
		result["device_name"] = rd->get_device_name();
		result["device_vendor"] = rd->get_device_vendor_name();
		result["device_memory"] = ( int64_t )( rd->get_device_total_memory() );
	}
	return result;
}
