// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "lighting/terrain_3d_lightmap_streamer.h"

#include <EASTL/sort.h>

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include "terrain_3d.h"
#include "data/terrain_3d_data.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "data/terrain_3d_region.h"

// Godot renders at most eight LightmapGI instances reliably in one camera view.
// Per-region packages therefore remain independent on disk, while residency is
// prioritized around the streaming center and capped at that engine limit.

namespace {
uint64_t terrain3dHashReceiver( uint64_t nHash, const NodePath &rPath, const uint64_t nObjectId ) {
	const CharString utf8 = String( rPath ).utf8();
	uint64_t hash = nHash;
	for ( int index = 0; index < utf8.length(); ++index ) {
		hash ^= ( uint8_t ) ( utf8[index] );
		hash *= TERRAIN3D_LIGHTMAP_FNV_PRIME;
	}
	uint64_t object_id = nObjectId;
	for ( int byte_index = 0; byte_index < 8; ++byte_index ) {
		hash ^= ( uint8_t ) ( object_id & 0xffu );
		hash *= TERRAIN3D_LIGHTMAP_FNV_PRIME;
		object_id >>= 8u;
	}
	return hash;
}

Node *terrain3dResolveLightmapReceiver( T3DTerrain *pTerrain, const NodePath &rBakedPath ) {
	if ( pTerrain == nullptr ) {
		return nullptr;
	}
	// LightmapGI stores baked-user paths relative to the LightmapGI node. The
	// bake hierarchy and runtime hierarchy both place LightmapGI beside the
	// Terrain3DNodeStreamHost under Terrain3D/the bake world, so an expected path
	// starts with "../Terrain3DNodeStreamHost/...". Resolve that sibling path from
	// Terrain3D while preserving the original path for LightmapGIData itself.
	String path = String( rBakedPath );
	if ( path.begins_with( "../" ) ) {
		path = path.substr( 3 );
	}
	static const String stream_host( "Terrain3DNodeStreamHost" );
	if ( path != stream_host && !path.begins_with( String( "Terrain3DNodeStreamHost/" ) ) ) {
		return nullptr;
	}
	return pTerrain->get_node_or_null( NodePath( path ) );
}
} // namespace

T3DLightmapStreamer::~T3DLightmapStreamer( void ) {
	destroy();
}

void T3DLightmapStreamer::initialize( T3DTerrain *const pTerrain ) {
	if ( m_pTerrain == pTerrain ) {
		return;
	}
	destroy();
	m_pTerrain = pTerrain;
	m_center = V2I_MAX;
	m_nNodeRuntimeGeneration = 0u;
}

void T3DLightmapStreamer::destroy( void ) {
	while ( !m_active.empty() ) {
		deactivate( m_active.size() - 1u );
	}
	m_pTerrain = nullptr;
	m_center = V2I_MAX;
	m_nNodeRuntimeGeneration = 0u;
	m_bHasPendingActivation = false;
}

void T3DLightmapStreamer::suspend( void ) {
	while ( !m_active.empty() ) {
		deactivate( m_active.size() - 1u );
	}
	m_center = V2I_MAX;
	m_nNodeRuntimeGeneration = 0u;
	m_bHasPendingActivation = false;
}

void T3DLightmapStreamer::setEnabled( const bool bEnabled ) {
	if ( m_bEnabled == bEnabled ) {
		return;
	}
	m_bEnabled = bEnabled;
	if ( !m_bEnabled ) {
		suspend();
	}
}

void T3DLightmapStreamer::setResidencyBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 1024 );
	if ( m_nResidencyBudgetMb == budget_mb ) {
		return;
	}
	m_nResidencyBudgetMb = budget_mb;
	const size_t budget_bytes = ( size_t ) ( getEffectiveResidencyBudgetMb() ) * 1024u * 1024u;
	while ( !m_active.empty() && getResidentEstimatedBytes() > budget_bytes ) {
		deactivate( m_active.size() - 1u );
	}
	m_center = V2I_MAX;
}

int T3DLightmapStreamer::getEffectiveResidencyBudgetMb( void ) const {
	// The terrain memory-budget coordinator already assigns this subsystem's
	// residency budget. Authoring pins successfully loaded packages, but does not
	// manufacture a second editor-only allowance outside that aggregate budget.
	return MAX( 1, m_nResidencyBudgetMb );
}

void T3DLightmapStreamer::setRadiusRegions( const int nRadius ) {
	const int radius = CLAMP( nRadius, 0, 16 );
	if ( m_nRadiusRegions == radius ) {
		return;
	}
	m_nRadiusRegions = radius;
	m_center = V2I_MAX;
}

T3DLightmapStreamer::t3dactivePackage_t *T3DLightmapStreamer::findActive(
	const Ref<T3DRegionLighting> &rPackage
) {
	for ( t3dactivePackage_t &active : m_active ) {
		if ( active.pPackage == rPackage ) {
			return &active;
		}
	}
	return nullptr;
}

const T3DLightmapStreamer::t3dactivePackage_t *T3DLightmapStreamer::findActive(
	const Ref<T3DRegionLighting> &rPackage
) const {
	for ( const t3dactivePackage_t &active : m_active ) {
		if ( active.pPackage == rPackage ) {
			return &active;
		}
	}
	return nullptr;
}

T3DLightmapStreamer::t3dsourceLoadResult_t T3DLightmapStreamer::requestSourceData(
	const Ref<T3DRegionLighting> &rPackage, Ref<LightmapGIData> &rData
) {
	rData.unref();
	if ( rPackage.is_null() || !rPackage->hasLightData() ) {
		return SOURCE_LOAD_FAILED;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( pLoader == nullptr ) {
		return SOURCE_LOAD_FAILED;
	}
	const String path = rPackage->getLightDataPath();
	const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
	if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
		const Error error = pLoader->load_threaded_request( path, "LightmapGIData", true,
			ResourceLoader::CACHE_MODE_IGNORE_DEEP );
		if ( error != OK ) {
			++m_nResourceFailuresLastTick;
			++m_nResourceFailuresTotal;
			return SOURCE_LOAD_FAILED;
		}
		++m_nResourceRequestsLastTick;
		++m_nResourceRequestsTotal;
		return SOURCE_LOAD_PENDING;
	}
	if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
		return SOURCE_LOAD_PENDING;
	}
	if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
		++m_nResourceFailuresLastTick;
		++m_nResourceFailuresTotal;
		return SOURCE_LOAD_FAILED;
	}
	const Ref<Resource> pResource = pLoader->load_threaded_get( path );
	rData = pResource;
	if ( rData.is_null() ) {
		++m_nResourceFailuresLastTick;
		++m_nResourceFailuresTotal;
		return SOURCE_LOAD_FAILED;
	}
	return SOURCE_LOAD_READY;
}

bool T3DLightmapStreamer::collectReceiverState(
	const Ref<T3DRegionLighting> &rPackage, uint64_t &nSignature, int &nReceiverCount
) const {
	nSignature = TERRAIN3D_LIGHTMAP_FNV_OFFSET;
	nReceiverCount = 0;
	if ( m_pTerrain == nullptr || rPackage.is_null() ) { return false; }
	const Array &raw = rPackage->getBakedUserDataInternal();
	if ( raw.is_empty() || ( raw.size() % 4 ) != 0 ) { return rPackage->hasProbeData(); }
	for ( int index = 0; index < raw.size(); index += 4 ) {
		const NodePath path = raw[index];
		Node *const pReceiver = terrain3dResolveLightmapReceiver( m_pTerrain, path );
		if ( pReceiver == nullptr || !pReceiver->is_inside_tree() ) { continue; }
		nSignature = terrain3dHashReceiver( nSignature, path, pReceiver->get_instance_id() );
		++nReceiverCount;
	}
	return nReceiverCount > 0 || rPackage->hasProbeData();
}

bool T3DLightmapStreamer::buildRuntimeData(
	const Ref<T3DRegionLighting> &rPackage,
	const Ref<LightmapGIData> &rSource, Ref<LightmapGIData> &rData,
	uint64_t &nSignature, int &nReceiverCount
) const {
	rData.unref();
	nSignature = TERRAIN3D_LIGHTMAP_FNV_OFFSET;
	nReceiverCount = 0;
	if ( m_pTerrain == nullptr || rPackage.is_null() || rSource.is_null() ) {
		return false;
	}
	const Array &raw = rPackage->getBakedUserDataInternal();
	if ( raw.is_empty() || ( raw.size() % 4 ) != 0 ) {
		return false;
	}

	const Ref<Resource> pDuplicated = rSource->duplicate( false );
	Ref<LightmapGIData> pRuntimeData = pDuplicated;
	if ( pRuntimeData.is_null() ) {
		return false;
	}
	pRuntimeData->clear_users();
	for ( int index = 0; index < raw.size(); index += 4 ) {
		const NodePath path = raw[index];
		Node *const pReceiver = terrain3dResolveLightmapReceiver( m_pTerrain, path );
		if ( pReceiver == nullptr || !pReceiver->is_inside_tree() ) {
			continue;
		}
		const Rect2 uv_scale = raw[index + 1];
		const int slice_index = ( int ) ( raw[index + 2] );
		const int sub_instance = ( int ) ( raw[index + 3] );
		pRuntimeData->add_user( path, uv_scale, slice_index, sub_instance );
		nSignature = terrain3dHashReceiver( nSignature, path, pReceiver->get_instance_id() );
		++nReceiverCount;
	}
	if ( nReceiverCount == 0 && !rPackage->hasProbeData() ) {
		return false;
	}
	rData = pRuntimeData;
	return true;
}

bool T3DLightmapStreamer::isPackageCompatibleWithActive( const Ref<T3DRegionLighting> &rPackage ) const {
	if ( rPackage.is_null() ) {
		return false;
	}
	const String signature = rPackage->getDirectionalLightSignature();
	for ( const t3dactivePackage_t &active : m_active ) {
		if ( active.pPackage.is_null() ) {
			continue;
		}
		if ( active.pPackage->getShadowmaskMode() != rPackage->getShadowmaskMode() ) {
			return false;
		}
		const String active_signature = active.pPackage->getDirectionalLightSignature();
		if ( !signature.is_empty() && !active_signature.is_empty() && signature != active_signature ) {
			return false;
		}
	}
	return true;
}

bool T3DLightmapStreamer::activate(
	const Ref<T3DRegionLighting> &rPackage,
	const Ref<LightmapGIData> &rSourceData, const Ref<LightmapGIData> &rRuntimeData,
	const uint64_t nSignature, const int nReceiverCount
) {
	if ( m_pTerrain == nullptr || rPackage.is_null() || rSourceData.is_null() || rRuntimeData.is_null() || ( nReceiverCount <= 0 && !rPackage->hasProbeData() ) ) {
		return false;
	}
	if ( findActive( rPackage ) != nullptr ) {
		return true;
	}
	const int active_limit = TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT;
	if ( ( int ) ( m_active.size() ) >= active_limit ) {
		return false;
	}
	const size_t incoming_bytes = ( size_t ) ( MAX<int64_t>( 0, rPackage->getEstimatedGpuBytes() ) );
	const size_t budget_bytes = ( size_t ) ( getEffectiveResidencyBudgetMb() ) * 1024u * 1024u;
	const size_t resident_bytes = m_nResidentEstimatedBytes;
	if ( incoming_bytes > budget_bytes || resident_bytes > budget_bytes - incoming_bytes ) {
		return false;
	}

	LightmapGI *const pLightmap = memnew( LightmapGI );
	const Vector2i cluster = rPackage->getClusterLocation();
	pLightmap->set_name( String( "Terrain3DLightmapGI_" ) + String::num_int64( cluster.x ) + "_" + String::num_int64( cluster.y ) );
	pLightmap->set_light_data( rRuntimeData );
	pLightmap->set_shadowmask_mode( ( LightmapGIData::ShadowmaskMode ) ( rPackage->getShadowmaskMode() ) );
	m_pTerrain->add_child( pLightmap, false, Node::INTERNAL_MODE_BACK );

	t3dactivePackage_t active;
	active.pPackage = rPackage;
	active.pSourceData = rSourceData;
	active.pRuntimeData = rRuntimeData;
	active.pNode = pLightmap;
	active.receiverSignature = nSignature;
	active.nReceiverCount = nReceiverCount;
	active.bDesired = true;
	m_active.push_back( active );
	m_nResidentEstimatedBytes += incoming_bytes;
	++m_nLoadsLastTick;
	++m_nLoadsTotal;
	return true;
}

bool T3DLightmapStreamer::refreshActive(
	t3dactivePackage_t &rActive,
	const Ref<LightmapGIData> &rRuntimeData, const uint64_t nSignature, const int nReceiverCount
) {
	if ( m_pTerrain == nullptr || rActive.pPackage.is_null() || rRuntimeData.is_null() || ( nReceiverCount <= 0 && !rActive.pPackage->hasProbeData() ) ) {
		return false;
	}
	if ( rActive.receiverSignature == nSignature && rActive.nReceiverCount == nReceiverCount && rActive.pNode != nullptr ) {
		return true;
	}
	if ( rActive.pNode != nullptr ) {
		if ( rActive.pNode->get_parent() != nullptr ) {
			rActive.pNode->get_parent()->remove_child( rActive.pNode );
		}
		memdelete( rActive.pNode );
		rActive.pNode = nullptr;
	}
	LightmapGI *const pLightmap = memnew( LightmapGI );
	const Vector2i cluster = rActive.pPackage->getClusterLocation();
	pLightmap->set_name( String( "Terrain3DLightmapGI_" ) + String::num_int64( cluster.x ) + "_" + String::num_int64( cluster.y ) );
	pLightmap->set_light_data( rRuntimeData );
	pLightmap->set_shadowmask_mode( ( LightmapGIData::ShadowmaskMode ) ( rActive.pPackage->getShadowmaskMode() ) );
	m_pTerrain->add_child( pLightmap, false, Node::INTERNAL_MODE_BACK );
	rActive.pRuntimeData = rRuntimeData;
	rActive.pNode = pLightmap;
	rActive.receiverSignature = nSignature;
	rActive.nReceiverCount = nReceiverCount;
	++m_nRefreshesLastTick;
	++m_nRefreshesTotal;
	return true;
}

void T3DLightmapStreamer::deactivate( const size_t iIndex ) {
	if ( iIndex >= m_active.size() ) {
		return;
	}
	t3dactivePackage_t &active = m_active[iIndex];
	if ( active.pNode != nullptr ) {
		if ( active.pNode->get_parent() != nullptr ) {
			active.pNode->get_parent()->remove_child( active.pNode );
		}
		memdelete( active.pNode );
		active.pNode = nullptr;
	}
	const size_t removed_bytes = active.pPackage.is_valid() ? ( size_t ) ( MAX<int64_t>( 0, active.pPackage->getEstimatedGpuBytes() ) ) : 0u;
	active.pRuntimeData.unref();
	active.pSourceData.unref();
	m_active.erase( m_active.begin() + iIndex );
	m_nResidentEstimatedBytes = removed_bytes <= m_nResidentEstimatedBytes ? m_nResidentEstimatedBytes - removed_bytes : 0u;
	++m_nUnloadsLastTick;
	++m_nUnloadsTotal;
}

void T3DLightmapStreamer::markDesired( const Vector2i &rCenter ) {
	m_bHasPendingActivation = false;
	m_nPendingPackagesLastTick = 0;
	m_nCapacityDeferredLastTick = 0;
	for ( t3dactivePackage_t &pActive : m_active ) {
		pActive.bDesired = false;
	}
	if ( m_pTerrain == nullptr || m_pTerrain->getData() == nullptr ) {
		return;
	}

	T3DData *const pData = m_pTerrain->getData();
	T3DNodeStreamer *const pNodeStreamer = m_pTerrain->getNodeStreamer();
	const bool authoring_full_residency = m_pTerrain->isEditorAuthoringMode();
	const Vector2i *pLocations = pData->getRegionSlotsInternal();
	T3DRegion *const *ppRegions = pData->getRegionPtrMapInternal();
	const int slot_count = pData->getRegionSlotCountInternal();
	int remaining_loads = authoring_full_residency ? MAX( 1, m_nLoadBudget ) : m_nLoadBudget;

	// Region slots are storage-order, not camera-order. Sort desired candidates by
	// distance so a per-region package budget always favors the center and its
	// nearest neighbors instead of whichever regions happened to occupy low slots.
	eastl::vector<int> candidate_slots;
	candidate_slots.reserve( ( size_t ) ( slot_count ) );
	for ( int slot = 0; slot < slot_count; ++slot ) {
		const Vector2i location = pLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		const t3dregionStreamingState_t state = m_pTerrain->getRegionStreamingStateInternal( location );
		const bool bDesiredByTier = authoring_full_residency || state >= TERRAIN3D_REGION_STREAMING_COLD;
		const bool bDesiredByNodeCoverage = pNodeStreamer != nullptr && pNodeStreamer->isRegionDesired( location );
		if ( bDesiredByTier || bDesiredByNodeCoverage ) {
			candidate_slots.push_back( slot );
		}
	}
	eastl::sort( candidate_slots.begin(), candidate_slots.end(), [pLocations, &rCenter]( const int a, const int b ) {
		const Vector2i da = pLocations[a] - rCenter;
		const Vector2i db = pLocations[b] - rCenter;
		const int64_t distance_a = ( int64_t ) ( da.x ) * da.x + ( int64_t ) ( da.y ) * da.y;
		const int64_t distance_b = ( int64_t ) ( db.x ) * db.x + ( int64_t ) ( db.y ) * db.y;
		if ( distance_a != distance_b ) {
			return distance_a < distance_b;
		}
		if ( pLocations[a].y != pLocations[b].y ) {
			return pLocations[a].y < pLocations[b].y;
		}
		return pLocations[a].x < pLocations[b].x;
	} );

	eastl::vector<Ref<T3DRegionLighting>> processed;
	processed.reserve( TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT );

	for ( const int slot : candidate_slots ) {
		T3DRegion *const pRegion = ppRegions[slot];
		if ( pRegion == nullptr || !pRegion->hasLighting() ) {
			continue;
		}
		const Ref<T3DRegionLighting> package = pRegion->getLighting();
		if ( package.is_null() ) {
			continue;
		}
		bool bAlreadyProcessed = false;
		for ( const Ref<T3DRegionLighting> &seen : processed ) {
			if ( seen == package ) {
				bAlreadyProcessed = true;
				break;
			}
		}
		if ( bAlreadyProcessed ) {
			continue;
		}
		if ( ( int ) ( processed.size() ) >= TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT ) {
			++m_nCapacityDeferredLastTick;
			continue;
		}
		processed.push_back( package );

		t3dactivePackage_t *pActive = findActive( package );
		Ref<LightmapGIData> source_data;
		if ( pActive != nullptr ) {
			source_data = pActive->pSourceData;
		} else {
			const t3dsourceLoadResult_t load_result = requestSourceData( package, source_data );
			if ( load_result == SOURCE_LOAD_PENDING ) {
				m_bHasPendingActivation = true;
				++m_nPendingPackagesLastTick;
				continue;
			}
			if ( load_result != SOURCE_LOAD_READY ) {
				continue;
			}
		}

		Ref<LightmapGIData> pRuntimeData;
		uint64_t receiver_signature = 0u;
		int receiver_count = 0;
		if ( pActive != nullptr ) {
			const bool bHasActiveReceivers = collectReceiverState( package, receiver_signature, receiver_count );
			if ( bHasActiveReceivers && pActive->pNode != nullptr &&
				pActive->receiverSignature == receiver_signature && pActive->nReceiverCount == receiver_count ) {
				pActive->bDesired = true;
				continue;
			}
		}
		const bool bHasActiveReceivers = buildRuntimeData( package, source_data, pRuntimeData,
			receiver_signature, receiver_count );
		if ( pActive != nullptr ) {
			pActive->bDesired = bHasActiveReceivers;
			if ( bHasActiveReceivers ) {
				refreshActive( *pActive, pRuntimeData, receiver_signature, receiver_count );
			} else if ( package->getReceiverCount() > 0 ) {
				m_bHasPendingActivation = true;
				++m_nPendingPackagesLastTick;
			}
			continue;
		}
		if ( !bHasActiveReceivers ) {
			if ( package->getReceiverCount() > 0 ) {
				m_bHasPendingActivation = true;
				++m_nPendingPackagesLastTick;
			}
			continue;
		}
		if ( remaining_loads <= 0 ) {
			m_bHasPendingActivation = true;
			++m_nPendingPackagesLastTick;
			continue;
		}
		if ( !isPackageCompatibleWithActive( package ) ) {
			++m_nCompatibilityFailuresLastTick;
			++m_nCompatibilityFailuresTotal;
			m_bHasPendingActivation = true;
			++m_nPendingPackagesLastTick;
			ERR_PRINT_ONCE( "Terrain3D regional lightmap packages with incompatible shadowmask modes or directional-light signatures cannot be active together" );
			continue;
		}
		const int active_limit = TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT;
		if ( ( int ) ( m_active.size() ) >= active_limit ) {
			m_bHasPendingActivation = true;
			++m_nPendingPackagesLastTick;
			++m_nCapacityDeferredLastTick;
			continue;
		}
		if ( activate( package, source_data, pRuntimeData, receiver_signature, receiver_count ) ) {
			--remaining_loads;
		} else {
			m_bHasPendingActivation = true;
			++m_nPendingPackagesLastTick;
			++m_nCapacityDeferredLastTick;
		}
	}
}

void T3DLightmapStreamer::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
	m_nLoadsLastTick = 0;
	m_nUnloadsLastTick = 0;
	m_nRefreshesLastTick = 0;
	m_nResourceRequestsLastTick = 0;
	m_nResourceFailuresLastTick = 0;
	m_nCompatibilityFailuresLastTick = 0;
	if ( !m_bEnabled || m_pTerrain == nullptr || m_pTerrain->getData() == nullptr ) {
		return;
	}
	T3DNodeStreamer *const pNodeStreamer = m_pTerrain->getNodeStreamer();
	const uint64_t node_generation = pNodeStreamer != nullptr ? pNodeStreamer->getRuntimeGeneration() : 0u;
	const bool bReceiverSetChanged = node_generation != m_nNodeRuntimeGeneration;
	const Vector2i center = m_pTerrain->getData()->getRegionLocation( rTargetPosition );
	if ( !bForce && center == m_center && !m_bHasPendingActivation && !bReceiverSetChanged ) {
		return;
	}
	m_center = center;
	m_nNodeRuntimeGeneration = node_generation;
	markDesired( center );

	int remaining_unloads = m_pTerrain->isEditorAuthoringMode() ? MAX( 1, m_nUnloadBudget ) : m_nUnloadBudget;
	for ( size_t index = m_active.size(); index > 0u && remaining_unloads > 0; --index ) {
		const size_t active_index = index - 1u;
		if ( m_active[active_index].bDesired ) {
			continue;
		}
		deactivate( active_index );
		--remaining_unloads;
	}
}


Dictionary T3DLightmapStreamer::getStatsFast( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bEnabled;
	stats["radius_regions"] = m_nRadiusRegions;
	stats["unified_cold_radius"] = m_pTerrain ? m_pTerrain->getStreamingColdRadius() : 0;
	stats["active_packages"] = ( int ) ( m_active.size() );
	stats["max_active_packages"] = TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT;
	stats["pending_packages"] = m_nPendingPackagesLastTick;
	stats["capacity_deferred"] = m_nCapacityDeferredLastTick;
	stats["compatibility_failures_last_tick"] = m_nCompatibilityFailuresLastTick;
	stats["compatibility_failures_total"] = ( int64_t ) ( m_nCompatibilityFailuresTotal );
	stats["estimated_gpu_bytes"] = ( int64_t ) ( m_nResidentEstimatedBytes );
	stats["residency_budget_mb"] = m_nResidencyBudgetMb;
	stats["effective_residency_budget_mb"] = getEffectiveResidencyBudgetMb();
	stats["loads_last_tick"] = m_nLoadsLastTick;
	stats["unloads_last_tick"] = m_nUnloadsLastTick;
	stats["refreshes_last_tick"] = m_nRefreshesLastTick;
	stats["resource_requests_last_tick"] = m_nResourceRequestsLastTick;
	stats["resource_failures_last_tick"] = m_nResourceFailuresLastTick;
	stats["loads_total"] = ( int64_t ) ( m_nLoadsTotal );
	stats["unloads_total"] = ( int64_t ) ( m_nUnloadsTotal );
	stats["refreshes_total"] = ( int64_t ) ( m_nRefreshesTotal );
	stats["resource_requests_total"] = ( int64_t ) ( m_nResourceRequestsTotal );
	stats["resource_failures_total"] = ( int64_t ) ( m_nResourceFailuresTotal );
	return stats;
}

Dictionary T3DLightmapStreamer::getStats( void ) const {
	Dictionary stats;
	int lightmap_textures = 0;
	int shadowmask_textures = 0;
	int active_receivers = 0;
	int64_t estimated_gpu_bytes = 0;
	for ( const t3dactivePackage_t &active : m_active ) {
		if ( active.pPackage.is_null() ) {
			continue;
		}
		lightmap_textures += active.pPackage->getLightmapTextureCount();
		shadowmask_textures += active.pPackage->getShadowmaskTextureCount();
		active_receivers += active.nReceiverCount;
		estimated_gpu_bytes += active.pPackage->getEstimatedGpuBytes();
	}
	stats["enabled"] = m_bEnabled;
	stats["radius_regions"] = m_nRadiusRegions;
	stats["unified_cold_radius"] = m_pTerrain ? m_pTerrain->getStreamingColdRadius() : 0;
	stats["active_packages"] = ( int ) ( m_active.size() );
	stats["max_active_packages"] = TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT;
	stats["active_receivers"] = active_receivers;
	stats["pending_packages"] = m_nPendingPackagesLastTick;
	stats["capacity_deferred"] = m_nCapacityDeferredLastTick;
	stats["compatibility_failures_last_tick"] = m_nCompatibilityFailuresLastTick;
	stats["compatibility_failures_total"] = ( int64_t ) ( m_nCompatibilityFailuresTotal );
	stats["lightmap_textures"] = lightmap_textures;
	stats["shadowmask_textures"] = shadowmask_textures;
	stats["estimated_gpu_bytes"] = estimated_gpu_bytes;
	stats["residency_budget_mb"] = m_nResidencyBudgetMb;
	stats["effective_residency_budget_mb"] = getEffectiveResidencyBudgetMb();
	stats["loads_last_tick"] = m_nLoadsLastTick;
	stats["unloads_last_tick"] = m_nUnloadsLastTick;
	stats["refreshes_last_tick"] = m_nRefreshesLastTick;
	stats["resource_requests_last_tick"] = m_nResourceRequestsLastTick;
	stats["resource_failures_last_tick"] = m_nResourceFailuresLastTick;
	stats["loads_total"] = ( int64_t ) ( m_nLoadsTotal );
	stats["unloads_total"] = ( int64_t ) ( m_nUnloadsTotal );
	stats["refreshes_total"] = ( int64_t ) ( m_nRefreshesTotal );
	stats["resource_requests_total"] = ( int64_t ) ( m_nResourceRequestsTotal );
	stats["resource_failures_total"] = ( int64_t ) ( m_nResourceFailuresTotal );
	return stats;
}

void T3DLightmapStreamer::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "initialize", "terrain" ), &T3DLightmapStreamer::initialize );
	ClassDB::bind_method( D_METHOD( "destroy" ), &T3DLightmapStreamer::destroy );
	ClassDB::bind_method( D_METHOD( "update_residency", "target_position", "force" ), &T3DLightmapStreamer::updateResidency, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "suspend" ), &T3DLightmapStreamer::suspend );
	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &T3DLightmapStreamer::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &T3DLightmapStreamer::isEnabled );
	ClassDB::bind_method( D_METHOD( "set_radius_regions", "radius" ), &T3DLightmapStreamer::setRadiusRegions );
	ClassDB::bind_method( D_METHOD( "get_radius_regions" ), &T3DLightmapStreamer::getRadiusRegions );
	ClassDB::bind_method( D_METHOD( "set_load_budget", "budget" ), &T3DLightmapStreamer::setLoadBudget );
	ClassDB::bind_method( D_METHOD( "get_load_budget" ), &T3DLightmapStreamer::getLoadBudget );
	ClassDB::bind_method( D_METHOD( "set_unload_budget", "budget" ), &T3DLightmapStreamer::setUnloadBudget );
	ClassDB::bind_method( D_METHOD( "get_unload_budget" ), &T3DLightmapStreamer::getUnloadBudget );
	ClassDB::bind_method( D_METHOD( "set_residency_budget_mb", "budget_mb" ), &T3DLightmapStreamer::setResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_residency_budget_mb" ), &T3DLightmapStreamer::getResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_active_package_count" ), &T3DLightmapStreamer::getActivePackageCount );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &T3DLightmapStreamer::getStats );

	ADD_GROUP( "Residency", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "enabled" ), "set_enabled", "is_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "radius_regions", PROPERTY_HINT_RANGE, "0,16,1" ), "set_radius_regions", "get_radius_regions" );

	ADD_GROUP( "Work Budgets", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "load_budget", PROPERTY_HINT_RANGE, "0,64,1" ), "set_load_budget", "get_load_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "unload_budget", PROPERTY_HINT_RANGE, "0,64,1" ), "set_unload_budget", "get_unload_budget" );

	ADD_GROUP( "Runtime Status", "" );
	// Runtime counters remain available through get_stats(); there are no editable status fields.

}
