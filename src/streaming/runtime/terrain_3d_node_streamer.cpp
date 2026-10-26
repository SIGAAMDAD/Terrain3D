// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypeSupport.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"

#include "core/utility/logger.h"
#include "core/config/terrain_3d_compiler.h"
#include "data/model/terrain_3d_data.h"
#include "data/model/terrain_3d_region.h"
#include "lighting/policy/terrain_3d_lighting_policy.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static Vector2i t3dNodeStreamerLocationFromIndex( const int iIndex ) {
	const int nMapSize = T3D_REGION_MAP_SIZE;
	return Vector2i( iIndex % nMapSize - nMapSize / 2, iIndex / nMapSize - nMapSize / 2 );
}

typedef struct t3dNodeDesiredRegionLess_s {

	Vector2i m_center;

	bool operator()( const uint16_t nA, const uint16_t nB ) const {
		const Vector2i locationA = t3dNodeStreamerLocationFromIndex( nA );
		const Vector2i locationB = t3dNodeStreamerLocationFromIndex( nB );
		const int nDistanceA = MAX( Math::abs( locationA.x - m_center.x ), Math::abs( locationA.y - m_center.y ) );
		const int nDistanceB = MAX( Math::abs( locationB.x - m_center.x ), Math::abs( locationB.y - m_center.y ) );
		return nDistanceA != nDistanceB ? nDistanceA < nDistanceB : nA < nB;
	}
} t3dNodeDesiredRegionLess_t;


static void t3dNodeStreamerApplyStreamedLightingMetadata( Node *pNode,
        const t3dStreamedNodeRuntimeDescriptor_t &rDescriptor,
        const bool bShadowProxy = false,
        const bool bDisableVisibleShadow = false ) {
	if ( !pNode ) {
		return;
	}
	if ( VisualInstance3D *visual = Object::cast_to<VisualInstance3D>( pNode ) ) {
		uint32_t layers = visual->get_layer_mask();
		layers &= ~T3D_LIGHTING_MANAGED_LAYER_MASK;
		layers |= t3dLightingLayerForClass( rDescriptor.m_nLightingClass );
		if ( !bShadowProxy && rDescriptor.m_bReceiveDynamicDirectional ) {
			layers |= T3D_DIRECTIONAL_RECEIVER_LAYER;
		}
		visual->set_layer_mask( layers );
	}
	if ( GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>( pNode ) ) {
		if ( bShadowProxy ) {
			geometry->set_gi_mode( GeometryInstance3D::GI_MODE_DISABLED );
			geometry->set_cast_shadows_setting( GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY );
		} else {
			switch ( rDescriptor.m_nBakedLightingMode ) {
				case BAKED_LIGHTING_LIGHTMAP:
					geometry->set_gi_mode( GeometryInstance3D::GI_MODE_STATIC );
					break;
				case BAKED_LIGHTING_PROBE:
					geometry->set_gi_mode( GeometryInstance3D::GI_MODE_DYNAMIC );
					break;
				case BAKED_LIGHTING_DISABLED:
				case BAKED_LIGHTING_MATERIAL_ONLY:
				default:
					geometry->set_gi_mode( GeometryInstance3D::GI_MODE_DISABLED );
					break;
			}
			if ( !rDescriptor.m_bDynamicShadowEligible || bDisableVisibleShadow ) {
				geometry->set_cast_shadows_setting( GeometryInstance3D::SHADOW_CASTING_SETTING_OFF );
			}
		}
	}
	for ( int child_index = 0; child_index < pNode->get_child_count(); ++child_index ) {
		t3dNodeStreamerApplyStreamedLightingMetadata( pNode->get_child( child_index ), rDescriptor, bShadowProxy, bDisableVisibleShadow );
	}
}

static Node *t3dNodeStreamerCreateShadowProxy( const Ref<PackedScene> &rScene ) {
	return rScene.is_valid() ? rScene->instantiate() : NULL;
}

Terrain3DNodeStreamer::~Terrain3DNodeStreamer( void ) {
	destroy( false );
}

Vector2i Terrain3DNodeStreamer::locationFromIndex( const int iIndex ) {
	return t3dNodeStreamerLocationFromIndex( iIndex );
}

int Terrain3DNodeStreamer::indexFromLocation( const Vector2i &rLocation ) {
	return Terrain3DData::getRegionMapIndex( rLocation );
}

Node *Terrain3DNodeStreamer::getActiveNode( const t3dActiveNode_t &rActive ) const {
	Object *pObject = ObjectDB::get_instance( rActive.m_hObjectId );
	return pObject ? Object::cast_to<Node>( pObject ) : NULL;
}

bool Terrain3DNodeStreamer::runtimeHasSpawn( const t3dRegionRuntime_t &rRuntime, const int64_t nSpawnId ) const {
	const t3dActiveNode_t *const pNodes = regionNodes( rRuntime );
	for ( uint16_t index = 0u; index < rRuntime.m_nNodeCount; ++index ) {
		if ( pNodes[index].m_nSpawnId == nSpawnId ) {
			return true;
		}
	}
	return false;
}

int Terrain3DNodeStreamer::findActiveNodeIndex( const t3dRegionRuntime_t &rRuntime, const int64_t nSpawnId ) const {
	const t3dActiveNode_t *const pNodes = regionNodes( rRuntime );
	for ( uint16_t index = 0u; index < rRuntime.m_nNodeCount; ++index ) {
		if ( pNodes[index].m_nSpawnId == nSpawnId ) {
			return ( int )( index );
		}
	}
	return -1;
}

void Terrain3DNodeStreamer::trackActiveRegion( const int iIndex ) {
	if ( iIndex < 0 || iIndex >= T3D_REGION_SLOTS ) {
		return;
	}
	t3dRegionRuntime_t &runtime = m_szRegions[iIndex];
	if ( runtime.m_iActiveListPosition >= 0 || m_activeRegionIndices.full() ) {
		return;
	}
	runtime.m_iActiveListPosition = ( int )( m_activeRegionIndices.size() );
	m_activeRegionIndices.push_back( ( uint16_t )iIndex );
}

void Terrain3DNodeStreamer::untrackActiveRegion( const int iIndex ) {
	if ( iIndex < 0 || iIndex >= T3D_REGION_SLOTS ) {
		return;
	}
	t3dRegionRuntime_t &runtime = m_szRegions[iIndex];
	const int position = runtime.m_iActiveListPosition;
	if ( position < 0 || position >= ( int )( m_activeRegionIndices.size() ) ) {
		runtime.m_iActiveListPosition = -1;
		return;
	}
	const int moved_index = m_activeRegionIndices.back();
	m_activeRegionIndices.pop_back();
	if ( ( size_t )( position ) < m_activeRegionIndices.size() ) {
		m_activeRegionIndices[( size_t )( position )] = ( uint16_t )moved_index;
	}
	runtime.m_iActiveListPosition = -1;
	if ( moved_index != iIndex ) {
		m_szRegions[moved_index].m_iActiveListPosition = position;
	}
}

void Terrain3DNodeStreamer::setRegionLoading( t3dRegionRuntime_t &rRuntime, const bool bLoading ) {
	if ( rRuntime.m_bLoading == bLoading ) {
		return;
	}
	rRuntime.m_bLoading = bLoading;
	m_nLoadingRegionCount += bLoading ? 1 : -1;
	m_nLoadingRegionCount = MAX( m_nLoadingRegionCount, 0 );
}

void Terrain3DNodeStreamer::trackMovableNode( const int iRegionIndex, const t3dActiveNode_t &rActive ) {
	if ( ( rActive.m_nFlags & NODE_FLAG_STATIC ) != 0 ) {
		return;
	}
	if ( m_nMovableNodeCount >= m_nMovableNodeCapacity ) {
		++m_nCapacityMissCount;
		requestActivationStorageRebuild();
		LOG( ERROR, "NodeStreamer owner capacity exceeded for movable metadata; cold rebuild queued" );
		return;
	}
	t3dMovableNodeRef_t &ref = m_pMovableNodes[m_nMovableNodeCount++];
	ref.m_iRegionIndex = iRegionIndex;
	ref.m_nSpawnId = rActive.m_nSpawnId;
	ref.m_hObjectId = rActive.m_hObjectId;
}

void Terrain3DNodeStreamer::removeMovableNodeAt( const int iIndex ) {
	if ( iIndex < 0 || iIndex >= ( int )( m_nMovableNodeCount ) ) {
		return;
	}
	--m_nMovableNodeCount;
	if ( ( size_t )( iIndex ) != m_nMovableNodeCount ) {
		m_pMovableNodes[( size_t )( iIndex )] = m_pMovableNodes[m_nMovableNodeCount];
	}
}

void Terrain3DNodeStreamer::untrackMovableNode( const int64_t nSpawnId ) {
	for ( int index = ( int )( m_nMovableNodeCount )-1; index >= 0; --index ) {
		if ( m_pMovableNodes[( size_t )( index )].m_nSpawnId == nSpawnId ) {
			removeMovableNodeAt( index );
			return;
		}
	}
}

void Terrain3DNodeStreamer::eraseActiveNode( t3dRegionRuntime_t &rRuntime, const int iNodeIndex ) {
	if ( iNodeIndex < 0 || iNodeIndex >= ( int )( rRuntime.m_nNodeCount ) ) {
		return;
	}
	t3dActiveNode_t *const pNodes = regionNodes( rRuntime );
	--rRuntime.m_nNodeCount;
	if ( ( uint32_t )( iNodeIndex ) != rRuntime.m_nNodeCount ) {
		pNodes[( size_t )( iNodeIndex )] = pNodes[rRuntime.m_nNodeCount];
	}
}

void Terrain3DNodeStreamer::requestActivationStorageRebuild( void ) {
	m_bActivationStorageRebuildPending = true;
}

void Terrain3DNodeStreamer::requestCookedNodeDescriptors( const int iRegionIndex ) {
	pendingDescriptorRegionSet( iRegionIndex );
	m_bHasPendingActivation = true;
}

void Terrain3DNodeStreamer::servicePendingDescriptorLoadsCold( void ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	for ( int index = 0; index < T3D_REGION_SLOTS; ++index ) {
		if ( !pendingDescriptorRegionTest( index ) ) {
			continue;
		}
		if ( pData->ensureCookedNodeDescriptors( locationFromIndex( index ) ) ) {
			pendingDescriptorRegionClear( index );
		}
	}
}

void Terrain3DNodeStreamer::serviceColdRebuild( void ) {
	m_bColdRebuildServiceActive = true;
	if ( m_bActivationStorageRebuildPending ) {
		++m_nColdRebuildCount;
		if ( rebuildActivationStorageCold() ) {
			m_bActivationStorageRebuildPending = false;
		} else {
			++m_nColdRebuildFailures;
		}
	}
	servicePendingDescriptorLoadsCold();
	m_bColdRebuildServiceActive = false;
}

bool Terrain3DNodeStreamer::rebuildActivationStorageCold( void ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return false;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const size_t migrationSpare = ( size_t )( CLAMP( MAX( m_nLoadNodeBudget, 1 ), 1, 64 ) );
	size_t descriptorCapacity = 0u;
	size_t activeCapacity = 0u;
	size_t regionCapacities[T3D_REGION_SLOTS];
	size_t regionOffsets[T3D_REGION_SLOTS];

	for ( int index = 0; index < T3D_REGION_SLOTS; ++index ) {
		const Vector2i location = locationFromIndex( index );
		const size_t descriptorCount = ( size_t )( pData->getStreamedNodeCapacityHint( location ) );
		const size_t liveCount = ( size_t )( m_szRegions[index].m_nNodeCount );
		if ( descriptorCount > SIZE_MAX - migrationSpare ) {
			return false;
		}
		size_t regionCapacity = descriptorCount + migrationSpare;
		if ( liveCount > regionCapacity ) {
			regionCapacity = liveCount;
		}
		Terrain3DRegion *const pRegion = pData->getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() && !pRegion->reserveStreamedNodeRuntimeCapacityInternal( regionCapacity ) ) {
			LOG( ERROR, "Failed reserving streamed-node descriptor capacity for region ", location );
			return false;
		}
		regionOffsets[index] = activeCapacity;
		regionCapacities[index] = regionCapacity;
		descriptorCapacity += descriptorCount;
		activeCapacity += regionCapacity;
	}
	if ( m_nMovableNodeCount > activeCapacity ) {
		LOG( ERROR, "Streamed-node cold rebuild capacity is smaller than live movable-node state" );
		return false;
	}

	if ( activeCapacity > 0u && !m_pRuntimeAllocatorZone ) {
		LOG( ERROR, "Streamed-node runtime storage has no runtime allocator zone" );
		return false;
	}
	t3dActiveNode_t *pNewActive = activeCapacity > 0u ? ( t3dActiveNode_t * )( m_pRuntimeAllocatorZone->allocate(
	                                                            activeCapacity * sizeof( t3dActiveNode_t ), alignof( t3dActiveNode_t ) ) )
	                                                  : NULL;
	if ( activeCapacity > 0u && !pNewActive ) {
		LOG( ERROR, "Failed allocating streamed-node active metadata image" );
		return false;
	}
	t3dMovableNodeRef_t *pNewMovable = activeCapacity > 0u
	        ? ( t3dMovableNodeRef_t * )( m_pRuntimeAllocatorZone->allocate(
	                  activeCapacity * sizeof( t3dMovableNodeRef_t ), alignof( t3dMovableNodeRef_t ) ) )
	        : NULL;
	if ( activeCapacity > 0u && !pNewMovable ) {
		if ( pNewActive ) {
			m_pRuntimeAllocatorZone->deallocate( pNewActive );
		}
		LOG( ERROR, "Failed allocating streamed-node movable metadata image" );
		return false;
	}

	for ( int index = 0; index < T3D_REGION_SLOTS; ++index ) {
		const t3dRegionRuntime_t &runtime = m_szRegions[index];
		const size_t copyCount = ( size_t )( runtime.m_nNodeCount );
		if ( copyCount > 0u && m_pActiveNodeStorage ) {
			memcpy( pNewActive + regionOffsets[index], regionNodes( runtime ), copyCount * sizeof( t3dActiveNode_t ) );
		}
	}
	if ( m_nMovableNodeCount > 0u && m_pMovableNodes ) {
		memcpy( pNewMovable, m_pMovableNodes, m_nMovableNodeCount * sizeof( t3dMovableNodeRef_t ) );
	}

	const int requiredRoots = ( int )( MIN( MAX( descriptorCapacity, m_activeRegionIndices.size() ), ( size_t )( T3D_REGION_SLOTS ) ) );
	eastl::fixed_vector<Node3D *, T3D_REGION_SLOTS, false> new_roots;
	const int newRootCount = MAX( requiredRoots - m_nRegionRootPoolCount, 0 );
	for ( int index = 0; index < newRootCount; ++index ) {
		Node3D *const pNewRoot = memnew( Node3D );
		if ( !pNewRoot ) {
			for ( size_t iRoot = 0u; iRoot < ( size_t )( ( new_roots ).size() ); iRoot++ ) {
				Node3D *const pRoot = ( new_roots )[iRoot];
				memdelete( pRoot );
			}
			if ( pNewActive ) {
				m_pRuntimeAllocatorZone->deallocate( pNewActive );
			}
			if ( pNewMovable ) {
				m_pRuntimeAllocatorZone->deallocate( pNewMovable );
			}
			LOG( ERROR, "Failed allocating streamed-node region-root pool" );
			return false;
		}
		pNewRoot->set_name( "Terrain3DStreamRegionRoot" );
		new_roots.push_back( pNewRoot );
	}

	t3dActiveNode_t *const pOldActive = m_pActiveNodeStorage;
	t3dMovableNodeRef_t *const pOldMovable = m_pMovableNodes;
	m_pActiveNodeStorage = pNewActive;
	m_nActiveNodeStorageCapacity = activeCapacity;
	m_pMovableNodes = pNewMovable;
	m_nMovableNodeCapacity = activeCapacity;
	for ( int index = 0; index < T3D_REGION_SLOTS; ++index ) {
		t3dRegionRuntime_t &runtime = m_szRegions[index];
		runtime.m_iNodeOffset = ( uint32_t )( regionOffsets[index] );
		runtime.m_nNodeCapacity = ( uint32_t )( regionCapacities[index] );
		const Vector2i location = locationFromIndex( index );
		if ( m_szRegionRootNames[index].is_empty() ) {
			m_szRegionRootNames[index] =
			        StringName( String( "Region_" ) + String::num_int64( location.x ) + String( "_" ) + String::num_int64( location.y ) );
		}
	}
	for ( size_t index = 0u; index < new_roots.size(); ++index ) {
		m_szRegionRootPool[m_nRegionRootPoolCount + ( int )( index )] = new_roots[index];
	}
	m_nRegionRootPoolCount = requiredRoots > m_nRegionRootPoolCount ? requiredRoots : m_nRegionRootPoolCount;
	if ( pOldActive ) {
		m_pRuntimeAllocatorZone->deallocate( pOldActive );
	}
	if ( pOldMovable ) {
		m_pRuntimeAllocatorZone->deallocate( pOldMovable );
	}

	eastl::array<uint64_t, T3D_REGION_SLOTS / 64> leased_roots{};
	for ( int index = 0; index < T3D_REGION_SLOTS; ++index ) {
		const int poolIndex = m_szRegions[index].m_iRootPoolIndex;
		if ( poolIndex >= 0 && poolIndex < m_nRegionRootPoolCount ) {
			leased_roots[( size_t )( poolIndex ) >> 6u] |= 1ull << ( ( size_t )( poolIndex ) & 63u );
		}
	}
	m_freeRootPoolIndices.clear();
	for ( int index = 0; index < m_nRegionRootPoolCount; ++index ) {
		if ( ( leased_roots[( size_t )( index ) >> 6u] & ( 1ull << ( ( size_t )( index ) & 63u ) ) ) == 0u ) {
			m_freeRootPoolIndices.push_back( ( uint16_t )( index ) );
		}
	}
	return true;
}

Node3D *Terrain3DNodeStreamer::acquireRegionRoot( const int iIndex, Terrain3DRegion *const pRegion ) {
	if ( !m_pHost || !pRegion || iIndex < 0 || iIndex >= T3D_REGION_SLOTS ) {
		return NULL;
	}
	t3dRegionRuntime_t &runtime = m_szRegions[iIndex];
	if ( runtime.m_pRoot ) {
		return runtime.m_pRoot;
	}
	if ( m_freeRootPoolIndices.empty() ) {
		LOG( ERROR, "Streamed-node region-root capacity exceeded; cold activation storage rebuild required" );
		return NULL;
	}
	const int pool_index = m_freeRootPoolIndices.back();
	m_freeRootPoolIndices.pop_back();
	Node3D *const pRoot = m_szRegionRootPool[pool_index];
	if ( !pRoot || pRoot->get_child_count() != 0 ) {
		LOG( ERROR, "Invalid streamed-node region-root pool slot ", pool_index );
		return NULL;
	}
	runtime.m_pRoot = pRoot;
	runtime.m_iRootPoolIndex = pool_index;
	pRoot->set_name( m_szRegionRootNames[iIndex] );
	const Vector2i location = pRegion->getLocation();
	const real_t fWorldSize = ( real_t )( T3D_REGION_WORLD_SIZE_METERS );
	pRoot->set_position( Vector3( ( real_t )( location.x ) * fWorldSize, 0.0f, ( real_t )( location.y ) * fWorldSize ) );
	m_pHost->add_child( pRoot, false, Node::INTERNAL_MODE_BACK );
	return pRoot;
}

void Terrain3DNodeStreamer::releaseRegionRoot( t3dRegionRuntime_t &rRuntime ) {
	if ( !rRuntime.m_pRoot ) {
		rRuntime.m_iRootPoolIndex = -1;
		return;
	}
	if ( rRuntime.m_pRoot->is_inside_tree() && rRuntime.m_pRoot->get_parent() ) {
		rRuntime.m_pRoot->get_parent()->remove_child( rRuntime.m_pRoot );
	}
	const int pool_index = rRuntime.m_iRootPoolIndex;
	if ( pool_index < 0 || pool_index >= m_nRegionRootPoolCount || m_freeRootPoolIndices.size() >= ( size_t )( m_nRegionRootPoolCount ) ) {
		LOG( ERROR, "Invalid streamed-node region-root release slot ", pool_index );
	} else {
		m_freeRootPoolIndices.push_back( ( uint16_t )( pool_index ) );
	}
	rRuntime.m_pRoot = NULL;
	rRuntime.m_iRootPoolIndex = -1;
}

Node3D *Terrain3DNodeStreamer::ensureRegionRoot( const int iIndex, Terrain3DRegion *const pRegion ) {
	if ( !m_pTerrain || !pRegion || iIndex < 0 || iIndex >= T3D_REGION_SLOTS ) {
		return NULL;
	}
	t3dRegionRuntime_t &runtime = m_szRegions[iIndex];
	if ( runtime.m_pRoot ) {
		return runtime.m_pRoot;
	}
	if ( !acquireRegionRoot( iIndex, pRegion ) ) {
		return NULL;
	}
	trackActiveRegion( iIndex );
	setRegionLoading( runtime, true );
	runtime.m_bActive = false;
	runtime.m_nNextSpawn = 0;
	runtime.m_pendingScene.unref();
	runtime.m_pendingShadowProxy.unref();
	runtime.m_nPendingSpawnId = 0;
	runtime.m_bRequestsStarted = false;
	return runtime.m_pRoot;
}

bool Terrain3DNodeStreamer::migrateActiveNodes( void ) {
	m_nMigrationUsecLastTick = 0u;
	if ( !m_pTerrain || !m_pTerrain->dataObject() || m_nMovableNodeCount == 0u ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	bool bChanged = false;
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const real_t fRegionWorldSize = ( real_t )( T3D_REGION_WORLD_SIZE_METERS );
	if ( fRegionWorldSize <= 0.0f ) {
		return false;
	}
	const Transform3D terrain_inverse = m_pTerrain->sceneNode().get_global_transform().affine_inverse();
	for ( int movable_index = ( int )( m_nMovableNodeCount )-1; movable_index >= 0; --movable_index ) {
		++m_nNodesTestedForMigrationLastTick;
		t3dMovableNodeRef_t &movable = m_pMovableNodes[( size_t )( movable_index )];
		const int source_index = movable.m_iRegionIndex;
		if ( source_index < 0 || source_index >= T3D_REGION_SLOTS ) {
			removeMovableNodeAt( movable_index );
			continue;
		}
		t3dRegionRuntime_t &source_runtime = m_szRegions[source_index];
		const int active_index = findActiveNodeIndex( source_runtime, movable.m_nSpawnId );
		Terrain3DRegion *const pSourceRegion = pData->getRegionPtr( locationFromIndex( source_index ) );
		if ( active_index < 0 || !pSourceRegion ) {
			removeMovableNodeAt( movable_index );
			continue;
		}
		const t3dActiveNode_t active = regionNodes( source_runtime )[( size_t )( active_index )];
		Node3D *const pNode = Object::cast_to<Node3D>( getActiveNode( active ) );
		if ( !pNode ) {
			pSourceRegion->removeStreamedNode( active.m_nSpawnId );
			eraseActiveNode( source_runtime, active_index );
			removeMovableNodeAt( movable_index );
			m_nActiveNodeCount = MAX( 0, m_nActiveNodeCount - 1 );
			bChanged = true;
			if ( source_runtime.m_pRoot && source_runtime.m_nNodeCount == 0u && pSourceRegion->getStreamedNodeCount() == 0 ) {
				deactivateRegion( source_index, false );
			}
			continue;
		}
		Transform3D terrain_local;
		Vector2i destinationLocation;
		if ( pNode->get_parent() == source_runtime.m_pRoot ) {
			// The normal streamed-node topology already gives us terrain-local motion:
			// the region root is positioned at the region origin. Avoid a global
			// transform fetch and terrain inverse multiply for every movable node.
			terrain_local = pNode->get_transform();
			const Vector2i source_location = pSourceRegion->getLocation();
			destinationLocation = source_location
			        + Vector2i( ( int )( Math::floor( terrain_local.origin.x / fRegionWorldSize ) ),
			                ( int )( Math::floor( terrain_local.origin.z / fRegionWorldSize ) ) );
			terrain_local.origin.x += ( real_t )( source_location.x ) * fRegionWorldSize;
			terrain_local.origin.z += ( real_t )( source_location.y ) * fRegionWorldSize;
		} else {
			++m_nGlobalTransformsReadLastTick;
			terrain_local = terrain_inverse * pNode->get_global_transform();
			destinationLocation = Vector2i( ( int )( Math::floor( terrain_local.origin.x / fRegionWorldSize ) ),
			        ( int )( Math::floor( terrain_local.origin.z / fRegionWorldSize ) ) );
		}
		const int destination_index = indexFromLocation( destinationLocation );
		if ( destination_index < 0 || destination_index == source_index ) {
			continue;
		}

		const t3dStreamedNodeRuntimeDescriptor_t *pSourceDescriptor = pSourceRegion->findStreamedNodeRuntimeInternal( active.m_nSpawnId );
		if ( !pSourceDescriptor ) {
			continue;
		}
		const t3dStreamedNodeRuntimeDescriptor_t descriptor = *pSourceDescriptor;
		const String &scene_path = descriptor.m_scenePath;
		const Ref<PackedScene> &scene = descriptor.m_scene;
		if ( scene_path.is_empty() && scene.is_null() ) {
			continue;
		}
		Terrain3DRegion *pDestinationRegion = pData->getRegionPtr( destinationLocation );
		if ( !pDestinationRegion ) {
			const Ref<Terrain3DRegion> pCreated = pData->addRegionBlank( destinationLocation, true );
			pDestinationRegion = pCreated.ptr();
		}
		if ( !pDestinationRegion ) {
			continue;
		}
		t3dRegionRuntime_t &destination_runtime = m_szRegions[destination_index];
		if ( destination_runtime.m_nNodeCount >= destination_runtime.m_nNodeCapacity ) {
			LOG( ERROR,
			        "Streamed-node destination metadata capacity exceeded at ",
			        destinationLocation,
			        "; migration deferred until a cold activation storage rebuild" );
			requestActivationStorageRebuild();
			continue;
		}
		if ( pDestinationRegion->getStreamedNodesRuntimeInternal().size() + 1u
		        > pDestinationRegion->getStreamedNodesRuntimeInternal().capacity() ) {
			requestActivationStorageRebuild();
			continue;
		}
		Transform3D destination_transform = terrain_local;
		destination_transform.origin.x -= ( real_t )( destinationLocation.x ) * fRegionWorldSize;
		destination_transform.origin.z -= ( real_t )( destinationLocation.y ) * fRegionWorldSize;
		const Dictionary &state = descriptor.m_state;
		const int flags = descriptor.m_nFlags;
		const int boundary_policy = descriptor.m_nBoundaryPolicy;
		const Vector2i source_coverage_min = descriptor.m_coverageMin;
		const Vector2i source_coverage_max = descriptor.m_coverageMax;
		const Vector2i owner_delta = pSourceRegion->getLocation() - destinationLocation;
		const Vector2i destination_coverage_min = source_coverage_min + owner_delta;
		const Vector2i destination_coverage_max = source_coverage_max + owner_delta;
		const int64_t transferred_id = !scene_path.is_empty() ? pDestinationRegion->addStreamedNodePath( scene_path,
		                                                                destination_transform,
		                                                                state,
		                                                                flags,
		                                                                active.m_nSpawnId,
		                                                                boundary_policy,
		                                                                destination_coverage_min,
		                                                                destination_coverage_max )
		                                                      : pDestinationRegion->addStreamedNode( scene,
		                                                                destination_transform,
		                                                                state,
		                                                                flags,
		                                                                active.m_nSpawnId,
		                                                                boundary_policy,
		                                                                destination_coverage_min,
		                                                                destination_coverage_max );
		if ( transferred_id != active.m_nSpawnId ) {
			if ( transferred_id > 0 ) {
				pDestinationRegion->removeStreamedNode( transferred_id );
			}
			continue;
		}
		if ( !pSourceRegion->removeStreamedNode( active.m_nSpawnId ) ) {
			pDestinationRegion->removeStreamedNode( active.m_nSpawnId );
			continue;
		}
		Node3D *const pDestinationRoot = ensureRegionRoot( destination_index, pDestinationRegion );
		if ( !pDestinationRoot ) {
			if ( !scene_path.is_empty() ) {
				pSourceRegion->addStreamedNodePath( scene_path,
				        descriptor.m_transform,
				        state,
				        flags,
				        active.m_nSpawnId,
				        boundary_policy,
				        source_coverage_min,
				        source_coverage_max );
			} else {
				pSourceRegion->addStreamedNode( scene,
				        descriptor.m_transform,
				        state,
				        flags,
				        active.m_nSpawnId,
				        boundary_policy,
				        source_coverage_min,
				        source_coverage_max );
			}
			pDestinationRegion->removeStreamedNode( active.m_nSpawnId );
			continue;
		}
		pNode->reparent( pDestinationRoot, true );
		if ( !runtimeHasSpawn( destination_runtime, active.m_nSpawnId ) ) {
			regionNodes( destination_runtime )[destination_runtime.m_nNodeCount++] = active;
		}
		eraseActiveNode( source_runtime, active_index );
		movable.m_iRegionIndex = destination_index;
		++m_nMigrationsLastTick;
		bChanged = true;
		if ( source_runtime.m_pRoot && source_runtime.m_nNodeCount == 0u && pSourceRegion->getStreamedNodeCount() == 0 ) {
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

void Terrain3DNodeStreamer::ensureHost( void ) {
	if ( m_pHost || !m_pTerrain || !m_pTerrain->sceneNode().is_inside_tree() ) {
		return;
	}
	m_pHost = memnew( Node3D );
	m_pHost->set_name( "Terrain3DNodeStreamHost" );
	m_pTerrain->sceneNode().add_child( m_pHost, false, Node::INTERNAL_MODE_BACK );
}

void Terrain3DNodeStreamer::destroyHost( void ) {
	if ( !m_pHost ) {
		return;
	}
	if ( m_pHost->is_inside_tree() ) {
		m_pHost->get_parent()->remove_child( m_pHost );
	}
	memdelete( m_pHost );
	m_pHost = NULL;
}

bool Terrain3DNodeStreamer::spawnIdExists( const uint64_t nId ) const {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || nId == 0u || nId > ( uint64_t )( INT64_MAX ) ) {
		return false;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const t3dRegionCoord_t *pLocations = pData->getRegionSlotsInternal();
	const int location_count = pData->getRegionSlotCountInternal();
	for ( int slot = 0; slot < location_count; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( pLocations[slot] );
		if ( location == s_v2iMax ) {
			continue;
		}
		const Terrain3DRegion *pRegion = pData->getRegionPtr( location );
		if ( !pRegion ) {
			continue;
		}
		for ( size_t iNode = 0u; iNode < ( size_t )( ( pRegion->getStreamedNodesRuntimeInternal() ).size() ); iNode++ ) {
			const t3dStreamedNodeRuntimeDescriptor_t &spawn = ( pRegion->getStreamedNodesRuntimeInternal() )[iNode];
			if ( spawn.m_nId > 0 && ( uint64_t )( spawn.m_nId ) == nId ) {
				return true;
			}
		}
	}
	return false;
}

int64_t Terrain3DNodeStreamer::allocateSpawnId( void ) {
	int descriptor_count = 0;
	if ( m_pTerrain && m_pTerrain->dataObject() ) {
		Terrain3DData *const pData = m_pTerrain->dataObject();
		const t3dRegionCoord_t *pLocations = pData->getRegionSlotsInternal();
		const int location_count = pData->getRegionSlotCountInternal();
		for ( int slot = 0; slot < location_count; ++slot ) {
			if ( t3dRegionCoordValid( pLocations[slot] ) ) {
				Terrain3DRegion *const pRegion = pData->getRegionPtr( t3dUnpackRegionCoord( pLocations[slot] ) );
				if ( pRegion ) {
					descriptor_count += pRegion->getStreamedNodeCount();
				}
			}
		}
	}
	for ( int attempt = 0; attempt <= descriptor_count; ++attempt ) {
		if ( m_nNextSpawnId == 0u || m_nNextSpawnId > ( uint64_t )( INT64_MAX ) ) {
			m_nNextSpawnId = 1u;
		}
		const uint64_t candidate = m_nNextSpawnId++;
		if ( !spawnIdExists( candidate ) ) {
			return ( int64_t )( candidate );
		}
	}
	return 0;
}

void Terrain3DNodeStreamer::refreshSpawnId( void ) {
	m_nNextSpawnId = 1u;
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const t3dRegionCoord_t *pLocations = pData->getRegionSlotsInternal();
	const int location_count = pData->getRegionSlotCountInternal();
	for ( int slot = 0; slot < location_count; ++slot ) {
		if ( !t3dRegionCoordValid( pLocations[slot] ) ) {
			continue;
		}
		const Terrain3DRegion *pRegion = pData->getRegionPtr( t3dUnpackRegionCoord( pLocations[slot] ) );
		if ( !pRegion ) {
			continue;
		}
		for ( size_t iNode = 0u; iNode < ( size_t )( ( pRegion->getStreamedNodesRuntimeInternal() ).size() ); iNode++ ) {
			const t3dStreamedNodeRuntimeDescriptor_t &spawn = ( pRegion->getStreamedNodesRuntimeInternal() )[iNode];
			const int64_t signed_id = spawn.m_nId;
			if ( signed_id > 0 && ( uint64_t )( signed_id ) >= m_nNextSpawnId ) {
				m_nNextSpawnId = ( uint64_t )( signed_id ) < ( uint64_t )( INT64_MAX ) ? ( uint64_t )( signed_id ) + 1u : 1u;
			}
		}
	}
	if ( m_nNextSpawnId == 0u || m_nNextSpawnId > ( uint64_t )( INT64_MAX ) ) {
		m_nNextSpawnId = 1u;
	}
}

bool Terrain3DNodeStreamer::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone ) {
	if ( !pZone || ( m_pRuntimeAllocatorZone && m_pRuntimeAllocatorZone != pZone ) || m_pActiveNodeStorage || m_pMovableNodes ) {
		return m_pRuntimeAllocatorZone == pZone && !m_pActiveNodeStorage && !m_pMovableNodes;
	}
	m_pRuntimeAllocatorZone = pZone;
	return true;
}

void Terrain3DNodeStreamer::initialize( Terrain3DOwner *const pTerrain ) {
	if ( m_pTerrain == pTerrain && m_pHost ) {
		return;
	}
	destroy( false );
	m_pTerrain = pTerrain;
	m_methodActivate = StringName( "terrain3d_stream_activate" );
	m_methodDeactivate = StringName( "terrain3d_stream_deactivate" );
	m_methodLoadState = StringName( "terrain3d_stream_load_state" );
	m_methodSaveState = StringName( "terrain3d_stream_save_state" );
	m_shadowProxyName = StringName( "ShadowProxy" );
	ensureHost();
	requestActivationStorageRebuild();
	refreshSpawnId();
}

void Terrain3DNodeStreamer::captureNodeState( Terrain3DRegion *const pRegion, Node3D *const pRegionRoot, const t3dActiveNode_t &rActive ) {
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
		local_transform = pRegionRoot ? pRegionRoot->get_global_transform().affine_inverse() * pNode3d->get_global_transform()
		                              : pNode3d->get_transform();
	}
	Dictionary state;
	const t3dStreamedNodeRuntimeDescriptor_t *pDescriptor = pRegion->findStreamedNodeRuntimeInternal( rActive.m_nSpawnId );
	if ( pDescriptor ) {
		state = pDescriptor->m_state.duplicate( true );
	}
	if ( pNode->has_method( m_methodSaveState ) ) {
		const Variant result = pNode->call( m_methodSaveState );
		if ( result.get_type() == Variant::DICTIONARY ) {
			state = Dictionary( result ).duplicate( true );
		}
	}
	pRegion->updateStreamedNodeState( rActive.m_nSpawnId, local_transform, state, rActive.m_nFlags );
	pNode = getActiveNode( rActive );
	if ( pNode && pNode->has_method( m_methodDeactivate ) ) {
		pNode->call( m_methodDeactivate );
	}
}

void Terrain3DNodeStreamer::deactivateRegion( const int iIndex, const bool bCaptureState ) {
	pendingDescriptorRegionClear( iIndex );
	if ( iIndex < 0 || iIndex >= T3D_REGION_SLOTS ) {
		return;
	}
	t3dRegionRuntime_t &runtime = m_szRegions[iIndex];
	if ( !runtime.m_pRoot && !runtime.m_bActive && !runtime.m_bLoading ) {
		return;
	}
	Terrain3DRegion *const pRegion =
	        m_pTerrain && m_pTerrain->dataObject() ? m_pTerrain->dataObject()->getRegionPtr( locationFromIndex( iIndex ) ) : NULL;
	const t3dActiveNode_t *const pRegionNodes = regionNodes( runtime );
	for ( uint16_t nodeIndex = 0u; nodeIndex < runtime.m_nNodeCount; ++nodeIndex ) {
		const t3dActiveNode_t &active = pRegionNodes[nodeIndex];
		Node *pNode = getActiveNode( active );
		if ( bCaptureState && pNode ) {
			captureNodeState( pRegion, runtime.m_pRoot, active );
		}
		pNode = getActiveNode( active );
		if ( pNode && runtime.m_pRoot && !pNode->is_queued_for_deletion() && pNode->get_parent() != runtime.m_pRoot ) {
			if ( pNode->get_parent() ) {
				pNode->reparent( runtime.m_pRoot, true );
			} else {
				runtime.m_pRoot->add_child( pNode );
			}
		}
		if ( ( active.m_nFlags & NODE_FLAG_STATIC ) == 0 ) {
			untrackMovableNode( active.m_nSpawnId );
		}
		if ( pNode ) {
			memdelete( pNode );
		}
	}
	const int deactivated_node_count = ( int )( runtime.m_nNodeCount );
	m_nNodesDeactivatedLastTick += deactivated_node_count;
	if ( deactivated_node_count > 0 ) {
		++m_nRuntimeGeneration;
	}
	m_nActiveNodeCount -= deactivated_node_count;
	m_nActiveNodeCount = MAX( m_nActiveNodeCount, 0 );
	runtime.m_nNodeCount = 0u;
	releaseRegionRoot( runtime );
	setRegionLoading( runtime, false );
	runtime.m_bActive = false;
	untrackActiveRegion( iIndex );
	runtime.m_nNextSpawn = 0;
	runtime.m_pendingScene.unref();
	runtime.m_pendingShadowProxy.unref();
	runtime.m_nPendingSpawnId = 0;
	runtime.m_bRequestsStarted = false;
	++m_nUnloadsLastTick;
}

t3dSpawnActivationResult_t Terrain3DNodeStreamer::activateSpawn(
        Terrain3DRegion *const pRegion, t3dRegionRuntime_t &rRuntime, const t3dStreamedNodeRuntimeDescriptor_t &rSpawn ) {
	if ( !pRegion || rSpawn.m_nId <= 0 ) {
		return SPAWN_ACTIVATION_FAILED;
	}
	const int64_t spawn_id = rSpawn.m_nId;
	if ( runtimeHasSpawn( rRuntime, spawn_id ) ) {
		return SPAWN_ACTIVATION_CONSUMED;
	}
	const int flags = rSpawn.m_nFlags;
	if ( ( flags & NODE_FLAG_DISABLED ) != 0 ) {
		return SPAWN_ACTIVATION_CONSUMED;
	}
	if ( isHlodVisualSuppressed( pRegion->getLocation(), rSpawn ) ) {
		return SPAWN_ACTIVATION_CONSUMED;
	}
	if ( rRuntime.m_nNodeCount >= rRuntime.m_nNodeCapacity ) {
		LOG( ERROR, "Streamed-node active metadata capacity exceeded for region ", pRegion->getLocation() );
		return SPAWN_ACTIVATION_FAILED;
	}
	if ( ( flags & NODE_FLAG_STATIC ) == 0 && m_nMovableNodeCount >= m_nMovableNodeCapacity ) {
		LOG( ERROR, "Streamed-node movable metadata capacity exceeded during activation" );
		return SPAWN_ACTIVATION_FAILED;
	}
	if ( rRuntime.m_nPendingSpawnId != rSpawn.m_nId ) {
		rRuntime.m_nPendingSpawnId = rSpawn.m_nId;
		rRuntime.m_pendingScene.unref();
		rRuntime.m_pendingShadowProxy.unref();
	}
	Ref<PackedScene> scene = rSpawn.m_scene.is_valid() ? rSpawn.m_scene : rRuntime.m_pendingScene;
	const String scene_path = rSpawn.m_scenePath;
	if ( scene.is_null() && !scene_path.is_empty() ) {
		if ( !scene_path.is_empty() ) {
			ResourceLoader *const pLoader = ResourceLoader::get_singleton();
			if ( !pLoader ) {
				return SPAWN_ACTIVATION_FAILED;
			}
			const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( scene_path );
			if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
				const Error request_error =
				        pLoader->load_threaded_request( scene_path, "PackedScene", true, ResourceLoader::CACHE_MODE_REUSE );
				if ( request_error != OK ) {
					LOG( ERROR, "Failed requesting streamed scene ", scene_path, ": ", request_error );
					return SPAWN_ACTIVATION_FAILED;
				}
				++m_nSceneRequestsLastTick;
				return SPAWN_ACTIVATION_PENDING;
			}
			if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
				return SPAWN_ACTIVATION_PENDING;
			}
			if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
				LOG( ERROR, "Failed loading streamed scene ", scene_path );
				return SPAWN_ACTIVATION_FAILED;
			}
			const Ref<Resource> pResource = pLoader->load_threaded_get( scene_path );
			scene = pResource;
		}
	}
	if ( scene.is_null() ) {
		if ( !scene_path.is_empty() ) {
			LOG( ERROR, "Streamed scene is not a PackedScene: ", scene_path );
		}
		return SPAWN_ACTIVATION_FAILED;
	}
	rRuntime.m_pendingScene = scene;
	Ref<PackedScene> shadow_proxy = rSpawn.m_shadowProxyScene.is_valid()
	        ? rSpawn.m_shadowProxyScene : rRuntime.m_pendingShadowProxy;
	if ( rSpawn.m_bDynamicShadowEligible && shadow_proxy.is_null() && !rSpawn.m_shadowProxyScenePath.is_empty() ) {
		ResourceLoader *const loader = ResourceLoader::get_singleton();
		if ( !loader ) { return SPAWN_ACTIVATION_FAILED; }
		const String &path = rSpawn.m_shadowProxyScenePath;
		const auto status = loader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
			const Error error = loader->load_threaded_request( path, "PackedScene", true, ResourceLoader::CACHE_MODE_REUSE );
			if ( error == OK || error == ERR_BUSY ) {
				if ( error == OK ) { ++m_nSceneRequestsLastTick; }
				return SPAWN_ACTIVATION_PENDING;
			}
		} else if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
			return SPAWN_ACTIVATION_PENDING;
		} else if ( status == ResourceLoader::THREAD_LOAD_LOADED ) {
			const Ref<Resource> resource = loader->load_threaded_get( path );
			shadow_proxy = resource;
			rRuntime.m_pendingShadowProxy = shadow_proxy;
		}
		if ( shadow_proxy.is_null() ) {
			// Preserve the visible node and its normal shadow if a proxy fails.
			LOG( ERROR, "Failed loading streamed shadow proxy ", path );
		}
	}
	Node *pNode = scene->instantiate();
	if ( !pNode ) {
		LOG( ERROR, "Failed instantiating streamed scene ", scene_path );
		return SPAWN_ACTIVATION_FAILED;
	}
	if ( rSpawn.m_spawnName.is_empty() ) {
		LOG( ERROR, "Streamed-node descriptor is missing its precomputed activation name for spawn ", spawn_id );
		memdelete( pNode );
		return SPAWN_ACTIVATION_FAILED;
	}
	pNode->set_name( rSpawn.m_spawnName );
	Node3D *const pNode3d = Object::cast_to<Node3D>( pNode );
	if ( pNode3d ) {
		pNode3d->set_transform( rSpawn.m_transform );
	}
	const bool bHasShadowProxy = rSpawn.m_bDynamicShadowEligible && shadow_proxy.is_valid();
	t3dNodeStreamerApplyStreamedLightingMetadata( pNode, rSpawn, false, bHasShadowProxy );
	if ( bHasShadowProxy && rSpawn.m_bDynamicShadowEligible ) {
		Node *const proxy = t3dNodeStreamerCreateShadowProxy( shadow_proxy );
		if ( proxy ) {
			proxy->set_name( m_shadowProxyName );
			t3dNodeStreamerApplyStreamedLightingMetadata( proxy, rSpawn, true, false );
			pNode->add_child( proxy );
		}
	}
	rRuntime.m_pRoot->add_child( pNode );
	const ObjectID objectId( pNode->get_instance_id() );
	if ( !rSpawn.m_state.is_empty() && pNode->has_method( m_methodLoadState ) ) {
		const Dictionary state = rSpawn.m_state.duplicate( true );
		pNode->call( m_methodLoadState, state );
		pNode = Object::cast_to<Node>( ObjectDB::get_instance( objectId ) );
		if ( !pNode ) {
			pRegion->removeStreamedNode( spawn_id );
			return SPAWN_ACTIVATION_CONSUMED;
		}
	}
	if ( pNode->has_method( m_methodActivate ) ) {
		pNode->call( m_methodActivate );
		pNode = Object::cast_to<Node>( ObjectDB::get_instance( objectId ) );
		if ( !pNode ) {
			pRegion->removeStreamedNode( spawn_id );
			return SPAWN_ACTIVATION_CONSUMED;
		}
	}
	t3dActiveNode_t active;
	active.m_nSpawnId = spawn_id;
	active.m_hObjectId = objectId;
	active.m_nFlags = flags;
	regionNodes( rRuntime )[rRuntime.m_nNodeCount++] = active;
	Node3D *const pActiveNode3d = Object::cast_to<Node3D>( pNode );
	if ( pActiveNode3d && ( flags & NODE_FLAG_STATIC ) == 0 ) {
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

void Terrain3DNodeStreamer::requestRegionScenes(
        const Vector2i &rOwnerLocation, const eastl::vector<t3dStreamedNodeRuntimeDescriptor_t, Terrain3DEastlAllocator> &rSpawns ) {
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) { return; }
	for ( const auto &spawn : rSpawns ) {
		if ( isHlodVisualSuppressed( rOwnerLocation, spawn ) ) { continue; }
		const String *paths[2] = { &spawn.m_scenePath, &spawn.m_shadowProxyScenePath };
		const bool needed[2] = { spawn.m_scene.is_null(),
			spawn.m_bDynamicShadowEligible && spawn.m_shadowProxyScene.is_null() };
		for ( int dependency = 0; dependency < 2; ++dependency ) {
			const String &path = *paths[dependency];
			if ( !needed[dependency] || path.is_empty()
			        || loader->load_threaded_get_status( path ) != ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) { continue; }
			const Error error = loader->load_threaded_request( path, "PackedScene", true, ResourceLoader::CACHE_MODE_REUSE );
			if ( error == OK ) { ++m_nSceneRequestsLastTick; }
			else if ( error != ERR_BUSY ) { LOG( ERROR, "Failed requesting streamed scene ", path, ": ", error ); }
		}
	}
}

bool Terrain3DNodeStreamer::activateRegion( const int iIndex, int &nNodeBudget ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || iIndex < 0 || iIndex >= T3D_REGION_SLOTS ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pTerrain->dataObject()->getRegionPtr( locationFromIndex( iIndex ) );
	if ( !pRegion || pRegion->isDeleted() || pRegion->getStreamedNodeCount() == 0 ) {
		return false;
	}
	t3dRegionRuntime_t &runtime = m_szRegions[iIndex];
	Terrain3DData *const pData = m_pTerrain->dataObject();
	if ( pRegion->getStreamedNodesRuntimeInternal().empty() && pRegion->getStreamedNodeCount() > 0 ) {
		if ( !runtime.m_bRequestsStarted ) {
			ResourceLoader *const pLoader = ResourceLoader::get_singleton();
			const uint16_t *p_dependencies = NULL;
			uint16_t dependency_count = 0u;
			if ( pLoader && pData->getCookedSceneDependencySpan( pRegion->getLocation(), p_dependencies, dependency_count ) ) {
				for ( uint16_t dependency_index = 0u; dependency_index < dependency_count; ++dependency_index ) {
					const String &path = pData->getCookedSceneDependencyPath( p_dependencies[dependency_index] );
					if ( !path.is_empty() && pLoader->load_threaded_get_status( path ) == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
						if ( pLoader->load_threaded_request( path, "PackedScene", true, ResourceLoader::CACHE_MODE_REUSE ) == OK ) {
							++m_nSceneRequestsLastTick;
						}
					}
				}
			}
			runtime.m_bRequestsStarted = true;
		}
		requestCookedNodeDescriptors( iIndex );
		setRegionLoading( runtime, true );
		return true;
	}
	if ( !ensureRegionRoot( iIndex, pRegion ) ) {
		return false;
	}
	const eastl::vector<t3dStreamedNodeRuntimeDescriptor_t, Terrain3DEastlAllocator> &spawns = pRegion->getStreamedNodesRuntimeInternal();
	if ( spawns.size() > runtime.m_nNodeCapacity ) {
		LOG( ERROR, "Streamed-node active metadata capacity exceeded; cold activation storage rebuild required" );
		return false;
	}
	if ( !runtime.m_bRequestsStarted ) {
		requestRegionScenes( pRegion->getLocation(), spawns );
		runtime.m_bRequestsStarted = true;
	}
	while ( runtime.m_nNextSpawn < ( int )( spawns.size() ) && nNodeBudget > 0 ) {
		const t3dSpawnActivationResult_t result = activateSpawn( pRegion, runtime, spawns[( size_t )( runtime.m_nNextSpawn )] );
		if ( result == SPAWN_ACTIVATION_PENDING ) {
			++m_nScenePendingPollsLastTick;
			break;
		}
		runtime.m_pendingScene.unref();
		runtime.m_pendingShadowProxy.unref();
		runtime.m_nPendingSpawnId = 0;
		++runtime.m_nNextSpawn;
		--nNodeBudget;
		if ( result == SPAWN_ACTIVATION_FAILED ) {
			++m_nSceneLoadFailuresLastTick;
		}
	}
	if ( runtime.m_nNextSpawn >= ( int )( spawns.size() ) ) {
		setRegionLoading( runtime, false );
		runtime.m_bActive = true;
	}
	return true;
}

bool Terrain3DNodeStreamer::isHlodVisualSuppressed(
        const Vector2i &rOwnerLocation, const t3dStreamedNodeRuntimeDescriptor_t &rSpawn ) const {
	if ( !m_pTerrain || ( rSpawn.m_nFlags & NODE_FLAG_HLOD_VISUAL ) == 0 ) {
		return false;
	}
	const uint8_t baked_min = m_pTerrain->getHlodNodeMinimumLevel( rOwnerLocation, rSpawn.m_nId );
	return baked_min > 0 && m_pTerrain->isHlodRegionSuppressed( rOwnerLocation, baked_min );
}

bool Terrain3DNodeStreamer::streamedSpawnCoverageIntersects( const Vector2i &rOwnerLocation,
        const t3dStreamedNodeRuntimeDescriptor_t &rSpawn,
        const Vector2i &rTargetMin,
        const Vector2i &rTargetMax ) const {
	if ( rOwnerLocation.x >= rTargetMin.x && rOwnerLocation.x <= rTargetMax.x && rOwnerLocation.y >= rTargetMin.y
	        && rOwnerLocation.y <= rTargetMax.y ) {
		return true;
	}
	if ( rSpawn.m_nBoundaryPolicy != STREAMED_NODE_BOUNDARY_COVERAGE ) {
		return false;
	}
	const Vector2i coverage_min = rOwnerLocation + rSpawn.m_coverageMin;
	const Vector2i coverage_max = rOwnerLocation + rSpawn.m_coverageMax;
	return coverage_min.x <= rTargetMax.x && coverage_max.x >= rTargetMin.x && coverage_min.y <= rTargetMax.y
	        && coverage_max.y >= rTargetMin.y;
}

bool Terrain3DNodeStreamer::usesFullAuthoringResidency( void ) const {
	return m_pTerrain && m_pTerrain->isEditorAuthoringMode() && !m_pTerrain->isEditorNodeStreamingPreviewActive();
}

int Terrain3DNodeStreamer::getEffectiveRadiusRegions( void ) const {
	if ( usesFullAuthoringResidency() && m_pTerrain && m_pTerrain->dataObject() ) {
		return m_pTerrain->dataObject()->getRegionMapSizeInternal();
	}
	return m_nRadiusRegions;
}

void Terrain3DNodeStreamer::buildDesiredSet( const Vector2i &rCenter ) {
	desiredRegionsReset();
	m_desiredRegionIndices.clear();
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	const bool full_authoring_residency = usesFullAuthoringResidency();
	const int radius = getEffectiveRadiusRegions();
	const Vector2i target_min = rCenter - Vector2i( radius, radius );
	const Vector2i target_max = rCenter + Vector2i( radius, radius );
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const t3dRegionCoord_t *pLocations = pData->getRegionSlotsInternal();
	const int location_count = pData->getRegionSlotCountInternal();
	for ( int slot = 0; slot < location_count; ++slot ) {
		const Vector2i owner_location = t3dUnpackRegionCoord( pLocations[slot] );
		if ( owner_location == s_v2iMax ) {
			continue;
		}
		Terrain3DRegion *const pRegion = pData->getRegionPtr( owner_location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->getStreamedNodeCount() == 0 ) {
			continue;
		}
		const int owner_distance = MAX( Math::abs( owner_location.x - rCenter.x ), Math::abs( owner_location.y - rCenter.y ) );
		const bool ordinary_owner_desired = full_authoring_residency || owner_distance <= radius;
		bool region_desired = false;
		for ( size_t iNode = 0u; iNode < ( size_t )( ( pRegion->getStreamedNodesRuntimeInternal() ).size() ); iNode++ ) {
			const t3dStreamedNodeRuntimeDescriptor_t &spawn = ( pRegion->getStreamedNodesRuntimeInternal() )[iNode];
			if ( ( spawn.m_nFlags & NODE_FLAG_DISABLED ) != 0 ) {
				continue;
			}
			if ( ( spawn.m_nFlags & NODE_FLAG_HLOD_VISUAL ) != 0 ) {
				const uint8_t baked_min = m_pTerrain->getHlodNodeMinimumLevel( owner_location, spawn.m_nId );
				if ( baked_min > 0 ) {
					if ( !m_pTerrain->isHlodRegionSuppressed( owner_location, baked_min ) ) {
						region_desired = true;
						break;
					}
					continue;
				}
			}
			if ( ordinary_owner_desired || streamedSpawnCoverageIntersects( owner_location, spawn, target_min, target_max ) ) {
				region_desired = true;
				break;
			}
		}
		if ( region_desired ) {
			const int index = indexFromLocation( owner_location );
			if ( index >= 0 ) {
				desiredRegionSet( index );
				if ( !m_desiredRegionIndices.full() ) {
					m_desiredRegionIndices.push_back( ( uint16_t )index );
				}
			}
		}
	}
	t3dNodeDesiredRegionLess_t less;
	less.m_center = rCenter;
	eastl::sort( m_desiredRegionIndices.begin(), m_desiredRegionIndices.end(), less );
}

bool Terrain3DNodeStreamer::isRegionDesired( const Vector2i &rLocation ) const {
	const int index = indexFromLocation( rLocation );
	return index >= 0 && desiredRegionTest( index );
}

void Terrain3DNodeStreamer::deferUpdateForFrame( void ) {
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
	m_bMigrationScanSkippedLastTick = false;
	m_bDeferredLastTick = true;
}

void Terrain3DNodeStreamer::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
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
	if ( !m_bEnabled || !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	const real_t fRegionWorldSize = ( real_t )( T3D_REGION_WORLD_SIZE_METERS );
	if ( fRegionWorldSize <= 0.0f ) {
		return;
	}
	const Transform3D inverse = m_pTerrain->sceneNode().get_global_transform().affine_inverse();
	const Vector3 local_position = inverse.xform( rTargetPosition );
	const int map_size = m_pTerrain->dataObject()->getRegionMapSizeInternal();
	const int half = map_size / 2;
	const Vector2i rawCenter(
	        ( int )( Math::floor( local_position.x / fRegionWorldSize ) ), ( int )( Math::floor( local_position.z / fRegionWorldSize ) ) );
	const Vector2i center( CLAMP( rawCenter.x, -half, half - 1 ), CLAMP( rawCenter.y, -half, half - 1 ) );
	const bool bCenterChanged = center != m_center;
	const bool bMigrationDue = bForce || bCenterChanged || m_nMovableNodeCount == 0u
	        || ( ++m_nMovableMigrationTick % MAX( m_nMovableMigrationCadence, 1u ) ) == 0u;
	const bool bOwnershipChanged = bMigrationDue ? migrateActiveNodes() : false;
	m_bMigrationScanSkippedLastTick = !bMigrationDue && m_nMovableNodeCount > 0u;
	const bool bRebuildDesired = bForce || bOwnershipChanged || bCenterChanged;
	if ( !bRebuildDesired && m_nLoadingRegionCount == 0 && !m_bHasPendingActivation ) {
		bool bPendingUnload = false;
		for ( int active_i = 0; active_i < ( int )( m_activeRegionIndices.size() ); ++active_i ) {
			const int index = m_activeRegionIndices[active_i];
			++m_nActiveRegionsExaminedLastTick;
			if ( !desiredRegionTest( index ) ) {
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
	const int effective_unload_budget =
	        authoring_full_residency ? MAX( 1, getEffectiveUnloadRegionBudget() ) : getEffectiveUnloadRegionBudget();
	int unload_budget = effective_unload_budget;
	int active_position = 0;
	while ( active_position < ( int )( m_activeRegionIndices.size() ) && unload_budget > 0 ) {
		const int index = m_activeRegionIndices[active_position];
		++m_nActiveRegionsExaminedLastTick;
		if ( !desiredRegionTest( index ) ) {
			deactivateRegion( index, true );
			--unload_budget;
			continue;
		}
		++active_position;
	}
	if ( unload_budget == 0 ) {
		for ( ; active_position < ( int )( m_activeRegionIndices.size() ); ++active_position ) {
			++m_nActiveRegionsExaminedLastTick;
			if ( !desiredRegionTest( m_activeRegionIndices[active_position] ) ) {
				m_bUnloadRegionBudgetLimitedLastTick = true;
				break;
			}
		}
	}
	m_nUnloadRegionBudgetUsedLastTick = effective_unload_budget - unload_budget;
	const int effective_load_budget = authoring_full_residency ? MAX( 1, getEffectiveLoadRegionBudget() ) : getEffectiveLoadRegionBudget();
	int region_budget = effective_load_budget;
	int node_budget = authoring_full_residency ? MAX( 1, m_nLoadNodeBudget ) : m_nLoadNodeBudget;
	// Desired owners are rebuilt only when selection changes and are sorted nearest
	// first. Service that compact list directly instead of re-walking every square
	// inside every radius on each activation tick.
	for ( uint16_t desired_i = 0u; desired_i < ( uint16_t )( m_desiredRegionIndices.size() ); ++desired_i ) {
		const int index = m_desiredRegionIndices[desired_i];
		if ( node_budget <= 0 ) {
			break;
		}
		t3dRegionRuntime_t &runtime = m_szRegions[index];
		if ( !runtime.m_pRoot && region_budget <= 0 ) {
			m_bLoadRegionBudgetLimitedLastTick = true;
			m_bHasPendingActivation = true;
			continue;
		}
		const bool bHadRoot = runtime.m_pRoot != NULL;
		if ( activateRegion( index, node_budget ) && !bHadRoot ) {
			--region_budget;
			++m_nLoadsLastTick;
		}
	}
	for ( uint16_t desired_i = 0u; desired_i < ( uint16_t )( m_desiredRegionIndices.size() ); ++desired_i ) {
		const int index = m_desiredRegionIndices[desired_i];
		if ( !m_szRegions[index].m_bActive ) {
			m_bHasPendingActivation = true;
			break;
		}
	}
	m_nLoadRegionBudgetUsedLastTick = effective_load_budget - region_budget;
	m_nLoadNodeBudgetUsedLastTick =
	        authoring_full_residency ? MAX( 0, MAX( 1, m_nLoadNodeBudget ) - node_budget ) : m_nLoadNodeBudget - node_budget;
}

void Terrain3DNodeStreamer::suspend( void ) {
	while ( !m_activeRegionIndices.empty() ) {
		deactivateRegion( m_activeRegionIndices.back(), true );
	}
	desiredRegionsReset();
	memset( m_szPendingDescriptorRegionBits, 0, sizeof( m_szPendingDescriptorRegionBits ) );
	m_desiredRegionIndices.clear();
	m_bHasPendingActivation = false;
	m_center = s_v2iMax;
	m_nMovableMigrationTick = 0u;
}

void Terrain3DNodeStreamer::destroy( const bool bCaptureState ) {
	while ( !m_activeRegionIndices.empty() ) {
		deactivateRegion( m_activeRegionIndices.back(), bCaptureState );
	}
	for ( int index = 0; index < T3D_REGION_SLOTS; ++index ) {
		m_szRegions[index].m_iNodeOffset = 0u;
		m_szRegions[index].m_nNodeCount = 0u;
		m_szRegions[index].m_nNodeCapacity = 0u;
		m_szRegions[index].m_iRootPoolIndex = -1;
		m_szRegions[index].m_iActiveListPosition = -1;
	}
	m_activeRegionIndices.clear();
	m_desiredRegionIndices.clear();
	if ( m_pActiveNodeStorage && m_pRuntimeAllocatorZone ) {
		m_pRuntimeAllocatorZone->deallocate( m_pActiveNodeStorage );
	}
	if ( m_pMovableNodes && m_pRuntimeAllocatorZone ) {
		m_pRuntimeAllocatorZone->deallocate( m_pMovableNodes );
	}
	m_pActiveNodeStorage = NULL;
	m_nActiveNodeStorageCapacity = 0u;
	m_pMovableNodes = NULL;
	m_nMovableNodeCount = 0u;
	m_nMovableNodeCapacity = 0u;
	m_bActivationStorageRebuildPending = false;
	for ( int index = 0; index < m_nRegionRootPoolCount; ++index ) {
		if ( m_szRegionRootPool[index] ) {
			memdelete( m_szRegionRootPool[index] );
			m_szRegionRootPool[index] = NULL;
		}
	}
	m_nRegionRootPoolCount = 0;
	m_freeRootPoolIndices.clear();
	destroyHost();
	desiredRegionsReset();
	memset( m_szPendingDescriptorRegionBits, 0, sizeof( m_szPendingDescriptorRegionBits ) );
	m_pTerrain = NULL;
	m_center = s_v2iMax;
	m_activeRegionIndices.clear();
	m_nActiveNodeCount = 0;
	m_nLoadingRegionCount = 0;
	m_bHasPendingActivation = false;
}

void Terrain3DNodeStreamer::setEnabled( const bool bEnabled ) {
	if ( m_bEnabled == bEnabled ) {
		return;
	}
	m_bEnabled = bEnabled;
	if ( !m_bEnabled ) {
		suspend();
	}
}

void Terrain3DNodeStreamer::setLoadNodeBudget( const int nBudget ) {
	const int budget = CLAMP( nBudget, 1, 4096 );
	if ( m_nLoadNodeBudget == budget ) {
		return;
	}
	m_nLoadNodeBudget = budget;
	if ( m_pTerrain ) {
		requestActivationStorageRebuild();
	}
}

int64_t Terrain3DNodeStreamer::addStreamedSceneInternal( const Ref<PackedScene> &rScene,
        const String &rScenePath,
        const Transform3D &rGlobalTransform,
        const Dictionary &rState,
        const int nFlags,
        const int nHlodMinLevel ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || ( rScene.is_null() && rScenePath.is_empty() ) ) {
		return 0;
	}
	const Transform3D local = m_pTerrain->sceneNode().get_global_transform().affine_inverse() * rGlobalTransform;
	const real_t fRegionWorldSize = ( real_t )( T3D_REGION_WORLD_SIZE_METERS );
	if ( fRegionWorldSize <= 0.0f ) {
		return 0;
	}
	const Vector2i location(
	        ( int )( Math::floor( local.origin.x / fRegionWorldSize ) ), ( int )( Math::floor( local.origin.z / fRegionWorldSize ) ) );
	if ( Terrain3DData::getRegionMapIndex( location ) < 0 ) {
		return 0;
	}
	Terrain3DRegion *pRegion = m_pTerrain->dataObject()->getRegionPtr( location );
	if ( !pRegion ) {
		const Ref<Terrain3DRegion> pCreated = m_pTerrain->dataObject()->addRegionBlank( location, true );
		pRegion = pCreated.ptr();
	}
	if ( !pRegion ) {
		return 0;
	}
	Transform3D region_local = local;
	region_local.origin.x -= ( real_t )( location.x ) * fRegionWorldSize;
	region_local.origin.z -= ( real_t )( location.y ) * fRegionWorldSize;
	const int64_t id = allocateSpawnId();
	if ( id <= 0 ) {
		return 0;
	}
	const int effective_flags = ( nFlags & NODE_FLAG_HLOD_VISUAL ) != 0 ? ( nFlags | NODE_FLAG_STATIC ) : nFlags;
	const int64_t stored_id = !rScenePath.is_empty() ? pRegion->addStreamedNodePath( rScenePath,
	                                                           region_local,
	                                                           rState,
	                                                           effective_flags,
	                                                           id,
	                                                           STREAMED_NODE_BOUNDARY_ANCHOR,
	                                                           s_v2iZero,
	                                                           s_v2iZero,
	                                                           nHlodMinLevel )
	                                                 : pRegion->addStreamedNode( rScene,
	                                                           region_local,
	                                                           rState,
	                                                           effective_flags,
	                                                           id,
	                                                           STREAMED_NODE_BOUNDARY_ANCHOR,
	                                                           s_v2iZero,
	                                                           s_v2iZero,
	                                                           nHlodMinLevel );
	if ( stored_id != id ) {
		if ( stored_id > 0 ) {
			pRegion->removeStreamedNode( stored_id );
		}
		return 0;
	}
	const int index = indexFromLocation( location );
	if ( index >= 0 && m_szRegions[index].m_pRoot ) {
		deactivateRegion( index, true );
	}
	requestActivationStorageRebuild();
	m_center = s_v2iMax;
	return id;
}

int64_t Terrain3DNodeStreamer::addStreamedScene( const Ref<PackedScene> &rScene,
        const Transform3D &rGlobalTransform,
        const Dictionary &rState,
        const int nFlags,
        const int nHlodMinLevel ) {
	return addStreamedSceneInternal( rScene, String(), rGlobalTransform, rState, nFlags, nHlodMinLevel );
}

int64_t Terrain3DNodeStreamer::addStreamedScenePath( const String &rScenePath,
        const Transform3D &rGlobalTransform,
        const Dictionary &rState,
        const int nFlags,
        const int nHlodMinLevel ) {
	return addStreamedSceneInternal( Ref<PackedScene>(), rScenePath, rGlobalTransform, rState, nFlags, nHlodMinLevel );
}

bool Terrain3DNodeStreamer::removeStreamedScene( const int64_t nSpawnId ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || nSpawnId <= 0 ) {
		return false;
	}
	Terrain3DRegion *const *ppRegions = m_pTerrain->dataObject()->getRegionPtrMapInternal();
	for ( int index = 0; index < T3D_REGION_SLOTS; ++index ) {
		Terrain3DRegion *const pRegion = ppRegions[index];
		if ( !pRegion || !pRegion->removeStreamedNode( nSpawnId ) ) {
			continue;
		}
		if ( m_szRegions[index].m_pRoot ) {
			deactivateRegion( index, false );
		}
		requestActivationStorageRebuild();
		m_center = s_v2iMax;
		return true;
	}
	return false;
}

size_t Terrain3DNodeStreamer::getRetainedCapacityBytes( void ) const {
	return m_nActiveNodeStorageCapacity * sizeof( t3dActiveNode_t ) + m_nMovableNodeCapacity * sizeof( t3dMovableNodeRef_t );
}

void Terrain3DNodeStreamer::onHlodRepresentationChanged( const Vector2i &rRegion ) {
	const int index = indexFromLocation( rRegion );
	if ( index < 0 || !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	t3dRegionRuntime_t &runtime = m_szRegions[index];
	Terrain3DRegion *const pRegion = m_pTerrain->dataObject()->getRegionPtr( rRegion );
	if ( !pRegion ) {
		return;
	}
	for ( int i = ( int )runtime.m_nNodeCount - 1; i >= 0; --i ) {
		const t3dActiveNode_t active = regionNodes( runtime )[( size_t )i];
		if ( ( active.m_nFlags & NODE_FLAG_HLOD_VISUAL ) == 0 ) {
			continue;
		}
		const t3dStreamedNodeRuntimeDescriptor_t *spawn = pRegion->findStreamedNodeRuntimeInternal( active.m_nSpawnId );
		if ( !spawn || !isHlodVisualSuppressed( rRegion, *spawn ) ) {
			continue;
		}
		Node *node = getActiveNode( active );
		if ( node ) {
			if ( node->get_parent() ) {
				node->get_parent()->remove_child( node );
			}
			memdelete( node );
		}
		eraseActiveNode( runtime, i );
		m_nActiveNodeCount = MAX( 0, m_nActiveNodeCount - 1 );
		++m_nNodesDeactivatedLastTick;
	}
	if ( runtime.m_pRoot ) {
		runtime.m_nNextSpawn = 0;
		runtime.m_bRequestsStarted = false;
		runtime.m_bActive = false;
		setRegionLoading( runtime, true );
	}
	m_center = s_v2iMax;
	m_bHasPendingActivation = true;
	++m_nRuntimeGeneration;
}

bool Terrain3DNodeStreamer::areHlodVisualRegionsReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return true;
	}
	for ( int z = rRegionMin.y; z <= rRegionMax.y; ++z ) {
		for ( int x = rRegionMin.x; x <= rRegionMax.x; ++x ) {
			const Vector2i region( x, z );
			const int index = indexFromLocation( region );
			if ( index < 0 ) {
				continue;
			}
			Terrain3DRegion *const pRegion = m_pTerrain->dataObject()->getRegionPtr( region );
			if ( !pRegion ) {
				continue;
			}
			const t3dRegionRuntime_t &runtime = m_szRegions[index];
			for ( size_t iNode = 0u; iNode < ( size_t )( ( pRegion->getStreamedNodesRuntimeInternal() ).size() ); iNode++ ) {
				const t3dStreamedNodeRuntimeDescriptor_t &spawn = ( pRegion->getStreamedNodesRuntimeInternal() )[iNode];
				if ( ( spawn.m_nFlags & NODE_FLAG_HLOD_VISUAL ) == 0 ) {
					continue;
				}
				const uint8_t baked_min = m_pTerrain->getHlodNodeMinimumLevel( region, spawn.m_nId );
				if ( baked_min == 0 || m_pTerrain->isHlodRegionSuppressed( region, baked_min ) ) {
					continue;
				}
				if ( !runtimeHasSpawn( runtime, spawn.m_nId ) ) {
					return false;
				}
			}
		}
	}
	return true;
}

Dictionary Terrain3DNodeStreamer::getStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bEnabled;
	stats["radius_regions"] = m_nRadiusRegions;
	stats["effective_radius_regions"] = getEffectiveRadiusRegions();
	stats["editor_preview"] = m_pTerrain && m_pTerrain->isEditorNodeStreamingPreviewActive();
	stats["full_authoring_residency"] = usesFullAuthoringResidency();
	stats["active_regions"] = ( int )( m_activeRegionIndices.size() );
	stats["active_nodes"] = m_nActiveNodeCount;
	stats["movable_nodes"] = ( int )( m_nMovableNodeCount );
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
	stats["activation_storage_rebuild_pending"] = m_bActivationStorageRebuildPending;
	stats["cold_rebuild_service_active"] = m_bColdRebuildServiceActive;
	stats["cold_rebuild_count"] = ( int64_t )( m_nColdRebuildCount );
	stats["cold_rebuild_failures"] = ( int64_t )( m_nColdRebuildFailures );
	stats["capacity_misses"] = ( int64_t )( m_nCapacityMissCount );
	stats["migration_cpu_usec_last_tick"] = ( int64_t )( m_nMigrationUsecLastTick );
	stats["migration_scan_skipped_last_tick"] = m_bMigrationScanSkippedLastTick;
	stats["movable_migration_cadence"] = ( int )( m_nMovableMigrationCadence );
	int virtual_nodes = 0;
	int pending_spawns = 0;
	int pending_regions = 0;
	int pending_unloads = 0;
	if ( m_pTerrain && m_pTerrain->dataObject() ) {
		Terrain3DData *const pData = m_pTerrain->dataObject();
		const t3dRegionCoord_t *pLocations = pData->getRegionSlotsInternal();
		const int location_count = pData->getRegionSlotCountInternal();
		for ( int slot = 0; slot < location_count; ++slot ) {
			if ( !t3dRegionCoordValid( pLocations[slot] ) ) {
				continue;
			}
			const Terrain3DRegion *pRegion = pData->getRegionPtr( t3dUnpackRegionCoord( pLocations[slot] ) );
			if ( pRegion ) {
				virtual_nodes += pRegion->getStreamedNodeCount();
			}
		}
		for ( uint16_t desired_i = 0u; desired_i < ( uint16_t )( m_desiredRegionIndices.size() ); ++desired_i ) {
			const int index = m_desiredRegionIndices[desired_i];
			if ( !m_szRegions[index].m_bActive ) {
				++pending_regions;
			}
		}
		for ( int active_i = 0; active_i < ( int )( m_activeRegionIndices.size() ); ++active_i ) {
			const int index = m_activeRegionIndices[active_i];
			if ( !desiredRegionTest( index ) ) {
				++pending_unloads;
			}
			if ( m_szRegions[index].m_bLoading ) {
				const Terrain3DRegion *pRegion = pData->getRegionPtr( locationFromIndex( index ) );
				const int spawn_count = pRegion ? pRegion->getStreamedNodeCount() : 0;
				pending_spawns += MAX( 0, spawn_count - m_szRegions[index].m_nNextSpawn );
			}
		}
	}
	stats["virtual_nodes"] = virtual_nodes;
	stats["desired_regions"] = ( int )( m_desiredRegionIndices.size() );
	stats["pending_spawns"] = pending_spawns;
	stats["pending_regions"] = pending_regions;
	stats["pending_unloads"] = pending_unloads;
	stats["load_region_budget_saturated_last_tick"] = m_bLoadRegionBudgetLimitedLastTick;
	stats["load_node_budget_saturated_last_tick"] =
	        m_nLoadNodeBudget > 0 && m_nLoadNodeBudgetUsedLastTick >= m_nLoadNodeBudget && pending_spawns > 0;
	stats["unload_region_budget_saturated_last_tick"] = m_bUnloadRegionBudgetLimitedLastTick;
	return stats;
}

void Terrain3DNodeStreamer::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( NODE_FLAG_NONE );
	BIND_ENUM_CONSTANT( NODE_FLAG_DISABLED );
	BIND_ENUM_CONSTANT( NODE_FLAG_STATIC );
	BIND_ENUM_CONSTANT( NODE_FLAG_HLOD_VISUAL );
	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &Terrain3DNodeStreamer::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &Terrain3DNodeStreamer::isEnabled );
	ClassDB::bind_method( D_METHOD( "set_radius_regions", "radius" ), &Terrain3DNodeStreamer::setRadiusRegions );
	ClassDB::bind_method( D_METHOD( "get_radius_regions" ), &Terrain3DNodeStreamer::getRadiusRegions );
	ClassDB::bind_method( D_METHOD( "set_load_region_budget", "budget" ), &Terrain3DNodeStreamer::setLoadRegionBudget );
	ClassDB::bind_method( D_METHOD( "get_load_region_budget" ), &Terrain3DNodeStreamer::getLoadRegionBudget );
	ClassDB::bind_method( D_METHOD( "set_load_node_budget", "budget" ), &Terrain3DNodeStreamer::setLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "get_load_node_budget" ), &Terrain3DNodeStreamer::getLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "set_unload_region_budget", "budget" ), &Terrain3DNodeStreamer::setUnloadRegionBudget );
	ClassDB::bind_method( D_METHOD( "get_unload_region_budget" ), &Terrain3DNodeStreamer::getUnloadRegionBudget );
	ClassDB::bind_method( D_METHOD( "add_streamed_scene", "scene", "global_transform", "state", "flags", "hlod_min_level" ),
	        &Terrain3DNodeStreamer::addStreamedScene,
	        DEFVAL( Dictionary() ),
	        DEFVAL( NODE_FLAG_NONE ),
	        DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "add_streamed_scene_path", "scene_path", "global_transform", "state", "flags", "hlod_min_level" ),
	        &Terrain3DNodeStreamer::addStreamedScenePath,
	        DEFVAL( Dictionary() ),
	        DEFVAL( NODE_FLAG_NONE ),
	        DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "remove_streamed_scene", "spawn_id" ), &Terrain3DNodeStreamer::removeStreamedScene );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &Terrain3DNodeStreamer::getStats );
}

// M40 extracted non-template header implementations.
void Terrain3DNodeStreamer::desiredRegionSet( const int iIndex ) {
		if ( iIndex >= 0 && iIndex < T3D_REGION_SLOTS ) {
			m_szDesiredRegionBits[( size_t )( iIndex ) >> 6u] |= 1ull << ( ( size_t )( iIndex ) & 63u );
		}
	}

void Terrain3DNodeStreamer::pendingDescriptorRegionSet( const int iIndex ) {
		if ( iIndex >= 0 && iIndex < T3D_REGION_SLOTS ) {
			m_szPendingDescriptorRegionBits[( size_t )( iIndex ) >> 6u] |= 1ull << ( ( size_t )( iIndex ) & 63u );
		}
	}

void Terrain3DNodeStreamer::pendingDescriptorRegionClear( const int iIndex ) {
		if ( iIndex >= 0 && iIndex < T3D_REGION_SLOTS ) {
			m_szPendingDescriptorRegionBits[( size_t )( iIndex ) >> 6u] &= ~( 1ull << ( ( size_t )( iIndex ) & 63u ) );
		}
	}

