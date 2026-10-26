#include "streaming/gpu/terrain_3d_gpu_readback_service.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include <godot_cpp/classes/file_access.hpp>

static constexpr uint32_t T3D_GPU_SNAPSHOT_MAGIC = 0x50473354u; // T3GP
static constexpr uint32_t T3D_GPU_SNAPSHOT_VERSION = 1u;

Terrain3DGpuReadbackService::Transaction *Terrain3DGpuReadbackService::find(
	uint64_t id ) {
	if ( !id )
		return nullptr;
	for ( uint32_t i = 0u; i < TRANSACTIONS; ++i )
		if ( m_transactions[i].state != EMPTY && m_transactions[i].id == id )
			return &m_transactions[i];
	return nullptr;
}
const Terrain3DGpuReadbackService::Transaction *Terrain3DGpuReadbackService::find(
	uint64_t id ) const {
	return const_cast< Terrain3DGpuReadbackService * >( this )->find( id );
}

uint64_t Terrain3DGpuReadbackService::begin(
	Terrain3DGpuPageStore &store,
	uint16_t			   region,
	uint8_t				   channelMask ) {
	if ( !store.active() || region >= T3D_REGION_SLOTS || !channelMask ||
		 ( channelMask & ~( ( 1u << T3D_GPU_PAGE_CHANNEL_COUNT ) - 1u ) ) )
		return 0u;
	Transaction *chosen = nullptr;
	for ( uint32_t i = 0u; i < TRANSACTIONS; ++i )
		if ( m_transactions[i].state == EMPTY ) {
			chosen = &m_transactions[i];
			break;
		}
	if ( !chosen )
		return 0u;
	Transaction candidate;
	uint64_t	publicationEpoch = 0u;
	candidate.region = region;
	candidate.channelMask = channelMask;
	candidate.state = CAPTURING;
	for ( uint32_t channel = 0u; channel < T3D_GPU_PAGE_CHANNEL_COUNT; ++channel ) {
		if ( !( channelMask & ( 1u << channel ) ) )
			continue;
		const t3dGpuPageLogicalKey_t key{ region, ( uint8_t ) channel, 0u };
		Channel						&entry = candidate.channels[channel];
		if ( !store.publishedSnapshot( key, entry.ticket, entry.handle ) )
			return 0u;
		if ( publicationEpoch == 0u )
			publicationEpoch = entry.ticket.m_nPublicationGeneration;
		else if ( publicationEpoch != entry.ticket.m_nPublicationGeneration )
			return 0u;
		if ( candidate.sourceGeneration == 0u ) {
			candidate.sourceGeneration = entry.ticket.m_nSourceGeneration;
			candidate.editGeneration = entry.ticket.m_nEditGeneration;
		} else if ( candidate.sourceGeneration != entry.ticket.m_nSourceGeneration ||
					candidate.editGeneration != entry.ticket.m_nEditGeneration )
			return 0u;
	}
	if ( ++m_serial == 0u )
		++m_serial;
	candidate.id = m_serial;
	*chosen = candidate;
	return candidate.id;
}

bool Terrain3DGpuReadbackService::dispatch(
	uint64_t			   id,
	uint8_t				   channel,
	Terrain3DRdContext	  &context,
	Terrain3DGpuPageStore &store,
	uint64_t			   submissionEpoch,
	const Callable		  &callback ) {
	Transaction *tx = find( id );
	if ( !tx || tx->state != CAPTURING || channel >= T3D_GPU_PAGE_CHANNEL_COUNT ||
		 !( tx->channelMask & ( 1u << channel ) ) || !submissionEpoch || !Terrain3DRdContext::isOnRenderThread() ||
		 !callback.is_valid() )
		return false;
	Channel &entry = tx->channels[channel];
	if ( entry.requested || !store.snapshotCurrent( entry.ticket, entry.handle ) ||
		 !store.pool( channel ).markReader( entry.handle, submissionEpoch ) )
		return false;
	const Error error =
		store.pool( channel ).texture().readbackAsyncTracked( context, entry.handle.m_nPhysicalLayer, callback );
	if ( error != OK )
		return false;
	entry.submissionEpoch = submissionEpoch;
	entry.requested = true;
	return true;
}

bool Terrain3DGpuReadbackService::dispatchAliasProbe(
	const Terrain3DGpuPageStore::AliasProbe &probe,
	Terrain3DRdContext						&context,
	const Terrain3DGpuPageStore				&store,
	const Callable							&originalCallback,
	const Callable							&candidateCallback ) {
	if ( !probe.readerEpoch || !Terrain3DRdContext::isOnRenderThread() || !originalCallback.is_valid() ||
		 !candidateCallback.is_valid() || !store.snapshotCurrent( probe.ticket, probe.original ) )
		return false;
	const uint8_t				channel = probe.ticket.m_key.m_nChannel;
	const Terrain3DGpuPagePool &pool = store.pool( channel );
	if ( !pool.isPublished( probe.candidate ) )
		return false;
	// Each callback must supply its full raw payload to finishAliasProbe().
	// The caller must keep the snapshots pinned and cannot reuse the probe ID.
	if ( pool.texture().readbackAsyncTracked( context, probe.original.m_nPhysicalLayer, originalCallback ) != OK )
		return false;
	return pool.texture().readbackAsyncTracked( context, probe.candidate.m_nPhysicalLayer, candidateCallback ) == OK;
}

bool Terrain3DGpuReadbackService::acceptChannel(
	uint64_t			   id,
	uint8_t				   channel,
	Terrain3DGpuPageStore &store,
	const PackedByteArray &gpuBytes ) {
	Transaction *tx = find( id );
	if ( !tx || tx->state != CAPTURING || channel >= T3D_GPU_PAGE_CHANNEL_COUNT ||
		 !( tx->channelMask & ( 1u << channel ) ) )
		return false;
	Channel		  &entry = tx->channels[channel];
	const uint64_t expected = store.pool( channel ).layerBytes();
	if ( !entry.requested || entry.received || !store.snapshotCurrent( entry.ticket, entry.handle ) || !expected ||
		 ( uint64_t ) gpuBytes.size() != expected || expected > MAX_STAGING_BYTES - m_stagingBytes )
		return false;
	entry.bytes = gpuBytes;
	entry.received = true;
	m_stagingBytes += expected;
	bool all = true;
	for ( uint32_t i = 0u; i < T3D_GPU_PAGE_CHANNEL_COUNT; ++i )
		if ( ( tx->channelMask & ( 1u << i ) ) && !tx->channels[i].received )
			all = false;
	if ( all )
		tx->state = READY;
	return true;
}

String Terrain3DGpuReadbackService::snapshotPath(
	const String &directory,
	uint16_t	  region ) {
	return directory.path_join( String( "terrain3d_gpu_region_" ) + String::num_int64( region ) + String( ".t3gp" ) );
}

Error Terrain3DGpuReadbackService::publish(
	uint64_t			   id,
	const String		  &directory,
	Terrain3DGpuPageStore &store ) {
	Transaction *tx = find( id );
	if ( !tx || tx->state != READY || directory.is_empty() )
		return ERR_INVALID_PARAMETER;
	// Freeze a complete region generation, even when edits land during disk I/O.
	for ( uint32_t channel = 0u; channel < T3D_GPU_PAGE_CHANNEL_COUNT; ++channel ) {
		if ( ( tx->channelMask & ( 1u << channel ) ) &&
			 !store.snapshotCurrent( tx->channels[channel].ticket, tx->channels[channel].handle ) )
			return ERR_BUSY;
	}
	const String			 path = snapshotPath( directory, tx->region );
	Terrain3DFilePublication transaction( path, path + String( ".staged" ), path + String( ".previous" ) );
	Ref< FileAccess >		 file;
	Error					 error = transaction.openStaged( file );
	if ( error != OK )
		return error;
	// Fixed header: magic, version, region, mask, source, edit, reserved.
	file->store_32( T3D_GPU_SNAPSHOT_MAGIC );
	file->store_32( T3D_GPU_SNAPSHOT_VERSION );
	file->store_32( tx->region );
	file->store_32( tx->channelMask );
	file->store_32( tx->sourceGeneration );
	file->store_32( tx->editGeneration );
	file->store_32( 0u );
	uint64_t bytesWritten = 28u;
	for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c ) {
		if ( !( tx->channelMask & ( 1u << c ) ) )
			continue;
		const Channel &entry = tx->channels[c];
		const auto	  &config = store.pool( ( uint8_t ) c ).configuration();
		const uint64_t hash = t3dHashStableBytes( entry.bytes.ptr(), ( uint64_t ) entry.bytes.size() );
		file->store_32( c );
		file->store_32( ( uint32_t ) config.format );
		file->store_32( config.width );
		file->store_32( config.height );
		file->store_32( config.mipmaps );
		file->store_32( ( uint32_t ) entry.bytes.size() );
		file->store_64( hash );
		file->store_buffer( entry.bytes );
		bytesWritten += 32u + ( uint64_t ) entry.bytes.size();
	}
	error = transaction.finishStaged( file, bytesWritten );
	if ( error != OK ) {
		transaction.rollback();
		return error;
	}
	error = transaction.publish();
	if ( error != OK )
		return error;
	// If an edit raced the write, restore the previous file and retain dirty state.
	for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c ) {
		if ( ( tx->channelMask & ( 1u << c ) ) &&
			 !store.snapshotCurrent( tx->channels[c].ticket, tx->channels[c].handle ) ) {
			transaction.rollback();
			return ERR_BUSY;
		}
	}
	error = transaction.commit();
	if ( error != OK )
		return error;
	for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c )
		if ( tx->channelMask & ( 1u << c ) )
			store.acknowledgeSaved( tx->channels[c].ticket, tx->channels[c].handle );
	cancel( id );
	return OK;
}

Error Terrain3DGpuReadbackService::loadSnapshot(
	const String   &path,
	uint16_t		expectedRegion,
	PackedByteArray output[T3D_GPU_PAGE_CHANNEL_COUNT],
	uint8_t		   &channelMask ) {
	channelMask = 0u;
	for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c )
		output[c] = PackedByteArray();
	if ( expectedRegion >= T3D_REGION_SLOTS )
		return ERR_INVALID_PARAMETER;
	Ref< FileAccess > file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() )
		return ERR_CANT_OPEN;
	if ( file->get_length() < 28u || file->get_32() != T3D_GPU_SNAPSHOT_MAGIC ||
		 file->get_32() != T3D_GPU_SNAPSHOT_VERSION || file->get_32() != expectedRegion )
		return ERR_FILE_CORRUPT;
	const uint32_t mask = file->get_32();
	if ( !mask || ( mask & ~( ( 1u << T3D_GPU_PAGE_CHANNEL_COUNT ) - 1u ) ) )
		return ERR_FILE_CORRUPT;
	const uint32_t sourceGeneration = file->get_32();
	const uint32_t editGeneration = file->get_32();
	if ( !sourceGeneration || !editGeneration )
		return ERR_FILE_CORRUPT;
	file->get_32(); // reserved
	uint64_t		accumulated = 0u;
	PackedByteArray verified[T3D_GPU_PAGE_CHANNEL_COUNT];
	for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c ) {
		if ( !( mask & ( 1u << c ) ) )
			continue;
		if ( file->get_length() - file->get_position() < 32u )
			return ERR_FILE_CORRUPT;
		const uint32_t							 actualChannel = file->get_32();
		const uint32_t							 format = file->get_32();
		const uint32_t							 width = file->get_32();
		const uint32_t							 height = file->get_32();
		const uint32_t							 mips = file->get_32();
		const uint32_t							 bytes = file->get_32();
		const uint64_t							 hash = file->get_64();
		static const RenderingDevice::DataFormat formats[T3D_GPU_PAGE_CHANNEL_COUNT] = {
			RenderingDevice::DATA_FORMAT_R16_UINT,
			RenderingDevice::DATA_FORMAT_R32_UINT,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::DATA_FORMAT_R8G8_UNORM,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::DATA_FORMAT_R8_UNORM };
		t3dRdTextureLayout_t layout;
		if ( ( RenderingDevice::DataFormat ) format != formats[c] || actualChannel != c || !width || !height || !mips ||
			 !bytes || bytes > MAX_STAGING_BYTES - accumulated || bytes > file->get_length() - file->get_position() ||
			 !Terrain3DRdTextureManager::buildRawPageLayout( ( RenderingDevice::DataFormat ) format,
															 width,
															 height,
															 1u,
															 mips,
															 false,
															 layout ) ||
			 layout.m_nBytes != bytes )
			return ERR_FILE_CORRUPT;
		accumulated += bytes;
		const PackedByteArray payload = file->get_buffer( bytes );
		if ( ( uint32_t ) payload.size() != bytes || t3dHashStableBytes( payload.ptr(), bytes ) != hash )
			return ERR_FILE_CORRUPT;
		verified[c] = payload;
	}
	if ( file->get_position() != file->get_length() )
		return ERR_FILE_CORRUPT;
	for ( uint32_t c = 0u; c < T3D_GPU_PAGE_CHANNEL_COUNT; ++c )
		output[c] = verified[c];
	channelMask = ( uint8_t ) mask;
	return OK;
}

void Terrain3DGpuReadbackService::cancel(
	uint64_t id ) {
	Transaction *tx = find( id );
	if ( !tx )
		return;
	for ( uint32_t i = 0u; i < T3D_GPU_PAGE_CHANNEL_COUNT; ++i )
		if ( tx->channels[i].received )
			m_stagingBytes -= ( uint64_t ) tx->channels[i].bytes.size();
	*tx = Transaction{};
}
void Terrain3DGpuReadbackService::clear() {
	for ( uint32_t i = 0u; i < TRANSACTIONS; ++i )
		if ( m_transactions[i].state != EMPTY )
			cancel( m_transactions[i].id );
}
uint32_t Terrain3DGpuReadbackService::pendingCount() const {
	uint32_t count = 0u;
	for ( uint32_t i = 0u; i < TRANSACTIONS; ++i )
		if ( m_transactions[i].state != EMPTY )
			++count;
	return count;
}
