// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_node_streamer.h"

#include <limits>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>

#include "logger.h"
#include "terrain_3d.h"
#include "terrain_3d_data.h"
#include "terrain_3d_lighting_policy.h"
#include "terrain_3d_region.h"

namespace {

// Godot Variant types must not be constructed at shared-library load time:
// GDExtension function pointers are installed only after dlopen() completes.
// These accessors defer StringName construction until the streamer is used.
static StringName terrain3dNodeMethodActivate( void ) {
	return StringName( "terrain3d_stream_activate" );
}

static StringName terrain3dNodeMethodDeactivate( void ) {
	return StringName( "terrain3d_stream_deactivate" );
}

static StringName terrain3dNodeMethodLoadState( void ) {
	return StringName( "terrain3d_stream_load_state" );
}

static StringName terrain3dNodeMethodSaveState( void ) {
	return StringName( "terrain3d_stream_save_state" );
}

static void terrain3dApplyStreamedLightingMetadata(
	Node *pNode,
	const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rDescriptor,
	const bool bShadowProxy = false, const bool bDisableVisibleShadow = false
) {
	if ( !pNode ) {
		return;
	}
	if ( VisualInstance3D *visual = Object::cast_to<VisualInstance3D>( pNode ) ) {
		uint32_t layers = visual->get_layer_mask();
		layers &= ~TERRAIN3D_LIGHTING_MANAGED_LAYER_MASK;
		layers |= terrain3dLightingLayerForClass( rDescriptor.lightingClass );
		if ( !bShadowProxy && rDescriptor.bReceiveDynamicDirectional ) {
			layers |= TERRAIN3D_DIRECTIONAL_RECEIVER_LAYER;
		}
		visual->set_layer_mask( layers );
	}
	if ( GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>( pNode ) ) {
		if ( bShadowProxy ) {
			geometry->set_gi_mode( GeometryInstance3D::GI_MODE_DISABLED );
			geometry->set_cast_shadows_setting( GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY );
		} else {
			switch ( rDescriptor.bakedLightingMode ) {
				case T3DRegion::BAKED_LIGHTING_LIGHTMAP:
					geometry->set_gi_mode( GeometryInstance3D::GI_MODE_STATIC );
					break;
				case T3DRegion::BAKED_LIGHTING_PROBE:
					geometry->set_gi_mode( GeometryInstance3D::GI_MODE_DYNAMIC );
					break;
				case T3DRegion::BAKED_LIGHTING_DISABLED:
				case T3DRegion::BAKED_LIGHTING_MATERIAL_ONLY:
				default:
					geometry->set_gi_mode( GeometryInstance3D::GI_MODE_DISABLED );
					break;
			}
			if ( !rDescriptor.bDynamicShadowEligible || bDisableVisibleShadow ) {
				geometry->set_cast_shadows_setting( GeometryInstance3D::SHADOW_CASTING_SETTING_OFF );
			}
		}
	}
	for ( int child_index = 0; child_index < pNode->get_child_count(); ++child_index ) {
		terrain3dApplyStreamedLightingMetadata( pNode->get_child( child_index ), rDescriptor,
			bShadowProxy, bDisableVisibleShadow );
	}
}

static Node *terrain3dCreateShadowProxy( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rDescriptor ) {
	Ref<PackedScene> proxy_scene = rDescriptor.pShadowProxyScene;
	if ( proxy_scene.is_null() && !rDescriptor.shadowProxyScenePath.is_empty() ) {
		proxy_scene = ResourceLoader::get_singleton()->load( rDescriptor.shadowProxyScenePath, "PackedScene" );
	}
	if ( proxy_scene.is_null() ) {
		return nullptr;
	}
	return proxy_scene->instantiate();
}

} // namespace

T3DNodeStreamer::~T3DNodeStreamer( void ) {
	destroy( false );
}

Vector2i T3DNodeStreamer::locationFromIndex( const int iIndex ) {
	const int map_size = TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
	return Vector2i( iIndex % map_size - map_size / 2, iIndex / map_size - map_size / 2 );
}

int T3DNodeStreamer::indexFromLocation( const Vector2i &rLocation ) {
	return T3DData::getRegionMapIndex( rLocation );
}

Node *T3DNodeStreamer::getActiveNode( const t3dactiveNode_t &rActive ) const {
	Object *pObject = ObjectDB::get_instance( rActive.objectId );
	return pObject ? Object::cast_to<Node>( pObject ) : nullptr;
}

bool T3DNodeStreamer::runtimeHasSpawn( const t3dregionRuntime_t &rRuntime, const int64_t nSpawnId ) const {
	for ( const t3dactiveNode_t &active : rRuntime.nodes ) {
		if ( active.spawnId == nSpawnId ) {
			return true;
		}
	}
	return false;
}

int T3DNodeStreamer::findActiveNodeIndex( const t3dregionRuntime_t &rRuntime, const int64_t nSpawnId ) const {
	for ( int index = 0; index < ( int ) ( rRuntime.nodes.size() ); ++index ) {
		if ( rRuntime.nodes[( size_t ) ( index )].spawnId == nSpawnId ) {
			return index;
		}
	}
	return -1;
}

void T3DNodeStreamer::trackActiveRegion( const int iIndex ) {
	if ( iIndex < 0 || iIndex >= TERRAIN3D_MAX_REGION_SLOTS ) {
		return;
	}
	t3dregionRuntime_t &runtime = m_szRegions[iIndex];
	if ( runtime.iActiveListPosition >= 0 ) {
		return;
	}
	runtime.iActiveListPosition = ( int ) ( m_activeRegionIndices.size() );
	m_activeRegionIndices.push_back( iIndex );
	m_nActiveRegionCount = ( int ) ( m_activeRegionIndices.size() );
}

void T3DNodeStreamer::untrackActiveRegion( const int iIndex ) {
	if ( iIndex < 0 || iIndex >= TERRAIN3D_MAX_REGION_SLOTS ) {
		return;
	}
	t3dregionRuntime_t &runtime = m_szRegions[iIndex];
	const int position = runtime.iActiveListPosition;
	if ( position < 0 || position >= ( int ) ( m_activeRegionIndices.size() ) ) {
		runtime.iActiveListPosition = -1;
		return;
	}
	const int moved_index = m_activeRegionIndices.back();
	m_activeRegionIndices[( size_t ) ( position )] = moved_index;
	m_activeRegionIndices.pop_back();
	runtime.iActiveListPosition = -1;
	if ( moved_index != iIndex ) {
		m_szRegions[moved_index].iActiveListPosition = position;
	}
	m_nActiveRegionCount = ( int ) ( m_activeRegionIndices.size() );
}

void T3DNodeStreamer::setRegionLoading( t3dregionRuntime_t &rRuntime, const bool bLoading ) {
	if ( rRuntime.bLoading == bLoading ) {
		return;
	}
	rRuntime.bLoading = bLoading;
	m_nLoadingRegionCount += bLoading ? 1 : -1;
	m_nLoadingRegionCount = MAX( m_nLoadingRegionCount, 0 );
}

void T3DNodeStreamer::trackMovableNode( const int iRegionIndex, const t3dactiveNode_t &rActive ) {
	if ( ( rActive.flags & NODE_FLAG_STATIC ) != 0 ) {
		return;
	}
	t3dmovableNodeRef_t ref;
	ref.iRegionIndex = iRegionIndex;
	ref.spawnId = rActive.spawnId;
	ref.objectId = rActive.objectId;
	m_movableNodes.push_back( ref );
}

void T3DNodeStreamer::removeMovableNodeAt( const int iIndex ) {
	if ( iIndex < 0 || iIndex >= ( int ) ( m_movableNodes.size() ) ) {
		return;
	}
	m_movableNodes[( size_t ) ( iIndex )] = m_movableNodes.back();
	m_movableNodes.pop_back();
}

void T3DNodeStreamer::untrackMovableNode( const int64_t nSpawnId ) {
	for ( int index = ( int ) ( m_movableNodes.size() ) - 1; index >= 0; --index ) {
		if ( m_movableNodes[( size_t ) ( index )].spawnId == nSpawnId ) {
			removeMovableNodeAt( index );
			return;
		}
	}
}

void T3DNodeStreamer::eraseActiveNode( t3dregionRuntime_t &rRuntime, const int iNodeIndex ) {
	if ( iNodeIndex < 0 || iNodeIndex >= ( int ) ( rRuntime.nodes.size() ) ) {
		return;
	}
	rRuntime.nodes[( size_t ) ( iNodeIndex )] = rRuntime.nodes.back();
	rRuntime.nodes.pop_back();
}

Node3D *T3DNodeStreamer::ensureRegionRoot( const int iIndex, T3DRegion *const pRegion ) {
	if ( !m_pTerrain || !pRegion || iIndex < 0 || iIndex >= TERRAIN3D_MAX_REGION_SLOTS ) {
		return nullptr;
	}
	ensureHost();
	if ( !m_pHost ) {
		return nullptr;
	}
	t3dregionRuntime_t &runtime = m_szRegions[iIndex];
	if ( runtime.pRoot ) {
		return runtime.pRoot;
	}
	runtime.pRoot = memnew( Node3D );
	const Vector2i location = pRegion->getLocation();
	runtime.pRoot->set_name( String( "Region_" ) + String::num_int64( location.x ) + "_" + String::num_int64( location.y ) );
	const real_t fWorldSize = ( real_t ) ( m_pTerrain->getRegionSize() ) * m_pTerrain->getVertexSpacing();
	runtime.pRoot->set_position( Vector3( ( real_t ) ( location.x ) * fWorldSize, 0.0f, ( real_t ) ( location.y ) * fWorldSize ) );
	m_pHost->add_child( runtime.pRoot, false, Node::INTERNAL_MODE_BACK );
	trackActiveRegion( iIndex );
	setRegionLoading( runtime, true );
	runtime.bActive = false;
	runtime.nextSpawn = 0;
	runtime.bRequestsStarted = false;
	return runtime.pRoot;
}

bool T3DNodeStreamer::migrateActiveNodes( void ) {
	m_nMigrationUsecLastTick = 0u;
	if ( !m_pTerrain || !m_pTerrain->getData() || m_movableNodes.empty() ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	bool bChanged = false;
	T3DData *const pData = m_pTerrain->getData();
	const real_t fRegionWorldSize = ( real_t ) ( m_pTerrain->getRegionSize() ) * m_pTerrain->getVertexSpacing();
	if ( fRegionWorldSize <= 0.0f ) {
		return false;
	}
	const Transform3D terrain_inverse = m_pTerrain->get_global_transform().affine_inverse();
	for ( int movable_index = ( int ) ( m_movableNodes.size() ) - 1; movable_index >= 0; --movable_index ) {
		++m_nNodesTestedForMigrationLastTick;
		t3dmovableNodeRef_t &movable = m_movableNodes[( size_t ) ( movable_index )];
		const int source_index = movable.iRegionIndex;
		if ( source_index < 0 || source_index >= TERRAIN3D_MAX_REGION_SLOTS ) {
			removeMovableNodeAt( movable_index );
			continue;
		}
		t3dregionRuntime_t &source_runtime = m_szRegions[source_index];
		const int active_index = findActiveNodeIndex( source_runtime, movable.spawnId );
		T3DRegion *const pSourceRegion = pData->getRegionPtr( locationFromIndex( source_index ) );
		if ( active_index < 0 || !pSourceRegion ) {
			removeMovableNodeAt( movable_index );
			continue;
		}
		const t3dactiveNode_t active = source_runtime.nodes[( size_t ) ( active_index )];
		Node3D *const pNode = Object::cast_to<Node3D>( getActiveNode( active ) );
		if ( !pNode ) {
			pSourceRegion->removeStreamedNode( active.spawnId );
			eraseActiveNode( source_runtime, active_index );
			removeMovableNodeAt( movable_index );
			m_nActiveNodeCount = MAX( 0, m_nActiveNodeCount - 1 );
			bChanged = true;
			if ( source_runtime.pRoot && source_runtime.nodes.empty() && pSourceRegion->getStreamedNodeCount() == 0 ) {
				deactivateRegion( source_index, false );
			}
			continue;
		}
		++m_nGlobalTransformsReadLastTick;
		const Transform3D terrain_local = terrain_inverse * pNode->get_global_transform();
		const Vector2i destinationLocation(
			( int ) ( Math::floor( terrain_local.origin.x / fRegionWorldSize ) ),
			( int ) ( Math::floor( terrain_local.origin.z / fRegionWorldSize ) )
		);
		const int destination_index = indexFromLocation( destinationLocation );
		if ( destination_index < 0 || destination_index == source_index ) {
			continue;
		}

		const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t *pSourceDescriptor =
			pSourceRegion->findStreamedNodeRuntimeInternal( active.spawnId );
		if ( !pSourceDescriptor ) {
			continue;
		}
		const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t descriptor = *pSourceDescriptor;
		const String &scene_path = descriptor.scenePath;
		const Ref<PackedScene> &scene = descriptor.pScene;
		if ( scene_path.is_empty() && scene.is_null() ) {
			continue;
		}
		T3DRegion *pDestinationRegion = pData->getRegionPtr( destinationLocation );
		if ( !pDestinationRegion ) {
			const Ref<T3DRegion> pCreated = pData->addRegionBlank( destinationLocation, true );
			pDestinationRegion = pCreated.ptr();
		}
		if ( !pDestinationRegion ) {
			continue;
		}
		Transform3D destination_transform = terrain_local;
		destination_transform.origin.x -= ( real_t ) ( destinationLocation.x ) * fRegionWorldSize;
		destination_transform.origin.z -= ( real_t ) ( destinationLocation.y ) * fRegionWorldSize;
		const Dictionary &state = descriptor.state;
		const int flags = descriptor.flags;
		const int boundary_policy = descriptor.boundaryPolicy;
		const Vector2i source_coverage_min = descriptor.coverageMin;
		const Vector2i source_coverage_max = descriptor.coverageMax;
		const Vector2i owner_delta = pSourceRegion->getLocation() - destinationLocation;
		const Vector2i destination_coverage_min = source_coverage_min + owner_delta;
		const Vector2i destination_coverage_max = source_coverage_max + owner_delta;
		const int64_t transferred_id = !scene_path.is_empty() ?
			pDestinationRegion->addStreamedNodePath( scene_path, destination_transform, state, flags, active.spawnId,
				boundary_policy, destination_coverage_min, destination_coverage_max ) :
			pDestinationRegion->addStreamedNode( scene, destination_transform, state, flags, active.spawnId,
				boundary_policy, destination_coverage_min, destination_coverage_max );
		if ( transferred_id != active.spawnId ) {
			if ( transferred_id > 0 ) {
				pDestinationRegion->removeStreamedNode( transferred_id );
			}
			continue;
		}
		if ( !pSourceRegion->removeStreamedNode( active.spawnId ) ) {
			pDestinationRegion->removeStreamedNode( active.spawnId );
			continue;
		}
		Node3D *const pDestinationRoot = ensureRegionRoot( destination_index, pDestinationRegion );
		if ( !pDestinationRoot ) {
			if ( !scene_path.is_empty() ) {
				pSourceRegion->addStreamedNodePath( scene_path, descriptor.transform, state, flags,
					active.spawnId, boundary_policy, source_coverage_min, source_coverage_max );
			} else {
				pSourceRegion->addStreamedNode( scene, descriptor.transform, state, flags,
					active.spawnId, boundary_policy, source_coverage_min, source_coverage_max );
			}
			pDestinationRegion->removeStreamedNode( active.spawnId );
			continue;
		}
		pNode->reparent( pDestinationRoot, true );
		t3dregionRuntime_t &destination_runtime = m_szRegions[destination_index];
		if ( !runtimeHasSpawn( destination_runtime, active.spawnId ) ) {
			destination_runtime.nodes.push_back( active );
		}
		eraseActiveNode( source_runtime, active_index );
		movable.iRegionIndex = destination_index;
		++m_nMigrationsLastTick;
		bChanged = true;
		if ( source_runtime.pRoot && source_runtime.nodes.empty() && pSourceRegion->getStreamedNodeCount() == 0 ) {
			deactivateRegion( source_index, false );
		}
	}
	if ( bChanged ) {
		++m_nRuntimeGeneration;
	}
	if ( pTime ) {
		m_nMigrationUsecLastTick = pTime->get_ticks_usec() - started_usec;
	}
	return bChanged;
}

void T3DNodeStreamer::ensureHost( void ) {
	if ( m_pHost || !m_pTerrain || !m_pTerrain->is_inside_tree() ) {
		return;
	}
	m_pHost = memnew( Node3D );
	m_pHost->set_name( "Terrain3DNodeStreamHost" );
	m_pTerrain->add_child( m_pHost, false, Node::INTERNAL_MODE_BACK );
}

void T3DNodeStreamer::destroyHost( void ) {
	if ( !m_pHost ) {
		return;
	}
	if ( m_pHost->is_inside_tree() ) {
		m_pHost->get_parent()->remove_child( m_pHost );
	}
	memdelete( m_pHost );
	m_pHost = nullptr;
}

bool T3DNodeStreamer::spawnIdExists( const uint64_t nId ) const {
	if ( !m_pTerrain || !m_pTerrain->getData() || nId == 0u || nId > ( uint64_t ) ( INT64_MAX ) ) {
		return false;
	}
	T3DData *const pData = m_pTerrain->getData();
	const Vector2i *pLocations = pData->getRegionSlotsInternal();
	const int location_count = pData->getRegionSlotCountInternal();
	for ( int slot = 0; slot < location_count; ++slot ) {
		const Vector2i location = pLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		const T3DRegion *pRegion = pData->getRegionPtr( location );
		if ( !pRegion ) {
			continue;
		}
		for ( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &spawn :
			pRegion->getStreamedNodesRuntimeInternal() ) {
			if ( spawn.id > 0 && ( uint64_t ) ( spawn.id ) == nId ) {
				return true;
			}
		}
	}
	return false;
}

int64_t T3DNodeStreamer::allocateSpawnId( void ) {
	int descriptor_count = 0;
	if ( m_pTerrain && m_pTerrain->getData() ) {
		T3DData *const pData = m_pTerrain->getData();
		const Vector2i *pLocations = pData->getRegionSlotsInternal();
		const int location_count = pData->getRegionSlotCountInternal();
		for ( int slot = 0; slot < location_count; ++slot ) {
			if ( pLocations[slot] != V2I_MAX ) {
				T3DRegion *const pRegion = pData->getRegionPtr( pLocations[slot] );
				if ( pRegion ) {
					descriptor_count += pRegion->getStreamedNodeCount();
				}
			}
		}
	}
	for ( int attempt = 0; attempt <= descriptor_count; ++attempt ) {
		if ( m_nNextSpawnId == 0u || m_nNextSpawnId > ( uint64_t ) ( INT64_MAX ) ) {
			m_nNextSpawnId = 1u;
		}
		const uint64_t candidate = m_nNextSpawnId++;
		if ( !spawnIdExists( candidate ) ) {
			return ( int64_t ) ( candidate );
		}
	}
	return 0;
}

void T3DNodeStreamer::refreshSpawnId( void ) {
	m_nNextSpawnId = 1u;
	if ( !m_pTerrain || !m_pTerrain->getData() ) {
		return;
	}
	T3DData *const pData = m_pTerrain->getData();
	const Vector2i *pLocations = pData->getRegionSlotsInternal();
	const int location_count = pData->getRegionSlotCountInternal();
	for ( int slot = 0; slot < location_count; ++slot ) {
		if ( pLocations[slot] == V2I_MAX ) {
			continue;
		}
		const T3DRegion *pRegion = pData->getRegionPtr( pLocations[slot] );
		if ( !pRegion ) {
			continue;
		}
		for ( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &spawn :
			pRegion->getStreamedNodesRuntimeInternal() ) {
			const int64_t signed_id = spawn.id;
			if ( signed_id > 0 && ( uint64_t ) ( signed_id ) >= m_nNextSpawnId ) {
				m_nNextSpawnId = ( uint64_t ) ( signed_id ) < ( uint64_t ) ( INT64_MAX ) ? ( uint64_t ) ( signed_id ) + 1u : 1u;
			}
		}
	}
	if ( m_nNextSpawnId == 0u || m_nNextSpawnId > ( uint64_t ) ( INT64_MAX ) ) {
		m_nNextSpawnId = 1u;
	}
}

void T3DNodeStreamer::initialize( T3DTerrain *const pTerrain ) {
	if ( m_pTerrain == pTerrain && m_pHost ) {
		return;
	}
	destroy( false );
	m_pTerrain = pTerrain;
	ensureHost();
	refreshSpawnId();
}

void T3DNodeStreamer::captureNodeState(
	T3DRegion *const pRegion, Node3D *const pRegionRoot,
	const t3dactiveNode_t &rActive
) {
	if ( !pRegion ) {
		return;
	}
	Node *pNode = getActiveNode( rActive );
	if ( !pNode ) {
		return;
	}
	Transform3D local_transform;
	Node3D *const pNode3d = Object::cast_to<Node3D>( pNode );
	if ( pNode3d ) {
		local_transform = pRegionRoot ?
			pRegionRoot->get_global_transform().affine_inverse() * pNode3d->get_global_transform() :
			pNode3d->get_transform();
	}
	Dictionary state;
	const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t *pDescriptor =
		pRegion->findStreamedNodeRuntimeInternal( rActive.spawnId );
	if ( pDescriptor ) {
		state = pDescriptor->state.duplicate( true );
	}
	if ( pNode->has_method( terrain3dNodeMethodSaveState() ) ) {
		const Variant result = pNode->call( terrain3dNodeMethodSaveState() );
		if ( result.get_type() == Variant::DICTIONARY ) {
			state = Dictionary( result ).duplicate( true );
		}
	}
	pRegion->updateStreamedNodeState( rActive.spawnId, local_transform, state, rActive.flags );
	pNode = getActiveNode( rActive );
	if ( pNode && pNode->has_method( terrain3dNodeMethodDeactivate() ) ) {
		pNode->call( terrain3dNodeMethodDeactivate() );
	}
}

void T3DNodeStreamer::deactivateRegion( const int iIndex, const bool bCaptureState ) {
	if ( iIndex < 0 || iIndex >= TERRAIN3D_MAX_REGION_SLOTS ) {
		return;
	}
	t3dregionRuntime_t &runtime = m_szRegions[iIndex];
	if ( !runtime.pRoot && !runtime.bActive && !runtime.bLoading ) {
		return;
	}
	T3DRegion *const pRegion = m_pTerrain && m_pTerrain->getData() ?
		m_pTerrain->getData()->getRegionPtr( locationFromIndex( iIndex ) ) : nullptr;
	for ( const t3dactiveNode_t &active : runtime.nodes ) {
		Node *pNode = getActiveNode( active );
		if ( bCaptureState && pNode ) {
			captureNodeState( pRegion, runtime.pRoot, active );
		}
		pNode = getActiveNode( active );
		if ( pNode && runtime.pRoot && !pNode->is_queued_for_deletion() && pNode->get_parent() != runtime.pRoot ) {
			if ( pNode->get_parent() ) {
				pNode->reparent( runtime.pRoot, true );
			} else {
				runtime.pRoot->add_child( pNode );
			}
		}
		if ( ( active.flags & NODE_FLAG_STATIC ) == 0 ) {
			untrackMovableNode( active.spawnId );
		}
	}
	const int deactivated_node_count = ( int ) ( runtime.nodes.size() );
	m_nNodesDeactivatedLastTick += deactivated_node_count;
	if ( deactivated_node_count > 0 ) {
		++m_nRuntimeGeneration;
	}
	m_nActiveNodeCount -= deactivated_node_count;
	m_nActiveNodeCount = MAX( m_nActiveNodeCount, 0 );
	runtime.nodes.clear();
	if ( runtime.pRoot ) {
		if ( runtime.pRoot->is_inside_tree() && runtime.pRoot->get_parent() ) {
			runtime.pRoot->get_parent()->remove_child( runtime.pRoot );
		}
		memdelete( runtime.pRoot );
		runtime.pRoot = nullptr;
	}
	setRegionLoading( runtime, false );
	runtime.bActive = false;
	untrackActiveRegion( iIndex );
	runtime.nextSpawn = 0;
	runtime.bRequestsStarted = false;
	++m_nUnloadsLastTick;
}

T3DNodeStreamer::t3dspawnActivationResult_t T3DNodeStreamer::activateSpawn(
	T3DRegion *const pRegion, t3dregionRuntime_t &rRuntime,
	const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rSpawn
) {
	if ( !pRegion || rSpawn.id <= 0 ) {
		return SPAWN_ACTIVATION_FAILED;
	}
	const int64_t spawn_id = rSpawn.id;
	if ( runtimeHasSpawn( rRuntime, spawn_id ) ) {
		return SPAWN_ACTIVATION_CONSUMED;
	}
	const int flags = rSpawn.flags;
	if ( ( flags & NODE_FLAG_DISABLED ) != 0 ) {
		return SPAWN_ACTIVATION_CONSUMED;
	}
	if ( isHlodVisualSuppressed( pRegion->getLocation(), rSpawn ) ) {
		return SPAWN_ACTIVATION_CONSUMED;
	}
	Ref<PackedScene> scene = rSpawn.pScene;
	const String scene_path = rSpawn.scenePath;
	if ( scene.is_null() && !scene_path.is_empty() ) {
		if ( !scene_path.is_empty() ) {
			ResourceLoader *const pLoader = ResourceLoader::get_singleton();
			if ( !pLoader ) {
				return SPAWN_ACTIVATION_FAILED;
			}
			const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( scene_path );
			if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
				const Error request_error = pLoader->load_threaded_request( scene_path, "PackedScene", true,
					ResourceLoader::CACHE_MODE_REUSE );
				if ( request_error != OK ) {
					LOG( T3DTerrain::ERROR, "Failed requesting streamed scene ", scene_path, ": ", request_error );
					return SPAWN_ACTIVATION_FAILED;
				}
				++m_nSceneRequestsLastTick;
				return SPAWN_ACTIVATION_PENDING;
			}
			if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
				return SPAWN_ACTIVATION_PENDING;
			}
			if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
				LOG( T3DTerrain::ERROR, "Failed loading streamed scene ", scene_path );
				return SPAWN_ACTIVATION_FAILED;
			}
			const Ref<Resource> pResource = pLoader->load_threaded_get( scene_path );
			scene = pResource;
		}
	}
	if ( scene.is_null() ) {
		if ( !scene_path.is_empty() ) {
			LOG( T3DTerrain::ERROR, "Streamed scene is not a PackedScene: ", scene_path );
		}
		return SPAWN_ACTIVATION_FAILED;
	}
	Node *pNode = scene->instantiate();
	if ( !pNode ) {
		LOG( T3DTerrain::ERROR, "Failed instantiating streamed scene ", scene_path );
		return SPAWN_ACTIVATION_FAILED;
	}
	pNode->set_name( String( "Spawn_" ) + String::num_int64( spawn_id ) );
	Node3D *const pNode3d = Object::cast_to<Node3D>( pNode );
	if ( pNode3d ) {
		pNode3d->set_transform( rSpawn.transform );
	}
	const bool bHasShadowProxy = rSpawn.pShadowProxyScene.is_valid() || !rSpawn.shadowProxyScenePath.is_empty();
	terrain3dApplyStreamedLightingMetadata( pNode, rSpawn, false, bHasShadowProxy );
	if ( bHasShadowProxy && rSpawn.bDynamicShadowEligible ) {
		Node *const proxy = terrain3dCreateShadowProxy( rSpawn );
		if ( proxy ) {
			proxy->set_name( "ShadowProxy" );
			terrain3dApplyStreamedLightingMetadata( proxy, rSpawn, true, false );
			pNode->add_child( proxy );
		}
	}
	rRuntime.pRoot->add_child( pNode );
	const ObjectID objectId( pNode->get_instance_id() );
	if ( !rSpawn.state.is_empty() && pNode->has_method( terrain3dNodeMethodLoadState() ) ) {
		const Dictionary state = rSpawn.state.duplicate( true );
		pNode->call( terrain3dNodeMethodLoadState(), state );
		pNode = Object::cast_to<Node>( ObjectDB::get_instance( objectId ) );
		if ( !pNode ) {
			pRegion->removeStreamedNode( spawn_id );
			return SPAWN_ACTIVATION_CONSUMED;
		}
	}
	if ( pNode->has_method( terrain3dNodeMethodActivate() ) ) {
		pNode->call( terrain3dNodeMethodActivate() );
		pNode = Object::cast_to<Node>( ObjectDB::get_instance( objectId ) );
		if ( !pNode ) {
			pRegion->removeStreamedNode( spawn_id );
			return SPAWN_ACTIVATION_CONSUMED;
		}
	}
	t3dactiveNode_t active;
	active.spawnId = spawn_id;
	active.objectId = objectId;
	active.flags = flags;
	rRuntime.nodes.push_back( active );
	if ( pNode3d && ( flags & NODE_FLAG_STATIC ) == 0 ) {
		const int region_index = indexFromLocation( pRegion->getLocation() );
		if ( region_index >= 0 ) {
			trackMovableNode( region_index, active );
		}
	}
	++m_nActiveNodeCount;
	++m_nNodesActivatedLastTick;
	++m_nRuntimeGeneration;
	return SPAWN_ACTIVATION_CONSUMED;
}

void T3DNodeStreamer::requestRegionScenes(
	const Vector2i &rOwnerLocation,
	const eastl::vector<T3DRegion::t3dstreamedNodeRuntimeDescriptor_t> &rSpawns
) {
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		return;
	}
	for ( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &spawn : rSpawns ) {
		if ( isHlodVisualSuppressed( rOwnerLocation, spawn ) ) {
			continue;
		}
		if ( spawn.pScene.is_valid() || spawn.scenePath.is_empty() ) {
			continue;
		}
		const String &scene_path = spawn.scenePath;
		if ( scene_path.is_empty() || pLoader->load_threaded_get_status( scene_path ) !=
			ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
			continue;
		}
		const Error error = pLoader->load_threaded_request( scene_path, "PackedScene", true,
			ResourceLoader::CACHE_MODE_REUSE );
		if ( error != OK ) {
			LOG( T3DTerrain::ERROR, "Failed requesting streamed scene ", scene_path, ": ", error );
		} else {
			++m_nSceneRequestsLastTick;
		}
	}
}

bool T3DNodeStreamer::activateRegion( const int iIndex, int &nNodeBudget ) {
	if ( !m_pTerrain || !m_pTerrain->getData() || iIndex < 0 || iIndex >= TERRAIN3D_MAX_REGION_SLOTS ) {
		return false;
	}
	T3DRegion *const pRegion = m_pTerrain->getData()->getRegionPtr( locationFromIndex( iIndex ) );
	if ( !pRegion || pRegion->isDeleted() || pRegion->getStreamedNodeCount() == 0 ) {
		return false;
	}
	t3dregionRuntime_t &runtime = m_szRegions[iIndex];
	if ( !ensureRegionRoot( iIndex, pRegion ) ) {
		return false;
	}
	const eastl::vector<T3DRegion::t3dstreamedNodeRuntimeDescriptor_t> &spawns =
		pRegion->getStreamedNodesRuntimeInternal();
	if ( !runtime.bRequestsStarted ) {
		requestRegionScenes( pRegion->getLocation(), spawns );
		runtime.bRequestsStarted = true;
	}
	while ( runtime.nextSpawn < ( int ) ( spawns.size() ) && nNodeBudget > 0 ) {
		const t3dspawnActivationResult_t result = activateSpawn( pRegion, runtime, spawns[( size_t ) ( runtime.nextSpawn )] );
		if ( result == SPAWN_ACTIVATION_PENDING ) {
			++m_nScenePendingPollsLastTick;
			break;
		}
		++runtime.nextSpawn;
		--nNodeBudget;
		if ( result == SPAWN_ACTIVATION_FAILED ) {
			++m_nSceneLoadFailuresLastTick;
		}
	}
	if ( runtime.nextSpawn >= ( int ) ( spawns.size() ) ) {
		setRegionLoading( runtime, false );
		runtime.bActive = true;
	}
	return true;
}

bool T3DNodeStreamer::isHlodVisualSuppressed(
	const Vector2i &rOwnerLocation, const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rSpawn
) const {
	if ( !m_pTerrain || ( rSpawn.flags & NODE_FLAG_HLOD_VISUAL ) == 0 ) { return false; }
	const uint8_t baked_min = m_pTerrain->getHlodNodeMinimumLevel( rOwnerLocation, rSpawn.id );
	return baked_min > 0 && m_pTerrain->isHlodRegionSuppressed( rOwnerLocation, baked_min );
}

bool T3DNodeStreamer::streamedSpawnCoverageIntersects(
	const Vector2i &rOwnerLocation, const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rSpawn,
	const Vector2i &rTargetMin, const Vector2i &rTargetMax
) const {
	if ( rOwnerLocation.x >= rTargetMin.x && rOwnerLocation.x <= rTargetMax.x &&
		rOwnerLocation.y >= rTargetMin.y && rOwnerLocation.y <= rTargetMax.y ) { return true; }
	if ( rSpawn.boundaryPolicy != T3DRegion::STREAMED_NODE_BOUNDARY_COVERAGE ) { return false; }
	const Vector2i coverage_min = rOwnerLocation + rSpawn.coverageMin;
	const Vector2i coverage_max = rOwnerLocation + rSpawn.coverageMax;
	return coverage_min.x <= rTargetMax.x && coverage_max.x >= rTargetMin.x &&
		coverage_min.y <= rTargetMax.y && coverage_max.y >= rTargetMin.y;
}

bool T3DNodeStreamer::usesFullAuthoringResidency( void ) const {
	return m_pTerrain && m_pTerrain->isEditorAuthoringMode() &&
		!m_pTerrain->isEditorNodeStreamingPreviewActive();
}

int T3DNodeStreamer::getEffectiveRadiusRegions( void ) const {
	if ( usesFullAuthoringResidency() && m_pTerrain && m_pTerrain->getData() ) {
		return m_pTerrain->getData()->getRegionMapSizeInternal();
	}
	return m_nRadiusRegions;
}

void T3DNodeStreamer::buildDesiredSet( const Vector2i &rCenter ) {
	m_desiredRegions.reset();
	m_desiredRegionIndices.clear();
	if ( !m_pTerrain || !m_pTerrain->getData() ) {
		return;
	}
	const bool full_authoring_residency = usesFullAuthoringResidency();
	const int radius = getEffectiveRadiusRegions();
	const Vector2i target_min = rCenter - Vector2i( radius, radius );
	const Vector2i target_max = rCenter + Vector2i( radius, radius );
	T3DData *const pData = m_pTerrain->getData();
	const Vector2i *pLocations = pData->getRegionSlotsInternal();
	const int location_count = pData->getRegionSlotCountInternal();
	for ( int slot = 0; slot < location_count; ++slot ) {
		const Vector2i owner_location = pLocations[slot];
		if ( owner_location == V2I_MAX ) {
			continue;
		}
		T3DRegion *const pRegion = pData->getRegionPtr( owner_location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->getStreamedNodeCount() == 0 ) {
			continue;
		}
		const int owner_distance = MAX( Math::abs( owner_location.x - rCenter.x ),
			Math::abs( owner_location.y - rCenter.y ) );
		const bool ordinary_owner_desired = full_authoring_residency || owner_distance <= radius;
		bool region_desired = false;
		for ( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &spawn : pRegion->getStreamedNodesRuntimeInternal() ) {
			if ( ( spawn.flags & NODE_FLAG_DISABLED ) != 0 ) { continue; }
			if ( ( spawn.flags & NODE_FLAG_HLOD_VISUAL ) != 0 ) {
				const uint8_t baked_min = m_pTerrain->getHlodNodeMinimumLevel( owner_location, spawn.id );
				if ( baked_min > 0 ) {
					if ( !m_pTerrain->isHlodRegionSuppressed( owner_location, baked_min ) ) { region_desired = true; break; }
					continue;
				}
			}
			if ( ordinary_owner_desired || streamedSpawnCoverageIntersects( owner_location, spawn, target_min, target_max ) ) {
				region_desired = true; break;
			}
		}
		if ( region_desired ) {
			const int index = indexFromLocation( owner_location );
			if ( index >= 0 ) {
				m_desiredRegions.set( ( size_t ) index );
				m_desiredRegionIndices.push_back( index );
			}
		}
	}
}


bool T3DNodeStreamer::isRegionDesired( const Vector2i &rLocation ) const {
	const int index = indexFromLocation( rLocation );
	return index >= 0 && m_desiredRegions.test( ( size_t ) ( index ) );
}

void T3DNodeStreamer::deferUpdateForFrame( void ) {
	m_nLoadsLastTick = 0;
	m_nUnloadsLastTick = 0;
	m_nSceneLoadFailuresLastTick = 0;
	m_nNodesActivatedLastTick = 0;
	m_nNodesDeactivatedLastTick = 0;
	m_nSceneRequestsLastTick = 0;
	m_nScenePendingPollsLastTick = 0;
	m_nLoadRegionBudgetUsedLastTick = 0;
	m_nLoadNodeBudgetUsedLastTick = 0;
	m_nUnloadRegionBudgetUsedLastTick = 0;
	m_bLoadRegionBudgetLimitedLastTick = false;
	m_bUnloadRegionBudgetLimitedLastTick = false;
	m_iFixedSlotsScannedLastTick = 0;
	m_nActiveRegionsExaminedLastTick = 0;
	m_nNodesTestedForMigrationLastTick = 0;
	m_nGlobalTransformsReadLastTick = 0;
	m_nMigrationsLastTick = 0;
	m_nMigrationUsecLastTick = 0u;
	m_bDeferredLastTick = true;
}

void T3DNodeStreamer::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
	m_bDeferredLastTick = false;
	m_nLoadsLastTick = 0;
	m_nUnloadsLastTick = 0;
	m_nSceneLoadFailuresLastTick = 0;
	m_nNodesActivatedLastTick = 0;
	m_nNodesDeactivatedLastTick = 0;
	m_nSceneRequestsLastTick = 0;
	m_nScenePendingPollsLastTick = 0;
	m_nLoadRegionBudgetUsedLastTick = 0;
	m_nLoadNodeBudgetUsedLastTick = 0;
	m_nUnloadRegionBudgetUsedLastTick = 0;
	m_bLoadRegionBudgetLimitedLastTick = false;
	m_bUnloadRegionBudgetLimitedLastTick = false;
	m_iFixedSlotsScannedLastTick = 0;
	m_nActiveRegionsExaminedLastTick = 0;
	m_nNodesTestedForMigrationLastTick = 0;
	m_nGlobalTransformsReadLastTick = 0;
	m_nMigrationsLastTick = 0;
	m_nMigrationUsecLastTick = 0u;
	if ( !m_bEnabled || !m_pTerrain || !m_pTerrain->getData() ) {
		return;
	}
	const real_t fRegionWorldSize = ( real_t ) ( m_pTerrain->getRegionSize() ) * m_pTerrain->getVertexSpacing();
	if ( fRegionWorldSize <= 0.0f ) {
		return;
	}
	const bool bOwnershipChanged = migrateActiveNodes();
	const Transform3D inverse = m_pTerrain->get_global_transform().affine_inverse();
	const Vector3 local_position = inverse.xform( rTargetPosition );
	const int map_size = m_pTerrain->getData()->getRegionMapSizeInternal();
	const int half = map_size / 2;
	const Vector2i rawCenter(
		( int ) ( Math::floor( local_position.x / fRegionWorldSize ) ),
		( int ) ( Math::floor( local_position.z / fRegionWorldSize ) )
	);
	const Vector2i center( CLAMP( rawCenter.x, -half, half - 1 ), CLAMP( rawCenter.y, -half, half - 1 ) );
	const bool bRebuildDesired = bForce || bOwnershipChanged || center != m_center;
	if ( !bRebuildDesired && m_nLoadingRegionCount == 0 && !m_bHasPendingActivation ) {
		bool bPendingUnload = false;
		for ( const int index : m_activeRegionIndices ) {
			++m_nActiveRegionsExaminedLastTick;
			if ( !m_desiredRegions.test( ( size_t ) ( index ) ) ) {
				bPendingUnload = true;
				break;
			}
		}
		if ( !bPendingUnload ) {
			return;
		}
	}
	if ( bRebuildDesired ) {
		m_center = center;
		buildDesiredSet( center );
	}
	const bool authoring_full_residency = usesFullAuthoringResidency();
	m_bHasPendingActivation = false;
	const int effective_unload_budget = authoring_full_residency ?
		MAX( 1, getEffectiveUnloadRegionBudget() ) : getEffectiveUnloadRegionBudget();
	int unload_budget = effective_unload_budget;
	int active_position = 0;
	while ( active_position < ( int ) ( m_activeRegionIndices.size() ) && unload_budget > 0 ) {
		const int index = m_activeRegionIndices[( size_t ) ( active_position )];
		++m_nActiveRegionsExaminedLastTick;
		if ( !m_desiredRegions.test( ( size_t ) ( index ) ) ) {
			deactivateRegion( index, true );
			--unload_budget;
			continue;
		}
		++active_position;
	}
	if ( unload_budget == 0 ) {
		for ( ; active_position < ( int ) ( m_activeRegionIndices.size() ); ++active_position ) {
			++m_nActiveRegionsExaminedLastTick;
			if ( !m_desiredRegions.test( ( size_t ) ( m_activeRegionIndices[( size_t ) ( active_position )] ) ) ) {
				m_bUnloadRegionBudgetLimitedLastTick = true;
				break;
			}
		}
	}
	m_nUnloadRegionBudgetUsedLastTick = effective_unload_budget - unload_budget;
	const int effective_load_budget = authoring_full_residency ?
		MAX( 1, getEffectiveLoadRegionBudget() ) : getEffectiveLoadRegionBudget();
	int region_budget = effective_load_budget;
	int node_budget = authoring_full_residency ? MAX( 1, m_nLoadNodeBudget ) : m_nLoadNodeBudget;
	const int residency_radius = getEffectiveRadiusRegions();
	for ( int radius = 0; radius <= residency_radius && node_budget > 0; ++radius ) {
		for ( int z = -radius; z <= radius && node_budget > 0; ++z ) {
			for ( int x = -radius; x <= radius && node_budget > 0; ++x ) {
				if ( MAX( Math::abs( x ), Math::abs( z ) ) != radius ) {
					continue;
				}
				const int index = indexFromLocation( center + Vector2i( x, z ) );
				if ( index < 0 || !m_desiredRegions.test( ( size_t ) ( index ) ) ) {
					continue;
				}
				t3dregionRuntime_t &runtime = m_szRegions[index];
				if ( !runtime.pRoot && region_budget <= 0 ) {
					m_bLoadRegionBudgetLimitedLastTick = true;
					m_bHasPendingActivation = true;
					continue;
				}
				const bool bHadRoot = runtime.pRoot != nullptr;
				if ( activateRegion( index, node_budget ) ) {
					if ( !bHadRoot ) {
						--region_budget;
						++m_nLoadsLastTick;
					}
				}
			}
		}
	}
	// Extended bounds coverage can make an owner region desired even when the
	// owner/pivot itself is outside the ordinary radius. Iterate only the compact
	// desired owner list so this path never walks the 4096-slot logical plane.
	for ( const int index : m_desiredRegionIndices ) {
		if ( node_budget <= 0 ) {
			break;
		}
		const Vector2i location = locationFromIndex( index );
		if ( MAX( Math::abs( location.x - center.x ), Math::abs( location.y - center.y ) ) <= residency_radius ) {
			continue;
		}
		t3dregionRuntime_t &runtime = m_szRegions[index];
		if ( !runtime.pRoot && region_budget <= 0 ) {
			m_bLoadRegionBudgetLimitedLastTick = true;
			m_bHasPendingActivation = true;
			continue;
		}
		const bool bHadRoot = runtime.pRoot != nullptr;
		if ( activateRegion( index, node_budget ) && !bHadRoot ) {
			--region_budget;
			++m_nLoadsLastTick;
		}
	}
	for ( const int index : m_desiredRegionIndices ) {
		if ( !m_szRegions[index].bActive ) {
			m_bHasPendingActivation = true;
			break;
		}
	}
	m_nLoadRegionBudgetUsedLastTick = effective_load_budget - region_budget;
	m_nLoadNodeBudgetUsedLastTick = authoring_full_residency ?
		MAX( 0, MAX( 1, m_nLoadNodeBudget ) - node_budget ) : m_nLoadNodeBudget - node_budget;
}

void T3DNodeStreamer::suspend( void ) {
	while ( !m_activeRegionIndices.empty() ) {
		deactivateRegion( m_activeRegionIndices.back(), true );
	}
	m_desiredRegions.reset();
	m_desiredRegionIndices.clear();
	m_bHasPendingActivation = false;
	m_center = V2I_MAX;
}

void T3DNodeStreamer::destroy( const bool bCaptureState ) {
	while ( !m_activeRegionIndices.empty() ) {
		deactivateRegion( m_activeRegionIndices.back(), bCaptureState );
	}
	for ( int index = 0; index < TERRAIN3D_MAX_REGION_SLOTS; ++index ) {
		// Normal residency churn retains node-vector capacity so revisiting a region
		// does not return to the allocator. Full streamer teardown is the cold path
		// where retained capacity should actually be released.
		eastl::vector<t3dactiveNode_t>().swap( m_szRegions[index].nodes );
		m_szRegions[index].iActiveListPosition = -1;
	}
	eastl::vector<int>().swap( m_activeRegionIndices );
	eastl::vector<int>().swap( m_desiredRegionIndices );
	eastl::vector<t3dmovableNodeRef_t>().swap( m_movableNodes );
	destroyHost();
	m_desiredRegions.reset();
	m_pTerrain = nullptr;
	m_center = V2I_MAX;
	m_nActiveRegionCount = 0;
	m_nActiveNodeCount = 0;
	m_nLoadingRegionCount = 0;
	m_bHasPendingActivation = false;
}

void T3DNodeStreamer::setEnabled( const bool bEnabled ) {
	if ( m_bEnabled == bEnabled ) {
		return;
	}
	m_bEnabled = bEnabled;
	if ( !m_bEnabled ) {
		suspend();
	}
}

int64_t T3DNodeStreamer::addStreamedSceneInternal(
	const Ref<PackedScene> &rScene,
	const String &rScenePath, const Transform3D &rGlobalTransform, const Dictionary &rState,
	const int nFlags, const int nHlodMinLevel
) {
	if ( !m_pTerrain || !m_pTerrain->getData() || ( rScene.is_null() && rScenePath.is_empty() ) ) {
		return 0;
	}
	const Transform3D local = m_pTerrain->get_global_transform().affine_inverse() * rGlobalTransform;
	const real_t fRegionWorldSize = ( real_t ) ( m_pTerrain->getRegionSize() ) * m_pTerrain->getVertexSpacing();
	if ( fRegionWorldSize <= 0.0f ) {
		return 0;
	}
	const Vector2i location(
		( int ) ( Math::floor( local.origin.x / fRegionWorldSize ) ),
		( int ) ( Math::floor( local.origin.z / fRegionWorldSize ) )
	);
	if ( T3DData::getRegionMapIndex( location ) < 0 ) {
		return 0;
	}
	T3DRegion *pRegion = m_pTerrain->getData()->getRegionPtr( location );
	if ( !pRegion ) {
		const Ref<T3DRegion> pCreated = m_pTerrain->getData()->addRegionBlank( location, true );
		pRegion = pCreated.ptr();
	}
	if ( !pRegion ) {
		return 0;
	}
	Transform3D region_local = local;
	region_local.origin.x -= ( real_t ) ( location.x ) * fRegionWorldSize;
	region_local.origin.z -= ( real_t ) ( location.y ) * fRegionWorldSize;
	const int64_t id = allocateSpawnId();
	if ( id <= 0 ) {
		return 0;
	}
	const int effective_flags = ( nFlags & NODE_FLAG_HLOD_VISUAL ) != 0 ? ( nFlags | NODE_FLAG_STATIC ) : nFlags;
	const int64_t stored_id = !rScenePath.is_empty() ?
		pRegion->addStreamedNodePath( rScenePath, region_local, rState, effective_flags, id, T3DRegion::STREAMED_NODE_BOUNDARY_ANCHOR, V2I_ZERO, V2I_ZERO, nHlodMinLevel ) :
		pRegion->addStreamedNode( rScene, region_local, rState, effective_flags, id, T3DRegion::STREAMED_NODE_BOUNDARY_ANCHOR, V2I_ZERO, V2I_ZERO, nHlodMinLevel );
	if ( stored_id != id ) {
		if ( stored_id > 0 ) {
			pRegion->removeStreamedNode( stored_id );
		}
		return 0;
	}
	const int index = indexFromLocation( location );
	if ( index >= 0 && m_szRegions[index].pRoot ) {
		deactivateRegion( index, true );
	}
	m_center = V2I_MAX;
	return id;
}

int64_t T3DNodeStreamer::addStreamedScene(
	const Ref<PackedScene> &rScene,
	const Transform3D &rGlobalTransform, const Dictionary &rState, const int nFlags, const int nHlodMinLevel
) {
	return addStreamedSceneInternal( rScene, String(), rGlobalTransform, rState, nFlags, nHlodMinLevel );
}

int64_t T3DNodeStreamer::addStreamedScenePath(
	const String &rScenePath,
	const Transform3D &rGlobalTransform, const Dictionary &rState, const int nFlags, const int nHlodMinLevel
) {
	return addStreamedSceneInternal( Ref<PackedScene>(), rScenePath, rGlobalTransform, rState, nFlags, nHlodMinLevel );
}

bool T3DNodeStreamer::removeStreamedScene( const int64_t nSpawnId ) {
	if ( !m_pTerrain || !m_pTerrain->getData() || nSpawnId <= 0 ) {
		return false;
	}
	T3DRegion *const *ppRegions = m_pTerrain->getData()->getRegionPtrMapInternal();
	for ( int index = 0; index < TERRAIN3D_MAX_REGION_SLOTS; ++index ) {
		T3DRegion *const pRegion = ppRegions[index];
		if ( !pRegion || !pRegion->removeStreamedNode( nSpawnId ) ) {
			continue;
		}
		if ( m_szRegions[index].pRoot ) {
			deactivateRegion( index, false );
		}
		m_center = V2I_MAX;
		return true;
	}
	return false;
}

size_t T3DNodeStreamer::getRetainedCapacityBytes( void ) const {
	size_t bytes = 0u;
	for ( int index = 0; index < TERRAIN3D_MAX_REGION_SLOTS; ++index ) {
		bytes += m_szRegions[index].nodes.capacity() * sizeof( t3dactiveNode_t );
	}
	bytes += m_activeRegionIndices.capacity() * sizeof( int );
	bytes += m_desiredRegionIndices.capacity() * sizeof( int );
	bytes += m_movableNodes.capacity() * sizeof( t3dmovableNodeRef_t );
	return bytes;
}

void T3DNodeStreamer::onHlodRepresentationChanged( const Vector2i &rRegion ) {
	const int index = indexFromLocation( rRegion );
	if ( index < 0 || !m_pTerrain || !m_pTerrain->getData() ) { return; }
	t3dregionRuntime_t &runtime = m_szRegions[index];
	T3DRegion *const pRegion = m_pTerrain->getData()->getRegionPtr( rRegion );
	if ( !pRegion ) { return; }
	for ( int i = ( int ) runtime.nodes.size() - 1; i >= 0; --i ) {
		const t3dactiveNode_t active = runtime.nodes[( size_t ) i];
		if ( ( active.flags & NODE_FLAG_HLOD_VISUAL ) == 0 ) { continue; }
		const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t *spawn = pRegion->findStreamedNodeRuntimeInternal( active.spawnId );
		if ( !spawn || !isHlodVisualSuppressed( rRegion, *spawn ) ) { continue; }
		Node *node = getActiveNode( active );
		if ( node ) {
			if ( node->get_parent() ) { node->get_parent()->remove_child( node ); }
			memdelete( node );
		}
		eraseActiveNode( runtime, i );
		m_nActiveNodeCount = MAX( 0, m_nActiveNodeCount - 1 );
		++m_nNodesDeactivatedLastTick;
	}
	if ( runtime.pRoot ) {
		runtime.nextSpawn = 0;
		runtime.bRequestsStarted = false;
		runtime.bActive = false;
		setRegionLoading( runtime, true );
	}
	m_center = V2I_MAX;
	m_bHasPendingActivation = true;
	++m_nRuntimeGeneration;
}

bool T3DNodeStreamer::areHlodVisualRegionsReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const {
	if ( !m_pTerrain || !m_pTerrain->getData() ) { return true; }
	for ( int z = rRegionMin.y; z <= rRegionMax.y; ++z ) {
		for ( int x = rRegionMin.x; x <= rRegionMax.x; ++x ) {
			const Vector2i region( x, z );
			const int index = indexFromLocation( region );
			if ( index < 0 ) { continue; }
			T3DRegion *const pRegion = m_pTerrain->getData()->getRegionPtr( region );
			if ( !pRegion ) { continue; }
			const t3dregionRuntime_t &runtime = m_szRegions[index];
			for ( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &spawn : pRegion->getStreamedNodesRuntimeInternal() ) {
				if ( ( spawn.flags & NODE_FLAG_HLOD_VISUAL ) == 0 ) { continue; }
				const uint8_t baked_min = m_pTerrain->getHlodNodeMinimumLevel( region, spawn.id );
				if ( baked_min == 0 || m_pTerrain->isHlodRegionSuppressed( region, baked_min ) ) { continue; }
				if ( !runtimeHasSpawn( runtime, spawn.id ) ) { return false; }
			}
		}
	}
	return true;
}

Dictionary T3DNodeStreamer::getStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bEnabled;
	stats["radius_regions"] = m_nRadiusRegions;
	stats["effective_radius_regions"] = getEffectiveRadiusRegions();
	stats["editor_preview"] = m_pTerrain && m_pTerrain->isEditorNodeStreamingPreviewActive();
	stats["full_authoring_residency"] = usesFullAuthoringResidency();
	stats["active_regions"] = m_nActiveRegionCount;
	stats["active_nodes"] = m_nActiveNodeCount;
	stats["movable_nodes"] = ( int ) ( m_movableNodes.size() );
	stats["loads_last_tick"] = m_nLoadsLastTick;
	stats["unloads_last_tick"] = m_nUnloadsLastTick;
	stats["scene_load_failures_last_tick"] = m_nSceneLoadFailuresLastTick;
	stats["nodes_activated_last_tick"] = m_nNodesActivatedLastTick;
	stats["nodes_deactivated_last_tick"] = m_nNodesDeactivatedLastTick;
	stats["scene_requests_last_tick"] = m_nSceneRequestsLastTick;
	stats["scene_pending_polls_last_tick"] = m_nScenePendingPollsLastTick;
	stats["load_region_budget"] = m_nLoadRegionBudget;
	stats["effective_load_region_budget"] = getEffectiveLoadRegionBudget();
	stats["load_region_budget_used_last_tick"] = m_nLoadRegionBudgetUsedLastTick;
	stats["load_node_budget"] = m_nLoadNodeBudget;
	stats["load_node_budget_used_last_tick"] = m_nLoadNodeBudgetUsedLastTick;
	stats["unload_region_budget"] = m_nUnloadRegionBudget;
	stats["effective_unload_region_budget"] = getEffectiveUnloadRegionBudget();
	stats["unload_region_budget_used_last_tick"] = m_nUnloadRegionBudgetUsedLastTick;
	stats["deferred_last_tick"] = m_bDeferredLastTick;
	stats["center"] = m_center;
	stats["fixed_slots_scanned_last_tick"] = m_iFixedSlotsScannedLastTick;
	stats["active_regions_examined_last_tick"] = m_nActiveRegionsExaminedLastTick;
	stats["nodes_tested_for_migration_last_tick"] = m_nNodesTestedForMigrationLastTick;
	stats["global_transforms_read_last_tick"] = m_nGlobalTransformsReadLastTick;
	stats["migrations_last_tick"] = m_nMigrationsLastTick;
	stats["loading_regions"] = m_nLoadingRegionCount;
	stats["pending_activation"] = m_bHasPendingActivation;
	stats["migration_cpu_usec_last_tick"] = ( int64_t ) ( m_nMigrationUsecLastTick );
	int virtual_nodes = 0;
	int pending_spawns = 0;
	int pending_regions = 0;
	int pending_unloads = 0;
	if ( m_pTerrain && m_pTerrain->getData() ) {
		T3DData *const pData = m_pTerrain->getData();
		const Vector2i *pLocations = pData->getRegionSlotsInternal();
		const int location_count = pData->getRegionSlotCountInternal();
		for ( int slot = 0; slot < location_count; ++slot ) {
			if ( pLocations[slot] == V2I_MAX ) {
				continue;
			}
			const T3DRegion *pRegion = pData->getRegionPtr( pLocations[slot] );
			if ( pRegion ) {
				virtual_nodes += pRegion->getStreamedNodeCount();
			}
		}
		for ( const int index : m_desiredRegionIndices ) {
			if ( !m_szRegions[index].bActive ) {
				++pending_regions;
			}
		}
		for ( const int index : m_activeRegionIndices ) {
			if ( !m_desiredRegions.test( ( size_t ) ( index ) ) ) {
				++pending_unloads;
			}
			if ( m_szRegions[index].bLoading ) {
				const T3DRegion *pRegion = pData->getRegionPtr( locationFromIndex( index ) );
				const int spawn_count = pRegion ? pRegion->getStreamedNodeCount() : 0;
				pending_spawns += MAX( 0, spawn_count - m_szRegions[index].nextSpawn );
			}
		}
	}
	stats["virtual_nodes"] = virtual_nodes;
	stats["desired_regions"] = ( int ) ( m_desiredRegionIndices.size() );
	stats["pending_spawns"] = pending_spawns;
	stats["pending_regions"] = pending_regions;
	stats["pending_unloads"] = pending_unloads;
	stats["load_region_budget_saturated_last_tick"] = m_bLoadRegionBudgetLimitedLastTick;
	stats["load_node_budget_saturated_last_tick"] = m_nLoadNodeBudget > 0 &&
		m_nLoadNodeBudgetUsedLastTick >= m_nLoadNodeBudget && pending_spawns > 0;
	stats["unload_region_budget_saturated_last_tick"] = m_bUnloadRegionBudgetLimitedLastTick;
	return stats;
}

void T3DNodeStreamer::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( NODE_FLAG_NONE );
	BIND_ENUM_CONSTANT( NODE_FLAG_DISABLED );
	BIND_ENUM_CONSTANT( NODE_FLAG_STATIC );
	BIND_ENUM_CONSTANT( NODE_FLAG_HLOD_VISUAL );
	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &T3DNodeStreamer::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &T3DNodeStreamer::isEnabled );
	ClassDB::bind_method( D_METHOD( "set_radius_regions", "radius" ), &T3DNodeStreamer::setRadiusRegions );
	ClassDB::bind_method( D_METHOD( "get_radius_regions" ), &T3DNodeStreamer::getRadiusRegions );
	ClassDB::bind_method( D_METHOD( "set_load_region_budget", "budget" ), &T3DNodeStreamer::setLoadRegionBudget );
	ClassDB::bind_method( D_METHOD( "get_load_region_budget" ), &T3DNodeStreamer::getLoadRegionBudget );
	ClassDB::bind_method( D_METHOD( "set_load_node_budget", "budget" ), &T3DNodeStreamer::setLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "get_load_node_budget" ), &T3DNodeStreamer::getLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "set_unload_region_budget", "budget" ),
		&T3DNodeStreamer::setUnloadRegionBudget );
	ClassDB::bind_method( D_METHOD( "get_unload_region_budget" ), &T3DNodeStreamer::getUnloadRegionBudget );
	ClassDB::bind_method( D_METHOD( "add_streamed_scene", "scene", "global_transform", "state", "flags", "hlod_min_level" ),
		&T3DNodeStreamer::addStreamedScene, DEFVAL( Dictionary() ), DEFVAL( NODE_FLAG_NONE ), DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "add_streamed_scene_path", "scene_path", "global_transform", "state", "flags", "hlod_min_level" ),
		&T3DNodeStreamer::addStreamedScenePath, DEFVAL( Dictionary() ), DEFVAL( NODE_FLAG_NONE ), DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "remove_streamed_scene", "spawn_id" ), &T3DNodeStreamer::removeStreamedScene );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &T3DNodeStreamer::getStats );
}
