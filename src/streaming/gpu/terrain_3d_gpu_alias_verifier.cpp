#include "streaming/gpu/terrain_3d_gpu_alias_verifier.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

bool Terrain3DGpuAliasVerifier::begin(
	Terrain3DGpuPageStore		 &store,
	const t3dGpuPageLogicalKey_t &key,
	uint64_t					  readerEpoch,
	uint16_t					  after ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || m_state != IDLE || !store.active() ||
		 !store.beginAliasProbe( key, readerEpoch, m_probe, after ) )
		return false;
	const auto &pool = store.pool( key.m_nChannel );
	const auto &config = pool.configuration();
	if ( !config.storageWrites || !config.mipmaps || !pool.isPublished( m_probe.original ) ||
		 !pool.isPublished( m_probe.candidate ) ) {
		m_probe = Terrain3DGpuPageStore::AliasProbe{};
		return false;
	}
	m_mipCount = config.mipmaps;
	m_mip = 0u;
	m_dispatchEpoch = 0u;
	m_aliased = false;
	m_readbackRequested = false;
	m_requestToken = 0u;
	m_state = READY_TO_DISPATCH;
	return true;
}

bool Terrain3DGpuAliasVerifier::dispatch(
	Terrain3DRdContext	  &context,
	Terrain3DGpuPageStore &store,
	Terrain3DPageCompute  &compute,
	uint64_t			   submissionEpoch,
	const Callable		  &callback ) {
	if ( m_state != READY_TO_DISPATCH || !Terrain3DRdContext::isOnRenderThread() || !submissionEpoch ||
		 !callback.is_valid() || !store.active() || m_tokenSerial == UINT64_MAX ||
		 !store.snapshotCurrent( m_probe.ticket, m_probe.original ) )
		return false;
	Terrain3DGpuPagePool &pool = store.pool( m_probe.ticket.m_key.m_nChannel );
	if ( !pool.isPublished( m_probe.candidate ) || compute.format() != pool.configuration().format ||
		 !pool.pinUntil( m_probe.original, submissionEpoch ) || !pool.pinUntil( m_probe.candidate, submissionEpoch ) )
		return false;
	if ( !compute.dispatchCompareMipHot( context,
										 pool.mutableTexture(),
										 m_probe.original.m_nPhysicalLayer,
										 m_probe.candidate.m_nPhysicalLayer,
										 m_mip,
										 submissionEpoch ) )
		return false;
	m_dispatchEpoch = submissionEpoch;
	m_requestToken = ++m_tokenSerial;
	m_state = WAITING_RESULT;
	// Comparison is already submitted; a failed readback enqueue may be retried
	// without risking a second dispatch against a reused result buffer.
	m_readbackRequested = compute.requestResultAsync( context, callback );
	return true;
}

bool Terrain3DGpuAliasVerifier::retryReadback(
	Terrain3DRdContext	 &context,
	Terrain3DPageCompute &compute,
	const Callable		 &callback ) {
	if ( m_state != WAITING_RESULT || m_readbackRequested || !callback.is_valid() )
		return false;
	m_readbackRequested = compute.requestResultAsync( context, callback );
	return m_readbackRequested;
}

bool Terrain3DGpuAliasVerifier::accept(
	Terrain3DGpuPageStore &store,
	Terrain3DPageCompute  &compute,
	const PackedByteArray &result,
	uint64_t			   completedEpoch,
	uint64_t			   requestToken ) {
	if ( m_state != WAITING_RESULT || !m_readbackRequested || !requestToken || requestToken != m_requestToken ||
		 !completedEpoch || completedEpoch < m_dispatchEpoch || !Terrain3DRdContext::isOnRenderThread() )
		return false;
	compute.markCompletedCold( completedEpoch );
	m_readbackRequested = false;
	m_state = IDLE;
	if ( !validResult( result ) || !Terrain3DPageCompute::comparisonEqual( result ) ||
		 !store.snapshotCurrent( m_probe.ticket, m_probe.original ) ||
		 !store.pool( m_probe.ticket.m_key.m_nChannel ).isPublished( m_probe.candidate ) ) {
		cancel();
		return false;
	}
	++m_mip;
	if ( m_mip < m_mipCount ) {
		m_state = READY_TO_DISPATCH;
		return true;
	}
	m_aliased = store.finishGpuVerifiedAlias( m_probe, completedEpoch );
	m_probe = Terrain3DGpuPageStore::AliasProbe{};
	m_mip = m_mipCount = 0u;
	m_dispatchEpoch = 0u;
	m_requestToken = 0u;
	return m_aliased;
}

void Terrain3DGpuAliasVerifier::cancel() {
	if ( m_state == WAITING_RESULT )
		return;
	m_state = IDLE;
	m_probe = Terrain3DGpuPageStore::AliasProbe{};
	m_mip = m_mipCount = 0u;
	m_dispatchEpoch = 0u;
	m_aliased = false;
	m_readbackRequested = false;
	m_requestToken = 0u;
}

void Terrain3DGpuAliasVerifier::abandonDevice() {
	// Invalidate pending async callbacks even if their old device never finishes.
	if ( m_tokenSerial != UINT64_MAX )
		++m_tokenSerial;
	m_state = IDLE;
	m_probe = Terrain3DGpuPageStore::AliasProbe{};
	m_mip = m_mipCount = 0u;
	m_dispatchEpoch = 0u;
	m_requestToken = 0u;
	m_readbackRequested = false;
	m_aliased = false;
}
