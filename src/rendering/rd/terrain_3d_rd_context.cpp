// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_context.h"
#include "core/terrain_3d_sync.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static const char *const s_memory_keys[Terrain3DRdContext::MEMORY_CATEGORY_COUNT] = {
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
	"scratch",
	"terrain_texture",
	"shadow",
};


static const char *const s_owner_keys[Terrain3DRdContext::OWNER_COUNT] = {
	"unspecified",
	"terrain",
	"instances",
	"hlod",
	"ocean",
	"forward_lighting",
	"render_world",
	"render_world_visibility",
	"render_world_vt",
	"render_world_hiz",
	"render_world_raster",
	"render_world_aux_raster",
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

static const char *const s_pass_names[] = {
	"none",
	"terrain_clipmap",
	"render_world",
};

static_assert( sizeof( s_memory_keys ) / sizeof( s_memory_keys[0] ) == Terrain3DRdContext::MEMORY_CATEGORY_COUNT );
static_assert( sizeof( s_owner_keys ) / sizeof( s_owner_keys[0] ) == Terrain3DRdContext::OWNER_COUNT );
static_assert( sizeof( s_state_names ) / sizeof( s_state_names[0] ) == Terrain3DRdContext::STATE_SHUTTING_DOWN + 1u );
static_assert( sizeof( s_failure_names ) / sizeof( s_failure_names[0] ) == Terrain3DRdContext::FAILURE_RENDER_PASS + 1u );
static_assert( sizeof( s_pass_names ) / sizeof( s_pass_names[0] ) == Terrain3DRdContext::PASS_COUNT );


RenderingDevice *Terrain3DRdContext::getGlobalDevice( void ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	return pServer ? pServer->get_rendering_device() : NULL;
}

RenderingDevice *Terrain3DRdContext::createLocalDevice( void ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	return pServer ? pServer->create_local_rendering_device() : NULL;
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
	if ( m_freedResources.size() >= s_nFreedHistoryCapacity ) {
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

Terrain3DRdContext::ResourceType Terrain3DRdContext::inferResourceType( const String &rName ) {
	if ( rName.find( String( "Shader" ) ) >= 0 ) {
		return RESOURCE_SHADER;
	}
	if ( rName.find( String( "Pipeline.Cull" ) ) >= 0 ) {
		return RESOURCE_COMPUTE_PIPELINE;
	}
	if ( rName.find( String( "Pipeline" ) ) >= 0 ) {
		return RESOURCE_RASTER_PIPELINE;
	}
	if ( rName.find( String( "Sampler" ) ) >= 0 ) {
		return RESOURCE_SAMPLER;
	}
	if ( rName.find( String( "UniformSet" ) ) >= 0 ) {
		return RESOURCE_UNIFORM_SET;
	}
	if ( rName.find( String( "Framebuffer" ) ) >= 0 ) {
		return RESOURCE_FRAMEBUFFER;
	}
	if ( rName.find( String( ".VB" ) ) >= 0 || rName.find( String( "VertexBuffer" ) ) >= 0 ) {
		return RESOURCE_VERTEX_BUFFER;
	}
	if ( rName.find( String( ".IB" ) ) >= 0 || rName.find( String( "IndexBuffer" ) ) >= 0 ) {
		return RESOURCE_INDEX_BUFFER;
	}
	if ( rName.find( String( ".VA" ) ) >= 0 || rName.find( String( "VertexArray" ) ) >= 0 ) {
		return RESOURCE_VERTEX_ARRAY;
	}
	if ( rName.find( String( ".IA" ) ) >= 0 || rName.find( String( "IndexArray" ) ) >= 0 ) {
		return RESOURCE_INDEX_ARRAY;
	}
	if ( rName.find( String( "Indirect" ) ) >= 0 ) {
		return RESOURCE_INDIRECT_BUFFER;
	}
	if ( rName.find( String( "Visible" ) ) >= 0 || rName.find( String( "Cull" ) ) >= 0 ) {
		return RESOURCE_CULLING_BUFFER;
	}
	if ( rName.find( String( "UBO" ) ) >= 0 || rName.find( String( "UniformBuffer" ) ) >= 0 ) {
		return RESOURCE_UNIFORM_BUFFER;
	}
	if ( rName.find( String( "Shadow" ) ) >= 0 ) {
		return RESOURCE_SHADOW;
	}
	if ( rName.find( String( "Texture" ) ) >= 0 || rName.find( String( "Atlas" ) ) >= 0 ) {
		return RESOURCE_TEXTURE;
	}
	return RESOURCE_STORAGE_BUFFER;
}

Terrain3DRdContext::MemoryCategory Terrain3DRdContext::inferMemoryCategory(
        const String &rName, const ResourceType type, const uint64_t nBytes ) {
	if ( nBytes == 0u ) {
		return MEMORY_NONE;
	}
	if ( rName.find( String( "MeshVertexArena" ) ) >= 0 ) {
		return MEMORY_MESH_VERTEX_ARENA;
	}
	if ( rName.find( String( "MeshIndexArena" ) ) >= 0 ) {
		return MEMORY_MESH_INDEX_ARENA;
	}
	if ( rName.find( String( "Material" ) ) >= 0 ) {
		return MEMORY_MATERIAL_TABLE;
	}
	if ( rName.find( String( "HLOD" ) ) >= 0 || rName.find( String( "Hlod" ) ) >= 0 ) {
		return MEMORY_HLOD;
	}
	if ( rName.find( String( "Instance" ) ) >= 0 ) {
		return MEMORY_INSTANCE;
	}
	if ( rName.find( String( "Patch" ) ) >= 0 ) {
		return MEMORY_PATCH;
	}
	if ( type == RESOURCE_VERTEX_BUFFER && rName.find( String( "Clipmap" ) ) >= 0 ) {
		return MEMORY_TOPOLOGY_VERTEX;
	}
	if ( type == RESOURCE_INDEX_BUFFER && rName.find( String( "Clipmap" ) ) >= 0 ) {
		return MEMORY_TOPOLOGY_INDEX;
	}
	if ( type == RESOURCE_INDIRECT_BUFFER ) {
		return MEMORY_INDIRECT;
	}
	if ( type == RESOURCE_UNIFORM_BUFFER ) {
		return MEMORY_UNIFORM;
	}
	if ( type == RESOURCE_CULLING_BUFFER ) {
		return MEMORY_SCRATCH;
	}
	if ( type == RESOURCE_TEXTURE ) {
		return MEMORY_TERRAIN_TEXTURE;
	}
	if ( type == RESOURCE_SHADOW ) {
		return MEMORY_SHADOW;
	}
	return MEMORY_STORAGE;
}

Terrain3DRdContext::ResourceOwner Terrain3DRdContext::inferResourceOwner( const String &rName ) {
	if ( rName.find( String( "Instance" ) ) >= 0 ) {
		return OWNER_INSTANCES;
	}
	if ( rName.find( String( "HLOD" ) ) >= 0 || rName.find( String( "Hlod" ) ) >= 0 ) {
		return OWNER_HLOD;
	}
	if ( rName.find( String( "Ocean" ) ) >= 0 ) {
		return OWNER_OCEAN;
	}
	if ( rName.find( String( "ForwardLight" ) ) >= 0 || rName.find( String( "ForwardLighting" ) ) >= 0 ) {
		return OWNER_FORWARD_LIGHTING;
	}
	if ( rName.find( String( "RenderWorld.Visibility" ) ) >= 0 ) {
		return OWNER_RENDER_WORLD_VISIBILITY;
	}
	if ( rName.find( String( "RenderWorld.VT" ) ) >= 0 ) {
		return OWNER_RENDER_WORLD_VT;
	}
	if ( rName.find( String( "RenderWorld.HiZ" ) ) >= 0 ) {
		return OWNER_RENDER_WORLD_HIZ;
	}
	if ( rName.find( String( "RenderWorld.Aux" ) ) >= 0 ) {
		return OWNER_RENDER_WORLD_AUX_RASTER;
	}
	if ( rName.find( String( "RenderWorld.Raster" ) ) >= 0 ) {
		return OWNER_RENDER_WORLD_RASTER;
	}
	if ( rName.find( String( "RenderWorld" ) ) >= 0 ) {
		return OWNER_RENDER_WORLD;
	}
	return OWNER_TERRAIN;
}

bool Terrain3DRdContext::isPipeline( const ResourceType type ) {
	return type == RESOURCE_RASTER_PIPELINE || type == RESOURCE_COMPUTE_PIPELINE;
}

bool Terrain3DRdContext::isSteadyStateAllocation( const ResourceType type ) {
	return type > RESOURCE_UNKNOWN && type < RESOURCE_TYPE_COUNT;
}

bool Terrain3DRdContext::hasResourceHistory( const String &rName, const ResourceType type ) const {
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
	if ( rRecord.m_owner < OWNER_COUNT ) {
		m_szOwnerCreates[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_szOwnerLive[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( rRecord.m_nBytes > 0u ) {
			m_szOwnerLiveBytes[rRecord.m_owner].fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	if ( isPipeline( rRecord.m_type ) ) {
		( bRebuild ? m_nPipelineRebuilds : m_nPipelineCreates ).fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	} else if ( rRecord.m_type == RESOURCE_UNIFORM_SET ) {
		( bRebuild ? m_nUniformSetRebuilds : m_nUniformSetCreates ).fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}

	if ( m_bWarmupComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) && m_nExplicitRebuildDepth.load( T3D_MEMORY_ORDER_ACQUIRE ) == 0u
	        && isSteadyStateAllocation( rRecord.m_type ) ) {
		m_nSteadyStateAllocationViolations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( rRecord.m_owner < OWNER_COUNT ) {
			m_szOwnerSteadyStateViolations[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
	}

	if ( rRecord.m_nBytes == 0u || rRecord.m_memoryCategory == MEMORY_NONE ) {
		return;
	}
	const uint64_t category_live =
	        m_szLiveMemory[rRecord.m_memoryCategory].fetch_add( rRecord.m_nBytes, T3D_MEMORY_ORDER_ACQ_REL ) + rRecord.m_nBytes;
	updatePeak( m_szPeakMemory[rRecord.m_memoryCategory], category_live );
	if ( rRecord.m_type == RESOURCE_TEXTURE ) {
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
	if ( rRecord.m_owner < OWNER_COUNT ) {
		m_szOwnerFrees[rRecord.m_owner].fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( !subtractSaturating( m_szOwnerLive[rRecord.m_owner], 1u ) ) {
			m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
		if ( rRecord.m_nBytes > 0u && !subtractSaturating( m_szOwnerLiveBytes[rRecord.m_owner], rRecord.m_nBytes ) ) {
			m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		}
	}
	if ( rRecord.m_nBytes == 0u || rRecord.m_memoryCategory == MEMORY_NONE ) {
		return;
	}
	if ( !subtractSaturating( m_szLiveMemory[rRecord.m_memoryCategory], rRecord.m_nBytes ) ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	t3dAtomicU64_t &aggregate = rRecord.m_type == RESOURCE_TEXTURE ? m_nLiveTextureBytes : m_nLiveBufferBytes;
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

void Terrain3DRdContext::setFailure( const FailureCode code, const String &rFailure ) {
	{
		t3dScopedLock_t lock( m_resourceMutex );
		m_lastFailure = rFailure;
	}
	m_failureCode.store( code, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( STATE_FAILED, T3D_MEMORY_ORDER_RELEASE );
}

bool Terrain3DRdContext::initialize( void ) {
	const ContextState state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( state == STATE_READY ) {
		return m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) != NULL;
	}
	if ( state != STATE_UNINITIALIZED ) {
		return false;
	}

	RenderingDevice *const rd = getGlobalDevice();
	if ( !rd ) {
		setFailure( FAILURE_DEVICE_UNAVAILABLE, "Global RenderingDevice is unavailable" );
		return false;
	}

	{
		t3dScopedLock_t lock( m_resourceMutex );
		m_lastFailure = String();
	}
	m_pRd.store( rd, T3D_MEMORY_ORDER_RELEASE );
	const uint64_t generation = m_nDeviceGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	m_nDeviceInitializations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	if ( generation > 1u ) {
		m_nDeviceRebuilds.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	m_failureCode.store( FAILURE_NONE, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( STATE_READY, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

bool Terrain3DRdContext::validateDevice( void ) {
	if ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) != STATE_READY ) {
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
	m_state.exchange( STATE_SHUTTING_DOWN, T3D_MEMORY_ORDER_ACQ_REL );
	RenderingDevice *const rd = m_pRd.exchange( NULL, T3D_MEMORY_ORDER_ACQ_REL );
	{
		t3dScopedLock_t lock( m_resourceMutex );
		for ( t3dRdResourceList_t::reverse_iterator iterator = m_resources.rbegin(); iterator != m_resources.rend(); ++iterator ) {
			const t3dRdResourceRecord_t &record = *iterator;
			accountRelease( record );
			rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
			if ( !record.m_hRid.is_valid() ) {
				continue;
			}
			if ( rd ) {
				if ( record.m_type != RESOURCE_UNIFORM_SET || rd->uniform_set_is_valid( record.m_hRid ) ) {
					rd->free_rid( record.m_hRid );
				}
				m_nFreedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			} else {
				m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			}
		}
		m_resources.clear();
		m_vertexFormats.clear();
		resetLiveAccounting();
	}
	m_state.store( STATE_UNINITIALIZED, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdContext::invalidateDevice( const String &rReason ) {
	m_pRd.store( NULL, T3D_MEMORY_ORDER_RELEASE );
	t3dScopedLock_t lock( m_resourceMutex );
	for ( size_t iResource = 0u; iResource < ( size_t )( ( m_resources ).size() ); iResource++ ) {
		const t3dRdResourceRecord_t &record = ( m_resources )[iResource];
		accountRelease( record );
		rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
		m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	m_resources.clear();
	m_vertexFormats.clear();
	resetLiveAccounting();
	m_lastFailure = rReason.is_empty() ? String( "RenderingDevice resources were invalidated" ) : rReason;
	m_nDeviceInvalidations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_failureCode.store( FAILURE_DEVICE_INVALIDATED, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( STATE_REBUILD_REQUIRED, T3D_MEMORY_ORDER_RELEASE );
}

bool Terrain3DRdContext::beginRebuild( void ) {
	const ContextState state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( state == STATE_REBUILDING ) {
		return m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) != NULL;
	}
	if ( state == STATE_READY || state == STATE_SHUTTING_DOWN ) {
		return false;
	}
	shutdown();

	RenderingDevice *const rd = getGlobalDevice();
	if ( !rd ) {
		setFailure( FAILURE_REBUILD, "RenderingDevice is unavailable during reconstruction" );
		return false;
	}
	m_pRd.store( rd, T3D_MEMORY_ORDER_RELEASE );
	m_nDeviceGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL );
	m_nDeviceInitializations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nDeviceRebuilds.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_failureCode.store( FAILURE_NONE, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( STATE_REBUILDING, T3D_MEMORY_ORDER_RELEASE );
	beginResourceRebuild();
	return true;
}

bool Terrain3DRdContext::completeRebuild( const bool bSuccess, const String &rFailure ) {
	if ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) != STATE_REBUILDING ) {
		return false;
	}
	endResourceRebuild();
	if ( !bSuccess || !m_pRd.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		setFailure( FAILURE_REBUILD, rFailure.is_empty() ? String( "RenderingDevice reconstruction failed" ) : rFailure );
		return false;
	}
	{
		t3dScopedLock_t lock( m_resourceMutex );
		m_lastFailure = String();
	}
	m_failureCode.store( FAILURE_NONE, T3D_MEMORY_ORDER_RELEASE );
	m_state.store( STATE_READY, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

void Terrain3DRdContext::recordFailure( const FailureCode code, const String &rFailure ) {
	setFailure( code == FAILURE_NONE ? FAILURE_RESOURCE_CREATION : code,
	        rFailure.is_empty() ? String( "Terrain3D RenderingDevice failure" ) : rFailure );
}

void Terrain3DRdContext::recordResourceCreationFailure( const String &rName, const ResourceType type, const String &rFailure ) {
	( void )type;
	m_nFailedResourceCreates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	const String failure = rFailure.is_empty() ? String( "Failed to create RD resource: " ) + rName : rFailure;
	setFailure( FAILURE_RESOURCE_CREATION, failure );
}

bool Terrain3DRdContext::registerResourceInternal( const RID &rRid,
        const String &rName,
        const ResourceType type,
        const MemoryCategory memoryCategory,
        const uint64_t nBytes,
        const ResourceOwner owner ) {
	RenderingDevice *const rd = getDevice();
	if ( !rd || !rRid.is_valid() ) {
		recordResourceCreationFailure( rName,
		        type,
		        rRid.is_valid() ? String( "RD context is not accepting resources: " ) + rName
		                        : String( "Invalid RID returned for: " ) + rName );
		return false;
	}
	if ( type >= RESOURCE_TYPE_COUNT || memoryCategory >= MEMORY_CATEGORY_COUNT || owner >= OWNER_COUNT ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		setFailure( FAILURE_ACCOUNTING, "Invalid RD resource accounting category" );
		return false;
	}

	const ResourceOwner resolved_owner = owner == OWNER_UNSPECIFIED ? inferResourceOwner( rName ) : owner;
	bool duplicate = false;
	bool capacity_exhausted = false;
	{
		t3dScopedLock_t lock( m_resourceMutex );
		const uint64_t rid_id = rRid.get_id();
		for ( size_t i = 0u; i < m_resources.size(); i++ ) {
			if ( ( uint64_t )( m_resources[i].m_hRid.get_id() ) == rid_id ) {
				duplicate = true;
				break;
			}
		}
		if ( !duplicate ) {
			const bool rebuild = hasResourceHistory( rName, type );
			if ( m_resources.size() >= s_nResourceCapacity ) {
				capacity_exhausted = true;
			} else {
				if ( !rebuild ) {
					const t3dRdResourceHistory_t history{ rName, type };
					if ( m_resourceHistory.size() < s_nResourceHistoryCapacity ) {
						m_resourceHistory.push_back( history );
					} else {
						m_resourceHistory[m_nResourceHistoryCursor] = history;
						m_nResourceHistoryCursor = ( m_nResourceHistoryCursor + 1u ) % s_nResourceHistoryCapacity;
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
		setFailure( FAILURE_DUPLICATE_RESOURCE, String( "RD resource registered twice: " ) + rName );
		return false;
	}
	if ( capacity_exhausted ) {
		m_nAccountingErrors.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		setFailure( FAILURE_ACCOUNTING, "Terrain3D fixed RD resource registry capacity exhausted" );
		return false;
	}
	// Vertex and index arrays are RenderingDevice-side views without a driver
	// object. Godot's set_resource_name() deliberately does not accept those
	// RID owners, so keep their names in Terrain3D's resource records only.
	if ( type != RESOURCE_VERTEX_ARRAY && type != RESOURCE_INDEX_ARRAY ) {
		rd->set_resource_name( rRid, rName );
	}
	return true;
}

void Terrain3DRdContext::registerResource( const RID &rRid, const String &rName, const uint64_t nBufferBytes ) {
	const ResourceType type = inferResourceType( rName );
	registerResourceInternal(
	        rRid, rName, type, inferMemoryCategory( rName, type, nBufferBytes ), nBufferBytes, inferResourceOwner( rName ) );
}

bool Terrain3DRdContext::registerResource( const RID &rRid,
        const String &rName,
        const ResourceType type,
        const MemoryCategory memoryCategory,
        const uint64_t nBytes,
        const ResourceOwner owner ) {
	return registerResourceInternal( rRid, rName, type, memoryCategory, nBytes, owner );
}

void Terrain3DRdContext::registerTextureResource( const RID &rRid, const String &rName, const uint64_t nTextureBytes ) {
	registerResourceInternal( rRid, rName, RESOURCE_TEXTURE, MEMORY_TERRAIN_TEXTURE, nTextureBytes, inferResourceOwner( rName ) );
}

bool Terrain3DRdContext::registerVertexFormat( const int64_t nFormat, const String &rName ) {
	if ( !getDevice() || nFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		recordResourceCreationFailure( rName, RESOURCE_VERTEX_ARRAY, "Invalid Terrain3D RD vertex format" );
		return false;
	}
	t3dScopedLock_t lock( m_resourceMutex );
	for ( size_t i = 0u; i < m_vertexFormats.size(); i++ ) {
		if ( m_vertexFormats[i].m_nFormat == nFormat ) {
			return true;
		}
	}
	if ( m_vertexFormats.size() >= s_nVertexFormatCapacity ) {
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
	t3dScopedLock_t lock( m_resourceMutex );
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
		t3dScopedLock_t lock( m_resourceMutex );
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
		setFailure( FAILURE_UNTRACKED_RESOURCE, "Attempted to free an untracked Terrain3D RD resource" );
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
		if ( record.m_type != RESOURCE_UNIFORM_SET || rd->uniform_set_is_valid( rRid ) ) {
			rd->free_rid( rRid );
		}
		m_nFreedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	} else {
		m_nAbandonedResources.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	rRid = RID();
}

void Terrain3DRdContext::freeTextureResource( RID &rRid, const uint64_t nTextureBytes ) {
	freeResource( rRid, nTextureBytes );
}

bool Terrain3DRdContext::ownsResource( const RID &rRid ) const {
	if ( !rRid.is_valid() ) {
		return false;
	}
	const uint64_t rid_id = rRid.get_id();
	t3dScopedLock_t lock( m_resourceMutex );
	for ( size_t i = 0u; i < m_resources.size(); i++ ) {
		if ( ( uint64_t )( m_resources[i].m_hRid.get_id() ) == rid_id ) {
			return true;
		}
	}
	return false;
}

Terrain3DRdContext::ResourceEpoch Terrain3DRdContext::getResourceEpoch( const ResourceOwner owner ) const {
	ResourceEpoch epoch;
	if ( owner >= OWNER_COUNT ) {
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

void Terrain3DRdContext::recordBufferUpdate( const uint64_t nBytes ) {
	m_nBufferUpdates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nBytesUploaded.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
	m_nFrameBufferUpdates.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nFrameBytesUploaded.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
}

void Terrain3DRdContext::recordSuccessfulPass( const SuccessfulPass pass, const uint64_t nGpuFrameId ) {
	m_lastSuccessfulPass.store( pass < PASS_COUNT ? pass : PASS_NONE, T3D_MEMORY_ORDER_RELEASE );
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
	const ContextState state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	const FailureCode failure = m_failureCode.load( T3D_MEMORY_ORDER_ACQUIRE );
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
	result["vb_creates"] = ( int64_t )( m_szTypeCreates[RESOURCE_VERTEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["vb_frees"] = ( int64_t )( m_szTypeFrees[RESOURCE_VERTEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["ib_creates"] = ( int64_t )( m_szTypeCreates[RESOURCE_INDEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["ib_frees"] = ( int64_t )( m_szTypeFrees[RESOURCE_INDEX_BUFFER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["shader_creates"] = ( int64_t )( m_szTypeCreates[RESOURCE_SHADER].load( T3D_MEMORY_ORDER_ACQUIRE ) );
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
	for ( uint32_t category = MEMORY_NONE + 1u; category < MEMORY_CATEGORY_COUNT; ++category ) {
		const uint64_t live = m_szLiveMemory[category].load( T3D_MEMORY_ORDER_ACQUIRE );
		const uint64_t peak = m_szPeakMemory[category].load( T3D_MEMORY_ORDER_ACQUIRE );
		result[String( s_memory_keys[category] ) + "_bytes"] = ( int64_t )( live );
		result[String( "peak_" ) + s_memory_keys[category] + "_bytes"] = ( int64_t )( peak );
		owned_bytes += live;
	}
	result["topology_memory"] = ( int64_t )( m_szLiveMemory[MEMORY_TOPOLOGY_VERTEX].load( T3D_MEMORY_ORDER_ACQUIRE )
	        + m_szLiveMemory[MEMORY_TOPOLOGY_INDEX].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["mesh_arena_memory"] = ( int64_t )( m_szLiveMemory[MEMORY_MESH_VERTEX_ARENA].load( T3D_MEMORY_ORDER_ACQUIRE )
	        + m_szLiveMemory[MEMORY_MESH_INDEX_ARENA].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["texture_memory"] = ( int64_t )( m_szLiveMemory[MEMORY_TERRAIN_TEXTURE].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["owned_gpu_bytes"] = ( int64_t )( owned_bytes );
	for ( uint32_t owner = 0u; owner < OWNER_COUNT; ++owner ) {
		const String prefix = String( "owner_" ) + String( s_owner_keys[owner] );
		result[prefix + String( "_creates" )] = ( int64_t )( m_szOwnerCreates[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_frees" )] = ( int64_t )( m_szOwnerFrees[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_live" )] = ( int64_t )( m_szOwnerLive[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_live_bytes" )] = ( int64_t )( m_szOwnerLiveBytes[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
		result[prefix + String( "_steady_state_violations" )] =
		        ( int64_t )( m_szOwnerSteadyStateViolations[owner].load( T3D_MEMORY_ORDER_ACQUIRE ) );
	}

	{
		t3dScopedLock_t lock( m_resourceMutex );
		result["live_resource_count"] = ( int64_t )( m_resources.size() );
		result["vertex_format_count"] = ( int64_t )( m_vertexFormats.size() );
		result["last_failure"] = m_lastFailure;
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
