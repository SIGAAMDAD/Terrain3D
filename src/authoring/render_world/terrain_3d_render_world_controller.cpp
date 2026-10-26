// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/render_world/terrain_3d_render_world_controller.h"
#include "memory/terrain_3d_memory.h"
#include "rendering/rd/render_world/terrain_3d_render_world_contract.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include <string.h>

using namespace godot;

static uint32_t t3dRwCapacityFromBytes( const uint64_t nBytes, const uint32_t nStride ) {
	if ( nStride == 0u || nBytes > UINT32_MAX * ( uint64_t )( nStride ) ) {
		return 0u;
	}
	return ( uint32_t )( nBytes / nStride );
}

Terrain3DRenderWorldController::~Terrain3DRenderWorldController( void ) {
	releasePublishedSceneCold();
	releaseAuthoringImageCold();
}

void Terrain3DRenderWorldController::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &Terrain3DRenderWorldController::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &Terrain3DRenderWorldController::isEnabled );
	ClassDB::bind_method( D_METHOD( "set_mode", "mode" ), &Terrain3DRenderWorldController::setMode );
	ClassDB::bind_method( D_METHOD( "get_mode" ), &Terrain3DRenderWorldController::getMode );
	ClassDB::bind_method( D_METHOD( "set_convert_mesh_instances", "enabled" ), &Terrain3DRenderWorldController::setConvertMeshInstances );
	ClassDB::bind_method( D_METHOD( "is_convert_mesh_instances" ), &Terrain3DRenderWorldController::isConvertMeshInstances );
	ClassDB::bind_method( D_METHOD( "set_convert_dynamic_objects", "enabled" ), &Terrain3DRenderWorldController::setConvertDynamicObjects );
	ClassDB::bind_method( D_METHOD( "is_convert_dynamic_objects" ), &Terrain3DRenderWorldController::isConvertDynamicObjects );
	ClassDB::bind_method( D_METHOD( "set_convert_hidden_objects", "enabled" ), &Terrain3DRenderWorldController::setConvertHiddenObjects );
	ClassDB::bind_method( D_METHOD( "is_convert_hidden_objects" ), &Terrain3DRenderWorldController::isConvertHiddenObjects );
	ClassDB::bind_method( D_METHOD( "set_cell_size", "cell_size" ), &Terrain3DRenderWorldController::setCellSize );
	ClassDB::bind_method( D_METHOD( "get_cell_size" ), &Terrain3DRenderWorldController::getCellSize );
	ClassDB::bind_method( D_METHOD( "set_default_profile", "profile" ), &Terrain3DRenderWorldController::setDefaultProfile );
	ClassDB::bind_method( D_METHOD( "get_default_profile" ), &Terrain3DRenderWorldController::getDefaultProfile );
	ClassDB::bind_method( D_METHOD( "set_auto_rebuild_on_ready", "enabled" ), &Terrain3DRenderWorldController::setAutoRebuildOnReady );
	ClassDB::bind_method( D_METHOD( "is_auto_rebuild_on_ready" ), &Terrain3DRenderWorldController::isAutoRebuildOnReady );
	ClassDB::bind_method( D_METHOD( "request_cold_rebuild" ), &Terrain3DRenderWorldController::requestColdRebuild );
	ClassDB::bind_method( D_METHOD( "release_render_world_ownership" ), &Terrain3DRenderWorldController::releaseRenderWorldOwnership );
	ClassDB::bind_method(
	        D_METHOD( "get_last_build_stats_reference_debug" ), &Terrain3DRenderWorldController::getLastBuildStatsReferenceDebug );
	ClassDB::bind_method(
	        D_METHOD( "get_backend_status_reference_debug" ), &Terrain3DRenderWorldController::getBackendStatusReferenceDebug );

	ADD_GROUP( "RenderWorld", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "enabled" ), "set_enabled", "is_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mode", PROPERTY_HINT_ENUM, "Disabled,Explicit,Eligible Scene,Full Static Scene" ),
	        "set_mode",
	        "get_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "default_profile", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DRenderWorldImportProfile" ),
	        "set_default_profile",
	        "get_default_profile" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "4,256,1,suffix:m" ), "set_cell_size", "get_cell_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "auto_rebuild_on_ready" ), "set_auto_rebuild_on_ready", "is_auto_rebuild_on_ready" );
	ADD_GROUP( "Compatibility Ingress", "convert_" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "convert_mesh_instances" ), "set_convert_mesh_instances", "is_convert_mesh_instances" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "convert_dynamic_objects", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_convert_dynamic_objects",
	        "is_convert_dynamic_objects" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "convert_hidden_objects" ), "set_convert_hidden_objects", "is_convert_hidden_objects" );
}

void Terrain3DRenderWorldController::_notification( const int32_t nWhat ) {
	const Engine *const pEngine = Engine::get_singleton();
	if ( !pEngine || !pEngine->is_editor_hint() ) {
		if ( nWhat == NOTIFICATION_EXIT_TREE ) {
			releasePublishedSceneCold();
			m_pCoordinator = NULL;
		}
		return;
	}
	if ( nWhat == NOTIFICATION_ENTER_TREE ) {
		bindNearestTerrainCold();
	} else if ( nWhat == NOTIFICATION_READY ) {
		if ( m_bAutoRebuildOnReady && m_bEnabled && m_iMode != ( int32_t )( T3D_RW_MODE_DISABLED ) ) {
			requestColdRebuild();
		}
	} else if ( nWhat == NOTIFICATION_EXIT_TREE ) {
		releasePublishedSceneCold();
		m_pCoordinator = NULL;
	}
}

bool Terrain3DRenderWorldController::bindNearestTerrainCold( void ) {
	Terrain3D *pTerrain = NULL;
	Node *pNode = get_parent();
	while ( pNode ) {
		pTerrain = Object::cast_to<Terrain3D>( pNode );
		if ( pTerrain ) {
			break;
		}
		pNode = pNode->get_parent();
	}
	if ( !pTerrain ) {
		return false;
	}

	Terrain3DRenderCoordinator *const pCoordinator = pTerrain->getRenderCoordinatorInternal();
	if ( !pCoordinator ) {
		return false;
	}
	if ( m_pCoordinator && m_pCoordinator != pCoordinator && m_bOwnershipApplied ) {
		releasePublishedSceneCold();
	}
	m_pCoordinator = pCoordinator;
	if ( !m_backend.pPublishSceneCold ) {
		m_backend.pPublishSceneCold = &Terrain3DRenderWorldController::publishAutoSceneCold;
		m_backend.pReleaseSceneCold = &Terrain3DRenderWorldController::releaseAutoSceneCold;
		m_backend.pUser = this;
	}
	return true;
}

bool Terrain3DRenderWorldController::publishAutoSceneCold(
        void *const pUser, const T3DRenderWorldSceneBuildBuffers &rBuffers, const T3DRenderWorldSceneBuildStats &rStats ) {
	( void )( rStats );
	Terrain3DRenderWorldController *const pSelf = ( Terrain3DRenderWorldController * )( pUser );
	if ( !pSelf || !pSelf->m_pCoordinator || !rBuffers.pGeometry ) {
		return false;
	}
	return pSelf->m_pCoordinator->publishStaticSceneDatabaseCold( *rBuffers.pGeometry,
	        rBuffers.pCells,
	        rBuffers.nCellCount,
	        rBuffers.pInstances,
	        rBuffers.nInstanceCount,
	        rBuffers.pMaterials,
	        rBuffers.nMaterialCount );
}

bool Terrain3DRenderWorldController::resolveAutoMaterialVtCold(
        void *const pUser, const uint64_t nStableMaterialKey, uint32_t &rAllocationId ) {
	Terrain3DRenderWorldController *const pSelf = ( Terrain3DRenderWorldController * )( pUser );
	if ( !pSelf || !pSelf->m_pCoordinator ) {
		rAllocationId = T3D_VT_INVALID_ALLOCATION;
		return false;
	}
	return pSelf->m_pCoordinator->resolveVirtualTextureMaterialAllocationCold( nStableMaterialKey, rAllocationId );
}

void Terrain3DRenderWorldController::releaseAutoSceneCold( void *const pUser ) {
	Terrain3DRenderWorldController *const pSelf = ( Terrain3DRenderWorldController * )( pUser );
	if ( pSelf && pSelf->m_pCoordinator ) {
		pSelf->m_pCoordinator->releaseStaticSceneDatabaseCold();
	}
}

bool Terrain3DRenderWorldController::initializeAuthoringImageCold( void ) {
	if ( m_bAutoConverterInitialized ) {
		return true;
	}

	const T3DRenderWorldLimits Limits = t3d_render_world_make_prototype_limits();
	if ( !t3d_render_world_validate_limits( Limits ) ) {
		return false;
	}
	const uint32_t nVertexCapacity = t3dRwCapacityFromBytes( Limits.nGeometryVertexHeapBytes, sizeof( T3DPrototypeVertex ) );
	const uint32_t nClusterVertexCapacity = t3dRwCapacityFromBytes( Limits.nGeometryClusterVertexIndexHeapBytes, sizeof( uint32_t ) );
	const uint32_t nTriangleCapacity = t3dRwCapacityFromBytes( Limits.nGeometryTriangleHeapBytes, sizeof( T3DPackedClusterTriangle ) );
	if ( nVertexCapacity == 0u || nClusterVertexCapacity == 0u || nTriangleCapacity == 0u ) {
		return false;
	}

	Terrain3DLinearArenaLayout Layout;
	bool valid = true;
	valid = valid && Layout.addArray<T3DPrototypeVertex>( nVertexCapacity );
	valid = valid && Layout.addArray<uint32_t>( nClusterVertexCapacity );
	valid = valid && Layout.addArray<T3DPackedClusterTriangle>( nTriangleCapacity );
	valid = valid && Layout.addArray<T3DMeshRecord>( Limits.nMaxMeshes );
	valid = valid && Layout.addArray<T3DMeshLodRecord>( Limits.nMaxMeshLods );
	valid = valid && Layout.addArray<T3DClusterRecord>( Limits.nMaxClusters );
	valid = valid && Layout.addArray<t3dWorldCluster_t>( Limits.nMaxClusters );
	valid = valid && Layout.addArray<t3dWorldClusterVtMetadata_t>( Limits.nMaxClusters );
	valid = valid && Layout.addArray<t3dRenderingMaterialGpu_t>( Limits.nMaxMaterials );
	valid = valid && Layout.addArray<T3DSceneCell>( Limits.nMaxCells );
	valid = valid && Layout.addArray<T3DInstanceRecord>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint64_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<T3DRenderWorldOwnershipEntry>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<T3DRenderWorldPrototypeEntry>( Limits.nMaxMeshes );
	valid = valid && Layout.addArray<T3DRenderWorldPrototypeBounds>( Limits.nMaxMeshes );
	valid = valid && Layout.addArray<T3DRenderWorldMaterialEntry>( Limits.nMaxMaterials );
	valid = valid && Layout.addArray<T3DRenderWorldNativeVisualEntry>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<T3DPrototypeVertex>( s_nScratchVertexCapacity );
	valid = valid && Layout.addArray<uint32_t>( s_nScratchIndexCapacity );
	valid = valid && Layout.addArray<uint32_t>( s_nScratchTriangleCapacity );
	valid = valid && Layout.addArray<uint8_t>( s_nScratchTriangleCapacity );
	valid = valid && Layout.addArray<T3DInstanceRecord>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint64_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint32_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint32_t>( Limits.nMaxCells );
	valid = valid && Layout.addArray<uint32_t>( Limits.nMaxCells );
	valid = valid && Layout.addArray<T3DRenderWorldCellHashEntry>( s_nCellHashCapacity );
	if ( !valid || !Layout.valid() ) {
		return false;
	}

	Terrain3DColdPhaseScope cold_scope;
	if ( !m_AuthoringArena.configure( Layout.size() ) ) {
		return false;
	}
	m_nAuthoringArenaBytes = ( uint64_t )( Layout.size() );
	memset( &m_AuthoringGeometry, 0, sizeof( m_AuthoringGeometry ) );
	memset( &m_AuthoringOwnership, 0, sizeof( m_AuthoringOwnership ) );
	memset( &m_AuthoringBuffers, 0, sizeof( m_AuthoringBuffers ) );

	T3DGeometryDatabaseBuffers &GeometryBuffers = m_AuthoringGeometry.Buffers;
	GeometryBuffers.pVertexHeap = m_AuthoringArena.allocateArray<T3DPrototypeVertex>( nVertexCapacity );
	GeometryBuffers.nVertexCapacity = nVertexCapacity;
	GeometryBuffers.pClusterVertexIndexHeap = m_AuthoringArena.allocateArray<uint32_t>( nClusterVertexCapacity );
	GeometryBuffers.nClusterVertexIndexCapacity = nClusterVertexCapacity;
	GeometryBuffers.pTriangleHeap = m_AuthoringArena.allocateArray<T3DPackedClusterTriangle>( nTriangleCapacity );
	GeometryBuffers.nTriangleCapacity = nTriangleCapacity;
	GeometryBuffers.pMeshRecords = m_AuthoringArena.allocateArray<T3DMeshRecord>( Limits.nMaxMeshes );
	GeometryBuffers.nMeshCapacity = Limits.nMaxMeshes;
	GeometryBuffers.pLodRecords = m_AuthoringArena.allocateArray<T3DMeshLodRecord>( Limits.nMaxMeshLods );
	GeometryBuffers.nLodCapacity = Limits.nMaxMeshLods;
	GeometryBuffers.pClusterRecords = m_AuthoringArena.allocateArray<T3DClusterRecord>( Limits.nMaxClusters );
	GeometryBuffers.pWorldClusterRecords = m_AuthoringArena.allocateArray<t3dWorldCluster_t>( Limits.nMaxClusters );
	GeometryBuffers.pWorldClusterVtMetadata = m_AuthoringArena.allocateArray<t3dWorldClusterVtMetadata_t>( Limits.nMaxClusters );
	GeometryBuffers.nClusterCapacity = Limits.nMaxClusters;

	m_AuthoringBuffers.pGeometry = &m_AuthoringGeometry;
	m_AuthoringBuffers.pMaterials = m_AuthoringArena.allocateArray<t3dRenderingMaterialGpu_t>( Limits.nMaxMaterials );
	m_AuthoringBuffers.nMaterialCapacity = Limits.nMaxMaterials;
	m_AuthoringBuffers.pCells = m_AuthoringArena.allocateArray<T3DSceneCell>( Limits.nMaxCells );
	m_AuthoringBuffers.nCellCapacity = Limits.nMaxCells;
	m_AuthoringBuffers.pInstances = m_AuthoringArena.allocateArray<T3DInstanceRecord>( Limits.nMaxInstances );
	m_AuthoringBuffers.nInstanceCapacity = Limits.nMaxInstances;
	m_AuthoringBuffers.pInstanceSourceIds = m_AuthoringArena.allocateArray<uint64_t>( Limits.nMaxInstances );
	T3DRenderWorldOwnershipEntry *const pOwnershipEntries =
	        m_AuthoringArena.allocateArray<T3DRenderWorldOwnershipEntry>( Limits.nMaxInstances );
	m_AuthoringBuffers.pOwnership = &m_AuthoringOwnership;
	m_AuthoringBuffers.pPrototypeEntries = m_AuthoringArena.allocateArray<T3DRenderWorldPrototypeEntry>( Limits.nMaxMeshes );
	m_AuthoringBuffers.pPrototypeBoundsByMeshId = m_AuthoringArena.allocateArray<T3DRenderWorldPrototypeBounds>( Limits.nMaxMeshes );
	m_AuthoringBuffers.nPrototypeBoundsCapacity = Limits.nMaxMeshes;
	m_AuthoringBuffers.nPrototypeCapacity = Limits.nMaxMeshes;
	m_AuthoringBuffers.pMaterialEntries = m_AuthoringArena.allocateArray<T3DRenderWorldMaterialEntry>( Limits.nMaxMaterials );
	m_AuthoringBuffers.nMaterialEntryCapacity = Limits.nMaxMaterials;
	m_AuthoringBuffers.pNativeVisuals = m_AuthoringArena.allocateArray<T3DRenderWorldNativeVisualEntry>( Limits.nMaxInstances );
	m_AuthoringBuffers.nNativeVisualCapacity = Limits.nMaxInstances;

	m_AuthoringBuffers.Scratch.pVertices = m_AuthoringArena.allocateArray<T3DPrototypeVertex>( s_nScratchVertexCapacity );
	m_AuthoringBuffers.Scratch.nVertexCapacity = s_nScratchVertexCapacity;
	m_AuthoringBuffers.Scratch.pIndices = m_AuthoringArena.allocateArray<uint32_t>( s_nScratchIndexCapacity );
	m_AuthoringBuffers.Scratch.nIndexCapacity = s_nScratchIndexCapacity;
	m_AuthoringBuffers.Scratch.pTriangleMaterialOffsets = m_AuthoringArena.allocateArray<uint32_t>( s_nScratchTriangleCapacity );
	m_AuthoringBuffers.Scratch.pTriangleTiers = m_AuthoringArena.allocateArray<uint8_t>( s_nScratchTriangleCapacity );
	m_AuthoringBuffers.Scratch.nTriangleCapacity = s_nScratchTriangleCapacity;

	m_AuthoringBuffers.CellScratch.pReorderedInstances = m_AuthoringArena.allocateArray<T3DInstanceRecord>( Limits.nMaxInstances );
	m_AuthoringBuffers.CellScratch.pReorderedSourceIds = m_AuthoringArena.allocateArray<uint64_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.CellScratch.nInstanceCapacity = Limits.nMaxInstances;
	m_AuthoringBuffers.CellScratch.pInstanceCellIds = m_AuthoringArena.allocateArray<uint32_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.CellScratch.pCellCounts = m_AuthoringArena.allocateArray<uint32_t>( Limits.nMaxCells );
	m_AuthoringBuffers.CellScratch.pCellWriteCursor = m_AuthoringArena.allocateArray<uint32_t>( Limits.nMaxCells );
	m_AuthoringBuffers.CellScratch.nCellScratchCapacity = Limits.nMaxCells;
	m_AuthoringBuffers.CellScratch.pCellHash = m_AuthoringArena.allocateArray<T3DRenderWorldCellHashEntry>( s_nCellHashCapacity );
	m_AuthoringBuffers.CellScratch.nCellHashCapacity = s_nCellHashCapacity;

	if ( !GeometryBuffers.pVertexHeap || !GeometryBuffers.pClusterVertexIndexHeap || !GeometryBuffers.pTriangleHeap
	        || !GeometryBuffers.pMeshRecords || !GeometryBuffers.pLodRecords || !GeometryBuffers.pClusterRecords
	        || !GeometryBuffers.pWorldClusterRecords || !GeometryBuffers.pWorldClusterVtMetadata || !m_AuthoringBuffers.pMaterials
	        || !m_AuthoringBuffers.pCells || !m_AuthoringBuffers.pInstances || !m_AuthoringBuffers.pInstanceSourceIds || !pOwnershipEntries
	        || !m_AuthoringBuffers.pPrototypeEntries || !m_AuthoringBuffers.pPrototypeBoundsByMeshId || !m_AuthoringBuffers.pMaterialEntries
	        || !m_AuthoringBuffers.pNativeVisuals || !m_AuthoringBuffers.Scratch.pVertices || !m_AuthoringBuffers.Scratch.pIndices
	        || !m_AuthoringBuffers.Scratch.pTriangleMaterialOffsets || !m_AuthoringBuffers.Scratch.pTriangleTiers
	        || !m_AuthoringBuffers.CellScratch.pReorderedInstances || !m_AuthoringBuffers.CellScratch.pReorderedSourceIds
	        || !m_AuthoringBuffers.CellScratch.pInstanceCellIds || !m_AuthoringBuffers.CellScratch.pCellCounts
	        || !m_AuthoringBuffers.CellScratch.pCellWriteCursor || !m_AuthoringBuffers.CellScratch.pCellHash ) {
		releaseAuthoringImageCold();
		return false;
	}

	t3d_render_world_geometry_database_initialize_cold( m_AuthoringGeometry, GeometryBuffers );
	if ( !t3d_render_world_ownership_registry_initialize_cold( m_AuthoringOwnership, pOwnershipEntries, Limits.nMaxInstances ) ) {
		releaseAuthoringImageCold();
		return false;
	}
	T3DRenderWorldGodotMaterialResolver resolver{};
	resolver.pResolveMaterialVT = &Terrain3DRenderWorldController::resolveAutoMaterialVtCold;
	resolver.pUser = this;
	if ( !m_AutoConverter.initializeCold( buildPolicyCold(), resolver, &m_AuthoringBuffers ) ) {
		releaseAuthoringImageCold();
		return false;
	}
	m_pConverter = &m_AutoConverter;
	m_bAutoConverterInitialized = true;
	return true;
}

void Terrain3DRenderWorldController::releaseAuthoringImageCold( void ) {
	if ( m_pConverter == &m_AutoConverter ) {
		m_pConverter = NULL;
	}
	m_bAutoConverterInitialized = false;
	m_AuthoringArena.release();
	m_nAuthoringArenaBytes = 0u;
	memset( &m_AuthoringGeometry, 0, sizeof( m_AuthoringGeometry ) );
	memset( &m_AuthoringOwnership, 0, sizeof( m_AuthoringOwnership ) );
	memset( &m_AuthoringBuffers, 0, sizeof( m_AuthoringBuffers ) );
}

bool Terrain3DRenderWorldController::releasePublishedSceneCold( void ) {
	if ( !m_bOwnershipApplied ) {
		return true;
	}
	if ( m_backend.pReleaseSceneCold ) {
		m_backend.pReleaseSceneCold( m_backend.pUser );
	}
	if ( m_pConverter ) {
		m_pConverter->releaseNativeOwnershipCold();
	}
	m_bOwnershipApplied = false;
	return true;
}

void Terrain3DRenderWorldController::setEnabled( const bool bEnabled ) {
	if ( m_bEnabled == bEnabled ) {
		return;
	}
	m_bEnabled = bEnabled;
	if ( !m_bEnabled ) {
		releasePublishedSceneCold();
	}
}

void Terrain3DRenderWorldController::setMode( const int32_t iMode ) {
	const int32_t mode = iMode >= 0 && iMode <= ( int32_t )( T3D_RW_MODE_FULL_STATIC_SCENE ) ? iMode : ( int32_t )( T3D_RW_MODE_EXPLICIT );
	if ( m_iMode == mode ) {
		return;
	}
	m_iMode = mode;
	if ( m_iMode == ( int32_t )( T3D_RW_MODE_DISABLED ) ) {
		releasePublishedSceneCold();
	}
}

void Terrain3DRenderWorldController::setConvertMeshInstances( const bool bEnabled ) {
	m_bConvertMeshInstances = bEnabled;
}

void Terrain3DRenderWorldController::setConvertDynamicObjects( const bool bEnabled ) {
	m_bConvertDynamicObjects = bEnabled;
}

void Terrain3DRenderWorldController::setConvertHiddenObjects( const bool bEnabled ) {
	m_bConvertHiddenObjects = bEnabled;
}

void Terrain3DRenderWorldController::setCellSize( const float fCellSize ) {
	m_fCellSize = fCellSize < 4.0f ? 4.0f : ( fCellSize > 256.0f ? 256.0f : fCellSize );
}

void Terrain3DRenderWorldController::setDefaultProfile( const Ref<Terrain3DRenderWorldImportProfile> &rProfile ) {
	m_defaultProfile = rProfile;
}

void Terrain3DRenderWorldController::setAutoRebuildOnReady( const bool bEnabled ) {
	m_bAutoRebuildOnReady = bEnabled;
}

T3DRenderWorldConversionPolicy Terrain3DRenderWorldController::buildPolicyCold( void ) const {
	T3DRenderWorldConversionPolicy policy = t3d_render_world_authoring_default_policy();
	policy.eMode = m_bEnabled ? ( T3DRenderWorldMode )( m_iMode ) : T3D_RW_MODE_DISABLED;
	policy.bConvertMeshInstances = m_bConvertMeshInstances;
	policy.bConvertMultiMeshInstances = false;
	policy.bConvertDynamicObjects = false;
	policy.bConvertHiddenObjects = m_bConvertHiddenObjects;
	policy.fCellSize = m_fCellSize;
	if ( m_defaultProfile.is_valid() ) {
		policy.nDefaultTier = m_defaultProfile->resolveTier( policy.nDefaultTier );
		policy.eMaterialPolicy = ( T3DRenderWorldMaterialPolicy )( m_defaultProfile->getMaterialPolicy() );
		policy.eTransparencyPolicy = ( T3DRenderWorldTransparencyPolicy )( m_defaultProfile->getTransparencyPolicy() );
	}
	return policy;
}

bool Terrain3DRenderWorldController::requestColdRebuild( void ) {
	const Engine *const pEngine = Engine::get_singleton();
	if ( !pEngine || !pEngine->is_editor_hint() ) {
		return false;
	}
	if ( !bindNearestTerrainCold() && !m_backend.pPublishSceneCold ) {
		return false;
	}
	if ( !m_pConverter && !initializeAuthoringImageCold() ) {
		return false;
	}
	if ( !m_pConverter ) {
		return false;
	}
	if ( !m_bEnabled || m_iMode == ( int32_t )( T3D_RW_MODE_DISABLED ) ) {
		return releasePublishedSceneCold();
	}

	// Never leave an old RenderWorld publication alive while rebuilding the
	// authoring graph. Failure keeps Godot native ownership authoritative.
	if ( !releasePublishedSceneCold() ) {
		return false;
	}

	Terrain3DColdPhaseScope cold_scope;
	m_pConverter->setPolicyCold( buildPolicyCold() );
	if ( !m_pConverter->rebuildSceneCold( this, m_lastBuildStats ) ) {
		return false;
	}
	if ( !m_backend.pPublishSceneCold ) {
		return false;
	}
	if ( !m_backend.pPublishSceneCold( m_backend.pUser, *m_pConverter->getBuildBuffersCold(), m_lastBuildStats ) ) {
		return false;
	}

	m_bOwnershipApplied = m_pConverter->applyNativeOwnershipCold( true );
	if ( !m_bOwnershipApplied && m_backend.pReleaseSceneCold ) {
		m_backend.pReleaseSceneCold( m_backend.pUser );
	}
	return m_bOwnershipApplied;
}

bool Terrain3DRenderWorldController::releaseRenderWorldOwnership( void ) {
	return releasePublishedSceneCold();
}

Dictionary Terrain3DRenderWorldController::getLastBuildStatsReferenceDebug( void ) const {
	Dictionary stats;
	stats["visited_nodes"] = m_lastBuildStats.nVisitedNodes;
	stats["mesh_candidates"] = m_lastBuildStats.nMeshCandidates;
	stats["converted_instances"] = m_lastBuildStats.nConvertedInstances;
	stats["unique_prototypes"] = m_lastBuildStats.nUniquePrototypes;
	stats["unique_materials"] = m_lastBuildStats.nUniqueMaterials;
	stats["compatibility_instances"] = m_lastBuildStats.nCompatibilityInstances;
	stats["native_instances"] = m_lastBuildStats.nNativeInstances;
	stats["failures"] = m_lastBuildStats.nFailureCount;
	stats["authoring_arena_bytes"] = ( int64_t )( m_nAuthoringArenaBytes );
	return stats;
}

Dictionary Terrain3DRenderWorldController::getBackendStatusReferenceDebug( void ) const {
	Dictionary status;
	status["converter_attached"] = m_pConverter != NULL;
	status["backend_attached"] = m_backend.pPublishSceneCold != NULL;
	status["coordinator_attached"] = m_pCoordinator != NULL;
	status["auto_converter_initialized"] = m_bAutoConverterInitialized;
	status["ownership_applied"] = m_bOwnershipApplied;
	status["auto_rebuild_on_ready"] = m_bAutoRebuildOnReady;
	status["authoring_arena_bytes"] = ( int64_t )( m_nAuthoringArenaBytes );
	status["mode"] = m_iMode;
	status["enabled"] = m_bEnabled;
	return status;
}

void Terrain3DRenderWorldController::attachConverterCold( Terrain3DRenderWorldGodotConverter *const pConverter ) {
	releasePublishedSceneCold();
	if ( m_pConverter == &m_AutoConverter && pConverter != &m_AutoConverter ) {
		releaseAuthoringImageCold();
	}
	m_pConverter = pConverter;
}

void Terrain3DRenderWorldController::attachBackendCold( const T3DRenderWorldAuthoringBackend &rBackend ) {
	releasePublishedSceneCold();
	m_backend = rBackend;
}
