#include "streaming/gpu/terrain_3d_gpu_page_store.h"
#include "core/utility/terrain_3d_generation.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include <string.h>

bool Terrain3DGpuPageStore::configure(
	const PoolConfig &config,
	uint64_t		  maximumPhysicalBytes ) {
	if ( m_active || !maximumPhysicalBytes )
		return false;
	uint64_t total = 0u;
	// Validate all layouts and budgets before mutating a single pool.
	for ( uint32_t c = 0u; c < CHANNEL_COUNT; ++c ) {
		const auto								&p = config.channels[c];
		static const RenderingDevice::DataFormat expected[CHANNEL_COUNT] = {
			RenderingDevice::DATA_FORMAT_R16_UINT,
			RenderingDevice::DATA_FORMAT_R32_UINT,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::DATA_FORMAT_R8G8_UNORM,
			RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::DATA_FORMAT_R8_UNORM };
		if ( p.format != expected[c] )
			return false;
		t3dRdTextureLayout_t layout;
		if ( !Terrain3DRdTextureManager::buildRawPageLayout( p.format,
															 p.width,
															 p.height,
															 p.layers,
															 p.mipmaps,
															 p.storageWrites,
															 layout ) ||
			 layout.m_nBytes > p.maximumBytes || layout.m_nBytes > maximumPhysicalBytes - total )
			return false;
		total += layout.m_nBytes;
	}
	for ( uint32_t c = 0u; c < CHANNEL_COUNT; ++c ) {
		if ( !m_pools[c].configure( config.channels[c], ( uint16_t ) c ) )
			return false;
	}
	m_maximumPhysicalBytes = maximumPhysicalBytes;
	m_configured = true;
	return true;
}

bool Terrain3DGpuPageStore::createCold(
	Terrain3DRdTextureManager &manager ) {
	if ( !m_configured || m_active )
		return false;
	uint32_t created = 0u;
	for ( ; created < CHANNEL_COUNT; ++created ) {
		if ( !m_pools[created].createCold( manager ) )
			break;
	}
	if ( created != CHANNEL_COUNT ) {
		for ( uint32_t i = 0u; i < created; ++i )
			m_pools[i].destroyCold( manager );
		return false;
	}
	m_active = true;
	return true;
}

bool Terrain3DGpuPageStore::stageDefault(
	Terrain3DRdTextureManager &manager,
	t3dGpuPageChannel_t		   channel,
	const PackedByteArray	  &bytes,
	uint64_t				   submissionEpoch ) {
	return m_active && channel < CHANNEL_COUNT && Terrain3DRdContext::isOnRenderThread() &&
		   m_pools[channel].stageDefault( manager, bytes, submissionEpoch );
}

bool Terrain3DGpuPageStore::publishDefault(
	t3dGpuPageChannel_t channel,
	uint64_t			confirmedCompletedEpoch ) {
	return m_active && channel < CHANNEL_COUNT && m_pools[channel].publishDefault( confirmedCompletedEpoch );
}

bool Terrain3DGpuPageStore::stage(
	Terrain3DRdTextureManager &manager,
	const t3dGpuPageTicket_t  &ticket,
	const PackedByteArray	  &bytes,
	uint64_t				   submissionEpoch ) {
	if ( !m_active || !ticket.m_key.valid() || !submissionEpoch || !Terrain3DRdContext::isOnRenderThread() )
		return false;
	Logical &entry = m_logical[index( ticket.m_key )];
	if ( entry.hasStaged )
		return false;
	Terrain3DGpuPagePool &pool = m_pools[ticket.m_key.m_nChannel];
	if ( ticket.m_nDeviceEpoch != pool.deviceEpoch() )
		return false;
	const t3dGpuPageHandle_t handle = pool.reserve();
	if ( handle.m_nPhysicalLayer == UINT16_MAX )
		return false;
	if ( !pool.upload( manager, handle, bytes ) ) {
		pool.release( handle );
		// Failed staging has never been submitted to a shader; GPU retirement is still explicit.
		return false;
	}
	pool.pinUntil( handle, submissionEpoch );
	entry.staged = handle;
	entry.stagedFingerprint =
		Terrain3DGpuContentDirectory::fingerprint( ticket.m_key.m_nChannel, pool.configuration(), bytes );
	entry.hasFingerprint = entry.stagedFingerprint.m_nPayloadBytes != 0u;
	entry.ticket = ticket;
	entry.uploadEpoch = submissionEpoch;
	entry.hasStaged = true;
	return true;
}

bool Terrain3DGpuPageStore::commit(
	const t3dGpuPageTicket_t &ticket,
	uint64_t				  confirmedCompletedEpoch ) {
	return commitBatch( &ticket, 1u, confirmedCompletedEpoch );
}

bool Terrain3DGpuPageStore::commitBatch(
	const t3dGpuPageTicket_t *tickets,
	uint32_t				  count,
	uint64_t				  confirmedCompletedEpoch ) {
	if ( !m_active || !tickets || count == 0u || count > CHANNEL_COUNT )
		return false;
	const t3dGpuPageTicket_t &first = tickets[0];
	if ( !first.m_key.valid() )
		return false;
	uint32_t channelsSeen = 0u;
	for ( uint32_t i = 0u; i < count; ++i ) {
		const auto &ticket = tickets[i];
		if ( !ticket.m_key.valid() || ticket.m_key.m_nRegionIndex != first.m_key.m_nRegionIndex ||
			 ticket.m_nSourceGeneration != first.m_nSourceGeneration ||
			 ticket.m_nEditGeneration != first.m_nEditGeneration ||
			 ticket.m_nPublicationGeneration != first.m_nPublicationGeneration ||
			 ( channelsSeen & ( 1u << ticket.m_key.m_nChannel ) ) )
			return false;
		channelsSeen |= 1u << ticket.m_key.m_nChannel;
		const Logical &logical = m_logical[index( ticket.m_key )];
		if ( !logical.hasStaged || !t3dGpuPageTicketCurrent( ticket, logical.ticket ) ||
			 logical.uploadEpoch > confirmedCompletedEpoch ||
			 !m_pools[ticket.m_key.m_nChannel].valid( logical.staged ) )
			return false;
	}
	// Prevent a partial edit publication from leaving older sibling channels visible.
	const uint32_t base = ( uint32_t ) first.m_key.m_nRegionIndex * CHANNEL_COUNT;
	for ( uint32_t c = 0u; c < CHANNEL_COUNT; ++c ) {
		if ( ( channelsSeen & ( 1u << c ) ) || !m_logical[base + c].hasPublished )
			continue;
		const auto &published = m_directory[base + c];
		if ( published.sourceGeneration != first.m_nSourceGeneration ||
			 published.editGeneration != first.m_nEditGeneration )
			return false;
	}
	// Each physical slot is staged and checked before publishing any logical entry.
	for ( uint32_t i = 0u; i < count; ++i ) {
		const auto			 &ticket = tickets[i];
		Logical				 &logical = m_logical[index( ticket.m_key )];
		Terrain3DGpuPagePool &pool = m_pools[ticket.m_key.m_nChannel];
		if ( !pool.publish( logical.staged ) )
			return false;
		if ( logical.hasPublished ) {
			const t3dGpuPageHandle_t old = logical.published;
			pool.release( old );
			if ( !pool.isPublished( old ) )
				m_content.forget( old );
		}
		logical.published = logical.staged;
		logical.hasPublished = true;
		logical.hasStaged = false;
		logical.publishedTicket = ticket;
		logical.committedFingerprint = logical.hasFingerprint ? logical.stagedFingerprint : t3dGpuPageContentKey_t{};
		logical.dirty = ticket.m_nEditGeneration > 1u;
		if ( logical.hasFingerprint )
			m_content.record( logical.published, logical.stagedFingerprint );
		logical.hasFingerprint = false;
		auto &record = m_directory[index( ticket.m_key )];
		record.physicalLayer = logical.published.m_nPhysicalLayer;
		record.slotGeneration = logical.published.m_nSlotGeneration;
		record.sourceGeneration = ticket.m_nSourceGeneration;
		record.editGeneration = ticket.m_nEditGeneration;
	}
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	return true;
}

bool Terrain3DGpuPageStore::stageComputedDerived(
	const t3dGpuPageTicket_t &ticket,
	uint64_t				  submissionEpoch ) {
	if ( !m_active || !Terrain3DRdContext::isOnRenderThread() || !submissionEpoch || !ticket.m_key.valid() ||
		 ticket.m_key.m_nChannel != T3D_GPU_PAGE_DERIVED )
		return false;
	Logical				 &entry = m_logical[index( ticket.m_key )];
	Terrain3DGpuPagePool &pool = m_pools[T3D_GPU_PAGE_DERIVED];
	if ( entry.hasStaged || ticket.m_nDeviceEpoch != pool.deviceEpoch() || pool.configuration().mipmaps != 1u ||
		 !pool.configuration().storageWrites )
		return false;
	const t3dGpuPageHandle_t handle = pool.reserve();
	if ( handle.m_nPhysicalLayer == UINT16_MAX )
		return false;
	if ( !pool.pinUntil( handle, submissionEpoch ) ) {
		pool.release( handle );
		return false;
	}
	entry.staged = handle;
	entry.ticket = ticket;
	entry.uploadEpoch = submissionEpoch;
	entry.hasFingerprint = false;
	entry.hasStaged = true;
	return true;
}

bool Terrain3DGpuPageStore::commitComputedDerived(
	const t3dGpuPageTicket_t &ticket,
	const t3dGpuPageTicket_t &heightSource,
	uint64_t				  confirmedCompletedEpoch ) {
	if ( !m_active || !Terrain3DRdContext::isOnRenderThread() || !ticket.m_key.valid() ||
		 ticket.m_key.m_nChannel != T3D_GPU_PAGE_DERIVED || !heightSource.m_key.valid() ||
		 heightSource.m_key.m_nChannel != T3D_GPU_PAGE_HEIGHT ||
		 ticket.m_key.m_nRegionIndex != heightSource.m_key.m_nRegionIndex ||
		 ticket.m_nSourceGeneration != heightSource.m_nSourceGeneration ||
		 ticket.m_nEditGeneration != heightSource.m_nEditGeneration ||
		 ticket.m_nDeviceEpoch != m_pools[T3D_GPU_PAGE_DERIVED].deviceEpoch() ||
		 heightSource.m_nDeviceEpoch != m_pools[T3D_GPU_PAGE_HEIGHT].deviceEpoch() )
		return false;
	Logical				 &entry = m_logical[index( ticket.m_key )];
	const Logical		 &height = m_logical[index( heightSource.m_key )];
	Terrain3DGpuPagePool &pool = m_pools[T3D_GPU_PAGE_DERIVED];
	if ( !height.hasPublished || !t3dGpuPageTicketCurrent( heightSource, height.publishedTicket ) || !entry.hasStaged ||
		 !t3dGpuPageTicketCurrent( ticket, entry.ticket ) || entry.uploadEpoch > confirmedCompletedEpoch ||
		 !pool.valid( entry.staged ) )
		return false;
	if ( !pool.publish( entry.staged ) )
		return false;
	const t3dGpuPageHandle_t previous = entry.published;
	const bool				 hadPrevious = entry.hasPublished;
	entry.published = entry.staged;
	entry.publishedTicket = ticket;
	entry.hasPublished = true;
	entry.hasStaged = false;
	entry.hasFingerprint = false;
	entry.committedFingerprint = t3dGpuPageContentKey_t{};
	entry.dirty = false; // deterministic derivative, always regenerable
	auto &record = m_directory[index( ticket.m_key )];
	record.physicalLayer = entry.published.m_nPhysicalLayer;
	record.slotGeneration = entry.published.m_nSlotGeneration;
	record.sourceGeneration = ticket.m_nSourceGeneration;
	record.editGeneration = ticket.m_nEditGeneration;
	if ( hadPrevious ) {
		pool.release( previous );
		if ( !pool.isPublished( previous ) )
			m_content.forget( previous );
	}
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	return true;
}

bool Terrain3DGpuPageStore::stagedSnapshot(
	const t3dGpuPageTicket_t &ticket,
	t3dGpuPageHandle_t		 &handle ) const {
	if ( !m_active || !ticket.m_key.valid() )
		return false;
	const Logical &logical = m_logical[index( ticket.m_key )];
	if ( !logical.hasStaged || !t3dGpuPageTicketCurrent( ticket, logical.ticket ) ||
		 !m_pools[ticket.m_key.m_nChannel].valid( logical.staged ) )
		return false;
	handle = logical.staged;
	return true;
}

bool Terrain3DGpuPageStore::commitChannelEdit(
	const t3dGpuPageTicket_t &ticket,
	uint64_t				  confirmedCompletedEpoch ) {
	if ( !m_active || !ticket.m_key.valid() || !Terrain3DRdContext::isOnRenderThread() )
		return false;
	Logical				 &logical = m_logical[index( ticket.m_key )];
	Terrain3DGpuPagePool &pool = m_pools[ticket.m_key.m_nChannel];
	if ( !logical.hasStaged || !logical.hasPublished || !t3dGpuPageTicketCurrent( ticket, logical.ticket ) ||
		 ticket.m_nEditGeneration <= logical.publishedTicket.m_nEditGeneration ||
		 ticket.m_nSourceGeneration != logical.publishedTicket.m_nSourceGeneration ||
		 logical.uploadEpoch > confirmedCompletedEpoch || !pool.isPublished( logical.published ) ||
		 !pool.valid( logical.staged ) )
		return false;
	if ( !pool.publish( logical.staged ) )
		return false;
	const t3dGpuPageHandle_t previous = logical.published;
	logical.published = logical.staged;
	logical.publishedTicket = ticket;
	logical.hasStaged = false;
	logical.hasFingerprint = false; // GPU-modified contents require verified fingerprint.
	logical.committedFingerprint = t3dGpuPageContentKey_t{};
	logical.dirty = true;
	auto &record = m_directory[index( ticket.m_key )];
	record.physicalLayer = logical.published.m_nPhysicalLayer;
	record.slotGeneration = logical.published.m_nSlotGeneration;
	record.sourceGeneration = ticket.m_nSourceGeneration;
	record.editGeneration = ticket.m_nEditGeneration;
	pool.release( previous );
	if ( !pool.isPublished( previous ) )
		m_content.forget( previous );
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	return true;
}

bool Terrain3DGpuPageStore::retainHistoryHandle(
	const t3dGpuPageHandle_t &handle ) {
	return m_active && Terrain3DRdContext::isOnRenderThread() && handle.m_nPoolIndex < CHANNEL_COUNT &&
		   m_pools[handle.m_nPoolIndex].addReference( handle );
}

bool Terrain3DGpuPageStore::releaseHistoryHandle(
	const t3dGpuPageHandle_t &handle ) {
	return m_active && Terrain3DRdContext::isOnRenderThread() && handle.m_nPoolIndex < CHANNEL_COUNT &&
		   m_pools[handle.m_nPoolIndex].release( handle );
}

bool Terrain3DGpuPageStore::restoreHistoryHandle(
	const t3dGpuPageTicket_t &newTicket,
	const t3dGpuPageHandle_t &historyHandle,
	uint64_t				  confirmedCompletedEpoch ) {
	if ( !m_active || !newTicket.m_key.valid() || !Terrain3DRdContext::isOnRenderThread() )
		return false;
	Logical				 &logical = m_logical[index( newTicket.m_key )];
	Terrain3DGpuPagePool &pool = m_pools[newTicket.m_key.m_nChannel];
	if ( !logical.hasPublished || logical.hasStaged || historyHandle.m_nPoolIndex != newTicket.m_key.m_nChannel ||
		 !pool.isPublished( historyHandle ) || !pool.isPublished( logical.published ) ||
		 newTicket.m_nDeviceEpoch != pool.deviceEpoch() ||
		 newTicket.m_nSourceGeneration != logical.publishedTicket.m_nSourceGeneration ||
		 newTicket.m_nEditGeneration <= logical.publishedTicket.m_nEditGeneration ||
		 newTicket.m_nPublicationGeneration <= logical.publishedTicket.m_nPublicationGeneration ||
		 !confirmedCompletedEpoch )
		return false;
	if ( !pool.addReference( historyHandle ) )
		return false;
	const t3dGpuPageHandle_t previous = logical.published;
	logical.published = historyHandle;
	logical.publishedTicket = newTicket;
	logical.committedFingerprint = t3dGpuPageContentKey_t{};
	logical.dirty = true;
	auto &record = m_directory[index( newTicket.m_key )];
	record.physicalLayer = historyHandle.m_nPhysicalLayer;
	record.slotGeneration = historyHandle.m_nSlotGeneration;
	record.sourceGeneration = newTicket.m_nSourceGeneration;
	record.editGeneration = newTicket.m_nEditGeneration;
	pool.release( previous );
	if ( !pool.isPublished( previous ) )
		m_content.forget( previous );
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	return true;
}

bool Terrain3DGpuPageStore::cancelStaged(
	const t3dGpuPageLogicalKey_t &key ) {
	if ( !key.valid() )
		return false;
	Logical &logical = m_logical[index( key )];
	if ( !logical.hasStaged )
		return false;
	m_pools[key.m_nChannel].release( logical.staged );
	logical.hasStaged = false;
	logical.hasFingerprint = false;
	return true;
}

bool Terrain3DGpuPageStore::withdraw(
	const t3dGpuPageLogicalKey_t &key ) {
	if ( !key.valid() )
		return false;
	Logical				 &entry = m_logical[index( key )];
	Terrain3DGpuPagePool &pool = m_pools[key.m_nChannel];
	bool				  changed = false;
	if ( entry.hasStaged ) {
		pool.release( entry.staged );
		entry.hasStaged = false;
		entry.hasFingerprint = false;
		changed = true;
	}
	if ( entry.hasPublished ) {
		const auto old = entry.published;
		pool.release( old );
		if ( !pool.isPublished( old ) )
			m_content.forget( old );
		entry.hasPublished = false;
		changed = true;
	}
	if ( changed ) {
		m_directory[index( key )] = t3dGpuChannelDirectoryEntry_t{};
		m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	}
	return changed;
}

bool Terrain3DGpuPageStore::publishedSnapshot(
	const t3dGpuPageLogicalKey_t &key,
	t3dGpuPageTicket_t			 &ticket,
	t3dGpuPageHandle_t			 &handle ) const {
	if ( !key.valid() )
		return false;
	const Logical &l = m_logical[index( key )];
	if ( !l.hasPublished || !m_pools[key.m_nChannel].isPublished( l.published ) )
		return false;
	ticket = l.publishedTicket;
	handle = l.published;
	return true;
}

bool Terrain3DGpuPageStore::snapshotCurrent(
	const t3dGpuPageTicket_t &ticket,
	const t3dGpuPageHandle_t &handle ) const {
	if ( !ticket.m_key.valid() )
		return false;
	const Logical &l = m_logical[index( ticket.m_key )];
	return l.hasPublished && t3dGpuPageTicketCurrent( ticket, l.publishedTicket ) &&
		   l.published.m_nPoolIndex == handle.m_nPoolIndex && l.published.m_nPhysicalLayer == handle.m_nPhysicalLayer &&
		   l.published.m_nSlotGeneration == handle.m_nSlotGeneration &&
		   l.published.m_nDeviceEpoch == handle.m_nDeviceEpoch &&
		   m_pools[ticket.m_key.m_nChannel].isPublished( handle );
}

bool Terrain3DGpuPageStore::registerGpuFingerprint(
	const t3dGpuPageTicket_t &ticket,
	const t3dGpuPageHandle_t &handle,
	const PackedByteArray	 &actualGpuBytes ) {
	if ( !snapshotCurrent( ticket, handle ) || !Terrain3DRdContext::isOnRenderThread() )
		return false;
	Terrain3DGpuPagePool &pool = m_pools[ticket.m_key.m_nChannel];
	if ( ( uint64_t ) actualGpuBytes.size() != pool.layerBytes() )
		return false;
	const auto fingerprint =
		Terrain3DGpuContentDirectory::fingerprint( ticket.m_key.m_nChannel, pool.configuration(), actualGpuBytes );
	if ( !fingerprint.m_nPayloadBytes )
		return false;
	m_logical[index( ticket.m_key )].committedFingerprint = fingerprint;
	m_content.record( handle, fingerprint );
	return true;
}

bool Terrain3DGpuPageStore::beginAliasProbe(
	const t3dGpuPageLogicalKey_t &key,
	uint64_t					  readerEpoch,
	AliasProbe					 &probe,
	uint16_t					  afterPhysicalLayer ) {
	probe = AliasProbe{};
	if ( !m_active || !key.valid() || !readerEpoch )
		return false;
	Logical &l = m_logical[index( key )];
	if ( !l.hasPublished )
		return false;
	Terrain3DGpuPagePool &pool = m_pools[key.m_nChannel];
	// A GPU-written page must first be finalized and fingerprinted from its actual bytes.
	t3dGpuPageHandle_t candidate;
	if ( !m_content.findCandidate( key.m_nChannel,
								   l.committedFingerprint,
								   l.published,
								   pool,
								   candidate,
								   afterPhysicalLayer ) )
		return false;
	if ( !pool.pinUntil( l.published, readerEpoch ) || !pool.pinUntil( candidate, readerEpoch ) )
		return false;
	probe.ticket = l.publishedTicket;
	probe.original = l.published;
	probe.candidate = candidate;
	probe.readerEpoch = readerEpoch;
	probe.payloadBytes = l.committedFingerprint.m_nPayloadBytes;
	return probe.payloadBytes != 0u;
}

bool Terrain3DGpuPageStore::finishAliasProbe(
	const AliasProbe	  &probe,
	const PackedByteArray &originalGpuBytes,
	const PackedByteArray &candidateGpuBytes,
	uint64_t			   confirmedCompletedEpoch ) {
	if ( !probe.readerEpoch || probe.readerEpoch > confirmedCompletedEpoch ||
		 !snapshotCurrent( probe.ticket, probe.original ) ||
		 !Terrain3DGpuContentDirectory::equalBytes( originalGpuBytes, candidateGpuBytes, probe.payloadBytes ) )
		return false;
	Logical				 &l = m_logical[index( probe.ticket.m_key )];
	Terrain3DGpuPagePool &pool = m_pools[probe.ticket.m_key.m_nChannel];
	if ( !pool.isPublished( probe.candidate ) || !pool.addReference( probe.candidate ) )
		return false;
	const auto old = l.published;
	l.published = probe.candidate;
	auto &record = m_directory[index( probe.ticket.m_key )];
	record.physicalLayer = l.published.m_nPhysicalLayer;
	record.slotGeneration = l.published.m_nSlotGeneration;
	pool.release( old );
	if ( !pool.isPublished( old ) )
		m_content.forget( old );
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	return true;
}

bool Terrain3DGpuPageStore::finishGpuVerifiedAlias(
	const AliasProbe &probe,
	uint64_t		  completedEpoch ) {
	if ( !probe.readerEpoch || completedEpoch < probe.readerEpoch || !snapshotCurrent( probe.ticket, probe.original ) )
		return false;
	Logical				 &logical = m_logical[index( probe.ticket.m_key )];
	Terrain3DGpuPagePool &pool = m_pools[probe.ticket.m_key.m_nChannel];
	if ( !pool.isPublished( probe.candidate ) || !pool.addReference( probe.candidate ) )
		return false;
	const t3dGpuPageHandle_t old = logical.published;
	logical.published = probe.candidate;
	auto &record = m_directory[index( probe.ticket.m_key )];
	record.physicalLayer = logical.published.m_nPhysicalLayer;
	record.slotGeneration = logical.published.m_nSlotGeneration;
	pool.release( old );
	if ( !pool.isPublished( old ) )
		m_content.forget( old );
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	return true;
}

bool Terrain3DGpuPageStore::stageCopyForEdit(
	Terrain3DRdTextureManager &manager,
	Terrain3DRdContext		  &context,
	const t3dGpuPageTicket_t  &ticket,
	uint64_t				   submissionEpoch ) {
	if ( !m_active || !ticket.m_key.valid() || !submissionEpoch || !Terrain3DRdContext::isOnRenderThread() )
		return false;
	Logical &l = m_logical[index( ticket.m_key )];
	if ( !l.hasPublished || l.hasStaged || !m_pools[ticket.m_key.m_nChannel].isPublished( l.published ) ||
		 ticket.m_nSourceGeneration != l.publishedTicket.m_nSourceGeneration ||
		 ticket.m_nEditGeneration <= l.publishedTicket.m_nEditGeneration )
		return false;
	Terrain3DGpuPagePool &pool = m_pools[ticket.m_key.m_nChannel];
	if ( ticket.m_nDeviceEpoch != pool.deviceEpoch() )
		return false;
	const auto fresh = pool.reserve();
	if ( fresh.m_nPhysicalLayer == UINT16_MAX )
		return false;
	if ( !manager.copyRawPage( context,
							   pool.texture(),
							   pool.mutableTexture(),
							   l.published.m_nPhysicalLayer,
							   fresh.m_nPhysicalLayer ) ) {
		pool.release( fresh );
		return false;
	}
	pool.pinUntil( fresh, submissionEpoch );
	l.staged = fresh;
	l.ticket = ticket;
	l.uploadEpoch = submissionEpoch;
	l.hasStaged = true;
	l.hasFingerprint = false;
	return true;
}

bool Terrain3DGpuPageStore::acknowledgeSaved(
	const t3dGpuPageTicket_t &ticket,
	const t3dGpuPageHandle_t &handle ) {
	if ( !snapshotCurrent( ticket, handle ) )
		return false;
	m_logical[index( ticket.m_key )].dirty = false;
	return true;
}

bool Terrain3DGpuPageStore::isDirty(
	const t3dGpuPageLogicalKey_t &key ) const {
	return key.valid() && m_logical[index( key )].hasPublished && m_logical[index( key )].dirty;
}

void Terrain3DGpuPageStore::markReader(
	const t3dGpuPageLogicalKey_t &key,
	uint64_t					  epoch ) {
	if ( key.valid() && m_logical[index( key )].hasPublished )
		m_pools[key.m_nChannel].markReader( m_logical[index( key )].published, epoch );
}

void Terrain3DGpuPageStore::collect(
	uint64_t completedEpoch ) {
	for ( uint32_t c = 0u; c < CHANNEL_COUNT; ++c )
		m_pools[c].collect( completedEpoch );
}

bool Terrain3DGpuPageStore::buildDirectorySnapshot(
	PackedByteArray &bytes,
	uint64_t		&generation ) const {
	if ( !m_active )
		return false;
	bytes.resize( DIRECTORY_BYTES );
	if ( bytes.size() != DIRECTORY_BYTES )
		return false;
	memcpy( bytes.ptrw(), m_directory, DIRECTORY_BYTES );
	generation = m_publicationGeneration;
	return true;
}

const t3dGpuChannelDirectoryEntry_t &Terrain3DGpuPageStore::directoryEntry(
	uint16_t region,
	uint8_t	 channel ) const {
	static const t3dGpuChannelDirectoryEntry_t missing;
	if ( region >= REGION_COUNT || channel >= CHANNEL_COUNT )
		return missing;
	return m_directory[( uint32_t ) region * CHANNEL_COUNT + channel];
}

void Terrain3DGpuPageStore::destroyCold(
	Terrain3DRdTextureManager &manager ) {
	for ( uint32_t c = 0u; c < CHANNEL_COUNT; ++c )
		m_pools[c].destroyCold( manager );
	for ( uint32_t i = 0u; i < REGION_COUNT * CHANNEL_COUNT; ++i ) {
		m_logical[i] = Logical{};
		m_directory[i] = t3dGpuChannelDirectoryEntry_t{};
	}
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	m_content.reset();
	m_active = false;
}
void Terrain3DGpuPageStore::abandonDevice() {
	for ( uint32_t c = 0u; c < CHANNEL_COUNT; ++c )
		m_pools[c].abandonDevice();
	for ( uint32_t i = 0u; i < REGION_COUNT * CHANNEL_COUNT; ++i ) {
		m_logical[i] = Logical{};
		m_directory[i] = t3dGpuChannelDirectoryEntry_t{};
	}
	m_publicationGeneration = t3dNextGeneration( m_publicationGeneration );
	m_content.reset();
	m_active = false;
}
uint64_t Terrain3DGpuPageStore::physicalBytes() const {
	uint64_t total = 0u;
	for ( uint32_t c = 0u; c < CHANNEL_COUNT; ++c )
		total += m_pools[c].allocatedBytes();
	return total;
}

bool Terrain3DGpuChannelDirectoryBuffer::initializeCold(
	Terrain3DRdContext &context ) {
	if ( !Terrain3DRdContext::isOnRenderThread() )
		return false;
	for ( uint32_t i = 0u; i < BANK_COUNT; ++i ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.GpuPageChannelDirectory";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
		spec.m_growth = T3D_GPU_EXACT;
		spec.m_nMinimumCapacityBytes = Terrain3DGpuPageStore::DIRECTORY_BYTES;
		if ( !m_buffers[i].configure( spec ) ||
			 !m_buffers[i].createTracked( context, Terrain3DGpuPageStore::DIRECTORY_BYTES ) ) {
			releaseCold( context );
			return false;
		}
	}
	return true;
}
bool Terrain3DGpuChannelDirectoryBuffer::uploadCold(
	Terrain3DRdContext			&context,
	const Terrain3DGpuPageStore &store,
	uint64_t					 confirmedCompletedEpoch ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_buffers[0].isValid() )
		return false;
	if ( store.publicationGeneration() == m_uploadedGeneration )
		return true;
	uint32_t chosen = BANK_COUNT;
	for ( uint32_t i = 0u; i < BANK_COUNT; ++i ) {
		if ( i != m_publishedBank && m_buffers[i].isValid() &&
			 t3dGpuPageCanRetire( m_readerEpoch[i], confirmedCompletedEpoch ) ) {
			chosen = i;
			break;
		}
	}
	if ( chosen == BANK_COUNT )
		return false;
	uint64_t generation = 0u;
	if ( !store.buildDirectorySnapshot( m_scratch, generation ) )
		return false;
	PackedByteArray fallback;
	if ( !m_buffers[chosen].writeTracked( context,
										  0u,
										  m_scratch.ptr(),
										  Terrain3DGpuPageStore::DIRECTORY_BYTES,
										  fallback,
										  T3D_GPU_DIRTY_UPLOAD ) )
		return false;
	m_readerEpoch[chosen] = UINT64_MAX; // unpublished until safely acquired by the next frame
	m_uploadedGeneration = generation;
	m_publishedBank = chosen;
	return true;
}
const RID &Terrain3DGpuChannelDirectoryBuffer::rid() const {
	static RID invalid;
	return m_publishedBank < BANK_COUNT ? m_buffers[m_publishedBank].rid() : invalid;
}
bool Terrain3DGpuChannelDirectoryBuffer::markReader(
	uint64_t generation,
	uint64_t submissionEpoch ) {
	if ( generation != m_uploadedGeneration || m_publishedBank >= BANK_COUNT || !submissionEpoch )
		return false;
	uint64_t &epoch = m_readerEpoch[m_publishedBank];
	if ( epoch == UINT64_MAX || submissionEpoch > epoch )
		epoch = submissionEpoch;
	return true;
}
void Terrain3DGpuChannelDirectoryBuffer::releaseCold(
	Terrain3DRdContext &context ) {
	for ( uint32_t i = 0u; i < BANK_COUNT; ++i ) {
		if ( m_buffers[i].isValid() )
			m_buffers[i].releaseTracked( context );
		m_readerEpoch[i] = 0u;
	}
	m_publishedBank = BANK_COUNT;
	m_uploadedGeneration = 0u;
}
