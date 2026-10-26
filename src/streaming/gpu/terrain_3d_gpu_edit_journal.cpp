#include "streaming/gpu/terrain_3d_gpu_edit_journal.h"
#include "core/utility/terrain_3d_generation.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

uint8_t Terrain3DGpuEditJournal::invalidationsFor(
	uint8_t channel ) {
	if ( channel == T3D_GPU_PAGE_HEIGHT )
		return uint8_t( ( 1u << T3D_GPU_PAGE_DERIVED ) | ( 1u << T3D_GPU_PAGE_COMPOSITE ) |
						( 1u << T3D_GPU_PAGE_SHADOW ) );
	if ( channel == T3D_GPU_PAGE_CONTROL || channel == T3D_GPU_PAGE_COLOR )
		return uint8_t( 1u << T3D_GPU_PAGE_COMPOSITE );
	return 0u;
}

bool Terrain3DGpuEditJournal::validRect(
	uint32_t tw,
	uint32_t th,
	uint32_t x,
	uint32_t y,
	uint32_t w,
	uint32_t h ) {
	return tw > 0u && th > 0u && tw <= 255u && th <= 255u && w > 0u && h > 0u && x <= 255u && y <= 255u && w <= 255u &&
		   h <= 255u && x <= tw && y <= th && w <= tw - x && h <= th - y;
}

bool Terrain3DGpuEditJournal::begin(
	Terrain3DGpuPageStore	  &store,
	Terrain3DRdTextureManager &manager,
	Terrain3DRdContext		  &context,
	uint16_t				   region,
	uint8_t					   channel,
	uint32_t				   x,
	uint32_t				   y,
	uint32_t				   width,
	uint32_t				   height,
	uint64_t				   submissionEpoch ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !store.active() || m_pending || !submissionEpoch ||
		 channel >= T3D_GPU_PAGE_CHANNEL_COUNT )
		return false;
	const auto &config = store.pool( channel ).configuration();
	// Exact pixel edits supported for one-mip R16 heights and R32 controls.
	if ( ( channel != T3D_GPU_PAGE_HEIGHT && channel != T3D_GPU_PAGE_CONTROL ) || config.mipmaps != 1u ||
		 !validRect( config.width, config.height, x, y, width, height ) )
		return false;
	t3dGpuPageLogicalKey_t key{ region, channel, 0u };
	t3dGpuPageTicket_t	   previous;
	t3dGpuPageHandle_t	   oldHandle;
	if ( !store.publishedSnapshot( key, previous, oldHandle ) || previous.m_nEditGeneration == UINT32_MAX ||
		 previous.m_nPublicationGeneration == UINT64_MAX )
		return false;
	t3dGpuPageTicket_t ticket = previous;
	ticket.m_nEditGeneration = t3dNextGeneration( previous.m_nEditGeneration );
	ticket.m_nPublicationGeneration = previous.m_nPublicationGeneration + 1u;
	if ( !store.retainHistoryHandle( oldHandle ) )
		return false;
	if ( !store.stageCopyForEdit( manager, context, ticket, submissionEpoch ) ) {
		store.releaseHistoryHandle( oldHandle );
		return false;
	}
	t3dGpuPageHandle_t fresh;
	if ( !store.stagedSnapshot( ticket, fresh ) ) {
		store.cancelStaged( key );
		store.releaseHistoryHandle( oldHandle );
		return false;
	}
	m_active = Edit{};
	m_active.ticket = ticket;
	m_active.before = oldHandle;
	m_active.after = fresh;
	m_active.x = x;
	m_active.y = y;
	m_active.width = width;
	m_active.height = height;
	m_active.invalidatedChannels = invalidationsFor( channel );
	m_submissionEpoch = submissionEpoch;
	m_pending = true;
	m_dispatched = false;
	return true;
}

bool Terrain3DGpuEditJournal::dispatch(
	Terrain3DGpuPageStore		   &store,
	Terrain3DRdContext			   &context,
	Terrain3DPageCompute		   &compute,
	Terrain3DPageCompute::Operation operation,
	const Color					   &value,
	uint64_t						submissionEpoch,
	uint32_t						packedControlBits ) {
	if ( !m_pending || m_dispatched || !submissionEpoch || submissionEpoch != m_submissionEpoch ||
		 ( operation != Terrain3DPageCompute::RECT_FILL && operation != Terrain3DPageCompute::RECT_ADD ) )
		return false;
	t3dGpuPageHandle_t staged;
	if ( !store.stagedSnapshot( m_active.ticket, staged ) ||
		 staged.m_nSlotGeneration != m_active.after.m_nSlotGeneration ||
		 staged.m_nPhysicalLayer != m_active.after.m_nPhysicalLayer )
		return false;
	const uint8_t channel = m_active.ticket.m_key.m_nChannel;
	if ( channel == T3D_GPU_PAGE_CONTROL && operation != Terrain3DPageCompute::RECT_FILL )
		return false;
	Terrain3DGpuPagePool &pool = store.pool( channel );
	const auto			 &config = pool.configuration();
	if ( !compute.ready() )
		return false;
	if ( !compute.dispatchRectHot( context,
								   pool.mutableTexture(),
								   pool.mutableTexture(),
								   staged.m_nPhysicalLayer,
								   m_active.before.m_nPhysicalLayer,
								   config.width,
								   config.height,
								   m_active.x,
								   m_active.y,
								   m_active.width,
								   m_active.height,
								   operation,
								   value,
								   submissionEpoch,
								   packedControlBits ) )
		return false;
	m_dispatched = true;
	return true;
}

void Terrain3DGpuEditJournal::release(
	Terrain3DGpuPageStore &store,
	Edit				  &edit ) {
	if ( edit.before.m_nPhysicalLayer != UINT16_MAX )
		store.releaseHistoryHandle( edit.before );
	if ( edit.after.m_nPhysicalLayer != UINT16_MAX )
		store.releaseHistoryHandle( edit.after );
	edit = Edit{};
}

bool Terrain3DGpuEditJournal::commit(
	Terrain3DGpuPageStore &store,
	uint64_t			   confirmedCompletedEpoch ) {
	if ( !m_pending || !m_dispatched || !confirmedCompletedEpoch || confirmedCompletedEpoch < m_submissionEpoch )
		return false;
	if ( !store.commitChannelEdit( m_active.ticket, confirmedCompletedEpoch ) )
		return false;
	if ( !store.retainHistoryHandle( m_active.after ) ) {
		// Even if the history budget refuses a slot, the GPU edit remains committed.
		store.releaseHistoryHandle( m_active.before );
		m_pending = false;
		m_dispatched = false;
		return false;
	}
	// Discard redo revisions before inserting the newly committed edit.
	for ( uint32_t i = m_cursor; i < m_count; ++i )
		release( store, m_history[i] );
	m_count = m_cursor;
	if ( m_count == HISTORY_LIMIT ) {
		release( store, m_history[0] );
		for ( uint32_t i = 1u; i < m_count; ++i )
			m_history[i - 1u] = m_history[i];
		--m_count;
	}
	m_history[m_count++] = m_active;
	m_cursor = m_count;
	m_lastInvalidations = m_active.invalidatedChannels;
	if ( m_dirtyLedger ) {
		m_dirtyLedger->invalidate( m_active.ticket );
		m_dirtyLedger->invalidateHeightHaloNeighbors( store,
													  m_active.ticket.m_key,
													  m_active.x == 0u,
													  m_active.y == 0u );
	}
	m_active = Edit{};
	m_pending = false;
	m_dispatched = false;
	return true;
}

bool Terrain3DGpuEditJournal::abort(
	Terrain3DGpuPageStore &store ) {
	if ( !m_pending || m_dispatched )
		return false; // GPU dispatch needs confirmed completion before cancellation.
	store.cancelStaged( m_active.ticket.m_key );
	store.releaseHistoryHandle( m_active.before );
	m_active = Edit{};
	m_pending = false;
	return true;
}

bool Terrain3DGpuEditJournal::abortCompleted(
	Terrain3DGpuPageStore &store,
	uint64_t			   completedEpoch ) {
	if ( !m_pending || !completedEpoch || completedEpoch < m_submissionEpoch )
		return false;
	// Caller confirms the GPU copy/dispatch has completed. The staged unpublished
	// page may now be retired without ever becoming visible.
	if ( !store.cancelStaged( m_active.ticket.m_key ) )
		return false;
	store.releaseHistoryHandle( m_active.before );
	m_active = Edit{};
	m_pending = false;
	m_dispatched = false;
	return true;
}

bool Terrain3DGpuEditJournal::restore(
	Terrain3DGpuPageStore &store,
	bool				   redo,
	uint64_t			   confirmedCompletedEpoch ) {
	if ( m_pending || !confirmedCompletedEpoch || ( redo ? m_cursor == m_count : m_cursor == 0u ) )
		return false;
	Edit			  &edit = m_history[redo ? m_cursor : m_cursor - 1u];
	const auto		  &key = edit.ticket.m_key;
	t3dGpuPageTicket_t current;
	t3dGpuPageHandle_t handle;
	if ( !store.publishedSnapshot( key, current, handle ) ||
		 current.m_nSourceGeneration != edit.ticket.m_nSourceGeneration || current.m_nEditGeneration == UINT32_MAX ||
		 current.m_nPublicationGeneration == UINT64_MAX )
		return false;
	const auto &expected = redo ? edit.before : edit.after;
	if ( handle.m_nPoolIndex != expected.m_nPoolIndex || handle.m_nPhysicalLayer != expected.m_nPhysicalLayer ||
		 handle.m_nSlotGeneration != expected.m_nSlotGeneration || handle.m_nDeviceEpoch != expected.m_nDeviceEpoch )
		return false;
	current.m_nEditGeneration = t3dNextGeneration( current.m_nEditGeneration );
	++current.m_nPublicationGeneration;
	if ( !store.restoreHistoryHandle( current, redo ? edit.after : edit.before, confirmedCompletedEpoch ) )
		return false;
	if ( redo )
		++m_cursor;
	else
		--m_cursor;
	m_lastInvalidations = edit.invalidatedChannels;
	if ( m_dirtyLedger ) {
		m_dirtyLedger->invalidate( current );
		m_dirtyLedger->invalidateHeightHaloNeighbors( store, current.m_key, edit.x == 0u, edit.y == 0u );
	}
	return true;
}

bool Terrain3DGpuEditJournal::undo(
	Terrain3DGpuPageStore &store,
	uint64_t			   epoch ) {
	return restore( store, false, epoch );
}
bool Terrain3DGpuEditJournal::redo(
	Terrain3DGpuPageStore &store,
	uint64_t			   epoch ) {
	return restore( store, true, epoch );
}
void Terrain3DGpuEditJournal::clear(
	Terrain3DGpuPageStore &store ) {
	if ( m_pending ) {
		// Only valid after no GPU edit dispatch is in flight.
		if ( m_dispatched )
			return;
		abort( store );
	}
	for ( uint32_t i = 0u; i < m_count; ++i )
		release( store, m_history[i] );
	m_count = 0u;
	m_cursor = 0u;
	m_lastInvalidations = 0u;
}
