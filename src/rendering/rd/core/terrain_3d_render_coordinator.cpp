#include "diagnostics/core/terrain_3d_streaming_profiler.h"
#include "rendering/rd/core/terrain_3d_gpu_transfer_budget.h"
#include "memory/policy/terrain_3d_memory_account.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#if defined( T3D_EDITOR_NATIVE )
#include "authoring/vt/integration/terrain_3d_render_authoring_companion.h"
#include "authoring/vt/integration/terrain_3d_composed_surface_coordinator_hooks.h"
#include "authoring/vt/integration/terrain_3d_composed_surface_editor_cook.h"
#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "authoring/vt/build/terrain_3d_vt_demand_cooker.h"
#include <godot_cpp/core/memory.hpp>
#endif
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "core/utility/logger.h"
#include "data/model/terrain_3d_data.h"
#include "data/model/terrain_3d_region.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "instances/physics/terrain_3d_instance_occlusion.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"
#include "memory/core/terrain_3d_memory.h"
#include "rendering/material/terrain_3d_material.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <math.h>
#include <string.h>

static Terrain3DHLODStreamer *t3dRdGetInstanceHlodStreamer(
	Terrain3D *const pTerrain ) {
	return pTerrain ? pTerrain->getHlodStreamer() : NULL;
}

static bool t3dRdShouldReportInstanceFailure(
	const uint64_t nFailureCount ) {
	return nFailureCount <= 4u || ( nFailureCount & ( nFailureCount - 1u ) ) == 0u;
}

static uint64_t t3dRdStartupTicksUsec(
	void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_usec() : 0u;
}

const char *Terrain3DRenderCoordinator::startupGpuStageName(
	const t3dStartupGpuStage_t eStage ) {
	switch ( eStage ) {
	case T3D_STARTUP_GPU_VT_STORAGE:
		return "VTStorage";
	case T3D_STARTUP_GPU_VT_EXECUTABLE:
		return "VTExecutable";
	case T3D_STARTUP_GPU_VT_PUBLICATION:
		return "VTPublication";
	case T3D_STARTUP_GPU_SHADER_PREWARM:
		return "ShaderPrewarm";
	case T3D_STARTUP_GPU_TERRAIN_RENDERER:
		return "TerrainRenderer";
	case T3D_STARTUP_GPU_WORLD_RUNTIME:
		return "WorldRuntime";
	case T3D_STARTUP_GPU_INSTANCE_RENDERER:
		return "InstanceRenderer";
	case T3D_STARTUP_GPU_OCEAN_RENDERER:
		return "OceanRenderer";
	case T3D_STARTUP_GPU_COMPLETE:
		return "Complete";
	case T3D_STARTUP_GPU_FAILED:
		return "Failed";
	default:
		return "Unknown";
	}
}

#if defined( T3D_EDITOR_NATIVE )
static bool t3dRdPrepareComposedShippingV1Cold(
    Terrain3DOwner *const pTerrain,
    Terrain3DRenderAuthoringCompanion &rAuthoring,
    const PackedByteArray &rComposed,
    const uint32_t nRevision,
    String &rOutputPath,
    PackedByteArray &rShipping,
    String &rDemandPath,
    PackedByteArray &rDemand,
    String &rV1Path,
    PackedByteArray &rV1,
    String &rError ) {
    Terrain3DData *const pData = pTerrain ? pTerrain->dataObject() : NULL;
    if ( pData == NULL || rComposed.is_empty() ) {
        rError = "Approved composed V1 package has no terrain source";
        return false;
    }
    Terrain3DMemoryAccount *const pAccount = rAuthoring.m_cooker.getAuthoringMemoryAccountCold();
    Terrain3DMemoryAccountScope authoring_scope( pAccount );
    PackedByteArray base;
    uint32_t nBaseGeneration = 0u;
    uint32_t nMaterialCount = 0u;
    if ( !pData->publishRuntimeVirtualTexturesCold( rOutputPath, nBaseGeneration,
            nMaterialCount, rError, &base, pAccount ) ) {
        return false;
    }
    const PackedByteArray fragments[2] = { base, rComposed };
    const uint32_t nGeneration = t3dNextGeneration( nBaseGeneration ^ nRevision );
    if ( !t3d_vt_godot_merge_packages_v1( fragments, 2u, nGeneration,
            rShipping, t3dMemoryCookingLimit( pAccount ) ) ) {
        rError = "Composed V1 pages could not merge into the shipping package";
        return false;
    }
    const Ref< Terrain3DAssets > assets = pTerrain->assetsResource();
    if ( assets.is_valid() && !assets->get_path().is_empty() &&
         !t3d_vt_demand_cook_asset_sidecar_cold( *pData, assets, rShipping,
            rDemand, rDemandPath, rError, &rV1, &rV1Path ) ) {
        return false;
    }
    return true;
}

bool Terrain3DRenderCoordinator::cookComposedSurfaceSidecarEditorCold(
    const t3dCs14EditorCookRequest_t &request, t3dCs14EditorCookResult_t &result )
{
    // The editor owner supplies the edit-serialization commit hooks. The
    // source pack and control-map snapshots must remain pinned during the cook.
    if ( !t3d_cs14_editor_cook_cold( m_composedSurfaceEdits, request, result ) ) {
        return false;
    }
    // Candidate/reference cooks cannot supply production V1 allocations.
    // The canonical parity acceptance bit is the existing CS13/CS14 contract,
    // established by the caller under the editor commit lock.
    if ( !m_pAuthoring || request.m_cook.m_bReferenceOnly ||
         !request.m_cook.m_source.m_bCanonicalMaterialParityAccepted ) {
        return true;
    }
    PackedByteArray composed;
    String error;
    const uint32_t generation = t3dNextGeneration( ( uint32_t ) result.m_nClaimRevision );
    if ( !t3d_vt_godot_import_composed_sidecar_v1(
            request.m_cook.m_pOutput, result.m_cook.m_nWrittenBytes,
            result.m_cook.m_nSourceFingerprint, generation, composed, &error ) ) {
        m_pAuthoring->m_approvedComposedV1Fragment.clear();
        m_pAuthoring->m_nApprovedComposedFingerprint = 0u;
        m_pAuthoring->m_approvedComposedStatus = "v1_import_failed: " + error;
        requestMegaVTPackageRecomposeCold();
        return true; // The CS14 sidecar transaction was already committed.
    }
    const auto &stats = m_composedSurfaceEdits.statsCold();
    const uint64_t nEditStamp =
        stats.m_nEditEvents + stats.m_nSourceInvalidations + stats.m_nRemovedRegions;
    const uint64_t nSourceGeneration = m_nComposedSourceGeneration;
    const uint64_t nTopologyGeneration = m_terrainMegaVT.getTopologyGeneration();
    String shippingPath, demandPath, v1Path;
    PackedByteArray shipping, demand, v1;
    if ( !t3dRdPrepareComposedShippingV1Cold( m_pTerrain, *m_pAuthoring,
            composed, generation, shippingPath, shipping, demandPath,
            demand, v1Path, v1, error ) ) {
        m_pAuthoring->m_approvedComposedV1Fragment.clear();
        m_pAuthoring->m_nApprovedComposedFingerprint = 0u;
        m_pAuthoring->m_approvedComposedStatus = "v1_shipping_prepare_failed: " + error;
        requestMegaVTPackageRecomposeCold();
        return true;
    }
    const auto &currentStats = m_composedSurfaceEdits.statsCold();
    if ( nSourceGeneration != m_nComposedSourceGeneration ||
         nTopologyGeneration != m_terrainMegaVT.getTopologyGeneration() ||
         nEditStamp != currentStats.m_nEditEvents +
                       currentStats.m_nSourceInvalidations + currentStats.m_nRemovedRegions ) {
        m_pAuthoring->m_approvedComposedV1Fragment.clear();
        m_pAuthoring->m_nApprovedComposedFingerprint = 0u;
        m_pAuthoring->m_approvedComposedStatus = "v1_shipping_source_changed";
        requestMegaVTPackageRecomposeCold();
        return true;
    }
    if ( !t3d_vt_demand_publish_triplet_cold( shippingPath, shipping,
            demandPath, demand, v1Path, v1, error ) ) {
        m_pAuthoring->m_approvedComposedV1Fragment.clear();
        m_pAuthoring->m_nApprovedComposedFingerprint = 0u;
        m_pAuthoring->m_approvedComposedStatus = "v1_shipping_publish_failed: " + error;
        requestMegaVTPackageRecomposeCold();
        return true;
    }
    m_pAuthoring->m_approvedComposedV1Fragment = composed;
    m_pAuthoring->m_nApprovedComposedFingerprint = result.m_cook.m_nSourceFingerprint;
    m_pAuthoring->m_nApprovedComposedSourceGeneration = nSourceGeneration;
    m_pAuthoring->m_nApprovedComposedTopologyGeneration = nTopologyGeneration;
    m_pAuthoring->m_nApprovedComposedEditStamp = nEditStamp;
    m_pAuthoring->m_approvedComposedStatus = "v1_shipping_package_published";
    requestMegaVTPackageRecomposeCold();
    return true;
}
#endif

bool Terrain3DRenderCoordinator::bindRuntimeAllocator(
	Terrain3DMemoryZone *const		   pZone,
	const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone || !m_sharedVtRuntime.bindRuntimeAllocator( pZone ) ||
		 !m_terrainMegaVT.attachVirtualTextureCold( &m_sharedVtRuntime.virtualTexture() ) ||
		 !m_publicationBuilder.bindRuntimeAllocator( pZone, ePolicy ) ||
		 !m_previewService.bindRuntimeAllocator( previewServices(), pZone, ePolicy ) ) {
		return false;
	}
	m_pRuntimeAllocatorZone = pZone;
	m_eRuntimeAllocatorOverflowPolicy = ePolicy;
	m_bRuntimeAllocatorBound = true;
	return true;
}

bool Terrain3DRenderCoordinator::resolveVirtualTextureMaterialAllocationCold(
	const uint64_t nStableKey,
	uint32_t	  &rAllocationId ) const {
	return m_sharedVtRuntime.virtualTexture().findMaterialAllocationCold( nStableKey, rAllocationId );
}

bool Terrain3DRenderCoordinator::setVirtualTextureQualityProfile(
	const uint32_t nProfile ) {
	return m_sharedVtRuntime.virtualTexture().set_quality_profile( nProfile );
}

bool Terrain3DRenderCoordinator::loadVirtualTexturePackageCold(
	const void *const pPackageBytes,
	const uint64_t	  nPackageBytes ) {
	Terrain3DMemoryAccountScope		 memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( !m_bRuntimeAllocatorBound ||
		 ( ( renderStatus.m_nReadyMask & T3D_RENDER_READY_RESOURCE_CONSUMERS ) != 0u &&
		   ( m_sharedVtRuntime.get_shared_resource_class() != T3D_VT_SHARED_RESOURCES_NONE ||
			 !Terrain3DRdContext::isOnRenderThread() ) ) ) {
		return false;
	}
	return m_sharedVtRuntime.virtualTexture().loadPackageCold( pPackageBytes, nPackageBytes );
}

void Terrain3DRenderCoordinator::stageAssetVtDemandIndexCold(
	void ) {
	String path, status;
	if ( Terrain3DRdContext::isOnOwnerThread() ) {
		const Ref< Terrain3DAssets > assets = m_pTerrain ? m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
		String						 error;
		if ( assets.is_valid() && assets->resolveVtDemandIndexPathCold( path, &error ) ) {
			status = "path_resolved";
		} else {
			status = assets.is_null() ? "asset_missing" : error;
		}
	} else {
		// A preview-to-production transition can cold-load on the render thread.
		// Use the owner-thread-resolved identity; never fetch a Godot Resource here.
		Terrain3DScopedLock lock( m_vtDemandIndexMutex );
		path = m_vtDemandIndexPath;
		status = m_vtDemandIndexStatus;
	}
	Terrain3DVirtualTexture &vt = m_sharedVtRuntime.virtualTexture();
	if ( !path.is_empty() ) {
		if ( !FileAccess::file_exists( path ) ) {
			status = "sidecar_missing";
			( void ) vt.stageDemandIndexCold( NULL, 0u, path );
		} else {
			const PackedByteArray bytes = FileAccess::get_file_as_bytes( path );
			if ( bytes.is_empty() || !vt.stageDemandIndexCold( bytes.ptr(), ( uint64_t ) bytes.size(), path ) ) {
				status = "sidecar_invalid_or_not_cold";
				( void ) vt.stageDemandIndexCold( NULL, 0u, path );
				UtilityFunctions::push_warning( "[Terrain3D][VT][Demand] Ignoring invalid or unstaged sidecar: ",
												path );
			} else {
				status = "staged";
			}
		}
	} else {
		( void ) vt.stageDemandIndexCold( NULL, 0u, String() );
	}
    // The GPU visibility sidecar is a separate sibling of canonical v2.
    // T04 reads its directory only after the final VT package is loaded;
    // the on-disk bundle payloads are exclusively owned by its worker.
    ( void ) vt.stageDemandV1Cold( path.is_empty() ? String() : path + String( ".v1" ), path );
	Terrain3DScopedLock lock( m_vtDemandIndexMutex );
	m_vtDemandIndexPath = path;
	m_vtDemandIndexStatus = status;
}

bool Terrain3DRenderCoordinator::loadRuntimeVirtualTexturePackageFileCold(
	String *const pError ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( m_pTerrain == NULL ) {
		if ( pError ) {
			*pError = "RD runtime VT package has no Terrain3D owner";
		}
		return false;
	}
	stageAssetVtDemandIndexCold();
	const String path = m_pTerrain->getDataDirectory().path_join( "terrain3d_instances.t3dvt" );
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		if ( pError ) {
			*pError = String( "RD runtime VT package is missing: " ) + path;
		}
		return false;
	}
	if ( !m_bRuntimeAllocatorBound || !m_sharedVtRuntime.virtualTexture().loadPackageFileCold( path ) ) {
		if ( pError ) {
			*pError = String( "RD runtime VT package failed validation/publication: " ) + path;
		}
		return false;
	}
	bool bStaged = false;
	{
		Terrain3DScopedLock lock( m_vtDemandIndexMutex );
		bStaged = m_vtDemandIndexStatus == "staged";
	}
	if ( bStaged && !m_sharedVtRuntime.virtualTexture().stagedDemandMatchesLoadedPackageCold() ) {
		// Do not retain an index for the wrong cooked VT package: fall back to
		// ordinary virtual-texture feedback until a matching sidecar is baked.
		( void ) m_sharedVtRuntime.virtualTexture().stageDemandIndexCold( NULL, 0u, path );
		Terrain3DScopedLock lock( m_vtDemandIndexMutex );
		m_vtDemandIndexStatus = "sidecar_package_mismatch";
		UtilityFunctions::push_warning( "[Terrain3D][VT][Demand] Package signature differs from sidecar: ",
										m_vtDemandIndexPath );
	}
	if ( pError ) {
		*pError = String();
	}
	return true;
}

Terrain3DRenderCoordinator::~Terrain3DRenderCoordinator(
	void ) {
	shutdown();
}

uint64_t Terrain3DRenderCoordinator::hashBytes(
	const void *const pData,
	const size_t	  nBytes ) const {
	return t3dHashFnv1a64( pData, nBytes, 1469598103934665603ULL );
}

uint64_t Terrain3DRenderCoordinator::advanceGeneration(
	const uint64_t nGeneration ) const {
	return t3dNextGeneration( nGeneration );
}

void Terrain3DRenderCoordinator::serviceColdCapacity(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_publicationBuilder.serviceColdCapacity( publicationServices() );
}

void Terrain3DRenderCoordinator::publishSnapshot(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_publicationBuilder.publish( publicationServices() );
}

const t3dRdFrameSnapshotSlot_t *Terrain3DRenderCoordinator::acquireSnapshotOnRenderThread(
	void ) {
	return m_publicationBuilder.acquireOnRenderThread();
}

void Terrain3DRenderCoordinator::resetSnapshotPublication(
	void ) {
	m_publicationBuilder.reset();
	m_materialStateBuilder.resetPublicationState();
}

void Terrain3DRenderCoordinator::importTerrainMegaVTPersistentStateHot(
	void ) {
	Terrain3DData *const pData = m_pTerrain ? m_pTerrain->dataObject() : NULL;
	if ( pData == NULL || !m_terrainMegaVT.isReady() ) {
		return;
	}
	for ( uint32_t iRegion = 0u; iRegion < ( uint32_t ) ( T3D_REGION_SLOTS ); ++iRegion ) {
		const t3dTerrainMegaVTRegion_t *const pMegaRegion = m_terrainMegaVT.getRegion( iRegion );
		if ( pMegaRegion == NULL || ( pMegaRegion->m_nFlags & T3D_TERRAIN_MEGAVT_REGION_PRESENT ) == 0u ) {
			continue;
		}
		int32_t nRegionX = 0;
		int32_t nRegionZ = 0;
		Terrain3DTerrainMegaVT::regionIndexToLocation( iRegion, nRegionX, nRegionZ );
		Terrain3DRegion *const pRegion = pData->getRegionPtr( Vector2i( nRegionX, nRegionZ ) );
		if ( pRegion == NULL || pRegion->isDeleted() ) {
			continue;
		}
		m_terrainMegaVT.importPersistentEditorStateHot( iRegion,
														( uint32_t ) ( pRegion->getMegaVTSourceGeneration() ),
														( uint32_t ) ( pRegion->getMegaVTDirtyMip0Mask() ) );
	}
}

void Terrain3DRenderCoordinator::persistTerrainMegaVTEditorStateHot(
	void ) {
	Terrain3DData *const pData = m_pTerrain ? m_pTerrain->dataObject() : NULL;
	if ( pData == NULL || !m_terrainMegaVT.isReady() ) {
		return;
	}
	for ( uint32_t iRegion = 0u; iRegion < ( uint32_t ) ( T3D_REGION_SLOTS ); ++iRegion ) {
		const t3dTerrainMegaVTRegion_t *const pMegaRegion = m_terrainMegaVT.getRegion( iRegion );
		if ( pMegaRegion == NULL || ( pMegaRegion->m_nFlags & T3D_TERRAIN_MEGAVT_REGION_PRESENT ) == 0u ) {
			continue;
		}
		uint32_t nSourceGeneration = 1u;
		uint32_t nDirtyMip0Mask = 0u;
		if ( !m_terrainMegaVT.getPersistentEditorState( iRegion, nSourceGeneration, nDirtyMip0Mask ) ) {
			continue;
		}
		int32_t nRegionX = 0;
		int32_t nRegionZ = 0;
		Terrain3DTerrainMegaVT::regionIndexToLocation( iRegion, nRegionX, nRegionZ );
		Terrain3DRegion *const pRegion = pData->getRegionPtr( Vector2i( nRegionX, nRegionZ ) );
		if ( pRegion == NULL || pRegion->isDeleted() ) {
			continue;
		}
		if ( ( uint32_t ) ( pRegion->getMegaVTSourceGeneration() ) != nSourceGeneration ||
			 ( uint32_t ) ( pRegion->getMegaVTDirtyMip0Mask() ) != nDirtyMip0Mask ) {
			pRegion->setMegaVTEditorStateInternal( nSourceGeneration, nDirtyMip0Mask );
		}
	}
}

void Terrain3DRenderCoordinator::onTerrainMapsEdited(
	const AABB &rEditedArea ) {
	if ( !m_terrainMegaVT.isReady() || rEditedArea.size.x <= 0.0f || rEditedArea.size.z <= 0.0f ) {
		return;
	}
	const Vector3				 editEnd = rEditedArea.get_end();
	t3dTerrainMegaVTEditBounds_t bounds;
	bounds.m_fMinWorldX = rEditedArea.position.x;
	bounds.m_fMinWorldZ = rEditedArea.position.z;
	bounds.m_fMaxWorldX = editEnd.x;
	bounds.m_fMaxWorldZ = editEnd.z;
	if ( m_terrainMegaVT.markEditorDirtyBoundsHot( bounds ) ) {
#if defined( T3D_EDITOR_NATIVE ) && defined( T3D_COMPOSED_SURFACE_EDITOR_EXPERIMENT )
		// CS02 material parity and live composed-page publication are not yet
		// accepted. Never scan all editor regions on ordinary brush updates
		// while composed authoring is inactive.
		t3dComposedSurfaceEditBounds_t composedBounds = {
			bounds.m_fMinWorldX, bounds.m_fMinWorldZ,
			bounds.m_fMaxWorldX, bounds.m_fMaxWorldZ
		};
		t3d_cs_editor_dirty_bounds( m_terrainMegaVT, m_composedSurfaceEdits,
							composedBounds, m_nComposedSourceGeneration );
#endif
		persistTerrainMegaVTEditorStateHot();
	}
}

bool Terrain3DRenderCoordinator::claimTerrainMegaVTIncrementalCook(
	const uint32_t							  nRegionMapIndex,
	t3dTerrainMegaVTIncrementalCookRequest_t &rRequest,
	t3dTerrainMegaVTCookTicket_t			 &rTicket ) {
	return m_terrainMegaVT.claimIncrementalCookHot( nRegionMapIndex, rRequest, rTicket );
}

bool Terrain3DRenderCoordinator::validateTerrainMegaVTIncrementalCookCompletion(
	const t3dTerrainMegaVTCookTicket_t &rTicket ) {
	return m_terrainMegaVT.validateIncrementalCookCompletionHot( rTicket );
}

bool Terrain3DRenderCoordinator::cancelTerrainMegaVTIncrementalCook(
	const t3dTerrainMegaVTCookTicket_t &rTicket ) {
	return m_terrainMegaVT.cancelIncrementalCookHot( rTicket );
}

bool Terrain3DRenderCoordinator::acceptTerrainMegaVTIncrementalCookCompletion(
	const t3dTerrainMegaVTCookTicket_t &rTicket ) {
	if ( !m_terrainMegaVT.acceptIncrementalCookCompletionHot( rTicket ) ) {
		return false;
	}
	persistTerrainMegaVTEditorStateHot();
	return true;
}

bool Terrain3DRenderCoordinator::attachForwardBridge(
	void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	m_bForwardBridgeAttached = pBridge && pBridge->attachCoordinator( this );
	return m_bForwardBridgeAttached && pBridge->isBridgeReady();
}

void Terrain3DRenderCoordinator::detachForwardBridge(
	void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	if ( m_bForwardBridgeAttached && pBridge ) {
		pBridge->detachCoordinator( this );
	}
	m_bForwardBridgeAttached = false;
}

bool Terrain3DRenderCoordinator::isForwardBridgeReady(
	void ) const {
	const Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	return m_bForwardBridgeAttached && pBridge && pBridge->isBridgeReady();
}

void Terrain3DRenderCoordinator::updateRenderRoute(
	void ) {
	Terrain3DMemoryAccountScope		 memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	const bool						 bBridgeReady = isForwardBridgeReady();
	const uint32_t					 nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool						 bStatusGenerationMatch =
		nStartupGeneration != 0u && renderStatus.m_nStartupGeneration == nStartupGeneration;
	const bool bShaderGpuReady =
		bStatusGenerationMatch && ( renderStatus.m_nReadyMask & T3D_RENDER_READY_SHADER_GPU ) != 0u;
	const bool bVtGpuReady =
		bStatusGenerationMatch &&
		( renderStatus.m_nReadyMask &
		  ( T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED ) ) ==
			( T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED );
	const bool bLifecycleRevisionMatch = renderStatus.m_nRecoveryRevision == m_nRecoveryRequestRevision;
	const bool bRenderLifecycleReady =
		bStatusGenerationMatch && bLifecycleRevisionMatch && renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY &&
		( renderStatus.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) == T3D_RENDER_READY_STARTUP_REQUIRED;
	const bool bOwnerActive = m_pTerrain != NULL && m_pTerrain->isInitialized();
	const bool bActivationCommitted = nStartupGeneration == 0u || m_bStartupActivationCommitted;
	m_bRenderEnabled.store( bOwnerActive && bActivationCommitted && bBridgeReady && bRenderLifecycleReady &&
								!m_bFailureLatched,
							T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( bOwnerActive && m_bCompositorDebugEnabled && bShaderGpuReady && bVtGpuReady &&
							   bRenderLifecycleReady && !m_bFailureLatched );
	}
	if ( !bBridgeReady ) {
		Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
		const String							   bridge_error =
			  pBridge ? pBridge->getLastError() : String( "Terrain3D Forward+ bridge adapter is unavailable" );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		if ( m_initializationError.is_empty() ) {
			m_initializationError = bridge_error;
		}
	}
}

void Terrain3DRenderCoordinator::attachEffectToActiveCamera(
	void ) {
	if ( !m_pTerrain || m_effect.is_null() || !m_bCompositorDebugEnabled ) {
		return;
	}
	Camera3D *camera = NULL;
#if defined( T3D_EDITOR_NATIVE )
	if ( Engine::get_singleton()->is_editor_hint() ) {
		camera = editorViewportCameraCold();
	} else
#endif
		if ( m_pTerrain->sceneNode().get_viewport() ) {
		camera = m_pTerrain->sceneNode().get_viewport()->get_camera_3d();
	}
	if ( !camera ) {
		return;
	}
	if ( m_nAttachedCameraId == camera->get_instance_id() && m_attachedCompositor.is_valid() &&
		 camera->get_compositor() == m_attachedCompositor ) {
		return;
	}
	detachEffect();
	Ref< Compositor > compositor = camera->get_compositor();
	m_bCreatedCompositor = compositor.is_null();
	if ( compositor.is_null() ) {
		compositor.instantiate();
		camera->set_compositor( compositor );
	}
	TypedArray< Ref< CompositorEffect > > effects = compositor->get_compositor_effects();
	bool								  bFound = false;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		if ( effects[i] == m_effect ) {
			bFound = true;
			break;
		}
	}
	if ( !bFound ) {
		effects.push_back( m_effect );
		compositor->set_compositor_effects( effects );
	}
	m_attachedCompositor = compositor;
	m_nAttachedCameraId = camera->get_instance_id();
}

void Terrain3DRenderCoordinator::detachEffect(
	void ) {
	if ( m_effect.is_null() || m_attachedCompositor.is_null() ) {
		m_attachedCompositor.unref();
		m_nAttachedCameraId = 0u;
		m_bCreatedCompositor = false;
		return;
	}
	TypedArray< Ref< CompositorEffect > > effects = m_attachedCompositor->get_compositor_effects();
	TypedArray< Ref< CompositorEffect > > remaining;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		Ref< CompositorEffect > effect = effects[i];
		if ( effect != m_effect ) {
			remaining.push_back( effect );
		}
	}
	m_attachedCompositor->set_compositor_effects( remaining );
	if ( m_bCreatedCompositor && remaining.is_empty() && m_nAttachedCameraId != 0u ) {
		Object *const	object = ObjectDB::get_instance( m_nAttachedCameraId );
		Camera3D *const camera = Object::cast_to< Camera3D >( object );
		if ( camera && camera->get_compositor() == m_attachedCompositor ) {
			camera->set_compositor( Ref< Compositor >() );
		}
	}
	m_attachedCompositor.unref();
	m_nAttachedCameraId = 0u;
	m_bCreatedCompositor = false;
}

bool Terrain3DRenderCoordinator::reloadPublishedPreviewVtPackageCold(
	void ) {
	return m_previewService.reloadPublishedPreviewVtPackageCold( previewServices() );
}

void Terrain3DRenderCoordinator::logSharedVtEnsureStatusOnRenderThread(
	const uint32_t					 nStatus,
	const t3dVTSharedResourceClass_t eRequestedClass ) {
	const uint32_t nGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( m_nLastSharedVtLogStatus == nStatus && m_nLastSharedVtLogGeneration == nGeneration &&
		 m_eLastSharedVtLogClass == eRequestedClass ) {
		return;
	}
	m_nLastSharedVtLogStatus = nStatus;
	m_nLastSharedVtLogGeneration = nGeneration;
	m_eLastSharedVtLogClass = eRequestedClass;
	static const char *const s_pStatus[] = { "ready",
											 "shader_library_unavailable",
											 "preview_readback_in_flight",
											 "preview_package_reload_failed",
											 "production_package_load_failed",
											 "resource_ensure_failed" };
	const char				*pStatus = nStatus < T3D_ARRAY_COUNT( s_pStatus ) ? s_pStatus[nStatus] : "unknown";
	if ( nStatus == 0u ) {
		UtilityFunctions::print( "[Terrain3D][VT][Shared] READY generation=",
								 nGeneration,
								 " requested_class=",
								 ( uint32_t ) eRequestedClass,
								 " active_class=",
								 ( uint32_t ) m_sharedVtRuntime.get_shared_resource_class(),
								 " shader_ready=",
								 m_shaderLibrary.isReady(),
								 " resources_ready=",
								 m_sharedVtRuntime.are_shared_resources_ready() );
	} else {
		UtilityFunctions::push_warning( "[Terrain3D][VT][Shared] FAIL reason=",
										pStatus,
										" generation=",
										nGeneration,
										" requested_class=",
										( uint32_t ) eRequestedClass,
										" active_class=",
										( uint32_t ) m_sharedVtRuntime.get_shared_resource_class(),
										" shader_ready=",
										m_shaderLibrary.isReady(),
										" resources_ready=",
										m_sharedVtRuntime.are_shared_resources_ready(),
										" cpu_ready=",
										m_sharedVtRuntime.virtualTexture().is_cpu_ready(),
										" gpu_storage_ready=",
										m_sharedVtRuntime.virtualTexture().is_gpu_storage_ready(),
										" gpu_executable_ready=",
										m_sharedVtRuntime.virtualTexture().is_gpu_ready(),
										" initial_published=",
										m_sharedVtRuntime.virtualTexture().is_initial_published(),
										" runtime_initialized=",
										m_sharedVtRuntime.virtualTexture().is_initialized(),
										" warm_package_pending=",
										m_sharedVtRuntime.virtualTexture().isPackageRepublishPendingWarm(),
										" resource_rebuild_required=",
										m_sharedVtRuntime.virtualTexture().isPackageResourceRebuildRequiredWarm(),
										" preview_readback_in_flight=",
										m_previewService.hasReadbackInFlight() );
	}
}

bool Terrain3DRenderCoordinator::ensureSharedVtRuntimeCold(
	const t3dVTSharedResourceClass_t eResourceClass ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	// Cold configure one global-device descriptor owner before consumers start
	// building their leases. Keep headroom for unrelated buffers and textures.
	t3dRdDescriptorStatus_t descriptorStatus = T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	if ( !Terrain3DRdContext::getShared().initialize() ||
		 !Terrain3DRdContext::getShared().getGlobalDescriptorCacheCold( 256u, 96u, descriptorStatus ) ) {
		logSharedVtEnsureStatusOnRenderThread( 5u, eResourceClass );
		return false;
	}
	if ( !m_shaderLibrary.isReady() ) {
		logSharedVtEnsureStatusOnRenderThread( 1u, eResourceClass );
		return false;
	}
	if ( m_effect.is_valid() && !m_sharedVtRuntime.virtualTexture().get_gpu().has_fragment_feedback_callback() ) {
		m_sharedVtRuntime.virtualTexture().get_gpu().set_fragment_feedback_callback_cold(
			callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::consumeFragmentFeedbackOnRenderThread ) );
	}
	const t3dVTSharedResourceClass_t eCurrentClass = m_sharedVtRuntime.get_shared_resource_class();
	const bool						 bClassTransition =
		eCurrentClass == T3D_VT_SHARED_RESOURCES_PREVIEW && eResourceClass == T3D_VT_SHARED_RESOURCES_PRODUCTION;
	if ( m_sharedVtRuntime.are_shared_resources_ready() &&
		 ( eCurrentClass == eResourceClass || ( eCurrentClass == T3D_VT_SHARED_RESOURCES_PRODUCTION &&
												eResourceClass == T3D_VT_SHARED_RESOURCES_PREVIEW ) ) ) {
		publishRenderSharedResourceReadyOnRenderThread( eCurrentClass );
		logSharedVtEnsureStatusOnRenderThread( 0u, eResourceClass );
		return true;
	}
	if ( bClassTransition ) {
		if ( m_previewService.hasReadbackInFlight() ) {
			logSharedVtEnsureStatusOnRenderThread( 2u, eResourceClass );
			return false;
		}
		if ( !m_sharedVtRuntime.beginVtResourceReplacementCold( Terrain3DRdContext::getShared() ) ) {
			logSharedVtEnsureStatusOnRenderThread( 5u, eResourceClass );
			return false;
		}
		m_previewService.noteSharedResourceClassTransition();
		// Retire the previous VT publication only; the world/terrain/ocean
		// pipelines and their independent resources remain allocated.
		t3dRenderLifecycleStatus_t vtLifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
		vtLifecycle.m_nReadyMask &=
			~( T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED );
		vtLifecycle.m_nSharedResourceClass = ( uint32_t ) T3D_VT_SHARED_RESOURCES_NONE;
		publishRenderLifecycleStatusOnRenderThread( vtLifecycle );
		m_sharedVtRuntime.shutdown_cold( true );
		const t3dRdFrameSnapshotSlot_t *const pPublication = acquireSnapshotOnRenderThread();
		const bool bEditorAuthoringMode = pPublication != NULL && pPublication->m_snapshot.m_bEditorAuthoringMode;
		if ( bEditorAuthoringMode ) {
			if ( !m_previewService.reloadPublishedPreviewVtPackageCold( previewServices() ) ) {
				m_sharedVtRuntime.abortVtResourceReplacementCold();
				logSharedVtEnsureStatusOnRenderThread( 3u, eResourceClass );
				return false;
			}
		} else {
			String vtError;
			if ( !loadRuntimeVirtualTexturePackageFileCold( &vtError ) ) {
				m_sharedVtRuntime.abortVtResourceReplacementCold();
				logSharedVtEnsureStatusOnRenderThread( 4u, eResourceClass );
				UtilityFunctions::push_warning( "[Terrain3D][VT][Shared] PACKAGE LOAD FAIL error=", vtError );
				return false;
			}
		}
	}

	uint32_t nPreviewMaterialCapacity = 0u;
	uint32_t nPreviewVirtualPageCapacity = 0u;
	if ( eResourceClass == T3D_VT_SHARED_RESOURCES_PREVIEW ) {
		nPreviewMaterialCapacity = MAX( 1u, m_sharedVtRuntime.virtualTexture().getLoadedPackageAllocationCountCold() );
		nPreviewVirtualPageCapacity =
			MAX( 1u, m_sharedVtRuntime.virtualTexture().getLoadedPackageVirtualPageCountCold() );
	}
	Terrain3DColdPhaseScope cold_phase_scope;
	bool					bReady = m_sharedVtRuntime.ensure_shared_resources_cold(
		   Terrain3DRdContext::getShared(),
		   eResourceClass,
		   nPreviewMaterialCapacity,
		   nPreviewVirtualPageCapacity,
		   eResourceClass == T3D_VT_SHARED_RESOURCES_PRODUCTION ? &m_worldRuntime.getLimits() : NULL );
	if ( bReady && bClassTransition ) {
		bReady = m_sharedVtRuntime.commitVtResourceReplacementCold( Terrain3DRdContext::getShared() );
	}
	if ( !bReady && bClassTransition ) {
		m_sharedVtRuntime.abortVtResourceReplacementCold();
	}
	if ( bReady ) {
		publishRenderSharedResourceReadyOnRenderThread( eResourceClass );
	}
	logSharedVtEnsureStatusOnRenderThread( bReady ? 0u : 5u, eResourceClass );
	return bReady;
}

void Terrain3DRenderCoordinator::servicePreviewVtRepublishOnRenderThread(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_previewService.servicePreviewVtRepublishOnRenderThread( previewServices() );
}

bool Terrain3DRenderCoordinator::rebuildPreviewPackageResourcesCold(
	const void *const				 pBytes,
	const uint64_t					 nBytes,
	const bool						 bNeutral,
	const t3dVTSharedResourceClass_t eClass ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	if ( eClass == T3D_VT_SHARED_RESOURCES_NONE || ( !bNeutral && ( pBytes == NULL || nBytes == 0u ) ) ) {
		return false;
	}
	// Package capacity is independent of terrain/world/instance/ocean pipelines.
	// Their descriptor keys detect the new VT handles on the next preparation.
	t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	lifecycle.m_nReadyMask &=
		~( T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED );
	lifecycle.m_nSharedResourceClass = ( uint32_t ) T3D_VT_SHARED_RESOURCES_NONE;
	publishRenderLifecycleStatusOnRenderThread( lifecycle );
	Terrain3DColdPhaseScope cold_phase_scope;
	UtilityFunctions::print( "[Terrain3D][VT][Shared] REBUILD BEGIN class=",
							 ( uint32_t ) eClass,
							 " bytes=",
							 nBytes,
							 " neutral=",
							 bNeutral );
	const bool bReady = m_sharedVtRuntime.replacePackageResourcesCold(
		Terrain3DRdContext::getShared(),
		pBytes,
		nBytes,
		bNeutral,
		eClass,
		eClass == T3D_VT_SHARED_RESOURCES_PRODUCTION ? &m_worldRuntime.getLimits() : NULL );
	if ( bReady ) {
		publishRenderSharedResourceReadyOnRenderThread( eClass );
		UtilityFunctions::print( "[Terrain3D][VT][Shared] REBUILD READY class=",
								 ( uint32_t ) eClass,
								 " resources_ready=",
								 m_sharedVtRuntime.are_shared_resources_ready() );
	} else {
		UtilityFunctions::push_warning( "[Terrain3D][VT][Shared] REBUILD FAIL class=",
										( uint32_t ) eClass,
										" cpu_ready=",
										m_sharedVtRuntime.virtualTexture().is_cpu_ready(),
										" gpu_storage_ready=",
										m_sharedVtRuntime.virtualTexture().is_gpu_storage_ready(),
										" gpu_executable_ready=",
										m_sharedVtRuntime.virtualTexture().is_gpu_ready() );
	}
	return bReady;
}

bool Terrain3DRenderCoordinator::ensurePreviewSharedVt(
	const t3dVTSharedResourceClass_t eClass ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !ensureSharedVtRuntimeCold( eClass ) ) {
		return false;
	}
	if ( eClass != T3D_VT_SHARED_RESOURCES_PRODUCTION || !m_bStartupActivationCommitted ) {
		return true;
	}
	// Explicit preview-to-production capacity transitions recreate consumers.
	// Restore their readiness here before a disabled Forward+ route can be used.
	return initializeRendererOnRenderThread() &&
		   completeRendererRecoveryOnRenderThread( m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ),
												   snapshotRenderLifecycleStatusOnOwnerThread().m_nRecoveryRevision );
}

bool Terrain3DRenderCoordinator::initializeRendererOnRenderThread(
	const uint64_t nRenderFrame ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	// P08: forward/debug rendering never drives startup. The budgeted startup
	// service is the sole authority until the final activation barrier commits.
	if ( m_bStartupColdBegun && !m_bStartupActivationCommitted ) {
		return false;
	}
	if ( m_sharedVtRuntime.get_shared_resource_class() == T3D_VT_SHARED_RESOURCES_PREVIEW &&
		 !ensureSharedVtRuntimeCold( T3D_VT_SHARED_RESOURCES_PRODUCTION ) ) {
		return false;
	}
	t3dRdDescriptorStatus_t descriptorStatus = T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
	if ( !Terrain3DRdContext::getShared().initialize() ||
		 !Terrain3DRdContext::getShared().getGlobalDescriptorCacheCold( 256u, 96u, descriptorStatus ) ) {
		return false;
	}
	const bool shader_ready = m_shaderLibrary.isReady();
	bool	   terrain_ready = m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	bool	   instance_ready = m_bInstanceRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	bool	   ocean_ready = m_bOceanRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );

	if ( !terrain_ready ) {
		Terrain3DColdPhaseScope cold_phase_scope;
		const uint32_t coldBytes = Terrain3DWorldResources::estimateSharedColdArenaBytes( m_worldRuntime.getLimits() );
		m_renderer.setFeedbackOwner( &m_sharedVtRuntime.virtualTexture().get_gpu() );
		terrain_ready = shader_ready && coldBytes != 0u &&
						Terrain3DRdContext::getShared().initializeSharedArenas( coldBytes ) && m_renderer.initialize();
	}

	if ( terrain_ready ) {
		Terrain3DColdPhaseScope cold_phase_scope;
		terrain_ready = m_worldRuntime.initializeCold( m_renderer.usesPersistentFrameBuffer(),
													   m_renderer.usesPersistentCpuBuffers() );
	}

	if ( terrain_ready ) {
		// Unified World publication owns instance geometry; the old renderer has no GPU lifetime.
		instance_ready = false;

		const bool ocean_retry_allowed =
			!ocean_ready && ( nRenderFrame == UINT64_MAX || m_nLastOceanInitializationRenderFrame != nRenderFrame );
		if ( ocean_retry_allowed ) {
			Terrain3DColdPhaseScope cold_phase_scope;
			m_nLastOceanInitializationRenderFrame = nRenderFrame;
			ocean_ready = m_oceanRenderer.initialize();
			if ( !ocean_ready ) {
				LOG( WARN,
					 "[RD ocean] Companion renderer unavailable; terrain rendering remains active: ",
					 m_oceanRenderer.getLastError() );
			}
		}
	}

	m_bRendererInitialized.store( terrain_ready, T3D_MEMORY_ORDER_RELEASE );
	m_bInstanceRendererInitialized.store( instance_ready, T3D_MEMORY_ORDER_RELEASE );
	m_bOceanRendererInitialized.store( ocean_ready, T3D_MEMORY_ORDER_RELEASE );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( !terrain_ready ) {
		m_environmentField.resetAfterRendererTeardownCold();
		m_worldRuntime.shutdownCold();
		m_oceanRenderer.shutdown();
		m_renderer.shutdown();
		m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bOceanRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		resetForwardLifecycle();
		t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
		renderStatus.m_nReadyMask &= ~( T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD | T3D_RENDER_READY_INSTANCES |
										T3D_RENDER_READY_OCEAN );
		publishRenderLifecycleStatusOnRenderThread( renderStatus );
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
		if ( failures >= 8u ) {
			m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		}
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = shader_ready ? m_renderer.getLastError() : m_shaderLibrary.getLastError();
	} else {
		const uint32_t nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
		uint32_t	   nReadyMask = T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD;
		if ( shader_ready ) {
			nReadyMask |= T3D_RENDER_READY_SHADER_GPU;
		}
		if ( instance_ready ) {
			nReadyMask |= T3D_RENDER_READY_INSTANCES;
		}
		if ( ocean_ready ) {
			nReadyMask |= T3D_RENDER_READY_OCEAN;
		}
		setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, nReadyMask );
	}
	return terrain_ready;
}

void Terrain3DRenderCoordinator::initializeOcean(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_publicationBuilder.initializeOcean( publicationServices() );
}

bool Terrain3DRenderCoordinator::isStartupGenerationCurrent(
	const uint32_t nStartupGeneration ) const {
	// P4/P8 generation invalidation is the render-lane authority. Render callbacks
	// must never reach back into the live Terrain3D node to validate ownership.
	return nStartupGeneration != 0u && m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) == nStartupGeneration &&
		   m_bStartupColdBegun.load( T3D_MEMORY_ORDER_ACQUIRE );
}

void Terrain3DRenderCoordinator::publishRenderLifecycleStatusOnRenderThread(
	const t3dRenderLifecycleStatus_t &rStatus ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	Terrain3DScopedLock		   lock( m_renderLifecycleMutex );
	t3dRenderLifecycleStatus_t status = rStatus;
	status.m_nRevision = advanceGeneration( m_renderLifecycleStatus.m_nRevision );
	m_renderLifecycleStatus = status;
}

void Terrain3DRenderCoordinator::publishRenderLifecycleStatusCold(
	const t3dRenderLifecycleStatus_t &rStatus ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	Terrain3DScopedLock			lock( m_renderLifecycleMutex );
	t3dRenderLifecycleStatus_t	status = rStatus;
	status.m_nRevision = advanceGeneration( m_renderLifecycleStatus.m_nRevision );
	m_renderLifecycleStatus = status;
}

t3dRenderLifecycleStatus_t Terrain3DRenderCoordinator::snapshotRenderLifecycleStatusOnOwnerThread(
	void ) const {
	Terrain3DScopedLock lock( m_renderLifecycleMutex );
	return m_renderLifecycleStatus;
}

void Terrain3DRenderCoordinator::setRenderLifecycleReadyBitOnRenderThread(
	const uint32_t nStartupGeneration,
	const uint32_t nReadyBit ) {
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
	} else if ( status.m_eStage == T3D_RENDER_LIFECYCLE_IDLE || status.m_eStage == T3D_RENDER_LIFECYCLE_STOPPED ) {
		status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
	}
	status.m_nReadyMask |= nReadyBit;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::clearRenderLifecycleReadyBitOnRenderThread(
	const uint32_t nReadyBit ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nReadyMask &= ~nReadyBit;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::publishRenderSharedResourceReadyOnRenderThread(
	const t3dVTSharedResourceClass_t eResourceClass ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	const uint32_t			   nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( nStartupGeneration != 0u && status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
	}
	status.m_nReadyMask |= T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED;
	m_previewService.noteGpuPackageOnRenderThread( m_sharedVtRuntime.virtualTexture().getPackageGeneration(),
												   m_sharedVtRuntime.virtualTexture().getPackageContentSignature() );
	status.m_nSharedResourceClass = ( uint32_t ) eResourceClass;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::publishRenderLifecycleFailureOnRenderThread(
	const uint32_t					  nStartupGeneration,
	const t3dRenderLifecycleFailure_t eFailure ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	t3dRenderLifecycleStatus_t	status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
	}
	status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
	status.m_nFailureCode = ( uint32_t ) eFailure;
	status.m_nPreparedWorldPublicationGeneration = 0u;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::acknowledgeStartupCancellationOnRenderThread(
	const uint32_t nStartupGeneration ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	if ( nStartupGeneration == 0u ) {
		return;
	}
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
	}
	status.m_eStage = T3D_RENDER_LIFECYCLE_CANCELLING;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	publishRenderLifecycleStatusOnRenderThread( status );

	// The coordinator generation was invalidated before this callback was queued.
	// Once this callback runs, all earlier generation-bound callbacks have reached
	// the render lane and can no longer publish into a future startup generation.
	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupGpuServiceComplete.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupTerrainRendererPrepared = false;
	m_bStartupWorldRuntimePrepared = false;

	status.m_nReadyMask = T3D_RENDER_READY_NONE;
	status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
	status.m_nPreparedWorldPublicationGeneration = 0u;
	publishRenderLifecycleStatusOnRenderThread( status );
}

bool Terrain3DRenderCoordinator::queueStartupCancellationAckCold(
	const uint32_t nCancelledGeneration,
	const bool	   bForceDrain ) {
	if ( nCancelledGeneration == 0u ) {
		return true;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		acknowledgeStartupCancellationOnRenderThread( nCancelledGeneration );
		return true;
	}
	if ( m_effect.is_valid() &&
		 Terrain3DRdContext::enqueueOnRenderThread(
			 callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::acknowledgeStartupCancellationOnRenderThread )
				 .bind( ( int64_t ) nCancelledGeneration ) ) ) {
		if ( bForceDrain ) {
			Terrain3DRdContext::forceSync( T3D_RD_FORCE_SYNC_CONTROL );
			++m_nStartupRenderDrainCount;
		}
		const t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
		return !bForceDrain || ( status.m_nStartupGeneration == nCancelledGeneration &&
								 status.m_eStage == T3D_RENDER_LIFECYCLE_STOPPED );
	}

	RenderingServer *const	   pServer = RenderingServer::get_singleton();
	t3dRenderLifecycleStatus_t status{};
	status.m_nStartupGeneration = nCancelledGeneration;
	status.m_eStage = pServer == NULL ? T3D_RENDER_LIFECYCLE_STOPPED : T3D_RENDER_LIFECYCLE_FAILED;
	status.m_nFailureCode =
		pServer == NULL ? T3D_RENDER_LIFECYCLE_FAILURE_NONE : T3D_RENDER_LIFECYCLE_FAILURE_CANCELLATION_SCHEDULING;
	publishRenderLifecycleStatusCold( status );
	if ( pServer != NULL ) {
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Render-thread startup cancellation acknowledgement could not be scheduled";
		return false;
	}
	return true;
}

void Terrain3DRenderCoordinator::cancelStartupCold(
	const uint32_t nStartupGeneration ) {
	if ( nStartupGeneration == 0u || m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) != nStartupGeneration ) {
		return;
	}
	++m_nStartupCancellationCount;

	// P4 ordering: fail the external route first, then invalidate the coordinator
	// generation before mutating any generation-bound child startup domain.
	m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}
	const uint32_t nCancelledGeneration = m_nStartupGeneration.exchange( 0u, T3D_MEMORY_ORDER_ACQ_REL );
	if ( nCancelledGeneration == 0u ) {
		return;
	}
	m_shaderLibrary.cancelLoadCold( nCancelledGeneration );
	m_sharedVtRuntime.virtualTexture().cancel_startup_cold( nCancelledGeneration );

	// The render acknowledgement is queued after every already-queued startup
	// callback. forceSync() makes the acknowledgement a cold control-plane drain,
	// after which render-visible startup bookkeeping can be reset safely.
	const bool bAcknowledged = queueStartupCancellationAckCold( nCancelledGeneration, true );
	m_bStartupColdBegun.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupActivationPrepared = false;
	m_bStartupActivationCommitted = false;
	m_nStartupActivationSnapshotGeneration = 0u;
	if ( !bAcknowledged ) {
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
}

bool Terrain3DRenderCoordinator::beginStartupCold(
	Terrain3DOwner *const pTerrain,
	const uint32_t		  nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( pTerrain == NULL || nStartupGeneration == 0u ) {
		return false;
	}
	if ( m_bStartupColdBegun && m_pTerrain == pTerrain && m_nStartupGeneration == nStartupGeneration ) {
		return !m_bStartupColdFailed;
	}
	const uint64_t nBeginStartedUsec = t3dRdStartupTicksUsec();
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Begin] BEGIN generation=", nStartupGeneration );
	shutdown();
#if defined( T3D_EDITOR_NATIVE )
	m_pAuthoring = memnew( Terrain3DRenderAuthoringCompanion );
#endif
	m_pTerrain = pTerrain;
	m_nStartupGeneration.store( nStartupGeneration, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupPublicationPrepareCount = 0u;
	m_nStartupPublicationPrepareUsecLast = 0u;
	m_nStartupRendererInitializeAttemptCount = 0u;
	m_nStartupRendererInitializeSuccessCount = 0u;
	m_nStartupRendererInitializeUsecLast = 0u;
	m_nStartupRendererInitializeUsecTotal = 0u;
	m_nStartupGpuServiceCount = 0u;
	m_nStartupGpuServiceUsecLast = 0u;
	m_nStartupOwnerServiceCount = 0u;
	m_nStartupOwnerServiceLastLogUsec = 0u;
	m_nStartupOwnerServiceSignature = UINT64_MAX;
	m_nRecoveryRequestRevision = 0u;
	m_nRecoveryQueuedRevision = 0u;
	m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupStaleCompletionRejectCount.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupActivationBarrierCount = 0u;
	m_nStartupActivationBarrierUsecLast = 0u;
	m_nStartupActivationSnapshotGeneration = 0u;
	m_nStartupFinalSyncSuccessCount = 0u;
	m_nStartupFinalSyncFailureCount = 0u;
	m_nStartupFinalSyncBeginUsec = 0u;
	m_nStartupFinalSyncElapsedUsecLast = 0u;
	m_nStartupGpuStage.store( T3D_STARTUP_GPU_VT_STORAGE, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupTerrainRendererPrepared = false;
	m_bStartupWorldRuntimePrepared = false;
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupGpuServiceComplete.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupActivationPrepared = false;
	m_bStartupActivationCommitted = false;
	m_bStartupColdBegun.store( true, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
	{
		t3dRenderLifecycleStatus_t renderStatus{};
		renderStatus.m_nStartupGeneration = nStartupGeneration;
		renderStatus.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
		publishRenderLifecycleStatusCold( renderStatus );
	}
	m_shaderLibrary.setStartupGeneration( m_nStartupGeneration );
	m_sharedVtRuntime.virtualTexture().set_startup_generation( m_nStartupGeneration );
	m_sharedVtRuntime.virtualTexture().set_quality_profile(
		( uint32_t ) m_pTerrain->getVirtualTextureQualityProfile() );
	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );
	{
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = String();
	}
	if ( m_effect.is_null() ) {
		LOG( INFO, "[Terrain3D][Startup][Coordinator][CompositorEffect] BEGIN generation=", nStartupGeneration );
		m_effect.instantiate();
		m_effect->setCoordinator( this );
		m_effect->set_enabled( false );
		LOG( INFO, "[Terrain3D][Startup][Coordinator][CompositorEffect] END generation=", nStartupGeneration );
	}
	attachForwardBridge();
	LOG( INFO, "[Terrain3D][Startup][Coordinator][ShaderLibrary] BEGIN generation=", nStartupGeneration );
	if ( !m_shaderLibrary.beginLoadCold( m_nStartupGeneration ) ) {
		m_bStartupColdFailed = true;
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
		LOG( ERROR,
			 "[Terrain3D][Startup][Coordinator][ShaderLibrary] FAIL generation=",
			 nStartupGeneration,
			 " error=",
			 m_initializationError );
		return false;
	}
	LOG( INFO, "[Terrain3D][Startup][Coordinator][ShaderLibrary] END generation=", nStartupGeneration );
	LOG( INFO,
		 "[Terrain3D][Startup][Coordinator][Begin] END generation=",
		 nStartupGeneration,
		 " elapsed=",
		 t3dRdStartupTicksUsec() - nBeginStartedUsec,
		 " us" );
	return true;
}

void Terrain3DRenderCoordinator::serviceStartupCold(
	const uint32_t nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !m_bStartupColdBegun || m_bStartupColdFailed || nStartupGeneration == 0u ||
		 m_nStartupGeneration != nStartupGeneration ) {
		return;
	}
	++m_nStartupOwnerServiceCount;
	const uint64_t nNowUsec = t3dRdStartupTicksUsec();
	const t3dStartupDomainSnapshot_t shaderBefore = m_shaderLibrary.getStartupSnapshot();
	m_shaderLibrary.serviceLoadCold();
	if ( m_shaderLibrary.isFailed( nStartupGeneration ) ) {
		m_bStartupColdFailed = true;
		{
			Terrain3DScopedLock lock( m_diagnosticsMutex );
			m_initializationError = m_shaderLibrary.getLastError();
		}
		logStartupFailureDiagnosticsCold( "shader_source_load", nStartupGeneration );
		return;
	}
	if ( m_bStartupColdPrepared && !m_sharedVtRuntime.virtualTexture().is_cpu_ready() ) {
		const uint64_t nVtStartedUsec = t3dRdStartupTicksUsec();
		LOG( INFO, "[Terrain3D][Startup][CoordinatorService][VTCpu] BEGIN generation=", nStartupGeneration );
		if ( !m_sharedVtRuntime.prepare_shared_vt_cpu_cold( T3D_VT_SHARED_RESOURCES_PRODUCTION,
															0u,
															0u,
															&m_worldRuntime.getLimits() ) ) {
			m_sharedVtRuntime.virtualTexture().mark_startup_failed( nStartupGeneration );
			m_bStartupColdFailed = true;
			{
				Terrain3DScopedLock lock( m_diagnosticsMutex );
				m_initializationError = "Failed preparing startup VT CPU state";
			}
			LOG( ERROR, "[Terrain3D][Startup][CoordinatorService][VTCpu] FAIL generation=", nStartupGeneration,
				 " elapsed=", t3dRdStartupTicksUsec() - nVtStartedUsec, " us" );
			logStartupFailureDiagnosticsCold( "vt_cpu_prepare", nStartupGeneration );
			return;
		}
		LOG( INFO, "[Terrain3D][Startup][CoordinatorService][VTCpu] END generation=", nStartupGeneration,
			 " elapsed=", t3dRdStartupTicksUsec() - nVtStartedUsec, " us",
			 " ready=", m_sharedVtRuntime.virtualTexture().is_cpu_ready() );
	}
	if ( m_bStartupColdPrepared ) {
		serviceShaderStartupCold();
	}
	const t3dStartupDomainSnapshot_t shaderAfter = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t vtAfter = m_sharedVtRuntime.virtualTexture().get_startup_snapshot();
	const uint64_t nSignature = ( uint64_t ) shaderAfter.m_eStage |
		( ( uint64_t ) vtAfter.m_eStage << 8u ) |
		( ( uint64_t ) m_bStartupColdPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ) << 16u ) |
		( ( uint64_t ) m_bShaderGpuStartupQueued.load( T3D_MEMORY_ORDER_ACQUIRE ) << 17u ) |
		( ( uint64_t ) m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) << 18u );
	if ( nSignature != m_nStartupOwnerServiceSignature || m_nStartupOwnerServiceLastLogUsec == 0u ||
		 nNowUsec - m_nStartupOwnerServiceLastLogUsec >= 1000000u ) {
		m_nStartupOwnerServiceSignature = nSignature;
		m_nStartupOwnerServiceLastLogUsec = nNowUsec;
		const Dictionary shaderDetails = m_shaderLibrary.getDiagnostics();
		const t3dRenderLifecycleStatus_t renderState = snapshotRenderLifecycleStatusOnOwnerThread();
		LOG( INFO, "[Terrain3D][Startup][CoordinatorService] PROGRESS generation=", nStartupGeneration,
			 " call=", m_nStartupOwnerServiceCount,
			 " shader_before=", t3dStartupStageName( shaderBefore.m_eStage ),
			 " shader_after=", t3dStartupStageName( shaderAfter.m_eStage ),
			 " sources=", shaderDetails.get( "shader_source_ready", 0 ), "/",
			 shaderDetails.get( "shader_source_count", 0 ),
			 " shader_variants=", shaderDetails.get( "shader_compile_ready", 0 ), "/",
			 shaderDetails.get( "shader_compile_expected", 0 ),
			 " shader_failures=", shaderDetails.get( "shader_compile_failed", 0 ),
			 " vt=", t3dStartupStageName( vtAfter.m_eStage ),
			 " vt_units=", vtAfter.m_nCompletedUnits, "/", vtAfter.m_nTotalUnits,
			 " prepared=", m_bStartupColdPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ),
			 " gpu_queued=", m_bShaderGpuStartupQueued.load( T3D_MEMORY_ORDER_ACQUIRE ),
			 " gpu_stage=", startupGpuStageName( ( t3dStartupGpuStage_t ) m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE ) ),
			 " gpu_complete=", m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ),
			 " lifecycle=", ( uint32_t ) renderState.m_eStage,
			 " ready_mask=", renderState.m_nReadyMask,
			 " failure_code=", renderState.m_nFailureCode );
	}
}

void Terrain3DRenderCoordinator::logStartupFailureDiagnosticsCold(
	const char *const pszService,
	const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t shader = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t vt = m_sharedVtRuntime.virtualTexture().get_startup_snapshot();
	const t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	String error;
	{
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		error = m_initializationError;
	}
	const String shaderError = m_shaderLibrary.getLastError();
	LOG( ERROR, "[Terrain3D][Startup][FailureSnapshot] service=", pszService ? pszService : "unknown",
		 " requested_generation=", nStartupGeneration,
		 " coordinator_generation=", m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " owner_generation=", m_pTerrain ? m_pTerrain->getStartupGenerationInternal() : 0u,
		 " begun=", m_bStartupColdBegun.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " prepared=", m_bStartupColdPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " failed=", m_bStartupColdFailed.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " service_count=", m_nStartupOwnerServiceCount );
	LOG( ERROR, "[Terrain3D][Startup][FailureSnapshot][Services] shader_generation=", shader.m_nGeneration,
		 " shader=", t3dStartupStageName( shader.m_eStage ),
		 " shader_units=", shader.m_nCompletedUnits, "/", shader.m_nTotalUnits,
		 " shader_error=", shaderError,
		 " vt_generation=", vt.m_nGeneration,
		 " vt=", t3dStartupStageName( vt.m_eStage ),
		 " vt_units=", vt.m_nCompletedUnits, "/", vt.m_nTotalUnits,
		 " gpu_stage=", startupGpuStageName( ( t3dStartupGpuStage_t ) m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE ) ),
		 " gpu_queued=", m_bShaderGpuStartupQueued.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " gpu_complete=", m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	LOG( ERROR, "[Terrain3D][Startup][FailureSnapshot][Lifecycle] stage=", ( uint32_t ) lifecycle.m_eStage,
		 " ready_mask=", lifecycle.m_nReadyMask,
		 " failure_code=", lifecycle.m_nFailureCode,
		 " lifecycle_generation=", lifecycle.m_nStartupGeneration,
		 " world_publication_queued=", m_bStartupWorldPublicationQueued.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " world_publication_prepared=", m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " bridge_ready=", isForwardBridgeReady(),
		 " initialization_error=", error );
}

void Terrain3DRenderCoordinator::logStartupActivationFailureState(
	const char	  *pReason,
	const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t rShaderState = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t rVtState = m_sharedVtRuntime.virtualTexture().get_startup_snapshot();
	const Terrain3DData *const		 pData = m_pTerrain ? m_pTerrain->dataObject() : NULL;
	const t3dStartupDomainSnapshot_t dataState = pData ? pData->getStartupSnapshot() : t3dStartupDomainSnapshot_t{};
	const t3dStartupDomainSnapshot_t *const pDataState = pData ? &dataState : NULL;
	const bool								bOwnerPresent = m_pTerrain != NULL;
	const bool								bOwnerGenerationMatch =
		bOwnerPresent && m_pTerrain->getStartupGenerationInternal() == nStartupGeneration;
	const bool bDataGenerationMatch = pDataState != NULL && pDataState->m_nGeneration == nStartupGeneration;
	const bool bDataCommitted = pData != NULL && pData->isStartupCommitted( nStartupGeneration );
	const bool bShaderGenerationMatch = rShaderState.m_nGeneration == nStartupGeneration;
	const bool bVtGenerationMatch = rVtState.m_nGeneration == nStartupGeneration;
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool bRendererInitialized = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_TERRAIN ) != 0u;
	const bool bInstanceRendererInitialized = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_INSTANCES ) != 0u;
	const bool bOceanRendererInitialized = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_OCEAN ) != 0u;
	const bool bSharedVtReady = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_VT_PUBLISHED ) != 0u;
	const bool bVtInitialPublished = bSharedVtReady;
	const bool bForwardBridgeReady = isForwardBridgeReady();
	const bool bRendererFailed = renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED;
	const bool bRenderEnabled = m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );

	LOG( ERROR,
		 "[Terrain3D][Startup][Activation][FAIL] reason=",
		 pReason ? pReason : "unknown",
		 " requested_generation=",
		 nStartupGeneration,
		 " coordinator_generation=",
		 m_nStartupGeneration,
		 " owner_generation=",
		 bOwnerPresent ? m_pTerrain->getStartupGenerationInternal() : 0u,
		 " owner_present=",
		 bOwnerPresent,
		 " owner_generation_match=",
		 bOwnerGenerationMatch,
		 " owner_initialized=",
		 bOwnerPresent ? m_pTerrain->isInitialized() : false,
		 " startup_begun=",
		 m_bStartupColdBegun.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " startup_prepared=",
		 m_bStartupColdPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " startup_failed=",
		 m_bStartupColdFailed.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " failure_latched=",
		 m_bFailureLatched );
	LOG( ERROR,
		 "[Terrain3D][Startup][Activation][FAIL][Domains] data_present=",
		 pData != NULL,
		 " data_generation=",
		 pDataState ? pDataState->m_nGeneration : 0u,
		 " data_generation_match=",
		 bDataGenerationMatch,
		 " data_stage=",
		 pDataState ? t3dStartupStageName( t3dStartupDomainGetStage( *pDataState ) ) : "MISSING",
		 " data_committed=",
		 bDataCommitted,
		 " shader_generation=",
		 rShaderState.m_nGeneration,
		 " shader_generation_match=",
		 bShaderGenerationMatch,
		 " shader_stage=",
		 t3dStartupStageName( t3dStartupDomainGetStage( rShaderState ) ),
		 " shader_ready=",
		 ( renderStatus.m_nReadyMask & T3D_RENDER_READY_SHADER_GPU ) != 0u,
		 " vt_generation=",
		 rVtState.m_nGeneration,
		 " vt_generation_match=",
		 bVtGenerationMatch,
		 " vt_stage=",
		 t3dStartupStageName( t3dStartupDomainGetStage( rVtState ) ),
		 " vt_initial_published=",
		 bVtInitialPublished );
	LOG( ERROR,
		 "[Terrain3D][Startup][Activation][FAIL][Render] renderer_initialized=",
		 bRendererInitialized,
		 " renderer_ready=",
		 bRendererInitialized,
		 " renderer_failed=",
		 bRendererFailed,
		 " world_runtime_initialized=",
		 ( renderStatus.m_nReadyMask & T3D_RENDER_READY_WORLD ) != 0u,
		 " world_ready=",
		 ( renderStatus.m_nReadyMask & T3D_RENDER_READY_WORLD ) != 0u,
		 " world_publication_queued=",
		 m_bStartupWorldPublicationQueued.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " world_publication_prepared=",
		 m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " world_publication_generation=",
		 renderStatus.m_nPreparedWorldPublicationGeneration,
		 " instance_renderer_initialized=",
		 bInstanceRendererInitialized,
		 " ocean_renderer_initialized=",
		 bOceanRendererInitialized,
		 " shared_vt_ready=",
		 bSharedVtReady,
		 " forward_bridge_ready=",
		 bForwardBridgeReady,
		 " activation_prepared=",
		 m_bStartupActivationPrepared,
		 " activation_committed=",
		 m_bStartupActivationCommitted,
		 " render_enabled=",
		 bRenderEnabled );
	LOG( ERROR,
		 "[Terrain3D][Startup][Activation][FAIL][Lifecycle] stage=",
		 ( uint32_t ) renderStatus.m_eStage,
		 " generation=",
		 renderStatus.m_nStartupGeneration,
		 " ready_mask=",
		 renderStatus.m_nReadyMask,
		 " failure_code=",
		 renderStatus.m_nFailureCode,
		 " prepared_world_generation=",
		 renderStatus.m_nPreparedWorldPublicationGeneration,
		 " revision=",
		 renderStatus.m_nRevision );
	LOG( ERROR,
		 "[Terrain3D][Startup][Activation][FAIL][Snapshot] published_generation=",
		 m_publicationBuilder.getPublishedSnapshotGeneration(),
		 " activation_snapshot_generation=",
		 m_nStartupActivationSnapshotGeneration,
		 " barrier_count=",
		 m_nStartupActivationBarrierCount,
		 " final_sync_failures=",
		 m_nStartupFinalSyncFailureCount,
		 " gpu_stage=",
		 m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE ) );
}

bool Terrain3DRenderCoordinator::isStartupActivationBaseReady(
	const uint32_t nStartupGeneration ) const {
	if ( !m_bStartupColdBegun || m_bStartupColdFailed || !m_bStartupColdPrepared || m_pTerrain == NULL ||
		 nStartupGeneration == 0u || m_nStartupGeneration != nStartupGeneration ||
		 m_pTerrain->getStartupGenerationInternal() != nStartupGeneration ) {
		return false;
	}
	const t3dStartupDomainSnapshot_t rShaderState = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t rVtState = m_sharedVtRuntime.virtualTexture().get_startup_snapshot();
	if ( rShaderState.m_nGeneration != nStartupGeneration || rVtState.m_nGeneration != nStartupGeneration ) {
		return false;
	}
	const t3dStartupStage_t eShaderStage = t3dStartupDomainGetStage( rShaderState );
	const t3dStartupStage_t eVtStage = t3dStartupDomainGetStage( rVtState );
	const bool bShaderReady = eShaderStage == T3D_STARTUP_GPU_READY || eShaderStage == T3D_STARTUP_COMMITTED;
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool						 bRenderReady =
		renderStatus.m_nStartupGeneration == nStartupGeneration &&
		( renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_STARTING ||
		  renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY ) &&
		( renderStatus.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) == T3D_RENDER_READY_STARTUP_REQUIRED &&
		renderStatus.m_nFailureCode == T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	return bShaderReady && eVtStage == T3D_STARTUP_COMMITTED && bRenderReady && isForwardBridgeReady() &&
		   !m_bFailureLatched;
}

bool Terrain3DRenderCoordinator::isStartupActivationReady(
	const uint32_t nStartupGeneration ) const {
	if ( !isStartupActivationBaseReady( nStartupGeneration ) ||
		 !m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return false;
	}
	const uint64_t nPreparedWorldGeneration = m_nStartupWorldPublicationGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	return nPreparedWorldGeneration != 0u && renderStatus.m_nStartupGeneration == nStartupGeneration &&
		   renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY &&
		   renderStatus.m_nPreparedWorldPublicationGeneration == nPreparedWorldGeneration &&
		   renderStatus.m_nFailureCode == T3D_RENDER_LIFECYCLE_FAILURE_NONE;
}

bool Terrain3DRenderCoordinator::prepareStartupActivationCold(
	const uint32_t nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( m_bStartupActivationPrepared ) {
		return m_nStartupGeneration == nStartupGeneration && !m_bStartupColdFailed;
	}
	if ( m_bStartupColdFailed ) {
		return false;
	}
	if ( m_nStartupFinalSyncBeginUsec == 0u ) {
		m_nStartupFinalSyncBeginUsec = t3dRdStartupTicksUsec();
		++m_nStartupActivationBarrierCount;
		LOG( INFO, "[Terrain3D][Startup] FINAL SYNC BEGIN generation=", nStartupGeneration );
	}
	const uint64_t nStartedUsec = m_nStartupFinalSyncBeginUsec;
	if ( !isStartupActivationBaseReady( nStartupGeneration ) || m_pTerrain->dataObject() == NULL ||
		 !m_pTerrain->dataObject()->isStartupCommitted( nStartupGeneration ) ) {
		m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
		logStartupActivationFailureState( "barrier_prerequisites", nStartupGeneration );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError =
			"Final startup activation barrier prerequisites are incomplete or generation-mismatched";
		return false;
	}

	t3dSharedVTBindings_t sharedBindings{};
	if ( m_sharedVtRuntime.acquirePublishedBindingsHot( T3D_VT_SHARED_RESOURCES_PRODUCTION, sharedBindings ) !=
		 T3D_VT_ACQUIRE_READY ) {
		m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
		logStartupActivationFailureState( "shared_vt_bindings", nStartupGeneration );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation barrier found invalid shared VT descriptor bindings";
		return false;
	}

	if ( m_nStartupActivationSnapshotGeneration == 0u ) {
		const uint64_t nSnapshotBefore = m_publicationBuilder.getPublishedSnapshotGeneration();
		publishSnapshot();
		uint64_t nSnapshotAfter = m_publicationBuilder.getPublishedSnapshotGeneration();
		if ( nSnapshotAfter == 0u ) {
			serviceColdCapacity();
			publishSnapshot();
			nSnapshotAfter = m_publicationBuilder.getPublishedSnapshotGeneration();
		}
		const t3dRenderLifecycleStatus_t snapshotStatus = snapshotRenderLifecycleStatusOnOwnerThread();
		if ( nSnapshotAfter == 0u || snapshotStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED ) {
			m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
			m_bStartupColdFailed = true;
			++m_nStartupFinalSyncFailureCount;
			m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
			LOG( ERROR,
				 "[Terrain3D][Startup][Activation][FAIL][SnapshotPublish] before=",
				 nSnapshotBefore,
				 " after=",
				 nSnapshotAfter,
				 " renderer_failed=",
				 snapshotStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED );
			logStartupActivationFailureState( "snapshot_publish", nStartupGeneration );
			Terrain3DScopedLock lock( m_diagnosticsMutex );
			m_initializationError = "Final startup activation barrier could not publish a valid terrain/ocean snapshot";
			return false;
		}
		m_nStartupActivationSnapshotGeneration = nSnapshotAfter;
		LOG( INFO,
			 "[Terrain3D][Startup][Activation] SNAPSHOT generation=",
			 nStartupGeneration,
			 " snapshot_generation=",
			 nSnapshotAfter,
			 " previous_generation=",
			 nSnapshotBefore,
			 " reused=",
			 nSnapshotAfter == nSnapshotBefore );
	}

	if ( !m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		bool bExpected = false;
		if ( m_bStartupWorldPublicationQueued.compare_exchange_strong( bExpected,
																	   true,
																	   T3D_MEMORY_ORDER_ACQ_REL,
																	   T3D_MEMORY_ORDER_ACQUIRE ) ) {
			if ( m_effect.is_null() ||
				 !Terrain3DRdContext::enqueueOnRenderThread(
					 callable_mp( m_effect.ptr(),
								  &Terrain3DRdCompositorEffect::serviceStartupWorldPublicationOnRenderThread )
						 .bind( ( int64_t ) nStartupGeneration,
								( int64_t ) m_nStartupActivationSnapshotGeneration ) ) ) {
				m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
				m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
				++m_nStartupFinalSyncFailureCount;
				m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
				logStartupActivationFailureState( "world_publication_queue", nStartupGeneration );
				Terrain3DScopedLock lock( m_diagnosticsMutex );
				m_initializationError =
					"Final startup activation barrier could not queue unified-world publication preparation";
				return false;
			}
			LOG( INFO,
				 "[Terrain3D][Startup][Activation] WORLD PUBLICATION QUEUED generation=",
				 nStartupGeneration,
				 " snapshot_generation=",
				 m_nStartupActivationSnapshotGeneration );
		}
		return !m_bStartupColdFailed.load( T3D_MEMORY_ORDER_ACQUIRE );
	}

	if ( !isStartupActivationReady( nStartupGeneration ) ) {
		m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
		logStartupActivationFailureState( "world_publication_ready", nStartupGeneration );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation barrier unified-world publication did not become ready";
		return false;
	}

	m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
	m_bStartupActivationPrepared = true;
	LOG( INFO,
		 "[Terrain3D][Startup][Activation] PREPARED generation=",
		 nStartupGeneration,
		 " snapshot_generation=",
		 m_nStartupActivationSnapshotGeneration,
		 " world_publication_generation=",
		 m_nStartupWorldPublicationGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ),
		 " elapsed=",
		 m_nStartupActivationBarrierUsecLast,
		 " us" );
	return true;
}

bool Terrain3DRenderCoordinator::commitStartupActivationCold(
	const uint32_t nStartupGeneration ) {
	if ( m_bStartupActivationCommitted ) {
		return m_nStartupGeneration == nStartupGeneration && !m_bStartupColdFailed;
	}
	if ( !m_bStartupActivationPrepared || !isStartupActivationReady( nStartupGeneration ) || m_pTerrain == NULL ||
		 !m_pTerrain->isInitialized() ) {
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast =
			m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
		logStartupActivationFailureState( "commit_prerequisites", nStartupGeneration );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation commit rejected incomplete owner/render state";
		return false;
	}
	m_shaderLibrary.markStartupCommitted( nStartupGeneration );
	if ( t3dStartupDomainGetStage( m_shaderLibrary.getStartupSnapshot() ) != T3D_STARTUP_COMMITTED ) {
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast =
			m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
		logStartupActivationFailureState( "shader_commit", nStartupGeneration );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation commit could not commit shader generation";
		return false;
	}
	m_bStartupActivationCommitted = true;
	updateRenderRoute();
	if ( !m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		m_bStartupActivationCommitted = false;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast =
			m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
		logStartupActivationFailureState( "render_route_enable", nStartupGeneration );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation commit did not enable the render route";
		return false;
	}
	++m_nStartupFinalSyncSuccessCount;
	m_nStartupFinalSyncElapsedUsecLast =
		m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
	LOG( INFO,
		 "[Terrain3D][Startup][Activation] COMMITTED generation=",
		 nStartupGeneration,
		 " snapshot_generation=",
		 m_nStartupActivationSnapshotGeneration,
		 " final_sync_elapsed=",
		 m_nStartupFinalSyncElapsedUsecLast,
		 " us" );
	return true;
}

bool Terrain3DRenderCoordinator::prepareStartupCold(
	const uint32_t nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !m_bStartupColdBegun || m_pTerrain == NULL || nStartupGeneration == 0u ||
		 m_nStartupGeneration != nStartupGeneration ||
		 m_pTerrain->getStartupGenerationInternal() != nStartupGeneration ) {
		return false;
	}
	if ( m_bStartupColdPrepared ) {
		return !m_bStartupColdFailed;
	}
	if ( m_bStartupColdFailed ) {
		return false;
	}
	const uint64_t nPrepareStartedUsec = t3dRdStartupTicksUsec();
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare] BEGIN generation=", nStartupGeneration );
	uint64_t nPublicationPrepareUsec = 0u;
	++m_nStartupPublicationPrepareCount;
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare][WorldLimits] BEGIN generation=", nStartupGeneration );
	if ( !m_worldRuntime.configureLimitsCold( m_pTerrain->getWorldResourceLimitsInternal() ) ) {
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Unified world limits are invalid or already initialized";
		m_bStartupColdFailed = true;
		LOG( ERROR, "[Terrain3D][Startup][Coordinator][Prepare][WorldLimits] FAIL generation=", nStartupGeneration );
		return false;
	}
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare][WorldLimits] END generation=", nStartupGeneration );
	setUnifiedWorldFramePolicy( ( uint32_t ) m_pTerrain->getWorldUploadBudgetKb(),
								( uint32_t ) m_pTerrain->getWorldClusterUploadBudget(),
								( uint32_t ) m_pTerrain->getVtRequestBudget(),
								( uint32_t ) m_pTerrain->getVtAdmissionBudget(),
								false );
	// shutdown() deliberately destroys retained publication backing. Re-bind the
	// explicit Terrain3D owner before any cold capacity is re-established.
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare][Allocator] BEGIN generation=", nStartupGeneration );
	if ( !m_bRuntimeAllocatorBound || !m_pRuntimeAllocatorZone ||
		 !bindRuntimeAllocator( m_pRuntimeAllocatorZone, m_eRuntimeAllocatorOverflowPolicy ) ) {
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "RD publication owner allocator is not bound";
		m_bStartupColdFailed = true;
		LOG( ERROR, "[Terrain3D][Startup][Coordinator][Prepare][Allocator] FAIL generation=", nStartupGeneration );
		return false;
	}
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare][Allocator] END generation=", nStartupGeneration );
	{
		const uint64_t nPublicationSegmentStartedUsec = t3dRdStartupTicksUsec();
		LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare][FixedStorage] BEGIN generation=", nStartupGeneration );
		Terrain3DColdPhaseScope cold_phase_scope;
#if defined( T3D_EDITOR_NATIVE )
		if ( m_pAuthoring != NULL ) {
			m_pAuthoring->m_cooker.shutdownCold( m_terrainMegaVT );
		}
#endif
		m_terrainMegaVT.resetCold();
		m_publicationBuilder.invalidateTerrainMegaVTPersistence();
		if ( !m_publicationBuilder.initializeFixedStorageCold() ) {
			nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
			m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
			Terrain3DScopedLock lock( m_diagnosticsMutex );
			m_initializationError = "RD publication owner could not establish fixed region-map storage";
			m_bStartupColdFailed = true;
			LOG( ERROR,
				 "[Terrain3D][Startup][Coordinator][Prepare][FixedStorage] FAIL generation=",
				 nStartupGeneration );
			return false;
		}
		nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
		LOG( INFO,
			 "[Terrain3D][Startup][Coordinator][Prepare][FixedStorage] END generation=",
			 nStartupGeneration,
			 " elapsed=",
			 t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec,
			 " us" );
	}
	OS *const  os = OS::get_singleton();
	const bool bCompareCulling = os && os->get_environment( "TERRAIN3D_RD_COMPARE_CULL" ) == "1";
	const bool bCompareInstanceCulling =
		bCompareCulling || ( os && os->get_environment( "TERRAIN3D_RD_INSTANCE_COMPARE_CULL" ) == "1" );
	const bool bDirectDebug = os && os->get_environment( "TERRAIN3D_RD_DIRECT_DRAW" ) == "1";
	m_bCompositorDebugEnabled = false; // All geometry uses native World rendering.
	m_bMotionDebugEnabled = os && os->get_environment( "TERRAIN3D_RD_DEBUG_MOTION" ) == "1";
	m_renderer.setMotionDebugEnabled( m_bMotionDebugEnabled );
	m_renderer.setCullingComparisonEnabled( bCompareCulling );
	m_renderer.setDirectDrawDebugEnabled( bDirectDebug );
	const bool bInstanceDirectDebug = os && os->get_environment( "TERRAIN3D_RD_INSTANCE_DIRECT_DRAW" ) == "1";
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nInstanceInitializationAttempts = 0u;
	m_nInstanceInitializationFailures = 0u;
	m_nInstanceInitializationRecoveries = 0u;
	m_nLastInstanceInitializationRenderFrame = UINT64_MAX;
	m_nLastOceanInitializationRenderFrame = UINT64_MAX;
	m_forwardPassPreparer.reset();
	m_bFailureLatched = false;
	m_nFailureLatchTerrainTextureGeneration = 0u;
	m_nFailureLatchMaterialTextureGeneration = 0u;
	const uint64_t nPublicationSegmentStartedUsec = t3dRdStartupTicksUsec();
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare][Publication] BEGIN generation=", nStartupGeneration );
	if ( !m_pTerrain->getMesher() ||
		 !m_renderer.prepareStripParts( m_pTerrain->getMesher()->getClipmapStripParts( LOD0_MESH ),
										m_pTerrain->getMesher()->getClipmapStripPartCount( LOD0_MESH ),
										m_pTerrain->getMesher()->getClipmapStripParts( OUTER_RING_MESH ),
										m_pTerrain->getMesher()->getClipmapStripPartCount( OUTER_RING_MESH ) ) ) {
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Failed to prepare procedural RenderingDevice clipmap strip descriptors";
	}
	initializeOcean();
	// The coordinator owns these retained publication banks. Establish their
	// initial capacity before the first snapshot instead of treating capacity=0
	// as a runtime miss and waiting for the first physics-frame cold service.
	serviceColdCapacity();
	if ( !m_publicationBuilder.hasRequiredInstancePublicationCapacity( publicationServices() ) ||
		 !m_publicationBuilder.hasRequiredInstanceMeshBankCapacity() ) {
		nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
		m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "RD publication owner could not establish initial capacity";
		m_bStartupColdFailed = true;
		return false;
	}
	publishSnapshot();
	// The first cold publication may discover immutable mesh-source sizes that
	// were unavailable before publishSnapshot(). If so, satisfy that owner request
	// immediately and retry before initialization is exposed to READY/physics.
	if ( m_publicationBuilder.isInstancePublicationCapacityPending() ||
		 m_publicationBuilder.isInstanceMeshBankCapacityPending() ) {
		serviceColdCapacity();
		if ( m_publicationBuilder.isInstancePublicationCapacityPending() ||
			 m_publicationBuilder.isInstanceMeshBankCapacityPending() ) {
			nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
			m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
			Terrain3DScopedLock lock( m_diagnosticsMutex );
			m_initializationError = "RD publication owner could not satisfy discovered initial capacity";
			m_bStartupColdFailed = true;
			return false;
		}
		publishSnapshot();
	}
	nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
	m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
	LOG( INFO,
		 "[Terrain3D][Startup][Coordinator][Prepare][Publication] END generation=",
		 m_nStartupGeneration,
		 " elapsed=",
		 m_nStartupPublicationPrepareUsecLast,
		 " us" );
	const Ref< Terrain3DAssets >	 assets = m_pTerrain->assetsResource();
	const t3dRdMeshSourceSnapshot_t &instanceMeshes = m_publicationBuilder.getCachedInstanceMeshes();
	const bool						 bHasInstanceVtMaterials = !instanceMeshes.m_vtStableKeys.empty();
	const bool bHasTerrainVtMaterials = assets.is_valid() && assets->usesPackedPbr() && assets->getTextureCount() > 0;
	const bool bHasSharedVtMaterials = bHasInstanceVtMaterials || bHasTerrainVtMaterials;
	stageAssetVtDemandIndexCold(); // Also record the preview-to-production asset identity.
	String vtError;
	LOG( INFO,
		 "[Terrain3D][Startup][Coordinator][Prepare][VTPackage] BEGIN generation=",
		 nStartupGeneration,
		 " required=",
		 bHasSharedVtMaterials );
	const bool bUseCookedRuntimeVt =
#if defined( T3D_EDITOR_NATIVE )
		!m_pTerrain->isEditorAuthoringMode();
#else
		true;
#endif
	if ( bUseCookedRuntimeVt ) {
		if ( bHasSharedVtMaterials && !loadRuntimeVirtualTexturePackageFileCold( &vtError ) ) {
			m_sharedVtRuntime.virtualTexture().mark_startup_failed( m_nStartupGeneration );
			Terrain3DScopedLock lock( m_diagnosticsMutex );
			m_initializationError = vtError;
			m_bStartupColdFailed = true;
			LOG( ERROR,
				 "[Terrain3D][Startup][Coordinator][Prepare][VTPackage] FAIL generation=",
				 nStartupGeneration,
				 " error=",
				 vtError );
			return false;
		}
	}
#if defined( T3D_EDITOR_NATIVE )
	else if ( Engine::get_singleton() != NULL && Engine::get_singleton()->is_editor_hint() ) {
		Terrain3DData *const pSource = m_pTerrain->dataObject();
		if ( pSource != NULL ) {
			m_pAuthoring->m_cooker.restoreCachedRegionsCold(
				{ *pSource,
				  m_pTerrain->getDataDirectory(),
				  t3dMemoryCookingLimit( m_pAuthoring->m_cooker.getAuthoringMemoryAccountCold() ) },
				m_terrainMegaVT.getTexelsPerMeter() );
		}
		if ( assets.is_valid() && !preparePreviewAuthoringPackageCold( assets, &vtError ) ) {
			LOG( WARN,
				 "[RD instances] Editor VT package unavailable; continuing with neutral material backing: ",
				 vtError );
		}
	}
#endif
	LOG( INFO, "[Terrain3D][Startup][Coordinator][Prepare][VTPackage] END generation=", nStartupGeneration );
	m_bStartupColdPrepared = true;
	updateRenderRoute();
	if ( m_bCompositorDebugEnabled ) {
		attachEffectToActiveCamera();
	}
	LOG( INFO,
		 "[Terrain3D][Startup][Coordinator][Prepare] END generation=",
		 nStartupGeneration,
		 " elapsed=",
		 t3dRdStartupTicksUsec() - nPrepareStartedUsec,
		 " us" );
	return true;
}

void Terrain3DRenderCoordinator::shutdown(
	void ) {
#if defined( T3D_EDITOR_NATIVE )
	if ( m_pAuthoring != NULL ) {
		m_pAuthoring->m_cooker.shutdownCold( m_terrainMegaVT );
	}
#endif
	const bool						 bStartupBegun = m_bStartupColdBegun.load( T3D_MEMORY_ORDER_ACQUIRE );
	const bool						 bActivationCommitted = m_bStartupActivationCommitted;
	const t3dRenderLifecycleStatus_t renderStatusBeforeShutdown = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool						 bRenderTeardownRequired =
		( renderStatusBeforeShutdown.m_nReadyMask & T3D_RENDER_READY_RESOURCE_CONSUMERS ) != 0u;

	// P4: external route -> generation invalidation -> child cancellation ->
	// render acknowledgement/drain -> owner reset. Never mutate generation-bound
	// child state before the coordinator generation has been invalidated.
	m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}
	const uint32_t nShutdownGeneration = m_nStartupGeneration.exchange( 0u, T3D_MEMORY_ORDER_ACQ_REL );
	const bool	   bStartupInFlight = nShutdownGeneration != 0u && bStartupBegun && !bActivationCommitted;
	if ( bStartupInFlight ) {
		++m_nStartupCancellationCount;
		m_shaderLibrary.cancelLoadCold( nShutdownGeneration );
		m_sharedVtRuntime.virtualTexture().cancel_startup_cold( nShutdownGeneration );
	}
	const bool bCancellationAcknowledged =
		nShutdownGeneration == 0u || queueStartupCancellationAckCold( nShutdownGeneration, true );

	// The render acknowledgement has now passed every previously queued startup
	// callback for the invalid generation. Owner/control-plane bookkeeping can be
	// reset without racing those callbacks.
	m_bStartupActivationPrepared = false;
	m_bStartupActivationCommitted = false;
	m_nStartupActivationSnapshotGeneration = 0u;
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdBegun.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdFailed.store( !bCancellationAcknowledged, T3D_MEMORY_ORDER_RELEASE );

	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}
	// Remove the Forward+ publication before RD teardown. detachCoordinator()
	// includes its own render synchronization so an acquired callback cannot retain
	// this coordinator after the bridge pointer is cleared.
	detachForwardBridge();

	// P5: one complete RD destruction implementation, render thread only. There is
	// no owner-thread renderer shutdown fallback. A pristine/CPU-only coordinator
	// has no RD renderer state to schedule, which is important for the initial
	// beginStartupCold()->shutdown() normalization path before the effect exists.
	if ( !bRenderTeardownRequired ) {
		t3dRenderLifecycleStatus_t status{};
		status.m_nStartupGeneration = nShutdownGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
		publishRenderLifecycleStatusCold( status );
	} else if ( Terrain3DRdContext::isOnRenderThread() ) {
		shutdownRendererLifecycleOnRenderThread( nShutdownGeneration );
		++m_nStartupRenderDrainCount;
	} else if ( m_effect.is_valid() &&
				Terrain3DRdContext::enqueueOnRenderThread(
					callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::shutdownRendererLifecycleOnRenderThread )
						.bind( ( int64_t ) nShutdownGeneration ) ) ) {
		Terrain3DRdContext::forceSync( T3D_RD_FORCE_SYNC_CONTROL );
		++m_nStartupRenderDrainCount;
	} else {
		RenderingServer *const	   pServer = RenderingServer::get_singleton();
		t3dRenderLifecycleStatus_t status{};
		status.m_nStartupGeneration = nShutdownGeneration;
		if ( pServer == NULL ) {
			// The engine rendering boundary is already gone. Do not manufacture an
			// owner-thread RD teardown; device/server shutdown owns remaining handles.
			status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
			publishRenderLifecycleStatusCold( status );
		} else {
			// RenderingServer still exists, so failing to schedule the sole render-
			// thread teardown path is an invariant failure. Fail closed and drain any
			// already queued work without touching RD resources on this thread.
			status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
			status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_TEARDOWN_SCHEDULING;
			publishRenderLifecycleStatusCold( status );
			m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
			Terrain3DRdContext::forceSync( T3D_RD_FORCE_SYNC_CONTROL );
			Terrain3DScopedLock lock( m_diagnosticsMutex );
			m_initializationError = "Render-thread renderer teardown could not be scheduled";
		}
	}

	// With successful render teardown (or after the engine rendering boundary has
	// disappeared) queued render work can no longer observe these owner records.
	m_previewService.resetOwnerCold();
	m_staticScene.reset();
	if ( m_effect.is_valid() ) {
		m_effect->setCoordinator( NULL );
	}
	detachEffect();
	m_effect.unref();
	m_materialStateBuilder.clear();
	m_shaderLibrary.clear();
	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );

	m_publicationBuilder.clearCachedSources();
	resetSnapshotPublication();
	m_bCompositorDebugEnabled = false;
	m_bMotionDebugEnabled = false;
	m_pTerrain = NULL;
#if defined( T3D_EDITOR_NATIVE )
	if ( m_pAuthoring != NULL ) {
		memdelete( m_pAuthoring );
		m_pAuthoring = NULL;
	}
#endif
}

void Terrain3DRenderCoordinator::serviceShaderStartupCold(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	const uint32_t				nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		return;
	}
	m_shaderLibrary.serviceLoadCold();
	if ( m_shaderLibrary.isFailed( nStartupGeneration ) ) {
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
		return;
	}
	if ( !m_bStartupColdPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ) ||
		 !m_shaderLibrary.isCpuReady( nStartupGeneration ) ||
		 m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) || m_effect.is_null() ) {
		return;
	}
	bool expected = false;
	if ( !m_bShaderGpuStartupQueued.compare_exchange_strong( expected,
															 true,
															 T3D_MEMORY_ORDER_ACQ_REL,
															 T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return;
	}
	m_shaderLibrary.markStartupGpuPending( nStartupGeneration );
	LOG( INFO, "[Terrain3D][Startup][CoordinatorService][GpuQueue] BEGIN generation=", nStartupGeneration,
		 " gpu_stage=", startupGpuStageName( ( t3dStartupGpuStage_t ) m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE ) ) );
	if ( !queueStartupGpuServiceOnRenderThread( nStartupGeneration ) ) {
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
		status.m_nStartupGeneration = nStartupGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_REQUEUE;
		publishRenderLifecycleStatusCold( status );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Failed queuing initial budgeted GPU startup service";
		LOG( ERROR, "[Terrain3D][Startup][CoordinatorService][GpuQueue] FAIL generation=", nStartupGeneration );
	}
}

bool Terrain3DRenderCoordinator::queueStartupGpuServiceOnRenderThread(
	const uint32_t nStartupGeneration ) {
	if ( m_effect.is_null() ) {
		return false;
	}
	return Terrain3DRdContext::enqueueOnRenderThread(
		callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::serviceShaderStartupOnRenderThread )
			.bind( ( int64_t ) nStartupGeneration ) );
}

bool Terrain3DRenderCoordinator::serviceStartupGpuOnRenderThreadBudgeted(
	const uint32_t nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif

	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		return true;
	}
	{
		t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
		if ( status.m_nStartupGeneration != nStartupGeneration ) {
			status = t3dRenderLifecycleStatus_t{};
			status.m_nStartupGeneration = nStartupGeneration;
			status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
			publishRenderLifecycleStatusOnRenderThread( status );
		}
	}
	if ( m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return true;
	}

	bool	   bReady = true;
	const auto active_stage = ( t3dStartupGpuStage_t ) m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE );
	switch ( active_stage ) {
	case T3D_STARTUP_GPU_VT_STORAGE: {
		// All descriptor consumers, including World Hi-Z startup, require the
		// shared cache. The budgeted startup path bypasses the legacy
		// ensureSharedVtRuntimeCold()/initializeRendererOnRenderThread() paths,
		// so initialize it here before the first GPU resources are published.
		Terrain3DRdContext	   &rdContext = Terrain3DRdContext::getShared();
		t3dRdDescriptorStatus_t descriptorStatus = T3D_RD_DESCRIPTOR_INVALID_ARGUMENT;
		if ( !rdContext.initialize() || !rdContext.getGlobalDescriptorCacheCold( 256u, 96u, descriptorStatus ) ) {
			UtilityFunctions::push_error( "[Terrain3D][Startup][GPU][DescriptorCache] Initialization failed status=",
										  ( int64_t ) descriptorStatus,
										  " available_registry_slots=",
										  ( int64_t ) rdContext.getAvailableResourceSlots(),
										  " rd_failure=",
										  rdContext.getLastFailure() );
			bReady = false;
			break;
		}
		// Size the COLD backing before shader prewarm or clipmap setup.
		const uint32_t nColdBudget =
			Terrain3DWorldResources::estimateSharedColdArenaBytes( m_worldRuntime.getLimits() );
		bReady = nColdBudget != 0u && rdContext.initializeSharedArenas( nColdBudget ) &&
				 rdContext.arena( Terrain3DRdContext::COLD ).capacityBytes() >= nColdBudget &&
				 m_sharedVtRuntime.service_shared_vt_storage_cold( rdContext, T3D_RENDER_COORDINATOR_WORK_UNIT_BUDGET );
		if ( bReady && m_sharedVtRuntime.virtualTexture().is_gpu_storage_ready() ) {
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_VT_STORAGE );
			++m_nStartupGpuStage;
		}
		break;
	}
	case T3D_STARTUP_GPU_VT_EXECUTABLE:
		bReady = m_sharedVtRuntime.service_shared_vt_executable_cold( T3D_RENDER_COORDINATOR_WORK_UNIT_BUDGET );
		if ( bReady && m_sharedVtRuntime.virtualTexture().is_gpu_ready() ) {
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_VT_EXECUTABLE );
			++m_nStartupGpuStage;
		}
		break;
	case T3D_STARTUP_GPU_VT_PUBLICATION:
		bReady = m_sharedVtRuntime.service_shared_vt_publication_cold( T3D_RENDER_COORDINATOR_UPLOAD_BYTE_BUDGET,
																	   T3D_RENDER_COORDINATOR_WORK_UNIT_BUDGET );
		if ( bReady && m_sharedVtRuntime.are_shared_resources_ready() ) {
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_VT_PUBLISHED );
			m_previewService.noteGpuPackageOnRenderThread(
				m_sharedVtRuntime.virtualTexture().getPackageGeneration(),
				m_sharedVtRuntime.virtualTexture().getPackageContentSignature() );
			++m_nStartupGpuStage;
		}
		break;
	case T3D_STARTUP_GPU_SHADER_PREWARM:
		bReady = m_shaderLibrary.servicePhysicalPrewarm( Terrain3DRdContext::getShared(),
														 T3D_RENDER_COORDINATOR_WORK_UNIT_BUDGET );
		if ( bReady && m_shaderLibrary.isPhysicalReady() ) {
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_SHADER_GPU );
			++m_nStartupGpuStage;
		}
		break;
	case T3D_STARTUP_GPU_TERRAIN_RENDERER:
		if ( !m_bStartupTerrainRendererPrepared ) {
			Terrain3DColdPhaseScope cold_phase_scope;
			m_renderer.setFeedbackOwner( &m_sharedVtRuntime.virtualTexture().get_gpu() );
			m_bStartupTerrainRendererPrepared = m_renderer.initialize();
			bReady = m_bStartupTerrainRendererPrepared;
		}
		if ( bReady ) {
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_TERRAIN );
			++m_nStartupGpuStage;
		}
		break;
	case T3D_STARTUP_GPU_WORLD_RUNTIME:
		if ( !m_bStartupWorldRuntimePrepared ) {
			Terrain3DColdPhaseScope cold_phase_scope;
			m_bStartupWorldRuntimePrepared = m_worldRuntime.initializeCold( m_renderer.usesPersistentFrameBuffer(),
																			m_renderer.usesPersistentCpuBuffers() );
			bReady = m_bStartupWorldRuntimePrepared;
		}
		if ( bReady ) {
			m_bRendererInitialized.store( true, T3D_MEMORY_ORDER_RELEASE );
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_WORLD );
			++m_nStartupGpuStage;
		}
		break;
	case T3D_STARTUP_GPU_INSTANCE_RENDERER:
		// Retained enum slot for serialized startup diagnostics. No instance GPU lane.
		m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		++m_nStartupGpuStage;
		break;
	case T3D_STARTUP_GPU_OCEAN_RENDERER: {
		Terrain3DColdPhaseScope cold_phase_scope;
		const bool				bOceanReady = m_oceanRenderer.initialize();
		m_bOceanRendererInitialized.store( bOceanReady, T3D_MEMORY_ORDER_RELEASE );
		if ( !bOceanReady ) {
			bReady = false;
		} else {
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_OCEAN );
			++m_nStartupGpuStage;
		}
		break;
	}
	case T3D_STARTUP_GPU_COMPLETE:
		if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
			return true;
		}
		m_shaderLibrary.markStartupGpuReady( nStartupGeneration );
		setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_SHADER_GPU );
		if ( ( snapshotRenderLifecycleStatusOnOwnerThread().m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) !=
			 T3D_RENDER_READY_STARTUP_REQUIRED ) {
			bReady = false;
			break;
		}
		m_bStartupGpuServiceComplete.store( true, T3D_MEMORY_ORDER_RELEASE );
		++m_nStartupRendererInitializeSuccessCount;
		return true;
	default:
		bReady = false;
		break;
	}
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		return true;
	}
	if ( !bReady ) {
		String detail;
		if ( active_stage == T3D_STARTUP_GPU_TERRAIN_RENDERER ) {
			detail = m_renderer.getLastError();
		} else if ( active_stage == T3D_STARTUP_GPU_SHADER_PREWARM ) {
			detail = m_shaderLibrary.getLastError();
		} else if ( active_stage == T3D_STARTUP_GPU_WORLD_RUNTIME ) {
			const char *const pFailure = m_worldRuntime.getLastInitializationFailure();
			if ( pFailure ) {
				detail = String( "WorldRuntime initialization failed: " ) + pFailure;
			}
		}
		if ( detail.is_empty() ) {
			detail = Terrain3DRdContext::getShared().getLastFailure();
		}
		LOG( ERROR, "[Terrain3D][Startup][GPU][ServiceFailure] generation=", nStartupGeneration,
			 " service=", startupGpuStageName( active_stage ),
			 " detail=", detail,
			 " vt_storage_ready=", m_sharedVtRuntime.virtualTexture().is_gpu_storage_ready(),
			 " vt_executable_ready=", m_sharedVtRuntime.virtualTexture().is_gpu_ready(),
			 " shader_physical_ready=", m_shaderLibrary.isPhysicalReady(),
			 " terrain_renderer_prepared=", m_bStartupTerrainRendererPrepared,
			 " world_runtime_prepared=", m_bStartupWorldRuntimePrepared,
			 " required_ready_mask=", T3D_RENDER_READY_STARTUP_REQUIRED,
			 " actual_ready_mask=", snapshotRenderLifecycleStatusOnOwnerThread().m_nReadyMask );
		m_nStartupGpuStage.store( T3D_STARTUP_GPU_FAILED, T3D_MEMORY_ORDER_RELEASE );
		const bool	   bRecovery = m_bRecoveryInProgress.exchange( false, T3D_MEMORY_ORDER_ACQ_REL );
		const uint64_t nRecoveryRevision = m_nRecoveryActiveRevision.exchange( 0u, T3D_MEMORY_ORDER_ACQ_REL );
		if ( !bRecovery ) {
			m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		}
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		t3dRenderLifecycleStatus_t failure = snapshotRenderLifecycleStatusOnOwnerThread();
		failure.m_nStartupGeneration = nStartupGeneration;
		failure.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		failure.m_nFailureCode =
			bRecovery ? T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_INITIALIZATION : T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_GPU;
		failure.m_nRecoveryRevision = nRecoveryRevision;
		failure.m_nPreparedWorldPublicationGeneration = 0u;
		publishRenderLifecycleStatusOnRenderThread( failure );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		if ( !detail.is_empty() ) {
			m_initializationError = detail;
		}
		if ( m_initializationError.is_empty() )
			m_initializationError = bRecovery ? String( "Budgeted render-thread recovery work unit failed" )
											  : String( "Budgeted render-thread startup work unit failed" );
		return false;
	}
	return true;
}

void Terrain3DRenderCoordinator::serviceShaderStartupOnRenderThread(
	const uint32_t nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	if ( m_shaderLibrary.isFailed( nStartupGeneration ) || !m_shaderLibrary.isCpuReady( nStartupGeneration ) ) {
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	m_bShaderGpuStartupService.store( true, T3D_MEMORY_ORDER_RELEASE );
	const uint64_t nStartedUsec = t3dRdStartupTicksUsec();
	++m_nStartupGpuServiceCount;
	++m_nStartupRendererInitializeAttemptCount;
	const t3dStartupGpuStage_t eStageBefore =
		( t3dStartupGpuStage_t ) m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE );
	LOG( INFO,
		 "[Terrain3D][Startup][GPU][",
		 startupGpuStageName( eStageBefore ),
		 "] BEGIN generation=",
		 nStartupGeneration,
		 " callback=",
		 m_nStartupGpuServiceCount );
	const bool bReady = serviceStartupGpuOnRenderThreadBudgeted( nStartupGeneration );
	m_nStartupGpuServiceUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
	m_nStartupRendererInitializeUsecLast = m_nStartupGpuServiceUsecLast;
	m_nStartupRendererInitializeUsecTotal += m_nStartupGpuServiceUsecLast;
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	const t3dStartupGpuStage_t eStageAfter =
		( t3dStartupGpuStage_t ) m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( !bReady ) {
		String detail;
		{
			Terrain3DScopedLock lock( m_diagnosticsMutex );
			detail = m_initializationError;
		}
		LOG( ERROR,
			 "[Terrain3D][Startup][GPU][",
			 startupGpuStageName( eStageBefore ),
			 "] FAIL generation=",
			 nStartupGeneration,
			 " callback=",
			 m_nStartupGpuServiceCount,
			 " elapsed=",
			 m_nStartupGpuServiceUsecLast,
			 " us next_stage=",
			 startupGpuStageName( eStageAfter ),
			 " reason=",
			 detail );
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	LOG( INFO,
		 "[Terrain3D][Startup][GPU][",
		 startupGpuStageName( eStageBefore ),
		 m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) || eStageAfter != eStageBefore
			 ? "] END generation="
			 : "] WAIT generation=",
		 nStartupGeneration,
		 " callback=",
		 m_nStartupGpuServiceCount,
		 " elapsed=",
		 m_nStartupGpuServiceUsecLast,
		 " us next_stage=",
		 startupGpuStageName( eStageAfter ) );

	if ( m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		const uint64_t nRecoveryRevision = m_nRecoveryActiveRevision.load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( m_bRecoveryInProgress.load( T3D_MEMORY_ORDER_ACQUIRE ) && nRecoveryRevision != 0u ) {
			( void ) completeRendererRecoveryOnRenderThread( nStartupGeneration, nRecoveryRevision );
		}
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}

	// P6: after the first owner-thread kick, each bounded render callback schedules
	// the next bounded unit itself. The queued token remains owned until a terminal
	// state so owner physics cadence cannot insert bubbles between GPU work units.
	if ( queueStartupGpuServiceOnRenderThread( nStartupGeneration ) ) {
		return;
	}

	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupGpuStage.store( T3D_STARTUP_GPU_FAILED, T3D_MEMORY_ORDER_RELEASE );
	const bool bRecovery = m_bRecoveryInProgress.exchange( false, T3D_MEMORY_ORDER_ACQ_REL );
	m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	if ( !bRecovery ) {
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
	m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	publishRenderLifecycleFailureOnRenderThread( nStartupGeneration,
												 bRecovery ? T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_SCHEDULING
														   : T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_REQUEUE );
	{
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError =
			bRecovery ? String( "Render-thread recovery could not self-requeue the next bounded GPU startup unit" )
					  : String( "Render-thread startup could not self-requeue the next bounded GPU startup unit" );
	}
}

void Terrain3DRenderCoordinator::serviceStartupWorldPublicationOnRenderThread(
	const uint32_t nStartupGeneration,
	const uint64_t nSnapshotGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return;
	}
	const uint64_t						  nStartedUsec = t3dRdStartupTicksUsec();
	const t3dRdFrameSnapshotSlot_t *const pPublication = acquireSnapshotOnRenderThread();
	const bool							  bSnapshotMatches =
		pPublication != NULL && pPublication->m_snapshot.m_nPublicationGeneration == nSnapshotGeneration;
	const t3dWorldPublicationBank_t *const pWorld =
		bSnapshotMatches ? m_publicationBuilder.getWorldPublicationBank( pPublication->m_nWorldPublicationBank ) : NULL;
	const bool bWorldValid = pWorld != NULL && pWorld->m_publication.m_nGeneration != 0u;
	bool	   bPrepared = false;
	if ( bWorldValid ) {
		Terrain3DColdPhaseScope cold_phase_scope;
		bPrepared =
			m_worldRuntime.preparePublicationCold( pWorld->m_publication, 0u, &m_sharedVtRuntime.virtualTexture() );
	}
	const uint64_t nElapsedUsec = t3dRdStartupTicksUsec() - nStartedUsec;
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return;
	}
	if ( !bSnapshotMatches || !bWorldValid || !bPrepared || !m_worldRuntime.isReady() ) {
		m_nStartupWorldPublicationPrepareUsecLast.store( nElapsedUsec, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		publishRenderLifecycleFailureOnRenderThread( nStartupGeneration,
													 T3D_RENDER_LIFECYCLE_FAILURE_WORLD_PUBLICATION );
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast =
			m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
		LOG( ERROR,
			 "[Terrain3D][Startup][Activation][FAIL][WorldPublication] generation=",
			 nStartupGeneration,
			 " requested_snapshot_generation=",
			 nSnapshotGeneration,
			 " acquired_snapshot_generation=",
			 pPublication ? pPublication->m_snapshot.m_nPublicationGeneration : 0u,
			 " world_valid=",
			 bWorldValid,
			 " world_generation=",
			 pWorld ? pWorld->m_publication.m_nGeneration : 0u,
			 " prepared=",
			 bPrepared,
			 " world_ready=",
			 m_worldRuntime.isReady(),
			 " elapsed=",
			 nElapsedUsec,
			 " us" );
		logStartupActivationFailureState( "world_publication_prepare", nStartupGeneration );
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation render-thread unified-world publication preparation failed";
		return;
	}
	m_nStartupWorldPublicationGeneration.store( pWorld->m_publication.m_nGeneration, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( nElapsedUsec, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( true, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( lifecycle.m_nStartupGeneration != nStartupGeneration ) {
		lifecycle = t3dRenderLifecycleStatus_t{};
		lifecycle.m_nStartupGeneration = nStartupGeneration;
	}
	lifecycle.m_nPreparedWorldPublicationGeneration = pWorld->m_publication.m_nGeneration;
	lifecycle.m_eStage =
		( lifecycle.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) == T3D_RENDER_READY_STARTUP_REQUIRED
			? T3D_RENDER_LIFECYCLE_READY
			: T3D_RENDER_LIFECYCLE_FAILED;
	lifecycle.m_nFailureCode = lifecycle.m_eStage == T3D_RENDER_LIFECYCLE_READY
								   ? T3D_RENDER_LIFECYCLE_FAILURE_NONE
								   : T3D_RENDER_LIFECYCLE_FAILURE_WORLD_PUBLICATION;
	publishRenderLifecycleStatusOnRenderThread( lifecycle );
	LOG( INFO,
		 "[Terrain3D][Startup][Activation] WORLD PUBLICATION READY generation=",
		 nStartupGeneration,
		 " snapshot_generation=",
		 nSnapshotGeneration,
		 " world_publication_generation=",
		 pWorld->m_publication.m_nGeneration,
		 " elapsed=",
		 nElapsedUsec,
		 " us" );
}

void Terrain3DRenderCoordinator::update(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !m_pTerrain ) {
		return;
	}
	serviceShaderStartupCold();
	m_previewService.servicePreviewReadbackWatchdogCold( previewServices() );
#if defined( T3D_EDITOR_NATIVE )
	serviceAuthoringUpdateCold();
#endif
	publishSnapshot();
	if ( m_pTerrain->isEditorAuthoringMode() && Engine::get_singleton() != NULL &&
		 Engine::get_singleton()->is_editor_hint() &&
		 ( m_publicationBuilder.isInstancePublicationCapacityPending() ||
		   m_publicationBuilder.isInstanceMeshBankCapacityPending() ) ) {
		// Asset batches discover immutable mesh-bank requirements during publish.
		// Grow and republish immediately in authoring instead of leaving a frame
		// with the old bank and servicing it on the next scheduler tick.
		m_publicationBuilder.serviceColdCapacity( publicationServices() );
		publishSnapshot();
	}
	const uint64_t			   nTerrainTextureGeneration = m_publicationBuilder.getTerrainTextureGeneration();
	const uint64_t			   nMaterialTextureGeneration = m_publicationBuilder.getMaterialTextureGeneration();
	t3dRenderLifecycleStatus_t recoveryStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( recoveryStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED && !m_bFailureLatched ) {
		m_bFailureLatched = true;
		m_nFailureLatchTerrainTextureGeneration = nTerrainTextureGeneration;
		m_nFailureLatchMaterialTextureGeneration = nMaterialTextureGeneration;
		m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
		if ( m_effect.is_valid() ) {
			m_effect->set_enabled( false );
		}
	}
	if ( m_bFailureLatched ) {
		const bool bRecoveryReady = recoveryStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY &&
									recoveryStatus.m_nRecoveryRevision != 0u &&
									recoveryStatus.m_nRecoveryRevision == m_nRecoveryQueuedRevision;
		if ( bRecoveryReady ) {
			m_bFailureLatched = false;
			m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
			m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
			m_nRecoveryQueuedRevision = 0u;
			++m_nRendererRecoveryCount;
			LOG( INFO, "[RD terrain] Render-thread recovery READY revision=", recoveryStatus.m_nRecoveryRevision );
		} else if ( recoveryStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED ) {
			const bool bRecoveryAttemptFailed = recoveryStatus.m_nRecoveryRevision != 0u &&
												recoveryStatus.m_nRecoveryRevision == m_nRecoveryQueuedRevision;
			if ( bRecoveryAttemptFailed ) {
				m_nFailureLatchTerrainTextureGeneration = nTerrainTextureGeneration;
				m_nFailureLatchMaterialTextureGeneration = nMaterialTextureGeneration;
				m_nRecoveryQueuedRevision = 0u;
			} else {
				const bool bResourceGenerationAdvanced =
					nTerrainTextureGeneration != m_nFailureLatchTerrainTextureGeneration ||
					nMaterialTextureGeneration != m_nFailureLatchMaterialTextureGeneration;
				if ( bResourceGenerationAdvanced && m_nRecoveryQueuedRevision == 0u ) {
					m_nRecoveryRequestRevision = advanceGeneration( m_nRecoveryRequestRevision );
					if ( queueRendererRecoveryCold( m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ),
													m_nRecoveryRequestRevision ) ) {
						m_nRecoveryQueuedRevision = m_nRecoveryRequestRevision;
						LOG( INFO,
							 "[RD terrain] Resource generation advanced; queued render recovery revision=",
							 m_nRecoveryQueuedRevision );
					} else {
						m_nFailureLatchTerrainTextureGeneration = nTerrainTextureGeneration;
						m_nFailureLatchMaterialTextureGeneration = nMaterialTextureGeneration;
					}
				}
			}
		}
	}
	updateRenderRoute();
	if ( m_bCompositorDebugEnabled && !m_bFailureLatched ) {
		attachEffectToActiveCamera();
	} else if ( !m_bCompositorDebugEnabled ) {
		detachEffect();
	}
}

bool Terrain3DRenderCoordinator::queueRendererRecoveryCold(
	const uint32_t nStartupGeneration,
	const uint64_t nRecoveryRevision ) {
	if ( nStartupGeneration == 0u || nRecoveryRevision == 0u || m_effect.is_null() ) {
		return false;
	}
	if ( Terrain3DRdContext::enqueueOnRenderThread(
			 callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::recoverRendererLifecycleOnRenderThread )
				 .bind( ( int64_t ) nStartupGeneration, ( int64_t ) nRecoveryRevision ) ) ) {
		return true;
	}
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nStartupGeneration = nStartupGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_SCHEDULING;
	status.m_nRecoveryRevision = nRecoveryRevision;
	publishRenderLifecycleStatusCold( status );
	{
		Terrain3DScopedLock lock( m_diagnosticsMutex );
		m_initializationError = "Render-thread renderer recovery could not be scheduled";
	}
	return false;
}

bool Terrain3DRenderCoordinator::completeRendererRecoveryOnRenderThread(
	const uint32_t nStartupGeneration,
	const uint64_t nRecoveryRevision ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	const t3dRdFrameSnapshotSlot_t *const  pPublication = acquireSnapshotOnRenderThread();
	const t3dWorldPublicationBank_t *const pWorld =
		pPublication ? m_publicationBuilder.getWorldPublicationBank( pPublication->m_nWorldPublicationBank ) : NULL;
	const bool bWorldPrepared =
		pWorld != NULL && pWorld->m_publication.m_nGeneration != 0u &&
		m_worldRuntime.preparePublicationCold( pWorld->m_publication, 0u, &m_sharedVtRuntime.virtualTexture() );
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nStartupGeneration = nStartupGeneration;
	status.m_nRecoveryRevision = nRecoveryRevision;
	if ( !bWorldPrepared ||
		 ( status.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) != T3D_RENDER_READY_STARTUP_REQUIRED ) {
		status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_INITIALIZATION;
		publishRenderLifecycleStatusOnRenderThread( status );
		m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	status.m_nPreparedWorldPublicationGeneration = pWorld->m_publication.m_nGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_READY;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	publishRenderLifecycleStatusOnRenderThread( status );
	m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

void Terrain3DRenderCoordinator::recoverRendererLifecycleOnRenderThread(
	const uint32_t nStartupGeneration,
	const uint64_t nRecoveryRevision ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	if ( nStartupGeneration == 0u || nRecoveryRevision == 0u ||
		 m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) != nStartupGeneration ||
		 !m_bStartupActivationCommitted ) {
		return;
	}
	m_bRecoveryInProgress.store( true, T3D_MEMORY_ORDER_RELEASE );
	m_nRecoveryActiveRevision.store( nRecoveryRevision, T3D_MEMORY_ORDER_RELEASE );
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nStartupGeneration = nStartupGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_RECOVERING;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	status.m_nRecoveryRevision = nRecoveryRevision;
	status.m_nReadyMask &=
		~( T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD | T3D_RENDER_READY_INSTANCES | T3D_RENDER_READY_OCEAN );
	publishRenderLifecycleStatusOnRenderThread( status );

	m_worldRuntime.shutdownCold();
	m_environmentField.resetAfterRendererTeardownCold();
	m_oceanRenderer.shutdown();
	m_renderer.shutdown();
	resetForwardLifecycle();
	m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bOceanRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );

	if ( !initializeRendererOnRenderThread( UINT64_MAX ) ||
		 !completeRendererRecoveryOnRenderThread( nStartupGeneration, nRecoveryRevision ) ) {
		t3dRenderLifecycleStatus_t failure = snapshotRenderLifecycleStatusOnOwnerThread();
		failure.m_nStartupGeneration = nStartupGeneration;
		failure.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		failure.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_INITIALIZATION;
		failure.m_nRecoveryRevision = nRecoveryRevision;
		publishRenderLifecycleStatusOnRenderThread( failure );
		m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
}

#if defined( T3D_EDITOR_NATIVE )
// GPU resources are allocated, read back, and freed by the existing shared RD
// owner. They are deliberately not exposed through the editor attachment.
bool Terrain3DRenderCoordinator::beginEditorBrushStrokeCold(
    const Terrain3DEditorBrushTransaction &rTransaction,
    const PackedByteArray &rSource, const PackedByteArray &rMaskBits,
    const uint32_t nMaskWidth, const uint32_t nMaskHeight ) {
    if ( !m_effect.is_valid() || rSource.is_empty() ||
         rTransaction.state() != T3D_EDITOR_BRUSH_ACTIVE ||
         rTransaction.payloadBytes() != ( uint32_t ) rSource.size() ||
         !Terrain3DRdContext::getShared().isReady() ||
         rTransaction.deviceGeneration() != Terrain3DRdContext::getShared().getDeviceGeneration() ||
         rTransaction.strokeId() > INT64_MAX ||
         nMaskWidth == 0u || nMaskHeight == 0u ||
         uint64_t( nMaskWidth ) * nMaskHeight > 1024u * 1024u ||
         uint64_t( nMaskWidth ) * nMaskHeight * 4u != ( uint64_t ) rMaskBits.size() ) return false;
    {
        Terrain3DScopedLock lock( m_editorBrushMutex );
        if ( m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_IDLE ||
             rTransaction.strokeId() <= m_nLastEditorBrushStrokeId ) return false;
        m_nLastEditorBrushStrokeId = rTransaction.strokeId();
        m_editorBrushTransaction = rTransaction;
        m_editorBrushInitial = rSource;
        m_editorBrushMaskUpload = rMaskBits;
        m_nEditorBrushMaskWidth = nMaskWidth;
        m_nEditorBrushMaskHeight = nMaskHeight;
        m_editorBrushCompleted.clear();
        m_bEditorBrushGpuPending = false;
        m_bEditorBrushOutputProduced = false;
        m_bEditorBrushReadbackInFlight = false;
    }
    const bool queued = Terrain3DRdContext::enqueueOnRenderThread(
        callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::initializeEditorBrushOnRenderThread )
        .bind( ( int64_t ) rTransaction.strokeId() ) );
    if ( !queued ) {
        Terrain3DScopedLock lock( m_editorBrushMutex );
        m_editorBrushTransaction.reset();
        m_editorBrushInitial.clear();
    }
    return queued;
}

bool Terrain3DRenderCoordinator::prepareEditorBrushComputeOnRenderThread() {
    Terrain3DRdContext &context = Terrain3DRdContext::getShared();
    RenderingDevice *const rd = context.getDevice();
    if ( !rd ) return false;
    if ( !m_editorBrushShader.isValid() && !m_editorBrushShader.createTracked( context,
         T3D_RD_CONTEXT_OWNER_TERRAIN, "T3D.Editor.Brush.Shader", T3D_RD_SHADER_EDITOR_BRUSH_TILES ) ) return false;
    if ( !m_editorBrushPipeline.isValid() && !m_editorBrushPipeline.createTrackedCompute( context,
         T3D_RD_CONTEXT_OWNER_TERRAIN, "T3D.Editor.Brush.Pipeline", m_editorBrushShader.rid() ) ) return false;
    const RID shader = m_editorBrushShader.rid();
    const RID working = m_editorBrushWorking.rid();
    const RID tiles = m_editorBrushTileTable.rid();
    const RID mask = m_editorBrushMask.rid();
    TypedArray< Ref< RDUniform > > bindings;
    bindings.push_back( Terrain3DRdBindingBuilder::storage( 0, working ) );
    bindings.push_back( Terrain3DRdBindingBuilder::storage( 1, tiles ) );
    bindings.push_back( Terrain3DRdBindingBuilder::storage( 2, mask ) );
    m_hEditorBrushComputeSet = m_editorBrushSets.getOrCreate(
        {shader, working, tiles, mask}, "T3D.Editor.Brush.ComputeSet", bindings, shader, 0u );
    return m_hEditorBrushComputeSet.is_valid();
}

void Terrain3DRenderCoordinator::initializeEditorBrushOnRenderThread(
    const uint64_t nStrokeId ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
    Terrain3DScopedLock lock( m_editorBrushMutex );
    Terrain3DRdContext &context = Terrain3DRdContext::getShared();
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         ( m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_ACTIVE &&
           m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_SEALED ) ||
         m_editorBrushTransaction.deviceGeneration() != context.getDeviceGeneration() ) return;
    // Fixed, bounded buffer, permitting first-touch tile uploads during a
    // moving stroke without replacing its RID or descriptors.
    t3dRdGpuBufferSpec_t spec{};
    spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
    spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
    spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
    spec.m_growth = T3D_GPU_EXACT;
    spec.m_pName = "T3D.Editor.Brush.Working";
    if ( !m_editorBrushWorking.isConfigured() && !m_editorBrushWorking.configure( spec ) ) {
        m_editorBrushTransaction.fail(); return;
    }
    spec.m_pName = "T3D.Editor.Brush.Tiles";
    if ( !m_editorBrushTileTable.isConfigured() && !m_editorBrushTileTable.configure( spec ) ) {
        m_editorBrushTransaction.fail(); return;
    }
    spec.m_pName = "T3D.Editor.Brush.Mask";
    if ( !m_editorBrushMask.isConfigured() && !m_editorBrushMask.configure( spec ) ) {
        m_editorBrushTransaction.fail(); return;
    }
    PackedByteArray payload;
    payload.resize( T3D_EDITOR_BRUSH_MAX_TRANSFER_BYTES );
    memset( payload.ptrw(), 0, ( size_t ) payload.size() );
    memcpy( payload.ptrw(), m_editorBrushInitial.ptr(), ( size_t ) m_editorBrushInitial.size() );
    PackedByteArray tileBytes;
    tileBytes.resize( sizeof( t3dEditorBrushGpuTile_t ) * T3D_EDITOR_BRUSH_MAX_TILES + 65536u * 4u );
    memset( tileBytes.ptrw(), 0, ( size_t ) tileBytes.size() );
    for ( uint32_t i = 0u; i < m_editorBrushTransaction.tileCount(); ++i ) {
        const t3dEditorBrushTile_t &tile = m_editorBrushTransaction.tile( i );
        const t3dEditorBrushGpuTile_t entry{tile.m_nRegionX, tile.m_nRegionY, tile.m_nByteOffset,
            tile.m_nEncoding, tile.m_nMinX, tile.m_nMinY, tile.m_nWidth, tile.m_nHeight};
        memcpy( tileBytes.ptrw() + i * sizeof( entry ), &entry, sizeof( entry ) );
        const uint32_t address = ( uint32_t ) ( ( ( tile.m_nRegionY + 16 ) * 8 + tile.m_nMinY / 16 ) * 256 +
            ( tile.m_nRegionX + 16 ) * 8 + tile.m_nMinX / 16 );
        if ( address < 65536u ) {
            const uint32_t index = i + 1u;
            memcpy( tileBytes.ptrw() + sizeof( t3dEditorBrushGpuTile_t ) * T3D_EDITOR_BRUSH_MAX_TILES + address * 4u,
                &index, 4u );
        }
    }
    if ( !m_editorBrushWorking.createTracked( context, ( uint32_t ) payload.size(), payload ) ||
         !m_editorBrushTileTable.createTracked( context, ( uint32_t ) tileBytes.size(), tileBytes ) ||
         !m_editorBrushMask.createTracked( context, ( uint32_t ) m_editorBrushMaskUpload.size(), m_editorBrushMaskUpload ) ||
         !prepareEditorBrushComputeOnRenderThread() ) {
        m_editorBrushTransaction.fail();
        m_editorBrushSets.clear();
        m_editorBrushWorking.releaseTracked( context );
        m_editorBrushTileTable.releaseTracked( context );
        m_editorBrushMask.releaseTracked( context );
        return;
    }
    m_editorBrushPushBytes.resize( sizeof( t3dEditorBrushGpuPush_t ) );
    m_bEditorBrushGpuPending = true;
    m_renderer.setEditorBrushOverlayOnRenderThread( m_editorBrushWorking.rid(), m_editorBrushTileTable.rid() );
    m_editorBrushInitial.clear();
    m_editorBrushMaskUpload.clear();
}

void Terrain3DRenderCoordinator::uploadEditorBrushTileOnRenderThread(
    const uint64_t nStrokeId, const uint32_t nTile, const PackedByteArray &rSource ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
    Terrain3DScopedLock lock( m_editorBrushMutex );
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         ( m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_ACTIVE &&
           m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_SEALED ) ||
         nTile >= m_editorBrushTransaction.tileCount() || !m_bEditorBrushGpuPending ) return;
    const t3dEditorBrushTile_t &tile = m_editorBrushTransaction.tile( nTile );
    const uint32_t bytes = tile.m_nHeight * tile.m_nRowStrideBytes;
    if ( ( uint32_t ) rSource.size() != bytes || tile.m_nByteOffset + bytes > m_editorBrushWorking.capacityBytes() ) {
        m_editorBrushTransaction.fail(); return;
    }
    RenderingDevice *const rd = Terrain3DRdContext::getShared().getDevice();
    if ( !rd || rd->buffer_update( m_editorBrushWorking.rid(), tile.m_nByteOffset, bytes, rSource ) != OK ) {
        m_editorBrushTransaction.fail(); return;
    }
    const t3dEditorBrushGpuTile_t entry{tile.m_nRegionX, tile.m_nRegionY, tile.m_nByteOffset,
        tile.m_nEncoding, tile.m_nMinX, tile.m_nMinY, tile.m_nWidth, tile.m_nHeight};
    PackedByteArray tileBytes;
    tileBytes.resize( sizeof( entry ) );
    memcpy( tileBytes.ptrw(), &entry, sizeof( entry ) );
    if ( rd->buffer_update( m_editorBrushTileTable.rid(), nTile * sizeof( entry ), sizeof( entry ), tileBytes ) != OK ) {
        m_editorBrushTransaction.fail(); return;
    }
    if ( tile.m_nRegionX < -16 || tile.m_nRegionX > 15 ||
         tile.m_nRegionY < -16 || tile.m_nRegionY > 15 ) { m_editorBrushTransaction.fail(); return; }
    const uint32_t lookupIndex = ( ( tile.m_nRegionY + 16 ) * 8 + tile.m_nMinY / 16 ) * 256u +
        ( tile.m_nRegionX + 16 ) * 8u + tile.m_nMinX / 16;
    const uint32_t index = nTile + 1u;
    PackedByteArray lookupBytes;
    lookupBytes.resize( 4 );
    memcpy( lookupBytes.ptrw(), &index, 4 );
    if ( rd->buffer_update( m_editorBrushTileTable.rid(),
            sizeof( t3dEditorBrushGpuTile_t ) * T3D_EDITOR_BRUSH_MAX_TILES + lookupIndex * 4u,
            4u, lookupBytes ) != OK ) m_editorBrushTransaction.fail();
}

void Terrain3DRenderCoordinator::executeEditorBrushCommandOnRenderThread(
    const uint64_t nStrokeId, const uint32_t nCommand ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
    Terrain3DScopedLock lock( m_editorBrushMutex );
    Terrain3DRdContext &context = Terrain3DRdContext::getShared();
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         ( m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_ACTIVE &&
           m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_SEALED ) ||
         m_editorBrushTransaction.deviceGeneration() != context.getDeviceGeneration() ||
         nCommand >= m_editorBrushTransaction.commandCount() || !m_bEditorBrushGpuPending ) return;
    const t3dEditorBrushCommand_t &cmd = m_editorBrushTransaction.command( nCommand );
    // AVERAGE, gradient and filtered texture/color painting retain CPU parity
    // until they have a complete shader-side source/filter implementation.
    const bool control = cmd.m_nMapType == 1u && ( cmd.m_nTool == 3u || cmd.m_nTool == 6u || cmd.m_nTool == 7u || cmd.m_nTool == 8u ) &&
        ( cmd.m_nOperation <= 1u || ( cmd.m_nTool == 3u && cmd.m_nOperation == 2u ) );
    const bool height = cmd.m_nMapType == 0u && ( cmd.m_nTool == 1u || cmd.m_nTool == 2u ) &&
        ( cmd.m_nOperation <= 1u || cmd.m_nOperation == 4u );
    const bool color = cmd.m_nMapType == 2u && ( cmd.m_nTool == 4u || cmd.m_nTool == 5u ) && cmd.m_nOperation <= 1u;
    if ( ( !control && !height && !color ) ||
         cmd.m_nMaskWidth != m_nEditorBrushMaskWidth || cmd.m_nMaskHeight != m_nEditorBrushMaskHeight ) {
        m_editorBrushTransaction.fail(); return;
    }
    RenderingDevice *const rd = context.getDevice();
    const RID descriptor = m_editorBrushSets.resolve( m_hEditorBrushComputeSet );
    if ( !rd || !descriptor.is_valid() || !m_editorBrushPipeline.isValid() ) {
        m_editorBrushTransaction.fail(); return;
    }
    t3dEditorBrushGpuPush_t push{};
    push.m_nTileCount = cmd.m_nTileCountAtEnqueue;
    push.m_nMapType = cmd.m_nMapType;
    push.m_nTool = cmd.m_nTool;
    push.m_nOperation = cmd.m_nOperation;
    push.m_fCenterX = cmd.m_fCenterX;
    push.m_fCenterZ = cmd.m_fCenterZ;
    push.m_fBrushSize = cmd.m_fRadius * 2.f;
    push.m_fStrength = cmd.m_fStrength;
    push.m_fRotationCos = cosf( cmd.m_fRotation );
    push.m_fRotationSin = sinf( cmd.m_fRotation );
    push.m_fVertexSpacing = cmd.m_fVertexSpacing;
    push.m_fSelectedHeight = cmd.m_fSelectedHeight;
    push.m_nMaskWidth = cmd.m_nMaskWidth;
    push.m_nMaskHeight = cmd.m_nMaskHeight;
    push.m_nFlags = cmd.m_nFlags;
    push.m_nColorRGBA8 = cmd.m_nPackedColor;
    push.m_fRoughness = cmd.m_fRoughness;
    push.m_fCenterHeight = cmd.m_fCenterHeight;
    push.m_nAssetId = cmd.m_nAssetId;
    push.m_nUvRotation = cmd.m_nUvRotation;
    push.m_nUvScale = cmd.m_nUvScale;
    push.m_fGradientX1 = cmd.m_fGradientX1;
    push.m_fGradientY1 = cmd.m_fGradientY1;
    push.m_fGradientZ1 = cmd.m_fGradientZ1;
    push.m_fGradientX2 = cmd.m_fGradientX2;
    push.m_fGradientY2 = cmd.m_fGradientY2;
    push.m_fGradientZ2 = cmd.m_fGradientZ2;
    push.m_fMotionX = cmd.m_fMotionX;
    push.m_fMotionZ = cmd.m_fMotionZ;
    memcpy( m_editorBrushPushBytes.ptrw(), &push, sizeof( push ) );
    const int64_t list = rd->compute_list_begin();
    if ( list == RenderingDevice::INVALID_ID ) { m_editorBrushTransaction.fail(); return; }
    rd->compute_list_bind_compute_pipeline( list, m_editorBrushPipeline.rid() );
    rd->compute_list_bind_uniform_set( list, descriptor, 0u );
    rd->compute_list_set_push_constant( list, m_editorBrushPushBytes, sizeof( push ) );
    rd->compute_list_dispatch( list, 4u, push.m_nTileCount, 1u );
    rd->compute_list_add_barrier( list );
    rd->compute_list_end();
    m_bEditorBrushOutputProduced = true;
}

t3dEditorBrushStrokeState_t Terrain3DRenderCoordinator::getEditorBrushStrokeStateCold(
    const uint64_t nStrokeId ) const {
    Terrain3DScopedLock lock( m_editorBrushMutex );
    return m_editorBrushTransaction.strokeId() == nStrokeId ?
        m_editorBrushTransaction.state() : T3D_EDITOR_BRUSH_IDLE;
}

bool Terrain3DRenderCoordinator::appendEditorBrushTileCold(
    const uint64_t nStrokeId, const t3dEditorBrushTile_t &rTile, const PackedByteArray &rSource ) {
    if ( !m_effect.is_valid() || nStrokeId > INT64_MAX || rTile.m_nMinX % 16u ||
         rTile.m_nMinY % 16u || rTile.m_nWidth != 16u || rTile.m_nHeight != 16u ||
         rSource.size() != ( int64_t ) ( rTile.m_nHeight * rTile.m_nRowStrideBytes ) ) return false;
    uint32_t tileIndex = 0u;
    {
        Terrain3DScopedLock lock( m_editorBrushMutex );
        if ( m_editorBrushTransaction.strokeId() != nStrokeId ) return false;
        tileIndex = m_editorBrushTransaction.tileCount();
        if ( !m_editorBrushTransaction.appendTile( rTile ) ) return false;
    }
    return Terrain3DRdContext::enqueueOnRenderThread(
        callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::uploadEditorBrushTileOnRenderThread )
        .bind( ( int64_t ) nStrokeId, ( int64_t ) tileIndex, rSource ) );
}

bool Terrain3DRenderCoordinator::enqueueEditorBrushCommandCold(
    const uint64_t nStrokeId, const t3dEditorBrushCommand_t &rCommand ) {
    if ( !m_effect.is_valid() || nStrokeId > INT64_MAX ) return false;
    uint32_t commandIndex = 0u;
    {
        Terrain3DScopedLock lock( m_editorBrushMutex );
        if ( m_editorBrushTransaction.strokeId() != nStrokeId ) return false;
        commandIndex = m_editorBrushTransaction.commandCount();
        if ( !m_editorBrushTransaction.acceptCommand( rCommand ) ) return false;
    }
    return Terrain3DRdContext::enqueueOnRenderThread(
        callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::executeEditorBrushCommandOnRenderThread )
        .bind( ( int64_t ) nStrokeId, ( int64_t ) commandIndex ) );
}

bool Terrain3DRenderCoordinator::sealEditorBrushStrokeCold( const uint64_t nStrokeId ) {
    if ( !m_effect.is_valid() || nStrokeId > INT64_MAX ) return false;
    {
        Terrain3DScopedLock lock( m_editorBrushMutex );
        // Dispatch and seal callbacks are FIFO on the shared render queue.
        // The render thread verifies at least one successful dispatch.
        if ( m_editorBrushTransaction.strokeId() != nStrokeId ) return false;
        if ( !m_editorBrushTransaction.seal() ) return false;
    }
    if ( Terrain3DRdContext::enqueueOnRenderThread(
        callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::sealEditorBrushOnRenderThread )
        .bind( ( int64_t ) nStrokeId ) ) ) return true;
    Terrain3DScopedLock lock( m_editorBrushMutex );
    m_editorBrushTransaction.fail();
    return false;
}

void Terrain3DRenderCoordinator::sealEditorBrushOnRenderThread( const uint64_t nStrokeId ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
    Terrain3DScopedLock lock( m_editorBrushMutex );
    Terrain3DRdContext &context = Terrain3DRdContext::getShared();
    RenderingDevice *const rd = context.getDevice();
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_SEALED ||
         m_editorBrushTransaction.deviceGeneration() != context.getDeviceGeneration() ||
         !m_bEditorBrushGpuPending || !m_bEditorBrushOutputProduced ||
         !rd || !m_editorBrushWorking.isValid() ) {
        if ( m_editorBrushTransaction.strokeId() == nStrokeId ) m_editorBrushTransaction.fail();
        return;
    }
    const uint32_t bytes = m_editorBrushTransaction.payloadBytes();
    t3dGpuTransferTicket_t ticket;
    if ( !Terrain3DGpuTransferBudget::get().reserve( T3D_GPU_TRANSFER_REGION, bytes, 1u, ticket ) ) {
        m_editorBrushTransaction.fail();
        return;
    }
    const Callable callback = callable_mp( m_effect.ptr(),
        &Terrain3DRdCompositorEffect::consumeEditorBrushReadbackOnRenderThread )
        .bind( ( int64_t ) nStrokeId, ( int64_t ) m_editorBrushTransaction.ownerGeneration(),
            ( int64_t ) m_editorBrushTransaction.deviceGeneration() );
    const Error result = rd->buffer_get_data_async( m_editorBrushWorking.rid(), callback, 0u, bytes );
    if ( result != OK ) {
        Terrain3DGpuTransferBudget::get().refund( ticket, bytes, 1u );
        m_editorBrushTransaction.fail();
        return;
    }
    m_editorBrushTransaction.markReadbackPending();
    m_bEditorBrushReadbackInFlight = true;
}

void Terrain3DRenderCoordinator::consumeEditorBrushReadbackOnRenderThread(
    const PackedByteArray &rBytes, const uint64_t nStrokeId,
    const uint32_t nOwnerGeneration, const uint64_t nDeviceGeneration ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
    Terrain3DScopedLock lock( m_editorBrushMutex );
    // A callback from a prior device/stroke must not touch the current slot.
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         m_editorBrushTransaction.ownerGeneration() != nOwnerGeneration ||
         m_editorBrushTransaction.deviceGeneration() != nDeviceGeneration ) {
        ++m_nEditorBrushRejectedCompletions;
        return;
    }
    m_bEditorBrushReadbackInFlight = false;
    const bool accepted = m_editorBrushTransaction.acceptReadback( nStrokeId,
        nOwnerGeneration, ( uint32_t ) nDeviceGeneration,
        ( uint32_t ) rBytes.size() );
    if ( !accepted ) ++m_nEditorBrushRejectedCompletions;
    else m_editorBrushCompleted = rBytes;
    m_renderer.setEditorBrushOverlayOnRenderThread( RID(), RID() );
    m_editorBrushSets.clear();
    Terrain3DRdContext &context = Terrain3DRdContext::getShared();
    if ( m_editorBrushMask.isValid() ) m_editorBrushMask.releaseTracked( context );
    if ( m_editorBrushTileTable.isValid() ) m_editorBrushTileTable.releaseTracked( context );
    if ( m_editorBrushWorking.isValid() ) m_editorBrushWorking.releaseTracked( context );
    m_hEditorBrushComputeSet = RID();
    m_bEditorBrushGpuPending = false;
    m_bEditorBrushOutputProduced = false;
    if ( m_editorBrushTransaction.state() == T3D_EDITOR_BRUSH_CANCELED ) {
        m_editorBrushTransaction.reset();
        m_editorBrushInitial.clear();
    } else if ( !accepted ) {
        m_editorBrushTransaction.fail();
    }
}

bool Terrain3DRenderCoordinator::takeEditorBrushReadbackCold(
    const uint64_t nStrokeId, Terrain3DEditorBrushTransaction &rTransaction,
    PackedByteArray &rBytes ) {
    Terrain3DScopedLock lock( m_editorBrushMutex );
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         !m_editorBrushTransaction.claimReadbackForCommit() ) return false;
    rTransaction = m_editorBrushTransaction;
    rBytes = m_editorBrushCompleted;
    return true;
}

bool Terrain3DRenderCoordinator::acknowledgeEditorBrushCommitCold( const uint64_t nStrokeId ) {
    Terrain3DScopedLock lock( m_editorBrushMutex );
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         !m_editorBrushTransaction.markCpuCommitted() ) return false;
    m_editorBrushTransaction.reset();
    m_editorBrushCompleted.clear();
    return true;
}

void Terrain3DRenderCoordinator::cancelEditorBrushStrokeCold( const uint64_t nStrokeId ) {
    if ( !nStrokeId || nStrokeId > INT64_MAX ) return;
    {
        Terrain3DScopedLock lock( m_editorBrushMutex );
        if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
             m_editorBrushTransaction.state() == T3D_EDITOR_BRUSH_IDLE ) return;
        m_editorBrushTransaction.cancel();
        m_editorBrushCompleted.clear();
    }
    // FIFO with any earlier init/seal; only the RD owner may free a live RID.
    if ( m_effect.is_valid() ) {
        ( void ) Terrain3DRdContext::enqueueOnRenderThread(
            callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::discardEditorBrushOnRenderThread )
            .bind( ( int64_t ) nStrokeId ) );
    }
}

void Terrain3DRenderCoordinator::discardEditorBrushOnRenderThread( const uint64_t nStrokeId ) {
#ifdef DEBUG_ENABLED
    ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
    Terrain3DScopedLock lock( m_editorBrushMutex );
    if ( m_editorBrushTransaction.strokeId() != nStrokeId ||
         m_editorBrushTransaction.state() != T3D_EDITOR_BRUSH_CANCELED ||
         m_bEditorBrushReadbackInFlight ) return;
    m_renderer.setEditorBrushOverlayOnRenderThread( RID(), RID() );
    m_editorBrushSets.clear();
    Terrain3DRdContext &context = Terrain3DRdContext::getShared();
    if ( m_editorBrushMask.isValid() ) m_editorBrushMask.releaseTracked( context );
    if ( m_editorBrushTileTable.isValid() ) m_editorBrushTileTable.releaseTracked( context );
    if ( m_editorBrushWorking.isValid() ) m_editorBrushWorking.releaseTracked( context );
    m_hEditorBrushComputeSet = RID();
    m_bEditorBrushGpuPending = false;
    m_bEditorBrushOutputProduced = false;
    m_editorBrushInitial.clear();
    m_editorBrushCompleted.clear();
    m_editorBrushTransaction.reset();
}

void Terrain3DRenderCoordinator::releaseEditorBrushOnRenderThread( void ) {
    Terrain3DScopedLock lock( m_editorBrushMutex );
    m_renderer.setEditorBrushOverlayOnRenderThread( RID(), RID() );
    m_editorBrushSets.clear();
    m_editorBrushPipeline.releaseTracked( Terrain3DRdContext::getShared() );
    m_editorBrushShader.releaseTracked( Terrain3DRdContext::getShared() );
    m_editorBrushTransaction.cancel();
    Terrain3DRdContext &context = Terrain3DRdContext::getShared();
    if ( m_editorBrushMask.isValid() ) m_editorBrushMask.releaseTracked( context );
    if ( m_editorBrushTileTable.isValid() ) m_editorBrushTileTable.releaseTracked( context );
    if ( m_editorBrushWorking.isValid() ) m_editorBrushWorking.releaseTracked( context );
    m_hEditorBrushComputeSet = RID();
    m_bEditorBrushGpuPending = false;
    m_bEditorBrushOutputProduced = false;
    m_editorBrushInitial.clear();
    m_editorBrushCompleted.clear();
    m_bEditorBrushReadbackInFlight = false;
    m_editorBrushTransaction.reset();
    // The callback carries a stroke + device token; any later completion is
    // discarded rather than being published into an owner map.
}
#endif

void Terrain3DRenderCoordinator::shutdownRendererOnRenderThread(
	const bool bPreserveTailStaging ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
#if defined( T3D_EDITOR_NATIVE )
    releaseEditorBrushOnRenderThread();
#endif
	m_previewService.shutdownRendererOnRenderThread( previewServices() );
	m_worldRuntime.shutdownCold();
	m_environmentField.resetAfterRendererTeardownCold();
	// Live VT replacement reuses the large CPU upload buffers. Full lifecycle
	// shutdown still releases them through the default argument.
	m_sharedVtRuntime.shutdown_cold( bPreserveTailStaging );
	m_oceanRenderer.shutdown();
	m_renderer.shutdown();
	resetForwardLifecycle();
	m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bOceanRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	lifecycle.m_nReadyMask &= T3D_RENDER_READY_SHADER_GPU;
	lifecycle.m_nSharedResourceClass = ( uint32_t ) T3D_VT_SHARED_RESOURCES_NONE;
	lifecycle.m_nPreparedWorldPublicationGeneration = 0u;
	publishRenderLifecycleStatusOnRenderThread( lifecycle );
}

void Terrain3DRenderCoordinator::shutdownRendererLifecycleOnRenderThread(
	const uint32_t nShutdownGeneration ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	shutdownRendererOnRenderThread();
	// The render-thread shutdown acknowledgement fences all previous draws.
	// Ordinary lane recovery intentionally keeps its reservations instead.
	m_worldRuntime.releaseArenaSpansFinal();
	m_renderer.releaseArenaSpansFinal();
	m_shaderLibrary.releasePhysical( Terrain3DRdContext::getShared() );
	t3dRenderLifecycleStatus_t status{};
	status.m_nStartupGeneration = nShutdownGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::renderPreviewOnRenderThread(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_previewService.renderPreviewOnRenderThread( previewServices() );
}

Ref< Image > Terrain3DRenderCoordinator::createMeshPreview(
	const uint16_t				 nAssetId,
	const Vector2i				&rSize,
	t3dRdPreviewFailure_t *const pFailure ) {
	return m_previewService.createMeshPreview( previewServices(), nAssetId, rSize, pFailure );
}

bool Terrain3DRenderCoordinator::requestMeshPreviewAsync(
	const uint16_t				 nAssetId,
	const Vector2i				&rSize,
	const uint64_t				 nCompletionOwnerId,
	const uint64_t				 nCompletionGeneration,
	t3dRdPreviewFailure_t *const pFailure ) {
	return m_previewService.requestMeshPreviewAsync( previewServices(),
													 nAssetId,
													 rSize,
													 nCompletionOwnerId,
													 nCompletionGeneration,
													 pFailure );
}

void Terrain3DRenderCoordinator::renderPreviewAsyncOnRenderThread(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_previewService.renderPreviewAsyncOnRenderThread( previewServices() );
}

void Terrain3DRenderCoordinator::consumePreviewReadbackOnRenderThread(
	const uint32_t		   nSlot,
	const uint64_t		   nRequestGeneration,
	const PackedByteArray &rReadbackBytes ) {
	m_previewService.consumePreviewReadbackOnRenderThread( previewServices(),
														   nSlot,
														   nRequestGeneration,
														   rReadbackBytes );
}

void Terrain3DRenderCoordinator::render(
	RenderData *const pRenderData ) {
	// Legacy CompositorEffect drawing was replaced by the native unified World pass.
	// Retain the callback entry point for the compositor object lifetime contract.
	( void ) pRenderData;
}

bool Terrain3DRenderCoordinator::publishStaticWorldCold(
	const uint64_t					 nProducerId,
	const Terrain3DWorldPublication &rWorld ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	return m_staticScene.publishCold( nProducerId, rWorld );
}
void Terrain3DRenderCoordinator::releaseStaticSceneDatabaseCold(
	const uint64_t nProducerId ) {
	m_staticScene.releaseCold( nProducerId );
}

bool Terrain3DRenderCoordinator::isStaticWorldReadyCold(
	const uint64_t nProducerId ) {
	if ( !m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE ) ||
		 m_bRendererFailed.load( T3D_MEMORY_ORDER_ACQUIRE ) ||
		 m_bRecoveryInProgress.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return false;
	}
	return m_staticScene.isReadyCold( nProducerId );
}

void Terrain3DRenderCoordinator::publishStaticWorldReadinessOnRenderThread(
	const Terrain3DWorldPublication &rWorld,
	const bool						 bReady ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	// preparePublicationCold may return true while retaining an older upload.
	// Only the exact uploaded generation with prepared raster bindings can hand off.
	const bool bExactReady =
		bReady && m_worldRuntime.isReady() && m_worldRuntime.getPreparedPublicationGeneration() == rWorld.m_nGeneration;
	m_staticScene.acknowledge( rWorld, bExactReady );
}

void Terrain3DRenderCoordinator::resetForwardLifecycle(
	void ) {
	m_forwardPassPreparer.reset();
}

void Terrain3DRenderCoordinator::consumeDescriptorFenceOnRenderThread(
	const PackedByteArray &rBytes, const uint64_t nEpoch,
	const uint64_t nDeviceGeneration, const uint64_t nTicket ) {
	Terrain3DRdGlobalDescriptorCache *const pCache =
		Terrain3DRdContext::getShared().tryGetGlobalDescriptorCacheHot();
	if ( pCache ) {
		pCache->consumeCompletionReadbackOnRenderThread( rBytes, nEpoch, nDeviceGeneration, nTicket );
	}
}

bool Terrain3DRenderCoordinator::renderForwardPass(
	const t3dRdForwardPassContext_t &rContext ) {
	// PREPARE and SUBMIT have different CPU cost profiles. Keep the legacy
	// forward_pass_preparation counter, but limit it to actual PREPARE callbacks.
	if ( rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE ) {
		Terrain3DRdGlobalDescriptorCache *const pCache =
			Terrain3DRdContext::getShared().tryGetGlobalDescriptorCacheHot();
		if ( pCache ) {
			if ( m_effect.is_valid() && !pCache->hasCompletionCallbackHot() ) {
				pCache->setCompletionCallbackCold( callable_mp( m_effect.ptr(),
					&Terrain3DRdCompositorEffect::consumeDescriptorFenceOnRenderThread ) );
			}
			pCache->beginRenderFrameHot( rContext.m_nRenderFrame );

		}
	}
	const t3dStreamProfileStage_t stage =
		rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE ? T3D_SP_FORWARD_PASS_PREPARE : T3D_SP_FORWARD_PASS_SUBMIT;
	Terrain3DStreamingProfileScope forward_scope( stage );
	Terrain3DMemoryAccountScope	   memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	t3dForwardPassServices_t	   services{ m_publicationBuilder,
										 m_renderer,
										 m_oceanRenderer,
										 m_environmentField,
										 m_worldRuntime,
										 m_sharedVtRuntime.virtualTexture(),
										 m_sharedVtRuntime,
										 m_bRenderEnabled,
										 m_bRendererInitialized,
										 m_bInstanceRendererInitialized,
										 m_bOceanRendererInitialized,
										 m_bRendererFailed,
										 m_nConsecutiveRenderFailures,
										 m_nWorldUploadBudgetKbPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ),
										 m_nWorldClusterBudgetPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ),
										 m_bWorldBoundaryDeferredPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ),
										 m_nVtRequestBudgetPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ),
										 m_nVtAdmissionBudgetPolicy.load( T3D_MEMORY_ORDER_ACQUIRE ) };
	const bool					   bResult = m_forwardPassPreparer.render( *this, services, rContext );
	{
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_FORWARD_DIAGNOSTICS );
		publishDiagnosticsOnRenderThread( rContext.m_nRenderFrame );
	}
	return bResult;
}

size_t Terrain3DRenderCoordinator::getRetainedCpuBytes(
	void ) const {
	return m_diagnosticsPublisher.getRetainedCpuBytes( diagnosticInputs() ) + m_staticScene.getRetainedCpuBytes();
}

Dictionary Terrain3DRenderCoordinator::getForwardLaneDiagnostics(
	void ) const {
	return m_diagnosticsPublisher.getForwardLaneDiagnostics( diagnosticInputs() );
}

bool Terrain3DRenderCoordinator::publishDiagnosticsOnRenderThread(
	const uint64_t nFrame ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	if ( m_nLastDiagnosticsFrameOnRenderThread != UINT64_MAX && nFrame >= m_nLastDiagnosticsFrameOnRenderThread &&
		 nFrame - m_nLastDiagnosticsFrameOnRenderThread < 16u ) {
		return false;
	}
	m_nLastDiagnosticsFrameOnRenderThread = nFrame;
	Terrain3DVirtualTexture	 &vt = m_sharedVtRuntime.virtualTexture();
	t3dPublishedDiagnostics_t sample;
	sample.nRenderFrame = nFrame;
	sample.vt = vt.get_final_telemetry_reference_debug();
	sample.bVtPackageFileBacked = vt.hasFileBackedPackageOnRenderThread();
	sample.nVtPackageMetadataBytes = vt.getPackageMetadataBytesOnRenderThread();
	sample.nVtPackageFileBytes = vt.getPackageFileBytesOnRenderThread();
	const t3dVTGpuStats_t &gpu = vt.get_gpu().get_stats();
	sample.nFeedbackAttempts = gpu.nRequestAttempts;
	sample.nFragmentFaults = gpu.nFragmentFaultCount;
	sample.nFeedbackAccepted = gpu.nAcceptedRequestCount;
	sample.nPredictionIncompleteFallbacks = vt.getPredictionIncompleteFallbacksOnRenderThread();
	sample.bPredictionCurrentCellComplete = vt.isPredictionCurrentCellCompleteOnRenderThread();
    sample.nMaskParamUploadsSaved = vt.get_gpu().get_predicted_mask_uploads_saved();
    sample.bEnvironmentEnabled = Terrain3DRdShaderLibrary::isEnvironmentFieldsEnabledCold();
    sample.bEnvironmentReady = m_environmentField.isReady();
    sample.bEnvironmentFaulted = m_environmentField.isFaulted();
    sample.nEnvironmentGeneration = m_environmentField.getGeneration();
    sample.nEnvironmentFramesIntegrated = m_environmentField.getFramesIntegrated();
    sample.nEnvironmentImpulsesAccepted = m_environmentField.getImpulsesAccepted();
    sample.nEnvironmentIntegrationFailures = m_environmentField.getIntegrationFailures();
    sample.nEnvironmentTeleportsReset = m_environmentField.getTeleportsReset();
    sample.nEnvironmentTimestampPairs = m_environmentField.getTimestampPairs();
    sample.nEnvironmentTextureBytes = m_environmentField.getTextureBytes();
    sample.nEnvironmentClimateUploads = m_environmentField.getClimateUploads();
    sample.nEnvironmentWorldSetMisses = m_worldRuntime.getRaster().getEnvironmentStaleSetMisses();
    sample.nEnvironmentOceanSetMisses = m_oceanRenderer.getEnvironmentStaleSetMisses();
	const t3dVTStreamingState_t &stream = vt.get_streaming();
	sample.nActiveJobs = stream.Stats.nActiveJobCount;
	const t3dVTResidencyState_t &residency = vt.get_residency();
	sample.nResidencyHotScans = residency.Stats.nHotScanCount;
	sample.nResidencyWarmScans = residency.Stats.nWarmScanCount;
	sample.nResidencyColdScans = residency.Stats.nColdScanCount;
	sample.nResidencyReservationFailures = residency.Stats.nReservationFailureCount;
	sample.nResidencyRetireDeferred = residency.Stats.nRetireDeferredCount;
	sample.nResidencyThrashWindowFrames = residency.Policy.nThrashWindowFrames;
	m_shaderLibrary.capturePhysicalCountersOnRenderThread( sample.nShaderPhysicalReady,
														   sample.nShaderPhysicalExpected,
														   sample.nShaderPhysicalMisses );
	if ( Terrain3DRdGlobalDescriptorCache *const pCache =
		Terrain3DRdContext::getShared().tryGetGlobalDescriptorCacheHot() ) {
		sample.descriptorCache = pCache->diagnostics();
	}
	Terrain3DScopedLock lock( m_publishedDiagnosticsMutex );
	m_publishedDiagnostics = sample;
	return true;
}

Dictionary Terrain3DRenderCoordinator::getDiagnostics(
	void ) const {
	t3dPublishedDiagnostics_t sample;
	{
		Terrain3DScopedLock lock( m_publishedDiagnosticsMutex );
		sample = m_publishedDiagnostics;
	}
	const t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	const t3dVTFinalTelemetry_t		&vt = sample.vt;
	Dictionary						 result;
	result["diagnostics_mode"] = "published_no_sync";
	Dictionary descriptor;
	const t3dRdGlobalDescriptorDiagnostics_t &dd = sample.descriptorCache;
	descriptor["slots_total"] = ( int64_t ) dd.m_nSlotsTotal;
	descriptor["slots_live"] = ( int64_t ) dd.m_nSlotsLive;
	descriptor["slots_peak"] = ( int64_t ) dd.m_nSlotsPeak;
	descriptor["retire_pending"] = ( int64_t ) dd.m_nRetirePending;
	descriptor["leases_live"] = ( int64_t ) dd.m_nLeasesLive;
	descriptor["cache_hits"] = ( int64_t ) dd.m_nCacheHits;
	descriptor["cache_misses"] = ( int64_t ) dd.m_nCacheMisses;
	descriptor["creates"] = ( int64_t ) dd.m_nCreates;
	descriptor["physical_releases"] = ( int64_t ) dd.m_nPhysicalReleases;
	descriptor["gpu_safe_retirements"] = ( int64_t ) dd.m_nGpuSafeRetirements;
	descriptor["capacity_denials"] = ( int64_t ) dd.m_nCapacityDenials;
	descriptor["invalid_dependency_rejects"] = ( int64_t ) dd.m_nInvalidDependencyRejects;
	descriptor["stale_lease_rejects"] = ( int64_t ) dd.m_nStaleLeaseRejects;
	descriptor["fence_submissions"] = ( int64_t ) dd.m_nFenceSubmissions;
	descriptor["fence_completions"] = ( int64_t ) dd.m_nFenceCompletions;
	descriptor["fence_submission_failures"] = ( int64_t ) dd.m_nFenceSubmissionFailures;
	descriptor["fence_readback_failures"] = ( int64_t ) dd.m_nFenceReadbackFailures;
	descriptor["fence_stale_callbacks"] = ( int64_t ) dd.m_nFenceStaleCallbacks;
	descriptor["active_submission_epoch"] = ( int64_t ) dd.m_nActiveSubmissionEpoch;
	descriptor["completed_submission_epoch"] = ( int64_t ) dd.m_nCompletedSubmissionEpoch;
	descriptor["fence_pending"] = dd.m_bFencePending;
    result["descriptor_cache"] = descriptor;
    Dictionary environment;
    environment["enabled"] = sample.bEnvironmentEnabled;
    environment["ready"] = sample.bEnvironmentReady;
    environment["faulted"] = sample.bEnvironmentFaulted;
    environment["generation"] = ( int64_t ) sample.nEnvironmentGeneration;
    environment["frames_integrated"] = ( int64_t ) sample.nEnvironmentFramesIntegrated;
    environment["impulses_accepted"] = ( int64_t ) sample.nEnvironmentImpulsesAccepted;
    environment["impulses_dropped"] = ( int64_t ) m_publicationBuilder.getEnvironmentDroppedCountOwner();
    environment["integration_failures"] = ( int64_t ) sample.nEnvironmentIntegrationFailures;
    environment["teleport_resets"] = ( int64_t ) sample.nEnvironmentTeleportsReset;
    environment["gpu_timestamp_pairs"] = ( int64_t ) sample.nEnvironmentTimestampPairs;
    environment["texture_bytes"] = ( int64_t ) sample.nEnvironmentTextureBytes;
    environment["height_contact_layers"] = 2;
    environment["height_contact_bytes"] = ( int64_t ) ( 2u * 1024u * 1024u );
    environment["region_climate_cells"] = ( int64_t ) T3D_REGION_SLOTS;
    environment["region_climate_cpu_bytes"] = ( int64_t ) ( T3D_REGION_SLOTS * sizeof( t3dEnvironmentRegionClimate_t ) );
    environment["region_climate_uploads"] = ( int64_t ) sample.nEnvironmentClimateUploads;
    environment["stale_binding_misses"] = ( int64_t ) (
        sample.nEnvironmentWorldSetMisses + sample.nEnvironmentOceanSetMisses );
    environment["world_stale_binding_misses"] = ( int64_t ) sample.nEnvironmentWorldSetMisses;
    environment["ocean_stale_binding_misses"] = ( int64_t ) sample.nEnvironmentOceanSetMisses;
    environment["gpu_time_available"] = false;
    environment["gpu_time_ms"] = -1.0; // There is no dedicated measured environment GPU timestamp yet.
    result["environment"] = environment;
	result["render_frame_sequence"] = ( int64_t ) sample.nRenderFrame;
	result["sync_count_by_reason"] = Terrain3DRdContext::getForceSyncDiagnostics();
	result["render_thread_queue_wait_usec"] = -1; // Not measured yet.
	const Dictionary transfers = Terrain3DRdContext::getShared().getGpuUploadDiagnostics();
	result["gpu_submitted_transfer_ops"] = transfers.get( "upload_calls_completed_frame", 0 );
	result["gpu_submitted_transfer_bytes"] = transfers.get( "bytes_uploaded_completed_frame", 0 );
	result["gpu_uploads"] = transfers;
	const t3dGpuTransferSnapshot_t admitted = Terrain3DGpuTransferBudget::get().snapshot();
	Dictionary					   sharedTransfers;
	sharedTransfers["window_usec"] = ( int64_t ) Terrain3DGpuTransferBudget::WINDOW_USEC;
	sharedTransfers["byte_limit"] = ( int64_t ) Terrain3DGpuTransferBudget::BYTE_LIMIT;
	sharedTransfers["call_limit"] = ( int64_t ) Terrain3DGpuTransferBudget::CALL_LIMIT;
	sharedTransfers["reserved_bytes"] = ( int64_t ) admitted.nBytes;
	sharedTransfers["reserved_calls"] = ( int64_t ) admitted.nCalls;
	sharedTransfers["peak_reserved_bytes"] = ( int64_t ) admitted.nWindowPeakBytes;
	sharedTransfers["peak_reserved_calls"] = ( int64_t ) admitted.nWindowPeakCalls;
	static const char *laneNames[T3D_GPU_TRANSFER_LANE_COUNT] = { "vt", "region", "world", "feedback" };
	for ( uint32_t i = 0u; i < T3D_GPU_TRANSFER_LANE_COUNT; ++i ) {
		const String lane( laneNames[i] );
		sharedTransfers[lane + String( "_bytes" )] = ( int64_t ) admitted.nLaneBytes[i];
		sharedTransfers[lane + String( "_calls" )] = ( int64_t ) admitted.nLaneCalls[i];
		sharedTransfers[lane + String( "_deferred_total" )] = ( int64_t ) admitted.nDeferred[i];
		sharedTransfers[lane + String( "_oversized_bypass_total" )] = ( int64_t ) admitted.nOversizedBypass[i];
		sharedTransfers[lane + String( "_accepted_bytes_total" )] = ( int64_t ) admitted.nAcceptedBytesTotal[i];
		sharedTransfers[lane + String( "_accepted_calls_total" )] = ( int64_t ) admitted.nAcceptedCallsTotal[i];
	}
	result["gpu_stream_transfer_budget"] = sharedTransfers;
	Dictionary		 renderer = transfers;
	const Dictionary resourceCounters = Terrain3DRdContext::getShared().getPublishedResourceDiagnostics();
	renderer.merge( resourceCounters, true );
	result["renderer"] = renderer;
	result["render_enabled"] = m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["renderer_initialized"] = m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["renderer_failure_latched"] = m_bRendererFailed.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["live_buffer_bytes"] = resourceCounters.get( "live_buffer_bytes", 0 );
	result["peak_buffer_bytes"] = resourceCounters.get( "peak_buffer_bytes", 0 );
	result["live_texture_bytes"] = resourceCounters.get( "live_texture_bytes", 0 );
	result["buffer_updates"] = resourceCounters.get( "buffer_updates", 0 );
	Dictionary shader;
	shader["physical_shader_ready"] = ( int64_t ) sample.nShaderPhysicalReady;
	shader["physical_shader_expected"] = ( int64_t ) sample.nShaderPhysicalExpected;
	shader["physical_shader_misses_after_ready"] = ( int64_t ) sample.nShaderPhysicalMisses;
	result["shader"] = shader;
	result["shaders"] = shader;
	result["instances"] = Dictionary();
	Dictionary world;
	world["ready"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_WORLD ) != 0u;
	result["world_renderer"] = world;
	result["shared_resource_class"] = ( int64_t ) lifecycle.m_nSharedResourceClass;
	result["shared_vt_resources_ready"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_VT_PUBLISHED ) != 0u;
	result["shared_vt_cpu_ready"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_VT_STORAGE ) != 0u;
	result["shared_vt_gpu_storage_ready"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_VT_STORAGE ) != 0u;
	result["shared_vt_gpu_executable_ready"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_VT_EXECUTABLE ) != 0u;
	result["shared_vt_initial_published"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_VT_PUBLISHED ) != 0u;
	result["vt_package_file_backed"] = sample.bVtPackageFileBacked;
	result["vt_package_metadata_bytes"] = ( int64_t ) sample.nVtPackageMetadataBytes;
	result["vt_package_file_bytes"] = ( int64_t ) sample.nVtPackageFileBytes;
	result["vt_physical_capacity"] = ( int64_t ) vt.nPhysicalCapacity;
	result["vt_physical_resident"] = ( int64_t ) vt.nPhysicalResident;
	result["vt_physical_free"] = ( int64_t ) vt.nPhysicalFree;
	result["vt_physical_retiring"] = ( int64_t ) vt.nPhysicalRetiring;
	result["vt_page_hits"] = ( int64_t ) vt.nPageHits;
	result["vt_page_misses"] = ( int64_t ) vt.nPageMisses;
	result["vt_parent_fallback_hits"] = ( int64_t ) vt.nParentFallbackHits;
	result["vt_tail_fallback_hits"] = ( int64_t ) vt.nTailFallbackHits;
	result["vt_request_attempts"] = ( int64_t ) vt.nRequestAttempts;
	result["vt_request_unique"] = ( int64_t ) vt.nRequestUnique;
	result["vt_request_same_key"] = ( int64_t ) vt.nRequestSameKey;
	result["vt_request_hash_overflow"] = ( int64_t ) vt.nRequestHashOverflow;
	result["vt_io_jobs_pending"] = ( int64_t ) vt.nIoJobsPending;
	result["vt_upload_page_sets_frame"] = ( int64_t ) vt.nUploadPageSetsFrame;
	result["vt_upload_bytes_frame"] = ( int64_t ) vt.nUploadBytesFrame;
	result["vt_upload_bytes_total"] = ( int64_t ) vt.nUploadBytesTotal;
	result["vt_evictions_frame"] = ( int64_t ) vt.nEvictionsFrame;
	result["vt_evictions_total"] = ( int64_t ) vt.nEvictionsTotal;
	result["vt_thrash_count"] = ( int64_t ) vt.nThrashCount;
	result["vt_resident_terrain"] = ( int64_t ) vt.nResidentTerrain;
	result["vt_resident_clutter"] = ( int64_t ) vt.nResidentClutter;
	result["vt_resident_ground_cover"] = ( int64_t ) vt.nResidentGroundCover;
	result["vt_resident_occluder"] = ( int64_t ) vt.nResidentOccluder;
	result["vt_resident_hlod"] = ( int64_t ) vt.nResidentHlod;
	result["vt_descriptor_rebuilds_runtime"] = ( int64_t ) vt.nDescriptorRebuildsRuntime;
	result["vt_gpu_resource_creates_after_seal"] = ( int64_t ) vt.nGpuResourceCreatesAfterSeal;
	result["vt_prediction_attempts"] = ( int64_t ) sample.nFeedbackAttempts;
	result["vt_fragment_faults"] = ( int64_t ) sample.nFragmentFaults;
	result["vt_feedback_accepted_requests"] = ( int64_t ) sample.nFeedbackAccepted;
	result["vt_prediction_incomplete_fallbacks"] = ( int64_t ) sample.nPredictionIncompleteFallbacks;
	result["vt_prediction_current_cell_complete"] = sample.bPredictionCurrentCellComplete;
	result["vt_feedback_mask_uploads_saved"] = ( int64_t ) sample.nMaskParamUploadsSaved;
	result["virtual_texture_stream_active_jobs"] = ( int64_t ) sample.nActiveJobs;
	result["virtual_texture_residency_hot_scans"] = ( int64_t ) sample.nResidencyHotScans;
	result["virtual_texture_residency_warm_scans"] = ( int64_t ) sample.nResidencyWarmScans;
	result["virtual_texture_residency_cold_scans"] = ( int64_t ) sample.nResidencyColdScans;
	result["virtual_texture_residency_reservation_failures"] = ( int64_t ) sample.nResidencyReservationFailures;
	result["virtual_texture_residency_retire_deferred"] = ( int64_t ) sample.nResidencyRetireDeferred;
	result["virtual_texture_residency_thrash_window_frames"] = ( int64_t ) sample.nResidencyThrashWindowFrames;
	{
		Terrain3DScopedLock lock( m_vtDemandIndexMutex );
		result["vt_demand_index_path"] = m_vtDemandIndexPath;
		result["vt_demand_index_status"] = m_vtDemandIndexStatus;
	}
#if defined( T3D_EDITOR_NATIVE )
    // Owner-side journal only: publish the event counters on diagnostics queries.
    // No RD lock, readback, sync or GPU resource creation is performed here.
    const t3dComposedSurfaceEditStats_t &cs = m_composedSurfaceEdits.statsCold();
    Dictionary composed;
    composed["enabled"] = false;
    composed["editor_backend"] = "live_until_parity_accepted";
    if ( m_pAuthoring ) {
        composed["reference_bake_state"] = m_pAuthoring->m_composedCandidateState;
        composed["reference_bake_pages"] = ( int64_t ) m_pAuthoring->m_composedCandidate.m_nPages;
        composed["reference_bake_bytes"] = ( int64_t ) m_pAuthoring->m_composedCandidate.m_nWrittenBytes;
        composed["reference_bake_error"] = m_pAuthoring->m_composedCandidate.m_error;
        composed["reference_bake_pending"] = m_pAuthoring->m_bComposedCandidatePending;
    }
    composed["authoritative_parity_ready"] = false;
    composed["dirty_regions"] = cs.m_nDirtyRegions;
    composed["edit_events"] = ( int64_t ) cs.m_nEditEvents;
    composed["source_invalidations"] = ( int64_t ) cs.m_nSourceInvalidations;
    composed["stale_discarded"] = ( int64_t ) cs.m_nStaleCompletions;
    composed["accepted_recooks"] = ( int64_t ) cs.m_nAcceptedCompletions;
    composed["removed_regions"] = ( int64_t ) cs.m_nRemovedRegions;
    composed["gpu_counters_available"] = false;
    result["composed_surface"] = composed;
#endif
	return result;
}

Dictionary Terrain3DRenderCoordinator::getDeepDiagnostics(
	void ) const {
	Terrain3DRdContext::forceSync( T3D_RD_FORCE_SYNC_DIAGNOSTICS );
	Dictionary result = m_diagnosticsPublisher.getDiagnostics( diagnosticInputs() );
	result["diagnostics_mode"] = "deep_explicit_sync";
	result["sync_count_by_reason"] = Terrain3DRdContext::getForceSyncDiagnostics();
#if defined( T3D_EDITOR_NATIVE )
	if ( m_pAuthoring != NULL ) {
		result["incremental_megavt_cooker"] = m_pAuthoring->m_cooker.getDiagnosticsCold();
	}
#endif
	{
		Terrain3DScopedLock lock( m_vtDemandIndexMutex );
		result["vt_demand_index_path"] = m_vtDemandIndexPath;
		result["vt_demand_index_status"] = m_vtDemandIndexStatus;
	}
	return result;
}

Dictionary Terrain3DRenderCoordinator::getPublishedStartupDiagnostics(
	void ) const {
	const t3dStartupDomainSnapshot_t shaderState = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t vtState = m_sharedVtRuntime.virtualTexture().get_startup_snapshot();
	const t3dStartupStage_t			 shaderStage = t3dStartupDomainGetStage( shaderState );
	const t3dStartupStage_t			 vtStage = t3dStartupDomainGetStage( vtState );
	const t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	const uint64_t					 nNow = t3dRdStartupTicksUsec();
	Dictionary						 shader;
	shader["startup_stage_name"] = String( t3dStartupStageName( shaderStage ) );
	shader["shader_completed"] = ( int64_t ) shaderState.m_nCompletedUnits;
	shader["shader_total"] = ( int64_t ) shaderState.m_nTotalUnits;
	shader["shader_cpu_ready"] = shaderStage == T3D_STARTUP_CPU_READY || shaderStage == T3D_STARTUP_GPU_PENDING ||
								 shaderStage == T3D_STARTUP_GPU_READY || shaderStage == T3D_STARTUP_COMMITTED;
	shader["shader_gpu_ready"] = shaderStage == T3D_STARTUP_GPU_READY || shaderStage == T3D_STARTUP_COMMITTED;
	shader["shader_elapsed_ms"] = ( double ) t3dStartupDomainElapsedUsec( shaderState, nNow ) / 1000.0;
	Dictionary vt;
	vt["startup_stage_name"] = String( t3dStartupStageName( vtStage ) );
	vt["vt_cpu_ready"] = vtStage == T3D_STARTUP_CPU_READY || vtStage == T3D_STARTUP_GPU_PENDING ||
						 vtStage == T3D_STARTUP_GPU_READY || vtStage == T3D_STARTUP_COMMITTED;
	vt["vt_storage_ready"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_VT_STORAGE ) != 0u;
	vt["vt_gpu_ready"] = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_VT_EXECUTABLE ) != 0u;
	// Byte estimates are deliberately omitted until their render-owned owners
	// publish them. Do not read mutable VT package internals from the owner thread.
	vt["vt_cpu_bytes"] = ( int64_t ) -1;
	vt["vt_gpu_bytes"] = ( int64_t ) -1;
	vt["vt_elapsed_ms"] = ( double ) t3dStartupDomainElapsedUsec( vtState, nNow ) / 1000.0;
	Dictionary result;
	result["shader"] = shader;
	result["virtual_texture"] = vt;
	result["render_lifecycle_stage"] = ( int64_t ) lifecycle.m_eStage;
	result["render_lifecycle_failure_code"] = ( int64_t ) lifecycle.m_nFailureCode;
	result["render_lifecycle_recovery_revision"] = ( int64_t ) lifecycle.m_nRecoveryRevision;
	result["startup_stage"] = "published_no_sync";
	return result;
}

Dictionary Terrain3DRenderCoordinator::getStartupDiagnostics(
	void ) const {
	Dictionary						 result;
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	result["startup_generation"] = ( int64_t ) ( m_nStartupGeneration );
	result["startup_gpu_stage"] = ( int64_t ) ( m_nStartupGpuStage );
	result["startup_gpu_service_count"] = ( int64_t ) ( m_nStartupGpuServiceCount );
	result["startup_gpu_service_usec_last"] = ( int64_t ) ( m_nStartupGpuServiceUsecLast );
	result["startup_gpu_service_complete"] = m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["startup_stale_completion_reject_count"] =
		( int64_t ) ( m_nStartupStaleCompletionRejectCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["startup_cancellation_count"] = ( int64_t ) ( m_nStartupCancellationCount );
	result["startup_render_drain_count"] = ( int64_t ) ( m_nStartupRenderDrainCount );
	result["activation_barrier_count"] = ( int64_t ) ( m_nStartupActivationBarrierCount );
	result["activation_barrier_usec_last"] = ( int64_t ) ( m_nStartupActivationBarrierUsecLast );
	result["activation_snapshot_generation"] = ( int64_t ) ( m_nStartupActivationSnapshotGeneration );
	result["activation_world_publication_queued"] = m_bStartupWorldPublicationQueued.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["activation_world_publication_prepared"] =
		m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["activation_world_publication_generation"] =
		( int64_t ) ( m_nStartupWorldPublicationGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["activation_world_publication_prepare_usec_last"] =
		( int64_t ) ( m_nStartupWorldPublicationPrepareUsecLast.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["activation_prepared"] = m_bStartupActivationPrepared;
	result["activation_committed"] = m_bStartupActivationCommitted;
	result["final_sync_attempts"] = ( int64_t ) ( m_nStartupActivationBarrierCount );
	result["final_sync_successes"] = ( int64_t ) ( m_nStartupFinalSyncSuccessCount );
	result["final_sync_failures"] = ( int64_t ) ( m_nStartupFinalSyncFailureCount );
	result["final_sync_elapsed_ms"] = ( double ) ( m_nStartupFinalSyncElapsedUsecLast ) / 1000.0;
	const t3dStartupDomainSnapshot_t shaderStartup = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t vtStartup = m_sharedVtRuntime.virtualTexture().get_startup_snapshot();
	result["shader_stage"] = ( int64_t ) ( t3dStartupDomainGetStage( shaderStartup ) );
	result["shader_stage_name"] = String( t3dStartupStageName( t3dStartupDomainGetStage( shaderStartup ) ) );
	result["vt_stage"] = ( int64_t ) ( t3dStartupDomainGetStage( vtStartup ) );
	result["vt_stage_name"] = String( t3dStartupStageName( t3dStartupDomainGetStage( vtStartup ) ) );
	result["publication_prepare_count"] = ( int64_t ) ( m_nStartupPublicationPrepareCount );
	result["publication_prepare_usec_last"] = ( int64_t ) ( m_nStartupPublicationPrepareUsecLast );
	result["renderer_initialize_attempts"] = ( int64_t ) ( m_nStartupRendererInitializeAttemptCount );
	result["renderer_initialize_successes"] = ( int64_t ) ( m_nStartupRendererInitializeSuccessCount );
	result["renderer_initialize_usec_last"] = ( int64_t ) ( m_nStartupRendererInitializeUsecLast );
	result["renderer_initialize_usec_total"] = ( int64_t ) ( m_nStartupRendererInitializeUsecTotal );
	result["renderer_initialized"] = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_TERRAIN ) != 0u;
	result["shared_vt_resources_ready"] = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_VT_PUBLISHED ) != 0u;
	result["render_lifecycle_generation"] = ( int64_t ) renderStatus.m_nStartupGeneration;
	result["render_lifecycle_stage"] = ( int64_t ) renderStatus.m_eStage;
	result["render_lifecycle_ready_mask"] = ( int64_t ) renderStatus.m_nReadyMask;
	result["render_lifecycle_failure_code"] = ( int64_t ) renderStatus.m_nFailureCode;
	result["render_lifecycle_prepared_world_generation"] =
		( int64_t ) renderStatus.m_nPreparedWorldPublicationGeneration;
	result["render_lifecycle_recovery_revision"] = ( int64_t ) renderStatus.m_nRecoveryRevision;
	result["render_lifecycle_revision"] = ( int64_t ) renderStatus.m_nRevision;
	result["shader"] = m_shaderLibrary.getDiagnostics();
	result["virtual_texture"] = m_sharedVtRuntime.virtualTexture().get_startup_diagnostics();
	return result;
}

void Terrain3DRenderCoordinator::setUnifiedWorldFramePolicy(
	const uint32_t nWorldUploadKb,
	const uint32_t nWorldClusters,
	const uint32_t nVtRequests,
	const uint32_t nVtAdmissions,
	const bool	   bBoundaryDeferred ) {
	m_nWorldUploadBudgetKbPolicy.store( MAX( 1u, nWorldUploadKb ), T3D_MEMORY_ORDER_RELEASE );
	m_nWorldClusterBudgetPolicy.store( MAX( 1u, nWorldClusters ), T3D_MEMORY_ORDER_RELEASE );
	m_nVtRequestBudgetPolicy.store( MAX( 1u, nVtRequests ), T3D_MEMORY_ORDER_RELEASE );
	m_nVtAdmissionBudgetPolicy.store( MAX( 1u, nVtAdmissions ), T3D_MEMORY_ORDER_RELEASE );
	m_bWorldBoundaryDeferredPolicy.store( bBoundaryDeferred, T3D_MEMORY_ORDER_RELEASE );
}

uint64_t Terrain3DRenderCoordinator::getWorldGpuBufferBytes(
	void ) const {
	return m_worldRuntime.getResources().getGpuBufferBytes();
}
uint64_t Terrain3DRenderCoordinator::getWorldHiZGpuBytes(
	void ) const {
	return m_worldRuntime.getHiZ().getGpuBytes();
}
size_t Terrain3DRenderCoordinator::getWorldRetainedCpuBytes(
	void ) const {
	return m_worldRuntime.getRetainedCpuBytes();
}
uint64_t Terrain3DRenderCoordinator::getWorldBudgetDeferralCount(
	void ) const {
	return m_worldRuntime.getPublicationBudgetDeferralCount() + m_worldRuntime.getPublicationBoundaryDeferralCount();
}
uint64_t Terrain3DRenderCoordinator::getVirtualTextureBudgetDeferralCount(
	void ) const {
	const t3dVTStreamingStats_t &rStats = m_sharedVtRuntime.virtualTexture().get_streaming().Stats;
	return rStats.nBudgetDeferredCount + rStats.nAdmissionBudgetDeferredCount;
}
uint64_t Terrain3DRenderCoordinator::getVirtualTextureGpuBytes() const {
	return m_sharedVtRuntime.virtualTexture().get_gpu_reserved_bytes();
}
uint64_t Terrain3DRenderCoordinator::getVirtualTextureCpuBytes(
	void ) const {
	return m_sharedVtRuntime.virtualTexture().get_cpu_image_bytes();
}
bool Terrain3DRenderCoordinator::isEditorAuthoringMode(
	void ) const {
	return m_pTerrain && m_pTerrain->isEditorAuthoringMode();
}

void Terrain3DRenderCoordinator::consumeFragmentFeedbackOnRenderThread(
	const PackedByteArray &rBytes ) {
	m_sharedVtRuntime.virtualTexture().get_gpu().consume_fragment_feedback_readback_hot( rBytes );
}

#if !defined( T3D_EDITOR_NATIVE )
bool Terrain3DRenderCoordinator::composePreviewTerrainPackageCold(
	PackedByteArray &,
	uint32_t &,
	uint64_t &,
	Terrain3DSharedMemoryReservation & ) {
	return false;
}
String Terrain3DRenderCoordinator::previewTerrainCompositionErrorCold() const {
	return "Preview terrain cooking is editor-only";
}
bool Terrain3DRenderCoordinator::bakePreviewVirtualTexturesCold(
	String *const pError ) {
	if ( pError ) {
		*pError = "VT baking requires the editor binary";
	}
	return false;
}
bool Terrain3DRenderCoordinator::preparePreviewAuthoringPackageCold(
	const Ref< Terrain3DAssets > &,
	String *const pError ) {
	if ( pError ) {
		*pError = "VT authoring requires the editor binary";
	}
	return false;
}
Dictionary Terrain3DRenderCoordinator::forceRebakeVirtualTexturesCold(
	void ) {
	Dictionary result;
	result["state"] = "failed";
	result["error"] = "VT baking requires the editor binary";
	return result;
}
Dictionary Terrain3DRenderCoordinator::bakeChangedVirtualTexturesCold(
	void ) {
	return forceRebakeVirtualTexturesCold();
}
Dictionary Terrain3DRenderCoordinator::getForceRebakeVirtualTexturesStatusCold(
	void ) const {
	Dictionary result;
	result["state"] = "unavailable";
	result["error"] = "VT baking requires the editor binary";
	return result;
}
void Terrain3DRenderCoordinator::serviceForceRebakeCold(
	void ) {
}
#endif
