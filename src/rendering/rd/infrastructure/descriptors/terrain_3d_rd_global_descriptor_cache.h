// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_GLOBAL_DESCRIPTOR_CACHE_H
#define TERRAIN3D_RD_GLOBAL_DESCRIPTOR_CACHE_H

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_recipe.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_uniform_set.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

class Terrain3DRdContext;

// One physical uniform-set owner for the shared/global RenderingDevice. All
// mutating methods and resolves run on the render thread. The cache owns a
// cold-allocated, fixed-size table and never allocates metadata during a draw.
class Terrain3DRdGlobalDescriptorCache
{
private:
	enum EntryState : uint8_t
	{
		ENTRY_FREE = 0u,
		ENTRY_LIVE,
		ENTRY_STALE,
		ENTRY_RETIRE_PENDING
	};

	typedef struct t3dRdGlobalDescriptorEntry_s
	{
		Terrain3DRdUniformSet	m_set;
		t3dRdDescriptorRecipe_t m_recipe;
		uint64_t				m_nFingerprint = 0u;
		uint64_t				m_nLastUseEpoch = 0u;
		uint32_t				m_nSlotGeneration = 1u;
		uint32_t				m_nLeases = 0u;
		EntryState				m_eState = ENTRY_FREE;
	} t3dRdGlobalDescriptorEntry_t;

	Terrain3DRdContext				  &m_context;
	t3dRdGlobalDescriptorEntry_t	  *m_pEntries = nullptr;
	uint32_t						   m_nCapacity = 0u;
	uint64_t						   m_nDeviceGeneration = 0u;
	uint64_t                           m_nCompletedEpoch = 0u;
	uint64_t                           m_nCurrentEpoch = 1u;
	uint64_t                           m_nLastObservedFrame = UINT64_MAX;
	uint64_t                           m_nFenceEpoch = 0u;
	uint64_t                           m_nFenceTicket = 0u;
	uint64_t                           m_nNextFenceTicket = 0u;
	uint64_t                           m_nFenceDeviceGeneration = 0u;
	bool                               m_bFencePending = false;
	bool                               m_bFenceRequired = false;
	Terrain3DRdGpuBuffer               m_fenceBuffer;
	godot::Callable                    m_fenceCallback;
	t3dRdGlobalDescriptorDiagnostics_t m_diagnostics{};

	void clearEntry( uint32_t nSlot, bool bAbandon );
	bool canRetire( const t3dRdGlobalDescriptorEntry_t &rEntry ) const;
	void markStale( t3dRdGlobalDescriptorEntry_t &rEntry );
	bool releasePhysicalCold( uint32_t nSlot );

public:
	explicit Terrain3DRdGlobalDescriptorCache( Terrain3DRdContext &rContext );
	~Terrain3DRdGlobalDescriptorCache();
	Terrain3DRdGlobalDescriptorCache( const Terrain3DRdGlobalDescriptorCache & ) = delete;
	Terrain3DRdGlobalDescriptorCache &operator=( const Terrain3DRdGlobalDescriptorCache & ) = delete;

	// A single cold allocation; capacity can change only after shutdown.
	t3dRdDescriptorStatus_t initializeCold( uint32_t nSlots, uint64_t nDeviceGeneration );
	bool					isInitialized() const {
		   return m_pEntries != nullptr;
	}
	// Enqueued after the PREVIOUS frame's draw submissions, using a 4-byte
	// async readback as a GPU completion token. Never treat a CPU frame or
	// RenderingServer::force_sync() as proof of GPU completion.
	bool hasCompletionCallbackHot() const { return m_fenceCallback.is_valid(); }
	void setCompletionCallbackCold( const godot::Callable &rCallback );
	void beginRenderFrameHot( uint64_t nRenderFrame );
	uint64_t currentSubmissionEpochHot() const { return m_nCurrentEpoch; }
	void consumeCompletionReadbackOnRenderThread( const godot::PackedByteArray &rBytes,
		uint64_t nEpoch, uint64_t nDeviceGeneration, uint64_t nTicket );
	uint32_t capacity() const {
		return m_nCapacity;
	}

	// Acquires an independent lease for every caller, including cache hits.
	t3dRdDescriptorStatus_t acquireDescriptorCold( const t3dRdDescriptorRecipe_t &rRecipe,
												   t3dRdResourceOwner_t			  eOwner,
												   t3dRdDescriptorLease_t		 &rLease );
	// A hot resolve records the submission epoch of the binding. The caller must
	// supply the epoch of the actual draw/dispatch, not a completed GPU epoch.
	t3dRdDescriptorStatus_t
	resolveDescriptorHot( const t3dRdDescriptorLease_t &rLease, uint64_t nSubmissionEpoch, godot::RID &rRid );
	// Exact recipe comparison against the canonical recipe owned by this service.
	// Allows consumers to retain a small lease without duplicating recipe arrays.
	bool					descriptorRecipeMatchesHot( const t3dRdDescriptorLease_t  &rLease,
														const t3dRdDescriptorRecipe_t &rRecipe,
														uint64_t					   nFingerprint ) const;
	t3dRdDescriptorStatus_t releaseDescriptorCold( t3dRdDescriptorLease_t &rLease );
	void					invalidateDependencyCold( const godot::RID &rRid, uint64_t nDeviceGeneration );
	void					invalidateShaderCold( const godot::RID &rShader, uint64_t nDeviceGeneration );
	// Caller MUST supply an epoch confirmed complete by the GPU. Merely ending
	// a render callback is not completion. Zero retires only never-submitted sets.
	uint32_t serviceRetirementsCold( uint64_t nCompletedSubmissionEpoch );
	// Called by the context before its generic RID registry drain. Requires the
	// enclosing renderer shutdown/quiesce protocol to have made GPU use safe.
	void shutdownCold();
	// On device loss the old RenderingDevice owns no usable RID namespace.
	// Abandon without calling free_rid on the lost device.
	void abandonDeviceCold();

	const t3dRdGlobalDescriptorDiagnostics_t &diagnostics() const {
		return m_diagnostics;
	}
};

#endif // TERRAIN3D_RD_GLOBAL_DESCRIPTOR_CACHE_H
