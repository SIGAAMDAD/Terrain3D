// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_context.h"

#include <algorithm>
#include <godot_cpp/classes/rendering_server.hpp>

namespace {
const char *const s_memory_keys[Terrain3DRdContext::MEMORY_CATEGORY_COUNT] = {
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

const char *const s_state_names[] = {
	"uninitialized",
	"ready",
	"failed",
	"rebuild_required",
	"rebuilding",
	"shutting_down",
};

const char *const s_failure_names[] = {
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

const char *const s_pass_names[] = {
	"none",
	"terrain_clipmap",
};

static_assert( sizeof( s_memory_keys ) / sizeof( s_memory_keys[0] ) == Terrain3DRdContext::MEMORY_CATEGORY_COUNT );
static_assert( sizeof( s_state_names ) / sizeof( s_state_names[0] ) == Terrain3DRdContext::STATE_SHUTTING_DOWN + 1u );
static_assert( sizeof( s_failure_names ) / sizeof( s_failure_names[0] ) == Terrain3DRdContext::FAILURE_RENDER_PASS + 1u );
static_assert( sizeof( s_pass_names ) / sizeof( s_pass_names[0] ) == Terrain3DRdContext::PASS_COUNT );

RenderingDevice *get_global_rendering_device( void ) {
	RenderingServer *const server = RenderingServer::get_singleton();
	return server ? server->get_rendering_device() : nullptr;
}
} // namespace

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

void Terrain3DRdContext::updatePeak( std::atomic_uint64_t &rPeak, const uint64_t nLiveBytes ) {
	uint64_t peak = rPeak.load( std::memory_order_relaxed );
	while ( nLiveBytes > peak &&
			!rPeak.compare_exchange_weak( peak, nLiveBytes, std::memory_order_release, std::memory_order_relaxed ) ) {
	}
}

bool Terrain3DRdContext::subtractSaturating( std::atomic_uint64_t &rValue, const uint64_t nAmount ) {
	uint64_t value = rValue.load( std::memory_order_relaxed );
	for ( ;; ) {
		const bool valid = value >= nAmount;
		const uint64_t replacement = valid ? value - nAmount : 0u;
		if ( rValue.compare_exchange_weak( value, replacement, std::memory_order_acq_rel, std::memory_order_relaxed ) ) {
			return valid;
		}
	}
}

void Terrain3DRdContext::rememberFreedResource( const uint64_t nRid, const uint64_t nDeviceGeneration ) {
	static constexpr size_t s_nFreedHistoryLimit = 256u;
	if ( m_freedResources.size() >= s_nFreedHistoryLimit ) m_freedResources.erase( m_freedResources.begin() );
	m_freedResources.push_back( { nRid, nDeviceGeneration } );
}

void Terrain3DRdContext::updateBufferPeak( const uint64_t nLiveBytes ) {
	updatePeak( m_nPeakBufferBytes, nLiveBytes );
}

void Terrain3DRdContext::updateTexturePeak( const uint64_t nLiveBytes ) {
	updatePeak( m_nPeakTextureBytes, nLiveBytes );
}

Terrain3DRdContext::ResourceType Terrain3DRdContext::inferResourceType( const String &rName ) {
	if ( rName.find( String( "Shader" ) ) >= 0 ) return RESOURCE_SHADER;
	if ( rName.find( String( "Pipeline.Cull" ) ) >= 0 ) return RESOURCE_COMPUTE_PIPELINE;
	if ( rName.find( String( "Pipeline" ) ) >= 0 ) return RESOURCE_RASTER_PIPELINE;
	if ( rName.find( String( "Sampler" ) ) >= 0 ) return RESOURCE_SAMPLER;
	if ( rName.find( String( "UniformSet" ) ) >= 0 ) return RESOURCE_UNIFORM_SET;
	if ( rName.find( String( "Framebuffer" ) ) >= 0 ) return RESOURCE_FRAMEBUFFER;
	if ( rName.find( String( ".VB" ) ) >= 0 || rName.find( String( "VertexBuffer" ) ) >= 0 ) return RESOURCE_VERTEX_BUFFER;
	if ( rName.find( String( ".IB" ) ) >= 0 || rName.find( String( "IndexBuffer" ) ) >= 0 ) return RESOURCE_INDEX_BUFFER;
	if ( rName.find( String( ".VA" ) ) >= 0 || rName.find( String( "VertexArray" ) ) >= 0 ) return RESOURCE_VERTEX_ARRAY;
	if ( rName.find( String( ".IA" ) ) >= 0 || rName.find( String( "IndexArray" ) ) >= 0 ) return RESOURCE_INDEX_ARRAY;
	if ( rName.find( String( "Indirect" ) ) >= 0 ) return RESOURCE_INDIRECT_BUFFER;
	if ( rName.find( String( "Visible" ) ) >= 0 || rName.find( String( "Cull" ) ) >= 0 ) return RESOURCE_CULLING_BUFFER;
	if ( rName.find( String( "UBO" ) ) >= 0 || rName.find( String( "UniformBuffer" ) ) >= 0 ) return RESOURCE_UNIFORM_BUFFER;
	if ( rName.find( String( "Shadow" ) ) >= 0 ) return RESOURCE_SHADOW;
	if ( rName.find( String( "Texture" ) ) >= 0 || rName.find( String( "Atlas" ) ) >= 0 ) return RESOURCE_TEXTURE;
	return RESOURCE_STORAGE_BUFFER;
}

Terrain3DRdContext::MemoryCategory Terrain3DRdContext::inferMemoryCategory( const String &rName,
		const ResourceType type,
		const uint64_t nBytes ) {
	if ( nBytes == 0u ) return MEMORY_NONE;
	if ( rName.find( String( "MeshVertexArena" ) ) >= 0 ) return MEMORY_MESH_VERTEX_ARENA;
	if ( rName.find( String( "MeshIndexArena" ) ) >= 0 ) return MEMORY_MESH_INDEX_ARENA;
	if ( rName.find( String( "Material" ) ) >= 0 ) return MEMORY_MATERIAL_TABLE;
	if ( rName.find( String( "HLOD" ) ) >= 0 || rName.find( String( "Hlod" ) ) >= 0 ) return MEMORY_HLOD;
	if ( rName.find( String( "Instance" ) ) >= 0 ) return MEMORY_INSTANCE;
	if ( rName.find( String( "Patch" ) ) >= 0 ) return MEMORY_PATCH;
	if ( type == RESOURCE_VERTEX_BUFFER && rName.find( String( "Clipmap" ) ) >= 0 ) return MEMORY_TOPOLOGY_VERTEX;
	if ( type == RESOURCE_INDEX_BUFFER && rName.find( String( "Clipmap" ) ) >= 0 ) return MEMORY_TOPOLOGY_INDEX;
	if ( type == RESOURCE_INDIRECT_BUFFER ) return MEMORY_INDIRECT;
	if ( type == RESOURCE_UNIFORM_BUFFER ) return MEMORY_UNIFORM;
	if ( type == RESOURCE_CULLING_BUFFER ) return MEMORY_SCRATCH;
	if ( type == RESOURCE_TEXTURE ) return MEMORY_TERRAIN_TEXTURE;
	if ( type == RESOURCE_SHADOW ) return MEMORY_SHADOW;
	return MEMORY_STORAGE;
}

bool Terrain3DRdContext::isPipeline( const ResourceType type ) {
	return type == RESOURCE_RASTER_PIPELINE || type == RESOURCE_COMPUTE_PIPELINE;
}

bool Terrain3DRdContext::isSteadyStateAllocation( const ResourceType type ) {
	return type > RESOURCE_UNKNOWN && type < RESOURCE_TYPE_COUNT;
}

bool Terrain3DRdContext::hasResourceHistory( const String &rName, const ResourceType type ) const {
	return std::any_of( m_resourceHistory.begin(), m_resourceHistory.end(),
			[&rName, type]( const t3dRdResourceHistory_t &rHistory ) {
				return rHistory.m_type == type && rHistory.m_name == rName;
			} );
}

void Terrain3DRdContext::accountRegistration( const t3dRdResourceRecord_t &rRecord, const bool bRebuild ) {
	m_nCreatedResources.fetch_add( 1u, std::memory_order_relaxed );
	m_szTypeCreates[rRecord.m_type].fetch_add( 1u, std::memory_order_relaxed );
	m_szTypeLive[rRecord.m_type].fetch_add( 1u, std::memory_order_relaxed );

	if ( isPipeline( rRecord.m_type ) ) {
		( bRebuild ? m_nPipelineRebuilds : m_nPipelineCreates ).fetch_add( 1u, std::memory_order_relaxed );
	} else if ( rRecord.m_type == RESOURCE_UNIFORM_SET ) {
		( bRebuild ? m_nUniformSetRebuilds : m_nUniformSetCreates ).fetch_add( 1u, std::memory_order_relaxed );
	}

	if ( m_bWarmupComplete.load( std::memory_order_acquire ) &&
			m_nExplicitRebuildDepth.load( std::memory_order_acquire ) == 0u && isSteadyStateAllocation( rRecord.m_type ) ) {
		m_nSteadyStateAllocationViolations.fetch_add( 1u, std::memory_order_relaxed );
	}

	if ( rRecord.m_nBytes == 0u || rRecord.m_memoryCategory == MEMORY_NONE ) return;
	const uint64_t category_live = m_szLiveMemory[rRecord.m_memoryCategory].fetch_add( rRecord.m_nBytes, std::memory_order_acq_rel ) + rRecord.m_nBytes;
	updatePeak( m_szPeakMemory[rRecord.m_memoryCategory], category_live );
	if ( rRecord.m_type == RESOURCE_TEXTURE ) {
		const uint64_t live = m_nLiveTextureBytes.fetch_add( rRecord.m_nBytes, std::memory_order_acq_rel ) + rRecord.m_nBytes;
		updateTexturePeak( live );
	} else {
		const uint64_t live = m_nLiveBufferBytes.fetch_add( rRecord.m_nBytes, std::memory_order_acq_rel ) + rRecord.m_nBytes;
		updateBufferPeak( live );
	}
}

void Terrain3DRdContext::accountRelease( const t3dRdResourceRecord_t &rRecord ) {
	m_szTypeFrees[rRecord.m_type].fetch_add( 1u, std::memory_order_relaxed );
	if ( !subtractSaturating( m_szTypeLive[rRecord.m_type], 1u ) ) {
		m_nAccountingErrors.fetch_add( 1u, std::memory_order_relaxed );
	}
	if ( rRecord.m_nBytes == 0u || rRecord.m_memoryCategory == MEMORY_NONE ) return;
	if ( !subtractSaturating( m_szLiveMemory[rRecord.m_memoryCategory], rRecord.m_nBytes ) ) {
		m_nAccountingErrors.fetch_add( 1u, std::memory_order_relaxed );
	}
	std::atomic_uint64_t &aggregate = rRecord.m_type == RESOURCE_TEXTURE ? m_nLiveTextureBytes : m_nLiveBufferBytes;
	if ( !subtractSaturating( aggregate, rRecord.m_nBytes ) ) {
		m_nAccountingErrors.fetch_add( 1u, std::memory_order_relaxed );
	}
}

void Terrain3DRdContext::resetLiveAccounting( void ) {
	m_nLiveBufferBytes.store( 0u, std::memory_order_release );
	m_nLiveTextureBytes.store( 0u, std::memory_order_release );
	for ( std::atomic_uint64_t &value : m_szTypeLive ) value.store( 0u, std::memory_order_release );
	for ( std::atomic_uint64_t &value : m_szLiveMemory ) value.store( 0u, std::memory_order_release );
}

void Terrain3DRdContext::setFailure( const FailureCode code, const String &rFailure ) {
	{
		std::lock_guard<std::mutex> lock( m_resourceMutex );
		m_lastFailure = rFailure;
	}
	m_failureCode.store( code, std::memory_order_release );
	m_state.store( STATE_FAILED, std::memory_order_release );
}

bool Terrain3DRdContext::initialize( void ) {
	const ContextState state = m_state.load( std::memory_order_acquire );
	if ( state == STATE_READY ) return m_pRd.load( std::memory_order_acquire ) != nullptr;
	if ( state != STATE_UNINITIALIZED ) return false;

	RenderingDevice *const rd = get_global_rendering_device();
	if ( !rd ) {
		setFailure( FAILURE_DEVICE_UNAVAILABLE, "Global RenderingDevice is unavailable" );
		return false;
	}

	{
		std::lock_guard<std::mutex> lock( m_resourceMutex );
		m_resources.reserve( 128u );
		m_resourceHistory.reserve( 128u );
		m_freedResources.reserve( 256u );
		m_vertexFormats.reserve( 8u );
		m_lastFailure = String();
	}
	m_pRd.store( rd, std::memory_order_release );
	const uint64_t generation = m_nDeviceGeneration.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	m_nDeviceInitializations.fetch_add( 1u, std::memory_order_relaxed );
	if ( generation > 1u ) m_nDeviceRebuilds.fetch_add( 1u, std::memory_order_relaxed );
	m_failureCode.store( FAILURE_NONE, std::memory_order_release );
	m_state.store( STATE_READY, std::memory_order_release );
	return true;
}

bool Terrain3DRdContext::validateDevice( void ) {
	if ( m_state.load( std::memory_order_acquire ) != STATE_READY ) return false;
	RenderingDevice *const owned = m_pRd.load( std::memory_order_acquire );
	RenderingDevice *const current = get_global_rendering_device();
	if ( owned && owned == current ) return true;
	invalidateDevice( current ? "Global RenderingDevice identity changed" : "Global RenderingDevice became unavailable" );
	return false;
}

void Terrain3DRdContext::shutdown( void ) {
	m_state.exchange( STATE_SHUTTING_DOWN, std::memory_order_acq_rel );
	RenderingDevice *const rd = m_pRd.exchange( nullptr, std::memory_order_acq_rel );
	std::vector<t3dRdResourceRecord_t> resources;
	{
		std::lock_guard<std::mutex> lock( m_resourceMutex );
		resources.swap( m_resources );
		m_vertexFormats.clear();
		for ( const t3dRdResourceRecord_t &record : resources ) {
			accountRelease( record );
			rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
		}
		resetLiveAccounting();
	}
	for ( auto iterator = resources.rbegin(); iterator != resources.rend(); ++iterator ) {
		if ( !iterator->m_hRid.is_valid() ) continue;
		if ( rd ) {
			rd->free_rid( iterator->m_hRid );
			m_nFreedResources.fetch_add( 1u, std::memory_order_relaxed );
		} else {
			m_nAbandonedResources.fetch_add( 1u, std::memory_order_relaxed );
		}
	}
	m_state.store( STATE_UNINITIALIZED, std::memory_order_release );
}

void Terrain3DRdContext::invalidateDevice( const String &rReason ) {
	m_pRd.store( nullptr, std::memory_order_release );
	std::lock_guard<std::mutex> lock( m_resourceMutex );
	for ( const t3dRdResourceRecord_t &record : m_resources ) {
		accountRelease( record );
		rememberFreedResource( record.m_hRid.get_id(), record.m_nDeviceGeneration );
		m_nAbandonedResources.fetch_add( 1u, std::memory_order_relaxed );
	}
	m_resources.clear();
	m_vertexFormats.clear();
	resetLiveAccounting();
	m_lastFailure = rReason.is_empty() ? String( "RenderingDevice resources were invalidated" ) : rReason;
	m_nDeviceInvalidations.fetch_add( 1u, std::memory_order_relaxed );
	m_failureCode.store( FAILURE_DEVICE_INVALIDATED, std::memory_order_release );
	m_state.store( STATE_REBUILD_REQUIRED, std::memory_order_release );
}

bool Terrain3DRdContext::beginRebuild( void ) {
	const ContextState state = m_state.load( std::memory_order_acquire );
	if ( state == STATE_REBUILDING ) return m_pRd.load( std::memory_order_acquire ) != nullptr;
	if ( state == STATE_READY || state == STATE_SHUTTING_DOWN ) return false;
	shutdown();

	RenderingDevice *const rd = get_global_rendering_device();
	if ( !rd ) {
		setFailure( FAILURE_REBUILD, "RenderingDevice is unavailable during reconstruction" );
		return false;
	}
	m_pRd.store( rd, std::memory_order_release );
	m_nDeviceGeneration.fetch_add( 1u, std::memory_order_acq_rel );
	m_nDeviceInitializations.fetch_add( 1u, std::memory_order_relaxed );
	m_nDeviceRebuilds.fetch_add( 1u, std::memory_order_relaxed );
	m_failureCode.store( FAILURE_NONE, std::memory_order_release );
	m_state.store( STATE_REBUILDING, std::memory_order_release );
	beginResourceRebuild();
	return true;
}

bool Terrain3DRdContext::completeRebuild( const bool bSuccess, const String &rFailure ) {
	if ( m_state.load( std::memory_order_acquire ) != STATE_REBUILDING ) return false;
	endResourceRebuild();
	if ( !bSuccess || !m_pRd.load( std::memory_order_acquire ) ) {
		setFailure( FAILURE_REBUILD, rFailure.is_empty() ? String( "RenderingDevice reconstruction failed" ) : rFailure );
		return false;
	}
	{
		std::lock_guard<std::mutex> lock( m_resourceMutex );
		m_lastFailure = String();
	}
	m_failureCode.store( FAILURE_NONE, std::memory_order_release );
	m_state.store( STATE_READY, std::memory_order_release );
	return true;
}

void Terrain3DRdContext::recordFailure( const FailureCode code, const String &rFailure ) {
	setFailure( code == FAILURE_NONE ? FAILURE_RESOURCE_CREATION : code,
			rFailure.is_empty() ? String( "Terrain3D RenderingDevice failure" ) : rFailure );
}

void Terrain3DRdContext::recordResourceCreationFailure( const String &rName,
		const ResourceType type,
		const String &rFailure ) {
	( void )type;
	m_nFailedResourceCreates.fetch_add( 1u, std::memory_order_relaxed );
	const String failure = rFailure.is_empty() ? String( "Failed to create RD resource: " ) + rName : rFailure;
	setFailure( FAILURE_RESOURCE_CREATION, failure );
}

bool Terrain3DRdContext::registerResourceInternal( const RID &rRid,
		const String &rName,
		const ResourceType type,
		const MemoryCategory memoryCategory,
		const uint64_t nBytes ) {
	RenderingDevice *const rd = getDevice();
	if ( !rd || !rRid.is_valid() ) {
		recordResourceCreationFailure( rName, type,
				rRid.is_valid() ? String( "RD context is not accepting resources: " ) + rName : String( "Invalid RID returned for: " ) + rName );
		return false;
	}
	if ( type >= RESOURCE_TYPE_COUNT || memoryCategory >= MEMORY_CATEGORY_COUNT ) {
		m_nAccountingErrors.fetch_add( 1u, std::memory_order_relaxed );
		setFailure( FAILURE_ACCOUNTING, "Invalid RD resource accounting category" );
		return false;
	}

	bool duplicate = false;
	{
		std::lock_guard<std::mutex> lock( m_resourceMutex );
		const uint64_t rid_id = rRid.get_id();
		duplicate = std::any_of( m_resources.begin(), m_resources.end(),
				[rid_id]( const t3dRdResourceRecord_t &rRecord ) { return rRecord.m_hRid.get_id() == rid_id; } );
		if ( !duplicate ) {
			const bool rebuild = hasResourceHistory( rName, type );
			if ( !rebuild ) m_resourceHistory.push_back( { rName, type } );
			m_freedResources.erase( std::remove_if( m_freedResources.begin(), m_freedResources.end(),
					[rid_id]( const t3dRdFreedResource_t &rFreed ) { return rFreed.m_nRid == rid_id; } ),
					m_freedResources.end() );
			const t3dRdResourceRecord_t record{ rRid, rName, type, memoryCategory, nBytes, getDeviceGeneration() };
			m_resources.push_back( record );
			accountRegistration( record, rebuild );
		}
	}
	if ( duplicate ) {
		m_nAccountingErrors.fetch_add( 1u, std::memory_order_relaxed );
		setFailure( FAILURE_DUPLICATE_RESOURCE, String( "RD resource registered twice: " ) + rName );
		return false;
	}
	rd->set_resource_name( rRid, rName );
	return true;
}

void Terrain3DRdContext::registerResource( const RID &rRid, const String &rName, const uint64_t nBufferBytes ) {
	const ResourceType type = inferResourceType( rName );
	registerResourceInternal( rRid, rName, type, inferMemoryCategory( rName, type, nBufferBytes ), nBufferBytes );
}

bool Terrain3DRdContext::registerResource( const RID &rRid,
		const String &rName,
		const ResourceType type,
		const MemoryCategory memoryCategory,
		const uint64_t nBytes ) {
	return registerResourceInternal( rRid, rName, type, memoryCategory, nBytes );
}

void Terrain3DRdContext::registerTextureResource( const RID &rRid, const String &rName, const uint64_t nTextureBytes ) {
	registerResourceInternal( rRid, rName, RESOURCE_TEXTURE, MEMORY_TERRAIN_TEXTURE, nTextureBytes );
}

bool Terrain3DRdContext::registerVertexFormat( const int64_t nFormat, const String &rName ) {
	if ( !getDevice() || nFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		recordResourceCreationFailure( rName, RESOURCE_VERTEX_ARRAY, "Invalid Terrain3D RD vertex format" );
		return false;
	}
	std::lock_guard<std::mutex> lock( m_resourceMutex );
	const bool exists = std::any_of( m_vertexFormats.begin(), m_vertexFormats.end(),
			[nFormat]( const t3dRdVertexFormatRecord_t &rRecord ) { return rRecord.m_nFormat == nFormat; } );
	if ( exists ) return true;
	m_vertexFormats.push_back( { nFormat, rName, getDeviceGeneration() } );
	return true;
}

void Terrain3DRdContext::releaseVertexFormat( int64_t &rFormat ) {
	if ( rFormat == RenderingDevice::INVALID_FORMAT_ID ) return;
	std::lock_guard<std::mutex> lock( m_resourceMutex );
	m_vertexFormats.erase( std::remove_if( m_vertexFormats.begin(), m_vertexFormats.end(),
			[rFormat]( const t3dRdVertexFormatRecord_t &rRecord ) { return rRecord.m_nFormat == rFormat; } ),
			m_vertexFormats.end() );
	rFormat = RenderingDevice::INVALID_FORMAT_ID;
}

void Terrain3DRdContext::freeResource( RID &rRid, const uint64_t nBufferBytes ) {
	if ( !rRid.is_valid() ) return;
	const uint64_t rid_id = rRid.get_id();
	t3dRdResourceRecord_t record;
	bool found = false;
	bool already_freed = false;
	bool byte_mismatch = false;
	{
		std::lock_guard<std::mutex> lock( m_resourceMutex );
		const auto iterator = std::find_if( m_resources.begin(), m_resources.end(),
				[rid_id]( const t3dRdResourceRecord_t &rRecord ) { return rRecord.m_hRid.get_id() == rid_id; } );
		if ( iterator != m_resources.end() ) {
			record = *iterator;
			found = true;
			byte_mismatch = nBufferBytes > 0u && record.m_nBytes != nBufferBytes;
			accountRelease( record );
			rememberFreedResource( rid_id, record.m_nDeviceGeneration );
			m_resources.erase( iterator );
		} else {
			already_freed = std::any_of( m_freedResources.begin(), m_freedResources.end(),
					[rid_id]( const t3dRdFreedResource_t &rFreed ) { return rFreed.m_nRid == rid_id; } );
		}
	}

	if ( byte_mismatch ) m_nAccountingErrors.fetch_add( 1u, std::memory_order_relaxed );
	if ( already_freed ) {
		m_nDoubleFreeAttempts.fetch_add( 1u, std::memory_order_relaxed );
		rRid = RID();
		return;
	}
	if ( !found ) {
		m_nUntrackedFreeAttempts.fetch_add( 1u, std::memory_order_relaxed );
		setFailure( FAILURE_UNTRACKED_RESOURCE, "Attempted to free an untracked Terrain3D RD resource" );
		// The registry does not own this RID. Leave it valid so its real owner can
		// release it, and never turn an accounting bug into destruction of an
		// external framebuffer attachment or texture.
		return;
	}
	RenderingDevice *const rd = m_pRd.load( std::memory_order_acquire );
	if ( rd ) {
		rd->free_rid( rRid );
		m_nFreedResources.fetch_add( 1u, std::memory_order_relaxed );
	} else {
		m_nAbandonedResources.fetch_add( 1u, std::memory_order_relaxed );
	}
	rRid = RID();
}

void Terrain3DRdContext::freeTextureResource( RID &rRid, const uint64_t nTextureBytes ) {
	freeResource( rRid, nTextureBytes );
}

void Terrain3DRdContext::markWarmupComplete( const bool bComplete ) {
	m_bWarmupComplete.store( bComplete, std::memory_order_release );
}

void Terrain3DRdContext::beginResourceRebuild( void ) {
	m_nExplicitRebuildDepth.fetch_add( 1u, std::memory_order_acq_rel );
}

void Terrain3DRdContext::endResourceRebuild( void ) {
	uint32_t depth = m_nExplicitRebuildDepth.load( std::memory_order_relaxed );
	while ( depth > 0u && !m_nExplicitRebuildDepth.compare_exchange_weak( depth, depth - 1u,
			std::memory_order_acq_rel, std::memory_order_relaxed ) ) {
	}
}

void Terrain3DRdContext::beginFrame( const uint64_t nGpuFrameId, const uint64_t nSnapshotGeneration ) {
	m_nGpuFrameId.store( nGpuFrameId, std::memory_order_release );
	m_nSnapshotGeneration.store( nSnapshotGeneration, std::memory_order_release );
	m_nFrameBufferUpdates.store( 0u, std::memory_order_release );
	m_nFrameBytesUploaded.store( 0u, std::memory_order_release );
}

void Terrain3DRdContext::recordBufferUpdate( const uint64_t nBytes ) {
	m_nBufferUpdates.fetch_add( 1u, std::memory_order_relaxed );
	m_nBytesUploaded.fetch_add( nBytes, std::memory_order_relaxed );
	m_nFrameBufferUpdates.fetch_add( 1u, std::memory_order_relaxed );
	m_nFrameBytesUploaded.fetch_add( nBytes, std::memory_order_relaxed );
}

void Terrain3DRdContext::recordSuccessfulPass( const SuccessfulPass pass, const uint64_t nGpuFrameId ) {
	m_lastSuccessfulPass.store( pass < PASS_COUNT ? pass : PASS_NONE, std::memory_order_release );
	m_nLastSuccessfulGpuFrame.store( nGpuFrameId, std::memory_order_release );
}

void Terrain3DRdContext::recordSceneCounts( const uint64_t nTerrainPatches,
		const uint64_t nVisibleTerrainPatches,
		const uint64_t nStaticInstances,
		const uint64_t nVisibleInstances,
		const uint64_t nHlods,
		const uint64_t nVisibleHlods ) {
	m_nTerrainPatchCount.store( nTerrainPatches, std::memory_order_release );
	m_nVisibleTerrainPatches.store( nVisibleTerrainPatches, std::memory_order_release );
	m_nStaticInstanceCount.store( nStaticInstances, std::memory_order_release );
	m_nVisibleInstances.store( nVisibleInstances, std::memory_order_release );
	m_nHlodCount.store( nHlods, std::memory_order_release );
	m_nVisibleHlods.store( nVisibleHlods, std::memory_order_release );
}

void Terrain3DRdContext::recordSubmissionCounts( const uint64_t nIndirectCommands, const uint64_t nComputeDispatches ) {
	m_nIndirectCommandCount.store( nIndirectCommands, std::memory_order_release );
	m_nComputeDispatchCount.store( nComputeDispatches, std::memory_order_release );
}

Dictionary Terrain3DRdContext::getDiagnostics( void ) const {
	Dictionary result;
	const ContextState state = m_state.load( std::memory_order_acquire );
	const FailureCode failure = m_failureCode.load( std::memory_order_acquire );
	result["state"] = s_state_names[state];
	result["failure_state"] = s_failure_names[failure];
	result["ready"] = isReady();
	result["device_generation"] = ( int64_t )( m_nDeviceGeneration.load( std::memory_order_acquire ) );
	result["device_initializations"] = ( int64_t )( m_nDeviceInitializations.load( std::memory_order_acquire ) );
	result["device_rebuilds"] = ( int64_t )( m_nDeviceRebuilds.load( std::memory_order_acquire ) );
	result["device_invalidations"] = ( int64_t )( m_nDeviceInvalidations.load( std::memory_order_acquire ) );
	result["resource_creates"] = ( int64_t )( m_nCreatedResources.load( std::memory_order_acquire ) );
	result["resource_frees"] = ( int64_t )( m_nFreedResources.load( std::memory_order_acquire ) );
	result["resource_abandons"] = ( int64_t )( m_nAbandonedResources.load( std::memory_order_acquire ) );
	result["failed_resource_creates"] = ( int64_t )( m_nFailedResourceCreates.load( std::memory_order_acquire ) );
	result["double_free_attempts"] = ( int64_t )( m_nDoubleFreeAttempts.load( std::memory_order_acquire ) );
	result["untracked_free_attempts"] = ( int64_t )( m_nUntrackedFreeAttempts.load( std::memory_order_acquire ) );
	result["accounting_errors"] = ( int64_t )( m_nAccountingErrors.load( std::memory_order_acquire ) );
	result["live_buffer_bytes"] = ( int64_t )( m_nLiveBufferBytes.load( std::memory_order_acquire ) );
	result["peak_buffer_bytes"] = ( int64_t )( m_nPeakBufferBytes.load( std::memory_order_acquire ) );
	result["live_texture_bytes"] = ( int64_t )( m_nLiveTextureBytes.load( std::memory_order_acquire ) );
	result["peak_texture_bytes"] = ( int64_t )( m_nPeakTextureBytes.load( std::memory_order_acquire ) );
	result["buffer_updates"] = ( int64_t )( m_nBufferUpdates.load( std::memory_order_acquire ) );
	result["bytes_uploaded"] = ( int64_t )( m_nBytesUploaded.load( std::memory_order_acquire ) );
	result["buffer_updates_frame"] = ( int64_t )( m_nFrameBufferUpdates.load( std::memory_order_acquire ) );
	result["bytes_uploaded_frame"] = ( int64_t )( m_nFrameBytesUploaded.load( std::memory_order_acquire ) );
	result["gpu_frame_id"] = ( int64_t )( m_nGpuFrameId.load( std::memory_order_acquire ) );
	result["snapshot_generation"] = ( int64_t )( m_nSnapshotGeneration.load( std::memory_order_acquire ) );
	result["last_successful_gpu_frame"] = ( int64_t )( m_nLastSuccessfulGpuFrame.load( std::memory_order_acquire ) );
	result["vb_creates"] = ( int64_t )( m_szTypeCreates[RESOURCE_VERTEX_BUFFER].load( std::memory_order_acquire ) );
	result["vb_frees"] = ( int64_t )( m_szTypeFrees[RESOURCE_VERTEX_BUFFER].load( std::memory_order_acquire ) );
	result["ib_creates"] = ( int64_t )( m_szTypeCreates[RESOURCE_INDEX_BUFFER].load( std::memory_order_acquire ) );
	result["ib_frees"] = ( int64_t )( m_szTypeFrees[RESOURCE_INDEX_BUFFER].load( std::memory_order_acquire ) );
	result["shader_creates"] = ( int64_t )( m_szTypeCreates[RESOURCE_SHADER].load( std::memory_order_acquire ) );
	result["pipeline_creates"] = ( int64_t )( m_nPipelineCreates.load( std::memory_order_acquire ) );
	result["pipeline_rebuilds"] = ( int64_t )( m_nPipelineRebuilds.load( std::memory_order_acquire ) );
	result["uniform_set_creates"] = ( int64_t )( m_nUniformSetCreates.load( std::memory_order_acquire ) );
	result["uniform_set_rebuilds"] = ( int64_t )( m_nUniformSetRebuilds.load( std::memory_order_acquire ) );
	result["steady_state_allocation_violations"] = ( int64_t )( m_nSteadyStateAllocationViolations.load( std::memory_order_acquire ) );
	result["terrain_patch_count"] = ( int64_t )( m_nTerrainPatchCount.load( std::memory_order_acquire ) );
	result["visible_terrain_patches"] = ( int64_t )( m_nVisibleTerrainPatches.load( std::memory_order_acquire ) );
	result["static_instance_count"] = ( int64_t )( m_nStaticInstanceCount.load( std::memory_order_acquire ) );
	result["visible_instances"] = ( int64_t )( m_nVisibleInstances.load( std::memory_order_acquire ) );
	result["hlod_count"] = ( int64_t )( m_nHlodCount.load( std::memory_order_acquire ) );
	result["visible_hlods"] = ( int64_t )( m_nVisibleHlods.load( std::memory_order_acquire ) );
	result["indirect_command_count"] = ( int64_t )( m_nIndirectCommandCount.load( std::memory_order_acquire ) );
	result["compute_dispatch_count"] = ( int64_t )( m_nComputeDispatchCount.load( std::memory_order_acquire ) );

	uint64_t owned_bytes = 0u;
	for ( uint32_t category = MEMORY_NONE + 1u; category < MEMORY_CATEGORY_COUNT; ++category ) {
		const uint64_t live = m_szLiveMemory[category].load( std::memory_order_acquire );
		const uint64_t peak = m_szPeakMemory[category].load( std::memory_order_acquire );
		result[String( s_memory_keys[category] ) + "_bytes"] = ( int64_t )( live );
		result[String( "peak_" ) + s_memory_keys[category] + "_bytes"] = ( int64_t )( peak );
		owned_bytes += live;
	}
	result["topology_memory"] = ( int64_t )( m_szLiveMemory[MEMORY_TOPOLOGY_VERTEX].load( std::memory_order_acquire ) +
			m_szLiveMemory[MEMORY_TOPOLOGY_INDEX].load( std::memory_order_acquire ) );
	result["mesh_arena_memory"] = ( int64_t )( m_szLiveMemory[MEMORY_MESH_VERTEX_ARENA].load( std::memory_order_acquire ) +
			m_szLiveMemory[MEMORY_MESH_INDEX_ARENA].load( std::memory_order_acquire ) );
	result["texture_memory"] = ( int64_t )( m_szLiveMemory[MEMORY_TERRAIN_TEXTURE].load( std::memory_order_acquire ) );
	result["owned_gpu_bytes"] = ( int64_t )( owned_bytes );

	{
		std::lock_guard<std::mutex> lock( m_resourceMutex );
		result["live_resource_count"] = ( int64_t )( m_resources.size() );
		result["vertex_format_count"] = ( int64_t )( m_vertexFormats.size() );
		result["last_failure"] = m_lastFailure;
	}
	result["last_successful_pass"] =
			s_pass_names[m_lastSuccessfulPass.load( std::memory_order_acquire )];
	RenderingDevice *const rd = m_pRd.load( std::memory_order_acquire );
	if ( rd ) {
		result["device_name"] = rd->get_device_name();
		result["device_vendor"] = rd->get_device_vendor_name();
		result["device_memory"] = ( int64_t )( rd->get_device_total_memory() );
	}
	return result;
}
