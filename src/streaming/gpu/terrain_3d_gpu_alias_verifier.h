#ifndef TERRAIN3D_GPU_ALIAS_VERIFIER_H
#define TERRAIN3D_GPU_ALIAS_VERIFIER_H

#include "streaming/gpu/terrain_3d_gpu_page_store.h"
#include "rendering/rd/terrain_pages/terrain_3d_page_compute.h"

// Render-owner transaction. A candidate is shared only after every mip was
// compared on GPU and each 8-byte comparison result completed asynchronously.
// One transaction/compute instance at a time, no Image/full-page readback.
class Terrain3DGpuAliasVerifier
{
public:
	enum State : uint8_t
	{
		IDLE,
		READY_TO_DISPATCH,
		WAITING_RESULT
	};
	bool begin( Terrain3DGpuPageStore		 &store,
				const t3dGpuPageLogicalKey_t &key,
				uint64_t					  readerEpoch,
				uint16_t					  afterPhysicalLayer = UINT16_MAX );
	bool dispatch( Terrain3DRdContext	 &context,
				   Terrain3DGpuPageStore &store,
				   Terrain3DPageCompute	 &compute,
				   uint64_t				  submissionEpoch,
				   const Callable		 &callback );
	// If the async readback could not be scheduled during dispatch, retry it
	// without repeating the GPU comparison or mutating the candidate.
	bool retryReadback( Terrain3DRdContext &context, Terrain3DPageCompute &compute, const Callable &callback );
	// Bind the immutable request token to the async callback. Stale callbacks
	// from a cancelled probe or earlier mip can never validate a later probe.
	// `completedEpoch` comes from actual GPU completion, never a frame count.
	bool  accept( Terrain3DGpuPageStore &store,
				  Terrain3DPageCompute	&compute,
				  const PackedByteArray &result,
				  uint64_t				 completedEpoch,
				  uint64_t				 requestToken );
	void  cancel();		   // Safe only after the latest dispatch has GPU-completed.
	void  abandonDevice(); // Device loss invalidates all work and queued callbacks.
	State state() const {
		return m_state;
	}
	uint32_t mipIndex() const {
		return m_mip;
	}
	bool aliased() const {
		return m_aliased;
	}
	// Obtain before dispatch to bind the token into the callback arguments.
	uint64_t nextRequestToken() const {
		return m_state == READY_TO_DISPATCH && m_tokenSerial != UINT64_MAX ? m_tokenSerial + 1u : 0u;
	}
	uint64_t requestToken() const {
		return m_requestToken;
	}
	static bool validResult(
		const PackedByteArray &bytes ) {
		return bytes.size() == 8;
	}

private:
	Terrain3DGpuPageStore::AliasProbe m_probe{};
	State							  m_state = IDLE;
	uint64_t						  m_dispatchEpoch = 0u;
	uint32_t						  m_mip = 0u;
	uint32_t						  m_mipCount = 0u;
	bool							  m_aliased = false;
	bool							  m_readbackRequested = false;
	uint64_t						  m_requestToken = 0u;
	uint64_t						  m_tokenSerial = 0u;
};
#endif
