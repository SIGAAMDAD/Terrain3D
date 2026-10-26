// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_cache.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "core/utility/terrain_3d_generation.h"
#include "rendering/rd/core/terrain_3d_gpu_transfer_budget.h"

#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <new>

using namespace godot;

Terrain3DRdGlobalDescriptorCache::Terrain3DRdGlobalDescriptorCache(
	Terrain3DRdContext &rContext ) :
	m_context( rContext ) {
}

Terrain3DRdGlobalDescriptorCache::~Terrain3DRdGlobalDescriptorCache() {
	// Context shutdown is responsible for releasing tracked RIDs first.
	if ( m_pEntries ) {
		delete[] m_pEntries;
		m_pEntries = nullptr;
	}
}

t3dRdDescriptorStatus_t Terrain3DRdGlobalDescriptorCache::initializeCold(
	const uint32_t nSlots,
	const uint64_t nDeviceGeneration ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_context.getDevice() || nDeviceGeneration == 0u ||
		 nDeviceGeneration != m_context.getDeviceGeneration() ) {
		return T3D_RD_DESCRIPTOR_STALE_DEVICE;
	}
	if ( m_pEntries ) {
		if ( nSlots != m_nCapacity || ( m_nDeviceGeneration != 0u && nDeviceGeneration != m_nDeviceGeneration ) ) {
			return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
		}
		if ( !m_fenceBuffer.isValid() ) {
			t3dRdGpuBufferSpec_t fenceSpec{};
			fenceSpec.m_pName = "GlobalDescriptor.CompletionFence";
			fenceSpec.m_owner = T3D_RD_CONTEXT_OWNER_SHADER_CACHE;
			fenceSpec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
			fenceSpec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
			if ( !m_fenceBuffer.isConfigured() && !m_fenceBuffer.configure( fenceSpec ) ) {
				return T3D_RD_DESCRIPTOR_CREATE_FAILED;
			}
			if ( !m_fenceBuffer.createTracked( m_context, 4u ) ) {
				return T3D_RD_DESCRIPTOR_CREATE_FAILED;
			}
		}
		m_nDeviceGeneration = nDeviceGeneration;
		return T3D_RD_DESCRIPTOR_OK;
	}
	if ( nSlots == 0u || nSlots > T3D_RESOURCE_CAPACITY ) {
		return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	}
	m_pEntries = new ( std::nothrow ) t3dRdGlobalDescriptorEntry_t[nSlots];
	if ( !m_pEntries ) {
		return T3D_RD_DESCRIPTOR_CACHE_CAPACITY_EXCEEDED;
	}
	m_nCapacity = nSlots;
	m_nDeviceGeneration = nDeviceGeneration;
	// The fence is cold-owned and has no hot-path allocations. A refused
	// readback leaves descriptors retained, rather than assuming completion.
	t3dRdGpuBufferSpec_t fenceSpec{};
	fenceSpec.m_pName = "GlobalDescriptor.CompletionFence";
	fenceSpec.m_owner = T3D_RD_CONTEXT_OWNER_SHADER_CACHE;
	fenceSpec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
	fenceSpec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
	if ( !m_fenceBuffer.configure( fenceSpec ) || !m_fenceBuffer.createTracked( m_context, 4u ) ) {
		delete[] m_pEntries;
		m_pEntries = nullptr;
		m_nCapacity = 0u;
		m_nDeviceGeneration = 0u;
		return T3D_RD_DESCRIPTOR_CREATE_FAILED;
	}
	m_nCurrentEpoch = 1u;
	m_nLastObservedFrame = UINT64_MAX;
	m_bFenceRequired = false;
	m_bFencePending = false;
	m_diagnostics.m_nSlotsTotal = nSlots;
	return T3D_RD_DESCRIPTOR_OK;
}

void Terrain3DRdGlobalDescriptorCache::setCompletionCallbackCold(
	const Callable &rCallback ) {
	if ( Terrain3DRdContext::isOnRenderThread() && rCallback.is_valid() ) {
		m_fenceCallback = rCallback;
	}
}

void Terrain3DRdGlobalDescriptorCache::beginRenderFrameHot(
	const uint64_t nRenderFrame ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_pEntries ||
		m_nDeviceGeneration != m_context.getDeviceGeneration() ||
		m_nLastObservedFrame == nRenderFrame ) {
		return;
	}
	m_nLastObservedFrame = nRenderFrame;
	const uint64_t nPreviousEpoch = m_nCurrentEpoch;
	if ( m_nCurrentEpoch != UINT64_MAX ) {
		++m_nCurrentEpoch;
	}
	m_diagnostics.m_nActiveSubmissionEpoch = m_nCurrentEpoch;
	if ( !m_bFenceRequired || m_bFencePending || !m_fenceCallback.is_valid() ||
		!m_fenceBuffer.isValid() || !m_context.getDevice() ||
		nPreviousEpoch <= m_nCompletedEpoch ) {
		return;
	}
	t3dGpuTransferTicket_t transfer;
	if ( !Terrain3DGpuTransferBudget::get().reserve( T3D_GPU_TRANSFER_FEEDBACK,
		4u, 1u, transfer ) ) {
		return;
	}
	const uint64_t nTicket = ++m_nNextFenceTicket;
	const Callable callback = m_fenceCallback.bind( ( int64_t ) nPreviousEpoch,
		( int64_t ) m_nDeviceGeneration, ( int64_t ) nTicket );
	const Error err = m_context.getDevice()->buffer_get_data_async(
		m_fenceBuffer.rid(), callback, 0u, 4u );
	if ( err != OK ) {
		Terrain3DGpuTransferBudget::get().refund( transfer, 4u, 1u );
		++m_diagnostics.m_nFenceSubmissionFailures;
		return;
	}
	m_nFenceEpoch = nPreviousEpoch;
	m_nFenceTicket = nTicket;
	m_nFenceDeviceGeneration = m_nDeviceGeneration;
	m_bFencePending = true;
	m_diagnostics.m_bFencePending = true;
	m_bFenceRequired = false;
	++m_diagnostics.m_nFenceSubmissions;
}

void Terrain3DRdGlobalDescriptorCache::consumeCompletionReadbackOnRenderThread(
	const PackedByteArray &rBytes, const uint64_t nEpoch,
	const uint64_t nDeviceGeneration, const uint64_t nTicket ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_bFencePending ||
		m_nFenceTicket != nTicket || m_nFenceEpoch != nEpoch ||
		m_nFenceDeviceGeneration != nDeviceGeneration ||
		m_nDeviceGeneration != nDeviceGeneration ||
		m_context.getDeviceGeneration() != nDeviceGeneration ) {
		++m_diagnostics.m_nFenceStaleCallbacks;
		return;
	}
	m_bFencePending = false;
	m_diagnostics.m_bFencePending = false;
	if ( rBytes.size() != 4 ) {
		m_bFenceRequired = true;
		++m_diagnostics.m_nFenceReadbackFailures;
		return;
	}
	++m_diagnostics.m_nFenceCompletions;
	( void ) serviceRetirementsCold( nEpoch );
}

bool Terrain3DRdGlobalDescriptorCache::canRetire(
	const t3dRdGlobalDescriptorEntry_t &rEntry ) const {
	return rEntry.m_nLeases == 0u && rEntry.m_nLastUseEpoch <= m_nCompletedEpoch;
}

void Terrain3DRdGlobalDescriptorCache::markStale(
	t3dRdGlobalDescriptorEntry_t &rEntry ) {
	if ( rEntry.m_eState != ENTRY_LIVE ) {
		return;
	}
	rEntry.m_eState = rEntry.m_nLeases ? ENTRY_STALE : ENTRY_RETIRE_PENDING;
	++m_diagnostics.m_nRetirePending;
	// Do not submit a completion probe while a stale descriptor still has
	// consumer leases. Its last lease will request completion if necessary.
	if ( !rEntry.m_nLeases && rEntry.m_nLastUseEpoch > m_nCompletedEpoch ) {
		m_bFenceRequired = true;
	}
}

void Terrain3DRdGlobalDescriptorCache::clearEntry(
	const uint32_t nSlot,
	const bool	   bAbandon ) {
	t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[nSlot];
	if ( bAbandon ) {
		rEntry.m_set.abandon();
	}
	rEntry.m_recipe = t3dRdDescriptorRecipe_t{};
	rEntry.m_nFingerprint = 0u;
	rEntry.m_nLastUseEpoch = 0u;
	rEntry.m_nLeases = 0u;
	rEntry.m_nSlotGeneration = t3dNextGeneration( rEntry.m_nSlotGeneration );
	rEntry.m_eState = ENTRY_FREE;
}

bool Terrain3DRdGlobalDescriptorCache::releasePhysicalCold(
	const uint32_t nSlot ) {
	t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[nSlot];
	if ( rEntry.m_eState == ENTRY_FREE || !canRetire( rEntry ) ) {
		return false;
	}
	const bool bPending = rEntry.m_eState == ENTRY_STALE || rEntry.m_eState == ENTRY_RETIRE_PENDING;
	if ( rEntry.m_set.isValid() ) {
		rEntry.m_set.releaseTracked( m_context );
		++m_diagnostics.m_nPhysicalReleases;
	}
	if ( bPending ) {
		--m_diagnostics.m_nRetirePending;
	}
	--m_diagnostics.m_nSlotsLive;
	++m_diagnostics.m_nGpuSafeRetirements;
	clearEntry( nSlot, false );
	return true;
}

t3dRdDescriptorStatus_t Terrain3DRdGlobalDescriptorCache::acquireDescriptorCold(
	const t3dRdDescriptorRecipe_t &rRecipe,
	const t3dRdResourceOwner_t	   eOwner,
	t3dRdDescriptorLease_t		  &rLease ) {
	rLease = t3dRdDescriptorLease_t{};
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_pEntries || !m_context.getDevice() ) {
		return T3D_RD_DESCRIPTOR_STALE_DEVICE;
	}
	const t3dRdDescriptorStatus_t recipeStatus =
		t3dRdDescriptorRecipeValidate( rRecipe, T3D_RD_DESCRIPTOR_DEVICE_GLOBAL );
	if ( recipeStatus != T3D_RD_DESCRIPTOR_OK ) {
		return recipeStatus;
	}
	if ( rRecipe.m_device.m_nDeviceGeneration != m_nDeviceGeneration ||
		 m_context.getDeviceGeneration() != m_nDeviceGeneration ) {
		return T3D_RD_DESCRIPTOR_STALE_DEVICE;
	}
	if ( eOwner >= T3D_RD_CONTEXT_OWNER_COUNT ) {
		return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	}
	++m_diagnostics.m_szPerOwnerRequests[eOwner];
	const uint64_t nFingerprint = t3dRdDescriptorRecipeFingerprint( rRecipe );
	for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
		t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[i];
		if ( rEntry.m_eState != ENTRY_LIVE ||
			 !t3dRdDescriptorRecipesMatch( rRecipe, nFingerprint, rEntry.m_recipe, rEntry.m_nFingerprint ) ) {
			continue;
		}
		// Godot may invalidate a set when some other owner retired one of its
		// dependencies. Never return its stale RID on a cache hit.
		if ( !m_context.getDevice()->uniform_set_is_valid( rEntry.m_set.rid() ) ) {
			markStale( rEntry );
			++m_diagnostics.m_nInvalidDependencyRejects;
			break;
		}
		if ( rEntry.m_nLeases == UINT32_MAX ) {
			return T3D_RD_DESCRIPTOR_CACHE_CAPACITY_EXCEEDED;
		}
		++rEntry.m_nLeases;
		++m_diagnostics.m_nLeasesLive;
		++m_diagnostics.m_nCacheHits;
		rLease = { i, rEntry.m_nSlotGeneration, m_nDeviceGeneration };
		return T3D_RD_DESCRIPTOR_OK;
	}
	++m_diagnostics.m_nCacheMisses;
	uint32_t nSlot = T3D_RD_DESCRIPTOR_INVALID_SLOT;
	for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
		if ( m_pEntries[i].m_eState == ENTRY_FREE ) {
			nSlot = i;
			break;
		}
	}
	if ( nSlot == T3D_RD_DESCRIPTOR_INVALID_SLOT ) {
		// Reuse only an unleased, already GPU-complete slot. Do not evict a
		// shared/in-flight descriptor merely because the fixed table is full.
		for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
			if ( canRetire( m_pEntries[i] ) && releasePhysicalCold( i ) ) {
				nSlot = i;
				break;
			}
		}
	}
	if ( nSlot == T3D_RD_DESCRIPTOR_INVALID_SLOT ) {
		// Probe GPU completion only when global capacity is blocked by
		// unleased descriptors that cannot yet be reclaimed. Ordinary
		// release of reusable ENTRY_LIVE leases never requests a readback.
		for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
			const t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[i];
			if ( rEntry.m_eState != ENTRY_FREE && !rEntry.m_nLeases &&
				 rEntry.m_nLastUseEpoch > m_nCompletedEpoch ) {
				m_bFenceRequired = true;
				break;
			}
		}
		++m_diagnostics.m_nCapacityDenials;
		return T3D_RD_DESCRIPTOR_CACHE_CAPACITY_EXCEEDED;
	}
	if ( m_context.getAvailableResourceSlots() == 0u ) {
		++m_diagnostics.m_nCapacityDenials;
		return T3D_RD_DESCRIPTOR_RESOURCE_REGISTRY_EXHAUSTED;
	}
	// Only a miss constructs Godot RDUniform and TypedArray objects.
	TypedArray< Ref< RDUniform > > uniforms;
	for ( uint32_t i = 0u; i < rRecipe.m_nBindingCount; ++i ) {
		const t3dRdDescriptorBinding_t &rBinding = rRecipe.m_szBindings[i];
		Ref< RDUniform >				uniform;
		uniform.instantiate();
		uniform->set_binding( rBinding.m_nBinding );
		uniform->set_uniform_type( ( RenderingDevice::UniformType ) rBinding.m_nType );
		for ( uint32_t j = 0u; j < rBinding.m_nResourceCount; ++j ) {
			uniform->add_id( rRecipe.m_szResources[rBinding.m_nResourceFirst + j] );
		}
		uniforms.append( uniform );
	}
	t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[nSlot];
	if ( !rEntry.m_set.createTracked( m_context,
									  eOwner,
									  "GlobalDescriptor.UniformSet",
									  uniforms,
									  rRecipe.m_hShader,
									  rRecipe.m_nSetIndex ) ) {
		return T3D_RD_DESCRIPTOR_CREATE_FAILED;
	}
	if ( !m_context.getDevice()->uniform_set_is_valid( rEntry.m_set.rid() ) ) {
		rEntry.m_set.releaseTracked( m_context );
		++m_diagnostics.m_nInvalidDependencyRejects;
		return T3D_RD_DESCRIPTOR_INVALID_DEPENDENCY;
	}
	rEntry.m_recipe = rRecipe;
	rEntry.m_nFingerprint = nFingerprint;
	rEntry.m_nLastUseEpoch = 0u;
	rEntry.m_nLeases = 1u;
	rEntry.m_eState = ENTRY_LIVE;
	++m_diagnostics.m_nCreates;
	++m_diagnostics.m_nSlotsLive;
	++m_diagnostics.m_nLeasesLive;
	if ( m_diagnostics.m_nSlotsPeak < m_diagnostics.m_nSlotsLive ) {
		m_diagnostics.m_nSlotsPeak = m_diagnostics.m_nSlotsLive;
	}
	rLease = { nSlot, rEntry.m_nSlotGeneration, m_nDeviceGeneration };
	return T3D_RD_DESCRIPTOR_OK;
}

t3dRdDescriptorStatus_t Terrain3DRdGlobalDescriptorCache::resolveDescriptorHot(
	const t3dRdDescriptorLease_t &rLease,
	const uint64_t				  nSubmissionEpoch,
	RID							 &rRid ) {
	rRid = RID();
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_pEntries ||
		 m_context.getDeviceGeneration() != m_nDeviceGeneration ) {
		return T3D_RD_DESCRIPTOR_STALE_DEVICE;
	}
	if ( !nSubmissionEpoch || nSubmissionEpoch <= m_nCompletedEpoch ) {
		return T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	}
	if ( rLease.m_nSlot >= m_nCapacity || !t3dRdDescriptorLeaseMatches( rLease,
																		rLease.m_nSlot,
																		m_pEntries[rLease.m_nSlot].m_nSlotGeneration,
																		m_nDeviceGeneration ) ) {
		++m_diagnostics.m_nStaleLeaseRejects;
		return T3D_RD_DESCRIPTOR_STALE_LEASE;
	}
	t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[rLease.m_nSlot];
	if ( rEntry.m_eState != ENTRY_LIVE || !rEntry.m_nLeases ) {
		++m_diagnostics.m_nStaleLeaseRejects;
		return T3D_RD_DESCRIPTOR_STALE_LEASE;
	}
	if ( !m_context.getDevice() || !m_context.getDevice()->uniform_set_is_valid( rEntry.m_set.rid() ) ) {
		markStale( rEntry );
		++m_diagnostics.m_nInvalidDependencyRejects;
		return T3D_RD_DESCRIPTOR_INVALID_DEPENDENCY;
	}
	if ( nSubmissionEpoch > rEntry.m_nLastUseEpoch ) {
		rEntry.m_nLastUseEpoch = nSubmissionEpoch;
	}
	rRid = rEntry.m_set.rid();
	return T3D_RD_DESCRIPTOR_OK;
}

bool Terrain3DRdGlobalDescriptorCache::descriptorRecipeMatchesHot(
	const t3dRdDescriptorLease_t  &rLease,
	const t3dRdDescriptorRecipe_t &rRecipe,
	const uint64_t				   nFingerprint ) const {
	if ( !m_pEntries || rLease.m_nSlot >= m_nCapacity ||
		 !t3dRdDescriptorLeaseMatches( rLease,
									   rLease.m_nSlot,
									   m_pEntries[rLease.m_nSlot].m_nSlotGeneration,
									   m_nDeviceGeneration ) ) {
		return false;
	}
	const t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[rLease.m_nSlot];
	return rEntry.m_eState == ENTRY_LIVE && rEntry.m_nLeases != 0u &&
		   t3dRdDescriptorRecipesMatch( rRecipe, nFingerprint, rEntry.m_recipe, rEntry.m_nFingerprint );
}

t3dRdDescriptorStatus_t Terrain3DRdGlobalDescriptorCache::releaseDescriptorCold(
	t3dRdDescriptorLease_t &rLease ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_pEntries ) {
		return T3D_RD_DESCRIPTOR_STALE_DEVICE;
	}
	if ( rLease.m_nSlot >= m_nCapacity ||
		 !t3dRdDescriptorLeaseMatches( rLease,
									   rLease.m_nSlot,
									   m_pEntries[rLease.m_nSlot].m_nSlotGeneration,
									   m_nDeviceGeneration ) ||
		 !m_pEntries[rLease.m_nSlot].m_nLeases ) {
		++m_diagnostics.m_nStaleLeaseRejects;
		rLease = t3dRdDescriptorLease_t{};
		return T3D_RD_DESCRIPTOR_STALE_LEASE;
	}
	t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[rLease.m_nSlot];
	--rEntry.m_nLeases;
	--m_diagnostics.m_nLeasesLive;
	if ( rEntry.m_eState == ENTRY_STALE && !rEntry.m_nLeases ) {
		rEntry.m_eState = ENTRY_RETIRE_PENDING;
		if ( rEntry.m_nLastUseEpoch > m_nCompletedEpoch ) {
			m_bFenceRequired = true;
		}
	}
	const uint32_t nReleasedSlot = rLease.m_nSlot;
	rLease = t3dRdDescriptorLease_t{};
	// A previously completed epoch can be retired without another GPU probe.
	if ( rEntry.m_eState == ENTRY_RETIRE_PENDING && canRetire( rEntry ) ) {
		( void ) releasePhysicalCold( nReleasedSlot );
	}
	return T3D_RD_DESCRIPTOR_OK;
}

void Terrain3DRdGlobalDescriptorCache::invalidateDependencyCold(
	const RID	  &rRid,
	const uint64_t nDeviceGeneration ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_pEntries || !rRid.is_valid() ||
		 m_nDeviceGeneration != nDeviceGeneration ) {
		return;
	}
	for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
		t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[i];
		if ( rEntry.m_eState != ENTRY_LIVE ) {
			continue;
		}
		for ( uint32_t j = 0u; j < rEntry.m_recipe.m_nResourceCount; ++j ) {
			if ( rEntry.m_recipe.m_szResources[j] == rRid ) {
				markStale( rEntry );
				break;
			}
		}
	}
}

void Terrain3DRdGlobalDescriptorCache::invalidateShaderCold(
	const RID	  &rShader,
	const uint64_t nDeviceGeneration ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_pEntries || !rShader.is_valid() ||
		 m_nDeviceGeneration != nDeviceGeneration ) {
		return;
	}
	for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
		t3dRdGlobalDescriptorEntry_t &rEntry = m_pEntries[i];
		if ( rEntry.m_eState == ENTRY_LIVE && rEntry.m_recipe.m_hShader == rShader ) {
			markStale( rEntry );
		}
	}
}

uint32_t Terrain3DRdGlobalDescriptorCache::serviceRetirementsCold(
	const uint64_t nCompletedSubmissionEpoch ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_pEntries ) {
		return 0u;
	}
	// Only past epochs can have been completed by a queued readback.
	// The current epoch may still receive descriptor resolves and draws.
	if ( nCompletedSubmissionEpoch >= m_nCurrentEpoch ) {
		return 0u;
	}
	if ( nCompletedSubmissionEpoch > m_nCompletedEpoch ) {
		m_nCompletedEpoch = nCompletedSubmissionEpoch;
		m_diagnostics.m_nCompletedSubmissionEpoch = m_nCompletedEpoch;
	}
	uint32_t nRetired = 0u;
	for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
		const EntryState state = m_pEntries[i].m_eState;
		if ( ( state == ENTRY_STALE || state == ENTRY_RETIRE_PENDING ) && releasePhysicalCold( i ) ) {
			++nRetired;
		}
	}
	return nRetired;
}

void Terrain3DRdGlobalDescriptorCache::shutdownCold() {
	if ( !m_pEntries ) {
		return;
	}
	// Called before context clears its tracked resources; GPU quiescence is
	// supplied by the enclosing shutdown path, not assumed from a frame tick.
	for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
		if ( m_pEntries[i].m_eState == ENTRY_FREE ) {
			continue;
		}
		if ( m_pEntries[i].m_set.isValid() ) {
			m_pEntries[i].m_set.releaseTracked( m_context );
			++m_diagnostics.m_nPhysicalReleases;
		}
		clearEntry( i, false );
	}
	m_diagnostics.m_nLeasesLive = 0u;
	m_diagnostics.m_nSlotsLive = 0u;
	m_diagnostics.m_nRetirePending = 0u;
	m_nCompletedEpoch = 0u;
	m_nDeviceGeneration = 0u;
	m_nCurrentEpoch = 1u;
	m_nLastObservedFrame = UINT64_MAX;
	m_bFencePending = m_bFenceRequired = false;
	m_diagnostics.m_bFencePending = false;
	m_diagnostics.m_nActiveSubmissionEpoch = 0u;
	m_diagnostics.m_nCompletedSubmissionEpoch = 0u;
	m_fenceCallback = Callable();
	m_fenceBuffer.releaseTracked( m_context );
}

void Terrain3DRdGlobalDescriptorCache::abandonDeviceCold() {
	if ( !m_pEntries ) {
		return;
	}
	for ( uint32_t i = 0u; i < m_nCapacity; ++i ) {
		if ( m_pEntries[i].m_eState != ENTRY_FREE ) {
			clearEntry( i, true );
		}
	}
	m_diagnostics.m_nLeasesLive = 0u;
	m_diagnostics.m_nSlotsLive = 0u;
	m_diagnostics.m_nRetirePending = 0u;
	m_nCompletedEpoch = 0u;
	m_nDeviceGeneration = 0u;
	m_nCurrentEpoch = 1u;
	m_nLastObservedFrame = UINT64_MAX;
	m_bFencePending = m_bFenceRequired = false;
	m_diagnostics.m_bFencePending = false;
	m_diagnostics.m_nActiveSubmissionEpoch = 0u;
	m_diagnostics.m_nCompletedSubmissionEpoch = 0u;
	m_fenceCallback = Callable();
	m_fenceBuffer.abandon();
}
