// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/world/core/terrain_3d_world_controller.h"
#include "rendering/rd/world/build/terrain_3d_world_static_builder.h"
#include "memory/core/terrain_3d_memory.h"
#include "rendering/rd/world/core/terrain_3d_world_system_limits.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include <string.h>

using namespace godot;

static uint32_t t3dWorldCapacityFromBytes( const uint64_t nBytes, const uint32_t nStride ) {
	if ( nStride == 0u || nBytes > UINT32_MAX * ( uint64_t )( nStride ) ) {
		return 0u;
	}
	return ( uint32_t )( nBytes / nStride );
}

Terrain3DWorldController::~Terrain3DWorldController( void ) {
	releasePublishedSceneCold();
	releaseAuthoringImageCold();
}

void Terrain3DWorldController::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &Terrain3DWorldController::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &Terrain3DWorldController::isEnabled );
	ClassDB::bind_method( D_METHOD( "set_mode", "mode" ), &Terrain3DWorldController::setMode );
	ClassDB::bind_method( D_METHOD( "get_mode" ), &Terrain3DWorldController::getMode );
	ClassDB::bind_method( D_METHOD( "set_convert_mesh_instances", "enabled" ), &Terrain3DWorldController::setConvertMeshInstances );
	ClassDB::bind_method( D_METHOD( "is_convert_mesh_instances" ), &Terrain3DWorldController::isConvertMeshInstances );
	ClassDB::bind_method( D_METHOD( "set_convert_dynamic_objects", "enabled" ), &Terrain3DWorldController::setConvertDynamicObjects );
	ClassDB::bind_method( D_METHOD( "is_convert_dynamic_objects" ), &Terrain3DWorldController::isConvertDynamicObjects );
	ClassDB::bind_method( D_METHOD( "set_convert_hidden_objects", "enabled" ), &Terrain3DWorldController::setConvertHiddenObjects );
	ClassDB::bind_method( D_METHOD( "is_convert_hidden_objects" ), &Terrain3DWorldController::isConvertHiddenObjects );
	ClassDB::bind_method( D_METHOD( "set_cell_size", "cell_size" ), &Terrain3DWorldController::setCellSize );
	ClassDB::bind_method( D_METHOD( "get_cell_size" ), &Terrain3DWorldController::getCellSize );
	ClassDB::bind_method( D_METHOD( "set_default_profile", "profile" ), &Terrain3DWorldController::setDefaultProfile );
	ClassDB::bind_method( D_METHOD( "get_default_profile" ), &Terrain3DWorldController::getDefaultProfile );
	ClassDB::bind_method( D_METHOD( "set_auto_rebuild_on_ready", "enabled" ), &Terrain3DWorldController::setAutoRebuildOnReady );
	ClassDB::bind_method( D_METHOD( "is_auto_rebuild_on_ready" ), &Terrain3DWorldController::isAutoRebuildOnReady );
	ClassDB::bind_method( D_METHOD( "request_cold_rebuild" ), &Terrain3DWorldController::requestColdRebuild );
	ClassDB::bind_method( D_METHOD( "release_world_ownership" ), &Terrain3DWorldController::releaseWorldOwnership );
	ClassDB::bind_method(
	        D_METHOD( "get_last_build_stats_reference_debug" ), &Terrain3DWorldController::getLastBuildStatsReferenceDebug );
	ClassDB::bind_method(
	        D_METHOD( "get_backend_status_reference_debug" ), &Terrain3DWorldController::getBackendStatusReferenceDebug );

	ADD_GROUP( "World", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "enabled" ), "set_enabled", "is_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mode", PROPERTY_HINT_ENUM, "Disabled,Explicit,Eligible Scene,Full Static Scene" ),
	        "set_mode",
	        "get_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "default_profile", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DWorldImportProfile" ),
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

void Terrain3DWorldController::_notification( const int32_t nWhat ) {
	if ( nWhat == NOTIFICATION_INTERNAL_PROCESS ) {
		completePublishedSceneCold();
		return;
	}
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
		if ( m_bAutoRebuildOnReady && m_bEnabled && m_iMode != ( int32_t )( T3D_WORLD_MODE_DISABLED ) ) {
			requestColdRebuild();
		}
	} else if ( nWhat == NOTIFICATION_EXIT_TREE ) {
		releasePublishedSceneCold();
		m_pCoordinator = NULL;
	}
}

bool Terrain3DWorldController::bindNearestTerrainCold( void ) {
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
	if ( m_pCoordinator && m_pCoordinator != pCoordinator && ( m_bOwnershipApplied || m_bPublicationPending ) ) {
		releasePublishedSceneCold();
	}
	m_pCoordinator = pCoordinator;
	if ( !m_backend.pPublishSceneCold ) {
		m_backend.pPublishSceneCold = &Terrain3DWorldController::publishAutoSceneCold;
		m_backend.pReleaseSceneCold = &Terrain3DWorldController::releaseAutoSceneCold;
		m_backend.pIsSceneReadyCold = &Terrain3DWorldController::isAutoSceneReadyCold;
		m_backend.pUser = this;
	}
	return true;
}

bool Terrain3DWorldController::publishAutoSceneCold(
        void *const pUser, const t3dWorldSceneBuildBuffers_t &rBuffers, const t3dWorldSceneBuildStats_t &rStats ) {
	( void )( rStats );
	Terrain3DWorldController *const pSelf = ( Terrain3DWorldController * )( pUser );
	if ( !pSelf || !pSelf->m_pCoordinator || !rBuffers.pGeometry || !rBuffers.pTransforms ) {
		return false;
	}
	t3dWorldStaticBuildSource_t source{};
	source.m_pGeometry = rBuffers.pGeometry;
	source.m_pTransforms = rBuffers.pTransforms;
	source.m_pInstances = rBuffers.pInstances;
	source.m_pCells = rBuffers.pCells;
	source.m_pMaterials = rBuffers.pMaterials;
	source.m_nTransformCount = rBuffers.nInstanceCount;
	source.m_nInstanceCount = rBuffers.nInstanceCount;
	source.m_nCellCount = rBuffers.nCellCount;
	source.m_nMaterialCount = rBuffers.nMaterialCount;
	if ( !Terrain3DWorldStaticBuilder::buildCold( source, pSelf->m_AuthoringWorldPublication ) ) {
		return false;
	}
	return pSelf->m_pCoordinator->publishStaticWorldCold( ( uint64_t )pSelf->get_instance_id(), pSelf->m_AuthoringWorldPublication );
}

bool Terrain3DWorldController::resolveAutoMaterialVtCold(
        void *const pUser, const uint64_t nStableMaterialKey, uint32_t &rAllocationId ) {
	Terrain3DWorldController *const pSelf = ( Terrain3DWorldController * )( pUser );
	if ( !pSelf || !pSelf->m_pCoordinator ) {
		rAllocationId = T3D_VT_INVALID_ALLOCATION;
		return false;
	}
	return pSelf->m_pCoordinator->resolveVirtualTextureMaterialAllocationCold( nStableMaterialKey, rAllocationId );
}

bool Terrain3DWorldController::isAutoSceneReadyCold( void *const pUser ) {
	Terrain3DWorldController *const pSelf = ( Terrain3DWorldController * )( pUser );
	return pSelf && pSelf->m_pCoordinator
	        && pSelf->m_pCoordinator->isStaticWorldReadyCold( ( uint64_t )pSelf->get_instance_id() );
}

void Terrain3DWorldController::releaseAutoSceneCold( void *const pUser ) {
	Terrain3DWorldController *const pSelf = ( Terrain3DWorldController * )( pUser );
	if ( pSelf && pSelf->m_pCoordinator ) {
		pSelf->m_pCoordinator->releaseStaticSceneDatabaseCold( ( uint64_t )pSelf->get_instance_id() );
	}
}

bool Terrain3DWorldController::initializeAuthoringImageCold( void ) {
	if ( m_bAutoConverterInitialized ) {
		return true;
	}

	const t3dWorldLimits_t Limits = t3d_world_make_prototype_limits();
	if ( !t3d_world_validate_limits( Limits ) ) {
		return false;
	}
	const uint32_t nVertexCapacity = t3dWorldCapacityFromBytes( Limits.nGeometryVertexHeapBytes, sizeof( t3dWorldVertex_t ) );
	const uint32_t nClusterVertexCapacity = t3dWorldCapacityFromBytes( Limits.nGeometryClusterVertexIndexHeapBytes, sizeof( uint32_t ) );
	const uint32_t nTriangleCapacity = t3dWorldCapacityFromBytes( Limits.nGeometryTriangleHeapBytes, sizeof( t3dWorldPackedClusterTriangle_t ) );
	if ( nVertexCapacity == 0u || nClusterVertexCapacity == 0u || nTriangleCapacity == 0u ) {
		return false;
	}

	Terrain3DLinearArenaLayout Layout;
	bool valid = true;
	valid = valid && Layout.addArray<t3dWorldVertex_t>( nVertexCapacity );
	valid = valid && Layout.addArray<uint32_t>( nClusterVertexCapacity );
	valid = valid && Layout.addArray<t3dWorldPackedClusterTriangle_t>( nTriangleCapacity );
	valid = valid && Layout.addArray<t3dWorldMeshRecord_t>( Limits.nMaxMeshes );
	valid = valid && Layout.addArray<t3dWorldMeshLodRecord_t>( Limits.nMaxMeshLods );
	valid = valid && Layout.addArray<t3dWorldClusterRecord_t>( Limits.nMaxClusters );
	valid = valid && Layout.addArray<t3dWorldCluster_t>( Limits.nMaxClusters );
	valid = valid && Layout.addArray<t3dWorldClusterVTMetadata_t>( Limits.nMaxClusters );
	valid = valid && Layout.addArray<t3dRenderingMaterialGpu_t>( Limits.nMaxMaterials );
	valid = valid && Layout.addArray<t3dWorldCell_t>( Limits.nMaxCells );
	valid = valid && Layout.addArray<t3dWorldInstance_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<t3dWorldTransform_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint64_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<t3dWorldOwnershipEntry_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<t3dWorldPrototypeEntry_t>( Limits.nMaxMeshes );
	valid = valid && Layout.addArray<t3dWorldPrototypeBounds_t>( Limits.nMaxMeshes );
	valid = valid && Layout.addArray<t3dWorldMaterialEntry_t>( Limits.nMaxMaterials );
	valid = valid && Layout.addArray<t3dWorldNativeVisualEntry_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<t3dWorldVertex_t>( T3D_WORLD_SCRATCH_VERTEX_CAPACITY );
	valid = valid && Layout.addArray<uint32_t>( T3D_WORLD_SCRATCH_INDEX_CAPACITY );
	valid = valid && Layout.addArray<uint32_t>( T3D_WORLD_SCRATCH_TRIANGLE_CAPACITY );
	valid = valid && Layout.addArray<uint8_t>( T3D_WORLD_SCRATCH_TRIANGLE_CAPACITY );
	valid = valid && Layout.addArray<t3dWorldInstance_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<t3dWorldTransform_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint64_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint32_t>( Limits.nMaxInstances );
	valid = valid && Layout.addArray<uint32_t>( Limits.nMaxCells );
	valid = valid && Layout.addArray<uint32_t>( Limits.nMaxCells );
	valid = valid && Layout.addArray<t3dWorldCellHashEntry_t>( T3D_WORLD_CELL_HASH_CAPACITY );
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

	t3dWorldGeometryDatabaseBuffers_t &GeometryBuffers = m_AuthoringGeometry.Buffers;
	GeometryBuffers.pVertexHeap = m_AuthoringArena.allocateArray<t3dWorldVertex_t>( nVertexCapacity );
	GeometryBuffers.nVertexCapacity = nVertexCapacity;
	GeometryBuffers.pClusterVertexIndexHeap = m_AuthoringArena.allocateArray<uint32_t>( nClusterVertexCapacity );
	GeometryBuffers.nClusterVertexIndexCapacity = nClusterVertexCapacity;
	GeometryBuffers.pTriangleHeap = m_AuthoringArena.allocateArray<t3dWorldPackedClusterTriangle_t>( nTriangleCapacity );
	GeometryBuffers.nTriangleCapacity = nTriangleCapacity;
	GeometryBuffers.pMeshRecords = m_AuthoringArena.allocateArray<t3dWorldMeshRecord_t>( Limits.nMaxMeshes );
	GeometryBuffers.nMeshCapacity = Limits.nMaxMeshes;
	GeometryBuffers.pLodRecords = m_AuthoringArena.allocateArray<t3dWorldMeshLodRecord_t>( Limits.nMaxMeshLods );
	GeometryBuffers.nLodCapacity = Limits.nMaxMeshLods;
	GeometryBuffers.pClusterRecords = m_AuthoringArena.allocateArray<t3dWorldClusterRecord_t>( Limits.nMaxClusters );
	GeometryBuffers.pWorldClusterRecords = m_AuthoringArena.allocateArray<t3dWorldCluster_t>( Limits.nMaxClusters );
	GeometryBuffers.pWorldClusterVtMetadata = m_AuthoringArena.allocateArray<t3dWorldClusterVTMetadata_t>( Limits.nMaxClusters );
	GeometryBuffers.nClusterCapacity = Limits.nMaxClusters;

	m_AuthoringBuffers.pGeometry = &m_AuthoringGeometry;
	m_AuthoringBuffers.pMaterials = m_AuthoringArena.allocateArray<t3dRenderingMaterialGpu_t>( Limits.nMaxMaterials );
	m_AuthoringBuffers.nMaterialCapacity = Limits.nMaxMaterials;
	m_AuthoringBuffers.pCells = m_AuthoringArena.allocateArray<t3dWorldCell_t>( Limits.nMaxCells );
	m_AuthoringBuffers.nCellCapacity = Limits.nMaxCells;
	m_AuthoringBuffers.pInstances = m_AuthoringArena.allocateArray<t3dWorldInstance_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.pTransforms = m_AuthoringArena.allocateArray<t3dWorldTransform_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.nInstanceCapacity = Limits.nMaxInstances;
	m_AuthoringBuffers.pInstanceSourceIds = m_AuthoringArena.allocateArray<uint64_t>( Limits.nMaxInstances );
	t3dWorldOwnershipEntry_t *const pOwnershipEntries =
	        m_AuthoringArena.allocateArray<t3dWorldOwnershipEntry_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.pOwnership = &m_AuthoringOwnership;
	m_AuthoringBuffers.pPrototypeEntries = m_AuthoringArena.allocateArray<t3dWorldPrototypeEntry_t>( Limits.nMaxMeshes );
	m_AuthoringBuffers.pPrototypeBoundsByMeshId = m_AuthoringArena.allocateArray<t3dWorldPrototypeBounds_t>( Limits.nMaxMeshes );
	m_AuthoringBuffers.nPrototypeBoundsCapacity = Limits.nMaxMeshes;
	m_AuthoringBuffers.nPrototypeCapacity = Limits.nMaxMeshes;
	m_AuthoringBuffers.pMaterialEntries = m_AuthoringArena.allocateArray<t3dWorldMaterialEntry_t>( Limits.nMaxMaterials );
	m_AuthoringBuffers.nMaterialEntryCapacity = Limits.nMaxMaterials;
	m_AuthoringBuffers.pNativeVisuals = m_AuthoringArena.allocateArray<t3dWorldNativeVisualEntry_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.nNativeVisualCapacity = Limits.nMaxInstances;

	m_AuthoringBuffers.Scratch.pVertices = m_AuthoringArena.allocateArray<t3dWorldVertex_t>( T3D_WORLD_SCRATCH_VERTEX_CAPACITY );
	m_AuthoringBuffers.Scratch.nVertexCapacity = T3D_WORLD_SCRATCH_VERTEX_CAPACITY;
	m_AuthoringBuffers.Scratch.pIndices = m_AuthoringArena.allocateArray<uint32_t>( T3D_WORLD_SCRATCH_INDEX_CAPACITY );
	m_AuthoringBuffers.Scratch.nIndexCapacity = T3D_WORLD_SCRATCH_INDEX_CAPACITY;
	m_AuthoringBuffers.Scratch.pTriangleMaterialOffsets = m_AuthoringArena.allocateArray<uint32_t>( T3D_WORLD_SCRATCH_TRIANGLE_CAPACITY );
	m_AuthoringBuffers.Scratch.pTriangleTiers = m_AuthoringArena.allocateArray<uint8_t>( T3D_WORLD_SCRATCH_TRIANGLE_CAPACITY );
	m_AuthoringBuffers.Scratch.nTriangleCapacity = T3D_WORLD_SCRATCH_TRIANGLE_CAPACITY;

	m_AuthoringBuffers.CellScratch.pReorderedInstances = m_AuthoringArena.allocateArray<t3dWorldInstance_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.CellScratch.pReorderedTransforms = m_AuthoringArena.allocateArray<t3dWorldTransform_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.CellScratch.pReorderedSourceIds = m_AuthoringArena.allocateArray<uint64_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.CellScratch.nInstanceCapacity = Limits.nMaxInstances;
	m_AuthoringBuffers.CellScratch.pInstanceCellIds = m_AuthoringArena.allocateArray<uint32_t>( Limits.nMaxInstances );
	m_AuthoringBuffers.CellScratch.pCellCounts = m_AuthoringArena.allocateArray<uint32_t>( Limits.nMaxCells );
	m_AuthoringBuffers.CellScratch.pCellWriteCursor = m_AuthoringArena.allocateArray<uint32_t>( Limits.nMaxCells );
	m_AuthoringBuffers.CellScratch.nCellScratchCapacity = Limits.nMaxCells;
	m_AuthoringBuffers.CellScratch.pCellHash = m_AuthoringArena.allocateArray<t3dWorldCellHashEntry_t>( T3D_WORLD_CELL_HASH_CAPACITY );
	m_AuthoringBuffers.CellScratch.nCellHashCapacity = T3D_WORLD_CELL_HASH_CAPACITY;

	if ( !GeometryBuffers.pVertexHeap || !GeometryBuffers.pClusterVertexIndexHeap || !GeometryBuffers.pTriangleHeap
	        || !GeometryBuffers.pMeshRecords || !GeometryBuffers.pLodRecords || !GeometryBuffers.pClusterRecords
	        || !GeometryBuffers.pWorldClusterRecords || !GeometryBuffers.pWorldClusterVtMetadata || !m_AuthoringBuffers.pMaterials
	        || !m_AuthoringBuffers.pCells || !m_AuthoringBuffers.pInstances || !m_AuthoringBuffers.pTransforms
	        || !m_AuthoringBuffers.pInstanceSourceIds || !pOwnershipEntries
	        || !m_AuthoringBuffers.pPrototypeEntries || !m_AuthoringBuffers.pPrototypeBoundsByMeshId || !m_AuthoringBuffers.pMaterialEntries
	        || !m_AuthoringBuffers.pNativeVisuals || !m_AuthoringBuffers.Scratch.pVertices || !m_AuthoringBuffers.Scratch.pIndices
	        || !m_AuthoringBuffers.Scratch.pTriangleMaterialOffsets || !m_AuthoringBuffers.Scratch.pTriangleTiers
	        || !m_AuthoringBuffers.CellScratch.pReorderedInstances || !m_AuthoringBuffers.CellScratch.pReorderedTransforms
	        || !m_AuthoringBuffers.CellScratch.pReorderedSourceIds
	        || !m_AuthoringBuffers.CellScratch.pInstanceCellIds || !m_AuthoringBuffers.CellScratch.pCellCounts
	        || !m_AuthoringBuffers.CellScratch.pCellWriteCursor || !m_AuthoringBuffers.CellScratch.pCellHash ) {
		releaseAuthoringImageCold();
		return false;
	}

	t3d_world_geometry_database_initialize_cold( m_AuthoringGeometry, GeometryBuffers );
	if ( !t3d_world_ownership_registry_initialize_cold( m_AuthoringOwnership, pOwnershipEntries, Limits.nMaxInstances ) ) {
		releaseAuthoringImageCold();
		return false;
	}
	t3dWorldGodotMaterialResolver_t resolver{};
	resolver.pResolveMaterialVT = &Terrain3DWorldController::resolveAutoMaterialVtCold;
	resolver.pUser = this;
	if ( !m_AutoConverter.initializeCold( buildPolicyCold(), resolver, &m_AuthoringBuffers ) ) {
		releaseAuthoringImageCold();
		return false;
	}
	m_pConverter = &m_AutoConverter;
	m_bAutoConverterInitialized = true;
	return true;
}

void Terrain3DWorldController::releaseAuthoringImageCold( void ) {
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

bool Terrain3DWorldController::releasePublishedSceneCold( void ) {
	if ( !m_bOwnershipApplied && !m_bPublicationPending ) {
		return true;
	}
	set_process_internal( false );
	if ( m_backend.pReleaseSceneCold ) {
		m_backend.pReleaseSceneCold( m_backend.pUser );
	}
	if ( m_pConverter && m_bOwnershipApplied ) {
		m_pConverter->releaseNativeOwnershipCold();
	}
	m_bOwnershipApplied = false;
	m_bPublicationPending = false;
	return true;
}

void Terrain3DWorldController::completePublishedSceneCold( void ) {
	if ( !m_bPublicationPending || !m_pConverter || !m_backend.pIsSceneReadyCold ) {
		return;
	}
	if ( !m_backend.pIsSceneReadyCold( m_backend.pUser ) ) {
		return;
	}
	Terrain3DColdPhaseScope cold_scope;
	m_bOwnershipApplied = m_pConverter->applyNativeOwnershipCold( true );
	if ( !m_bOwnershipApplied && m_backend.pReleaseSceneCold ) {
		m_backend.pReleaseSceneCold( m_backend.pUser );
	}
	m_bPublicationPending = false;
	set_process_internal( false );
}

void Terrain3DWorldController::setEnabled( const bool bEnabled ) {
	if ( m_bEnabled == bEnabled ) {
		return;
	}
	m_bEnabled = bEnabled;
	if ( !m_bEnabled ) {
		releasePublishedSceneCold();
	}
}

void Terrain3DWorldController::setMode( const int32_t iMode ) {
	const int32_t mode = iMode >= 0 && iMode <= ( int32_t )( T3D_WORLD_MODE_FULL_STATIC_SCENE ) ? iMode : ( int32_t )( T3D_WORLD_MODE_EXPLICIT );
	if ( m_iMode == mode ) {
		return;
	}
	m_iMode = mode;
	if ( m_iMode == ( int32_t )( T3D_WORLD_MODE_DISABLED ) ) {
		releasePublishedSceneCold();
	}
}

void Terrain3DWorldController::setConvertMeshInstances( const bool bEnabled ) {
	m_bConvertMeshInstances = bEnabled;
}

void Terrain3DWorldController::setConvertDynamicObjects( const bool bEnabled ) {
	m_bConvertDynamicObjects = bEnabled;
}

void Terrain3DWorldController::setConvertHiddenObjects( const bool bEnabled ) {
	m_bConvertHiddenObjects = bEnabled;
}

void Terrain3DWorldController::setCellSize( const float fCellSize ) {
	m_fCellSize = fCellSize < 4.0f ? 4.0f : ( fCellSize > 256.0f ? 256.0f : fCellSize );
}

void Terrain3DWorldController::setDefaultProfile( const Ref<Terrain3DWorldImportProfile> &rProfile ) {
	m_defaultProfile = rProfile;
}

void Terrain3DWorldController::setAutoRebuildOnReady( const bool bEnabled ) {
	m_bAutoRebuildOnReady = bEnabled;
}

t3dWorldConversionPolicy_t Terrain3DWorldController::buildPolicyCold( void ) const {
	t3dWorldConversionPolicy_t policy = t3d_world_authoring_default_policy();
	policy.eMode = m_bEnabled ? ( t3dWorldMode_t )( m_iMode ) : T3D_WORLD_MODE_DISABLED;
	policy.bConvertMeshInstances = m_bConvertMeshInstances;
	policy.bConvertMultiMeshInstances = false;
	policy.bConvertDynamicObjects = false;
	policy.bConvertHiddenObjects = m_bConvertHiddenObjects;
	policy.fCellSize = m_fCellSize;
	if ( m_defaultProfile.is_valid() ) {
		policy.nDefaultTier = m_defaultProfile->resolveTier( policy.nDefaultTier );
		policy.eMaterialPolicy = ( t3dWorldMaterialPolicy_t )( m_defaultProfile->getMaterialPolicy() );
		policy.eTransparencyPolicy = ( t3dWorldTransparencyPolicy_t )( m_defaultProfile->getTransparencyPolicy() );
	}
	return policy;
}

bool Terrain3DWorldController::requestColdRebuild( void ) {
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
	if ( !m_bEnabled || m_iMode == ( int32_t )( T3D_WORLD_MODE_DISABLED ) ) {
		return releasePublishedSceneCold();
	}

	// Never leave an old World publication alive while rebuilding the
	// authoring graph. Failure keeps Godot native ownership authoritative.
	if ( !releasePublishedSceneCold() ) {
		return false;
	}

	Terrain3DColdPhaseScope cold_scope;
	m_pConverter->setPolicyCold( buildPolicyCold() );
	if ( !m_pConverter->rebuildSceneCold( this, m_lastBuildStats ) ) {
		return false;
	}
	if ( !m_backend.pPublishSceneCold || !m_backend.pReleaseSceneCold || !m_backend.pIsSceneReadyCold ) {
		return false;
	}
	if ( !m_backend.pPublishSceneCold( m_backend.pUser, *m_pConverter->getBuildBuffersCold(), m_lastBuildStats ) ) {
		return false;
	}

	// Acceptance queues the handoff. Native visibility remains authoritative
	// until the backend acknowledges this producer's exact GPU-ready revision.
	m_bPublicationPending = true;
	set_process_internal( true );
	completePublishedSceneCold();
	return m_bPublicationPending || m_bOwnershipApplied;
}

bool Terrain3DWorldController::releaseWorldOwnership( void ) {
	return releasePublishedSceneCold();
}

Dictionary Terrain3DWorldController::getLastBuildStatsReferenceDebug( void ) const {
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

Dictionary Terrain3DWorldController::getBackendStatusReferenceDebug( void ) const {
	Dictionary status;
	status["converter_attached"] = m_pConverter != NULL;
	status["backend_attached"] = m_backend.pPublishSceneCold != NULL;
	status["coordinator_attached"] = m_pCoordinator != NULL;
	status["auto_converter_initialized"] = m_bAutoConverterInitialized;
	status["ownership_applied"] = m_bOwnershipApplied;
	status["publication_pending"] = m_bPublicationPending;
	status["readiness_callback_attached"] = m_backend.pIsSceneReadyCold != NULL;
	status["auto_rebuild_on_ready"] = m_bAutoRebuildOnReady;
	status["authoring_arena_bytes"] = ( int64_t )( m_nAuthoringArenaBytes );
	status["mode"] = m_iMode;
	status["enabled"] = m_bEnabled;
	return status;
}

void Terrain3DWorldController::attachConverterCold( Terrain3DWorldGodotConverter *const pConverter ) {
	releasePublishedSceneCold();
	if ( m_pConverter == &m_AutoConverter && pConverter != &m_AutoConverter ) {
		releaseAuthoringImageCold();
	}
	m_pConverter = pConverter;
}

void Terrain3DWorldController::attachBackendCold( const t3dWorldAuthoringBackend_t &rBackend ) {
	releasePublishedSceneCold();
	m_backend = rBackend;
}
