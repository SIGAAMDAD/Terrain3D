#include "rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h"
#include "core/utility/terrain_3d_generation.h"
#include <string.h>
using namespace godot;

Terrain3DSharedVTRuntime::Terrain3DSharedVTRuntime() = default;
Terrain3DSharedVTRuntime::~Terrain3DSharedVTRuntime() { shutdown_cold(); }

bool Terrain3DSharedVTRuntime::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone ) {
    if ( pZone == NULL || ( m_pRuntimeAllocatorZone != NULL && m_pRuntimeAllocatorZone != pZone ) ) { return false; }
    m_pRuntimeAllocatorZone = pZone;
    return true;
}
bool Terrain3DSharedVTRuntime::bindVirtualTexture( Terrain3DVirtualTexture *const pVirtualTexture ) {
    if ( pVirtualTexture == NULL || m_bSharedResourcesInitialized || ( m_pVirtualTexture != NULL && m_pVirtualTexture != pVirtualTexture ) ) { return false; }
    m_pVirtualTexture = pVirtualTexture;
    return true;
}

bool Terrain3DSharedVTRuntime::refresh_shared_vt_bindings_hot() {
	if ( !m_Resources.is_initialized() || m_pVirtualTexture == NULL || !m_pVirtualTexture->is_initialized() ) {
		return false;
	}
	t3dSharedVTBindings_t next = {};
	next.hAllocationTable = m_Resources.get_buffer_rid( T3D_VT_GPU_BUFFER_ALLOCATION_TABLE );
	next.hPageTable = m_Resources.get_buffer_rid( T3D_VT_GPU_BUFFER_PAGE_TABLE );
	next.hMipLayoutTable = m_Resources.get_buffer_rid( T3D_VT_GPU_BUFFER_MIP_LAYOUT_TABLE );
	next.hTailMetadata = m_Resources.get_buffer_rid( T3D_VT_GPU_BUFFER_TAIL_METADATA );
	if ( !next.hAllocationTable.is_valid() || !next.hPageTable.is_valid() || !next.hMipLayoutTable.is_valid()
	        || !next.hTailMetadata.is_valid() ) {
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_VT_CHANNEL_COUNT; ++c ) {
		next.hTail[c] = m_Resources.get_vt_tail_rid( ( t3dVTResourceChannel_t )c );
		next.nBankCount[c] = m_Resources.get_vt_bank_count( ( t3dVTResourceChannel_t )c );
		if ( !next.hTail[c].is_valid() || next.nBankCount[c] == 0u || next.nBankCount[c] > T3D_VT_MAX_TEXTURE_BANKS ) {
			return false;
		}
		for ( uint32_t b = 0u; b < next.nBankCount[c]; ++b ) {
			next.hPhysical[c][b] = m_Resources.get_vt_bank_rid( ( t3dVTResourceChannel_t )c, b );
			next.nBankLayers[c][b] = m_Resources.get_vt_bank_layer_count( ( t3dVTResourceChannel_t )c, b );
			if ( !next.hPhysical[c][b].is_valid() || next.nBankLayers[c][b] == 0u ) {
				return false;
			}
		}
	}
	next.nAllocationCount = m_pVirtualTexture->getActiveAllocationCount();
	next.nVirtualPageCount = m_pVirtualTexture->getActiveVirtualPageCount();
	next.nVTPageTableGeneration = m_pVirtualTexture->get_page_table_generation();
	next.nResourceGeneration = m_Resources.get_stats().nResourceGeneration;
	const bool bResourceChanged = m_SharedVTBindings.nBindingGeneration == 0u
	        || m_SharedVTBindings.nResourceGeneration != next.nResourceGeneration
	        || m_SharedVTBindings.hAllocationTable != next.hAllocationTable || m_SharedVTBindings.hPageTable != next.hPageTable
	        || m_SharedVTBindings.hMipLayoutTable != next.hMipLayoutTable || m_SharedVTBindings.hTailMetadata != next.hTailMetadata;
	uint64_t nBindingGeneration = m_SharedVTBindings.nBindingGeneration;
	if ( bResourceChanged ) {
		nBindingGeneration = t3dNextGeneration( nBindingGeneration );
		if ( nBindingGeneration == 0u ) {
			nBindingGeneration = 1u;
		}
	}
	next.nBindingGeneration = nBindingGeneration;
	m_SharedVTBindings = next;
	return true;
}

bool Terrain3DSharedVTRuntime::prepare_shared_vt_cpu_cold(
        const t3dVTSharedResourceClass_t eResourceClass,
        const uint32_t nPreviewMaterialCapacity,
        const uint32_t nPreviewVirtualPageCapacity,
        const t3dWorldResourceLimits_t *const pProductionWorldLimits ) {
	if ( eResourceClass != T3D_VT_SHARED_RESOURCES_PREVIEW && eResourceClass != T3D_VT_SHARED_RESOURCES_PRODUCTION ) {
		return false;
	}
	if ( m_bSharedResourcesInitialized || m_pRuntimeAllocatorZone == NULL || m_pVirtualTexture == NULL ) {
		return false;
	}
	if ( m_ePreparingSharedResourceClass != T3D_VT_SHARED_RESOURCES_NONE && m_ePreparingSharedResourceClass != eResourceClass ) {
		return false;
	}
	if ( m_pVirtualTexture->is_cpu_ready() ) {
		return m_ePreparingSharedResourceClass == eResourceClass;
	}
	if ( eResourceClass == T3D_VT_SHARED_RESOURCES_PREVIEW ) {
		m_Limits = t3d_world_make_preview_limits( nPreviewMaterialCapacity, nPreviewVirtualPageCapacity );
		m_Profile = t3d_vt_make_preview_gpu_profile();
	} else {
		if ( pProductionWorldLimits == NULL ) {
			return false;
		}
		m_Limits = t3d_world_make_runtime_limits( *pProductionWorldLimits );
		m_Profile = t3d_vt_make_runtime_gpu_profile( *pProductionWorldLimits );
	}
	if ( !t3d_world_validate_limits( m_Limits ) || !t3d_vt_validate_gpu_profile( m_Profile ) ) {
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_Profile, 0, sizeof( m_Profile ) );
		return false;
	}
	t3dVTGpuResourcePlan_t Plan = {};
	const uint32_t nTailHeight = m_pVirtualTexture->getLoadedPackageTailAtlasHeightCold();
	if ( nTailHeight == 0u || !t3d_vt_compute_resource_plan( m_Limits, m_Profile, Plan, nTailHeight ) ) {
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_Profile, 0, sizeof( m_Profile ) );
		return false;
	}
	if (g_t3dMemoryAccount) {
		const auto domain = g_t3dMemoryAccount->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE];
		if (domain.limit) {
			const uint64_t available = domain.limit > domain.committed ? domain.limit - domain.committed : 0;
			if (!t3d_vt_fit_resource_plan_to_budget(m_Limits, m_Profile, available, Plan, nTailHeight)) {
				g_t3dMemoryAccount->recordDenial(Terrain3DMemoryAccount::VIRTUAL_TEXTURE);
				memset(&m_Limits, 0, sizeof(m_Limits)); memset(&m_Profile, 0, sizeof(m_Profile));
				return false;
			}
		}
	}
	m_ePreparingSharedResourceClass = eResourceClass;
	const uint32_t nGeneration = m_pVirtualTexture->get_startup_snapshot().m_nGeneration;
	if ( !m_pVirtualTexture->prepare_cpu_cold( m_Limits, Plan, nGeneration ) ) {
		m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_Profile, 0, sizeof( m_Profile ) );
		return false;
	}
	return true;
}

bool Terrain3DSharedVTRuntime::prepare_shared_vt_storage_cold( Terrain3DRdContext &rContext ) {
	while ( !m_pVirtualTexture->is_gpu_storage_ready() ) {
		if ( !service_shared_vt_storage_cold( rContext, UINT32_MAX ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DSharedVTRuntime::service_shared_vt_storage_cold( Terrain3DRdContext &rContext,
        const uint32_t nMaxWorkUnits ) {
	if ( m_bSharedResourcesInitialized || m_ePreparingSharedResourceClass == T3D_VT_SHARED_RESOURCES_NONE
	        || m_pVirtualTexture == NULL || !m_pVirtualTexture->is_cpu_ready() || nMaxWorkUnits == 0u ) {
		return false;
	}
	if ( m_pVirtualTexture->is_gpu_storage_ready() ) {
		return m_Resources.is_initialized();
	}
	if ( !rContext.initialize() || !rContext.validateDevice() ) {
		return false;
	}
	uint32_t nUnitsRemaining = nMaxWorkUnits;
	if ( !m_Resources.is_initialized() ) {
		if ( !m_Resources.is_initialize_pending_cold()
		        && !m_Resources.begin_initialize_cold( rContext, m_Limits, m_Profile,
		                m_pVirtualTexture->get_gpu().get_cpu_resource_plan().nVtTailAtlasHeight ) ) {
			return false;
		}
		if ( !m_Resources.service_initialize_cold( nUnitsRemaining ) ) {
			return false;
		}
		if ( !m_Resources.is_initialized() ) {
			return true;
		}
		if ( nMaxWorkUnits != UINT32_MAX ) {
			return true;
		}
	}
	if ( !m_pVirtualTexture->prepare_gpu_storage_cold( &m_Resources ) ) {
		m_Resources.shutdown_cold();
		m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_Profile, 0, sizeof( m_Profile ) );
		return false;
	}
	return true;
}

bool Terrain3DSharedVTRuntime::bind_shared_vt_executable_cold() {
	while ( !m_pVirtualTexture->is_gpu_ready() ) {
		if ( !service_shared_vt_executable_cold( UINT32_MAX ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DSharedVTRuntime::service_shared_vt_executable_cold(
        const uint32_t nMaxWorkUnits ) {
	if ( m_bSharedResourcesInitialized || m_pVirtualTexture == NULL || !m_pVirtualTexture->is_gpu_storage_ready()
	        || nMaxWorkUnits == 0u ) {
		return false;
	}
	if ( m_pVirtualTexture->is_gpu_ready() ) {
		return true;
	}
	if ( !m_pVirtualTexture->service_gpu_executable_cold( nMaxWorkUnits ) ) {
		m_Resources.shutdown_cold();
		m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_Profile, 0, sizeof( m_Profile ) );
		return false;
	}
	return true;
}

bool Terrain3DSharedVTRuntime::publish_shared_vt_cold() {
	while ( !m_bSharedResourcesInitialized ) {
		if ( !service_shared_vt_publication_cold( UINT64_MAX, UINT32_MAX ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DSharedVTRuntime::service_shared_vt_publication_cold(
        const uint64_t nMaxBytes, const uint32_t nMaxWorkUnits ) {
	if ( m_bSharedResourcesInitialized ) {
		return true;
	}
	if ( m_ePreparingSharedResourceClass == T3D_VT_SHARED_RESOURCES_NONE || m_pVirtualTexture == NULL
	        || !m_pVirtualTexture->is_gpu_ready() || nMaxBytes == 0u || nMaxWorkUnits == 0u ) {
		return false;
	}
	if ( !m_pVirtualTexture->is_initial_published()
	        && !m_pVirtualTexture->service_initial_publication_cold( nMaxBytes, nMaxWorkUnits ) ) {
		m_Resources.shutdown_cold();
		m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_Profile, 0, sizeof( m_Profile ) );
		return false;
	}
	if ( !m_pVirtualTexture->is_initial_published() ) {
		return true;
	}
	m_eSharedResourceClass = m_ePreparingSharedResourceClass;
	m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	m_bSharedResourcesInitialized = true;
	if ( !refresh_shared_vt_bindings_hot() ) {
		m_pVirtualTexture->shutdown_cold();
		m_Resources.shutdown_cold();
		m_bSharedResourcesInitialized = false;
		m_eSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
		memset( &m_SharedVTBindings, 0, sizeof( m_SharedVTBindings ) );
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_Profile, 0, sizeof( m_Profile ) );
		return false;
	}
	return true;
}

bool Terrain3DSharedVTRuntime::ensure_shared_resources_cold( Terrain3DRdContext &rContext,
        const t3dVTSharedResourceClass_t eResourceClass,
        const uint32_t nPreviewMaterialCapacity,
        const uint32_t nPreviewVirtualPageCapacity,
        const t3dWorldResourceLimits_t *const pProductionWorldLimits ) {
	if ( eResourceClass != T3D_VT_SHARED_RESOURCES_PREVIEW && eResourceClass != T3D_VT_SHARED_RESOURCES_PRODUCTION ) {
		return false;
	}
	if ( m_bSharedResourcesInitialized ) {
		const bool bClassCompatible = m_eSharedResourceClass == eResourceClass
		        || ( m_eSharedResourceClass == T3D_VT_SHARED_RESOURCES_PRODUCTION && eResourceClass == T3D_VT_SHARED_RESOURCES_PREVIEW );
		if ( !bClassCompatible || !m_Resources.is_initialized() || m_pVirtualTexture == NULL || !m_pVirtualTexture->is_initialized() ) {
			return false;
		}
		const bool bBindingsReady = m_SharedVTBindings.hAllocationTable.is_valid() && m_SharedVTBindings.hPageTable.is_valid()
		        && m_SharedVTBindings.hMipLayoutTable.is_valid() && m_SharedVTBindings.hTailMetadata.is_valid()
		        && m_SharedVTBindings.nResourceGeneration == m_Resources.get_stats().nResourceGeneration;
		if ( bBindingsReady ) {
			m_SharedVTBindings.nAllocationCount = m_pVirtualTexture->getActiveAllocationCount();
			m_SharedVTBindings.nVirtualPageCount = m_pVirtualTexture->getActiveVirtualPageCount();
			m_SharedVTBindings.nVTPageTableGeneration = m_pVirtualTexture->get_page_table_generation();
			return true;
		}
		return refresh_shared_vt_bindings_hot();
	}
	return ( m_pVirtualTexture->is_cpu_ready()
	                || prepare_shared_vt_cpu_cold(
	                        eResourceClass, nPreviewMaterialCapacity, nPreviewVirtualPageCapacity, pProductionWorldLimits ) )
	        && ( m_pVirtualTexture->is_gpu_storage_ready() || prepare_shared_vt_storage_cold( rContext ) )
	        && ( m_pVirtualTexture->is_gpu_ready() || bind_shared_vt_executable_cold() )
	        && publish_shared_vt_cold();
}

void Terrain3DSharedVTRuntime::shutdown_cold( const bool bPreserveTailStaging ) {
    if ( m_pVirtualTexture != NULL ) m_pVirtualTexture->shutdown_cold( bPreserveTailStaging );
    m_Resources.unseal_hot_runtime_cold();
    m_Resources.shutdown_cold();
    m_bSharedResourcesInitialized = false;
    m_eSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
    m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
    memset( &m_SharedVTBindings, 0, sizeof( m_SharedVTBindings ) );
    memset( &m_Limits, 0, sizeof( m_Limits ) );
    memset( &m_Profile, 0, sizeof( m_Profile ) );
}

bool Terrain3DSharedVTRuntime::stagePackageWarm( const void *const pBytes, const uint64_t nBytes ) {
	return Terrain3DRdContext::isOnRenderThread() && are_shared_resources_ready()
	        && m_pVirtualTexture->stagePackageWarm( pBytes, nBytes );
}

bool Terrain3DSharedVTRuntime::servicePackageRepublishWarm( const uint64_t nMaxBytes, const uint32_t nMaxWorkUnits ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !are_shared_resources_ready() ) { return false; }
	const bool bResult = m_pVirtualTexture->servicePackageRepublishWarm( nMaxBytes, nMaxWorkUnits );
	// Compatible publication changes table contents/counts, never resource handles.
	if ( !m_pVirtualTexture->isPackageRepublishPendingWarm() && m_pVirtualTexture->is_initialized() ) {
		m_SharedVTBindings.nAllocationCount = m_pVirtualTexture->getActiveAllocationCount();
		m_SharedVTBindings.nVirtualPageCount = m_pVirtualTexture->getActiveVirtualPageCount();
		m_SharedVTBindings.nVTPageTableGeneration = m_pVirtualTexture->get_page_table_generation();
	}
	return bResult;
}

void Terrain3DSharedVTRuntime::abortPackageRepublishWarm() {
	if ( Terrain3DRdContext::isOnRenderThread() && m_pVirtualTexture != NULL ) {
		m_pVirtualTexture->abortPackageRepublishWarm();
	}
}

bool Terrain3DSharedVTRuntime::are_shared_resources_ready() const {
	return m_bSharedResourcesInitialized && m_Resources.is_initialized() && m_pVirtualTexture != NULL
	        && m_pVirtualTexture->is_initialized();
}
t3dVTSharedResourceClass_t Terrain3DSharedVTRuntime::get_shared_resource_class() const {
	return m_eSharedResourceClass;
}
const t3dWorldLimits_t &Terrain3DSharedVTRuntime::get_limits() const {
	return m_Limits;
}
const Terrain3DVTResourceImage &Terrain3DSharedVTRuntime::get_resources() const {
	return m_Resources;
}
const t3dSharedVTBindings_t &Terrain3DSharedVTRuntime::get_shared_vt_bindings() const {
	return m_SharedVTBindings;
}
bool Terrain3DSharedVTRuntime::snapshot_shared_vt_bindings_hot( t3dSharedVTBindings_t &rBindings ) const {
	if ( !are_shared_resources_ready() ) {
		return false;
	}
	const t3dSharedVTBindings_t snapshot = m_SharedVTBindings;
	if ( !snapshot.hAllocationTable.is_valid() || !snapshot.hPageTable.is_valid() || !snapshot.hMipLayoutTable.is_valid()
	        || !snapshot.hTailMetadata.is_valid() || snapshot.nBindingGeneration == 0u
	        || snapshot.nResourceGeneration != m_Resources.get_stats().nResourceGeneration ) {
		return false;
	}
	for ( uint32_t channel = 0u; channel < T3D_VT_CHANNEL_COUNT; ++channel ) {
		if ( !snapshot.hTail[channel].is_valid() || snapshot.nBankCount[channel] != 1u || !snapshot.hPhysical[channel][0].is_valid()
		        || snapshot.nBankLayers[channel][0] == 0u ) {
			return false;
		}
	}
	rBindings = snapshot;
	return true;
}
