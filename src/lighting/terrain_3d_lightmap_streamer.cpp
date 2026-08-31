// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "lighting/terrain_3d_lightmap_streamer.h"

#include "Terrain3DConstants.h"
#include "data/terrain_3d_data.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "eastl_config.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "terrain_3d.h"

#include <EASTL/bitset.h>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/rect2.hpp>

// Godot renders at most eight LightmapGI instances reliably in one camera view.
// Per-region packages therefore remain independent on disk, while residency is
// prioritized around the streaming center and capped at that engine limit.

typedef struct t3dLightmapCandidate_s {
	Ref<Terrain3DRegionLighting> m_ackage;
	int64_t m_nDistance = INT64_MAX;
	Vector2i m_location = s_v2iMax;
} t3dLightmapCandidate_t;

static bool t3dLightmapCandidateLess( const t3dLightmapCandidate_t &rA, const t3dLightmapCandidate_t &rB ) {
	if ( rA.m_nDistance != rB.m_nDistance ) {
		return rA.m_nDistance < rB.m_nDistance;
	}
	if ( rA.m_location.y != rB.m_location.y ) {
		return rA.m_location.y < rB.m_location.y;
	}
	return rA.m_location.x < rB.m_location.x;
}

static uint64_t t3dLightmapStreamerHashReceiver( uint64_t nHash, const NodePath &rPath, const uint64_t nObjectId ) {
	const CharString utf8 = String( rPath ).utf8();
	uint64_t hash = nHash;
	for ( int index = 0; index < utf8.length(); ++index ) {
		hash ^= ( uint8_t )( utf8[index] );
		hash *= s_nLightmapFnvPrime;
	}
	uint64_t object_id = nObjectId;
	for ( int byte_index = 0; byte_index < 8; ++byte_index ) {
		hash ^= ( uint8_t )( object_id & 0xffu );
		hash *= s_nLightmapFnvPrime;
		object_id >>= 8u;
	}
	return hash;
}

static Node *t3dLightmapStreamerResolveLightmapReceiver( Terrain3D *pTerrain, const NodePath &rBakedPath ) {
	if ( pTerrain == NULL ) {
		return NULL;
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
		return NULL;
	}
	return pTerrain->get_node_or_null( NodePath( path ) );
}

typedef void ( *t3dLightmapReceiverFn )( void *pContext, const t3dRuntimeLightingReceiverRecord_t &rReceiver );

static bool t3dLightmapStreamerForEachLightmapReceiver(
        Terrain3D *const pTerrain, const Ref<Terrain3DRegionLighting> &rPackage, t3dLightmapReceiverFn pCallback, void *const pContext ) {
	if ( pTerrain == NULL || rPackage.is_null() ) {
		return false;
	}
	if ( !pTerrain->isEditorAuthoringMode() ) {
		Terrain3DData *const pData = pTerrain->getData();
		PackedByteArray blob;
		if ( pData == NULL || !pData->loadCookedLightingReceiversNow( rPackage->getClusterLocation(), blob ) ) {
			return rPackage->getReceiverCount() == 0;
		}
		t3dLightingReceiverCursor_t cursor;
		if ( !Terrain3DRuntimeBakeCodec::beginLightingReceivers( blob, cursor ) ) {
			return false;
		}
		for ( uint32_t i = 0u; i < cursor.m_nCount; ++i ) {
			t3dRuntimeLightingReceiverRecord_t receiver;
			if ( !Terrain3DRuntimeBakeCodec::nextLightingReceiver( cursor, receiver ) ) {
				return false;
			}
			pCallback( pContext, receiver );
		}
		return true;
	}
	const Array &raw = rPackage->getBakedUserDataInternal();
	if ( raw.is_empty() ) {
		return rPackage->getReceiverCount() == 0;
	}
	if ( ( raw.size() % 4 ) != 0 ) {
		return false;
	}
	for ( int index = 0; index < raw.size(); index += 4 ) {
		t3dRuntimeLightingReceiverRecord_t receiver;
		receiver.m_path = raw[index];
		receiver.m_uvScale = raw[index + 1];
		receiver.m_iSliceIndex = ( int )raw[index + 2];
		receiver.m_nSubInstance = ( int )raw[index + 3];
		pCallback( pContext, receiver );
	}
	return true;
}

typedef struct t3dLightmapReceiverStateContext_s {
	Terrain3D *m_pTerrain;
	uint64_t *m_pSignature;
	int *m_pReceiverCount;
	Ref<LightmapGIData> *m_pRuntimeData;
} t3dLightmapReceiverStateContext_t;

static void t3dLightmapCollectReceiverState( void *const pContext, const t3dRuntimeLightingReceiverRecord_t &rReceiver ) {
	t3dLightmapReceiverStateContext_t *const pState = ( t3dLightmapReceiverStateContext_t * )( pContext );
	Node *const pReceiver = t3dLightmapStreamerResolveLightmapReceiver( pState->m_pTerrain, rReceiver.m_path );
	if ( pReceiver == NULL || !pReceiver->is_inside_tree() ) {
		return;
	}
	*pState->m_pSignature = t3dLightmapStreamerHashReceiver( *pState->m_pSignature, rReceiver.m_path, pReceiver->get_instance_id() );
	++( *pState->m_pReceiverCount );
}

static void t3dLightmapBuildReceiverState( void *const pContext, const t3dRuntimeLightingReceiverRecord_t &rReceiver ) {
	t3dLightmapReceiverStateContext_t *const pState = ( t3dLightmapReceiverStateContext_t * )( pContext );
	Node *const pReceiver = t3dLightmapStreamerResolveLightmapReceiver( pState->m_pTerrain, rReceiver.m_path );
	if ( pReceiver == NULL || !pReceiver->is_inside_tree() ) {
		return;
	}
	( *pState->m_pRuntimeData )->add_user( rReceiver.m_path, rReceiver.m_uvScale, rReceiver.m_iSliceIndex, rReceiver.m_nSubInstance );
	*pState->m_pSignature = t3dLightmapStreamerHashReceiver( *pState->m_pSignature, rReceiver.m_path, pReceiver->get_instance_id() );
	++( *pState->m_pReceiverCount );
}

Terrain3DLightmapStreamer::~Terrain3DLightmapStreamer( void ) {
	destroy();
}

void Terrain3DLightmapStreamer::initialize( Terrain3D *const pTerrain ) {
	if ( m_pTerrain == pTerrain ) {
		return;
	}
	destroy();
	m_pTerrain = pTerrain;
	for ( int i = 0; i < s_nLightmapActiveInstanceLimit; ++i ) {
		m_freeNodes.push_back( memnew( LightmapGI ) );
	}
	m_center = s_v2iMax;
	m_nNodeRuntimeGeneration = 0u;
}

void Terrain3DLightmapStreamer::destroy( void ) {
	while ( !m_activePackages.empty() ) {
		deactivate( m_activePackages.size() - 1u );
	}
	while ( !m_freeNodes.empty() ) {
		LightmapGI *const pNode = m_freeNodes.back();
		m_freeNodes.pop_back();
		memdelete( pNode );
	}
	m_pTerrain = NULL;
	m_center = s_v2iMax;
	m_nNodeRuntimeGeneration = 0u;
	m_bHasPendingActivation = false;
}

LightmapGI *Terrain3DLightmapStreamer::acquireNode( void ) {
	if ( m_freeNodes.empty() ) {
		return NULL;
	}
	LightmapGI *const pNode = m_freeNodes.back();
	m_freeNodes.pop_back();
	return pNode;
}

void Terrain3DLightmapStreamer::releaseNode( LightmapGI *const pNode ) {
	if ( pNode == NULL ) {
		return;
	}
	if ( pNode->get_parent() != NULL ) {
		pNode->get_parent()->remove_child( pNode );
	}
	pNode->set_light_data( Ref<LightmapGIData>() );
	if ( !m_freeNodes.full() ) {
		m_freeNodes.push_back( pNode );
		return;
	}
	ERR_PRINT_ONCE( "Terrain3D lightmap node pool overflowed during release" );
}

void Terrain3DLightmapStreamer::suspend( void ) {
	while ( !m_activePackages.empty() ) {
		deactivate( m_activePackages.size() - 1u );
	}
	m_center = s_v2iMax;
	m_nNodeRuntimeGeneration = 0u;
	m_bHasPendingActivation = false;
}

void Terrain3DLightmapStreamer::setEnabled( const bool bEnabled ) {
	if ( m_bEnabled == bEnabled ) {
		return;
	}
	m_bEnabled = bEnabled;
	if ( !m_bEnabled ) {
		suspend();
	}
}

void Terrain3DLightmapStreamer::setResidencyBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 1024 );
	if ( m_nResidencyBudgetMb == budget_mb ) {
		return;
	}
	m_nResidencyBudgetMb = budget_mb;
	const size_t budget_bytes = ( size_t )( getEffectiveResidencyBudgetMb() ) * 1024u * 1024u;
	while ( !m_activePackages.empty() && getResidentEstimatedBytes() > budget_bytes ) {
		deactivate( m_activePackages.size() - 1u );
	}
	m_center = s_v2iMax;
}

int Terrain3DLightmapStreamer::getEffectiveResidencyBudgetMb( void ) const {
	// The terrain memory-budget coordinator already assigns this subsystem's
	// residency budget. Authoring pins successfully loaded packages, but does not
	// manufacture a second editor-only allowance outside that aggregate budget.
	return MAX( 1, m_nResidencyBudgetMb );
}

void Terrain3DLightmapStreamer::setRadiusRegions( const int nRadius ) {
	const int radius = CLAMP( nRadius, 0, 16 );
	if ( m_nRadiusRegions == radius ) {
		return;
	}
	m_nRadiusRegions = radius;
	m_center = s_v2iMax;
}

t3dActivePackage_t *Terrain3DLightmapStreamer::findActive( const Ref<Terrain3DRegionLighting> &rPackage ) {
	for ( size_t iPackage = 0u; iPackage < ( size_t )( ( m_activePackages ).size() ); iPackage++ ) {
		t3dActivePackage_t &active = ( m_activePackages )[iPackage];
		if ( active.m_ackage == rPackage ) {
			return &active;
		}
	}
	return NULL;
}

const t3dActivePackage_t *Terrain3DLightmapStreamer::findActive( const Ref<Terrain3DRegionLighting> &rPackage ) const {
	for ( size_t iPackage = 0u; iPackage < ( size_t )( ( m_activePackages ).size() ); iPackage++ ) {
		const t3dActivePackage_t &active = ( m_activePackages )[iPackage];
		if ( active.m_ackage == rPackage ) {
			return &active;
		}
	}
	return NULL;
}

t3dSourceLoadResult_t Terrain3DLightmapStreamer::requestSourceData(
        const Ref<Terrain3DRegionLighting> &rPackage, Ref<LightmapGIData> &rData ) {
	rData.unref();
	if ( rPackage.is_null() || !rPackage->hasLightData() ) {
		return SOURCE_LOAD_FAILED;
	}
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( pLoader == NULL ) {
		return SOURCE_LOAD_FAILED;
	}
	const String path = rPackage->getLightDataPath();
	const ResourceLoader::ThreadLoadStatus status = pLoader->load_threaded_get_status( path );
	if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
		const Error error = pLoader->load_threaded_request( path, "LightmapGIData", true, ResourceLoader::CACHE_MODE_IGNORE_DEEP );
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

bool Terrain3DLightmapStreamer::collectReceiverState(
        const Ref<Terrain3DRegionLighting> &rPackage, uint64_t &nSignature, int &nReceiverCount ) const {
	nSignature = s_nLightmapFnvOffset;
	nReceiverCount = 0;
	if ( m_pTerrain == NULL || rPackage.is_null() ) {
		return false;
	}
	t3dLightmapReceiverStateContext_t context;
	context.m_pTerrain = m_pTerrain;
	context.m_pSignature = &nSignature;
	context.m_pReceiverCount = &nReceiverCount;
	context.m_pRuntimeData = NULL;
	const bool decoded = t3dLightmapStreamerForEachLightmapReceiver( m_pTerrain, rPackage, t3dLightmapCollectReceiverState, &context );
	return decoded && ( nReceiverCount > 0 || rPackage->hasProbeData() );
}

bool Terrain3DLightmapStreamer::buildRuntimeData( const Ref<Terrain3DRegionLighting> &rPackage,
        const Ref<LightmapGIData> &rSource,
        Ref<LightmapGIData> &rData,
        uint64_t &nSignature,
        int &nReceiverCount ) const {
	rData.unref();
	nSignature = s_nLightmapFnvOffset;
	nReceiverCount = 0;
	if ( m_pTerrain == NULL || rPackage.is_null() || rSource.is_null() ) {
		return false;
	}
	const Ref<Resource> pDuplicated = rSource->duplicate( false );
	Ref<LightmapGIData> pRuntimeData = pDuplicated;
	if ( pRuntimeData.is_null() ) {
		return false;
	}
	pRuntimeData->clear_users();
	t3dLightmapReceiverStateContext_t context;
	context.m_pTerrain = m_pTerrain;
	context.m_pSignature = &nSignature;
	context.m_pReceiverCount = &nReceiverCount;
	context.m_pRuntimeData = &pRuntimeData;
	const bool decoded = t3dLightmapStreamerForEachLightmapReceiver( m_pTerrain, rPackage, t3dLightmapBuildReceiverState, &context );
	if ( !decoded || ( nReceiverCount == 0 && !rPackage->hasProbeData() ) ) {
		return false;
	}
	rData = pRuntimeData;
	return true;
}

bool Terrain3DLightmapStreamer::isPackageCompatibleWithActive( const Ref<Terrain3DRegionLighting> &rPackage ) const {
	if ( rPackage.is_null() ) {
		return false;
	}
	const String signature = rPackage->getDirectionalLightSignature();
	for ( size_t iPackage = 0u; iPackage < ( size_t )( ( m_activePackages ).size() ); iPackage++ ) {
		const t3dActivePackage_t &active = ( m_activePackages )[iPackage];
		if ( active.m_ackage.is_null() ) {
			continue;
		}
		if ( active.m_ackage->getShadowmaskMode() != rPackage->getShadowmaskMode() ) {
			return false;
		}
		const String active_signature = active.m_ackage->getDirectionalLightSignature();
		if ( !signature.is_empty() && !active_signature.is_empty() && signature != active_signature ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DLightmapStreamer::activate( const Ref<Terrain3DRegionLighting> &rPackage,
        const Ref<LightmapGIData> &rSourceData,
        const Ref<LightmapGIData> &rRuntimeData,
        const uint64_t nSignature,
        const int nReceiverCount ) {
	if ( m_pTerrain == NULL || rPackage.is_null() || rSourceData.is_null() || rRuntimeData.is_null()
	        || ( nReceiverCount <= 0 && !rPackage->hasProbeData() ) ) {
		return false;
	}
	if ( findActive( rPackage ) != NULL ) {
		return true;
	}
	if ( m_activePackages.full() ) {
		return false;
	}
	const size_t incoming_bytes = ( size_t )( MAX<int64_t>( 0, rPackage->getEstimatedGpuBytes() ) );
	const size_t budget_bytes = ( size_t )( getEffectiveResidencyBudgetMb() ) * 1024u * 1024u;
	const size_t resident_bytes = m_nResidentEstimatedBytes;
	if ( incoming_bytes > budget_bytes || resident_bytes > budget_bytes - incoming_bytes ) {
		return false;
	}

	LightmapGI *const pLightmap = acquireNode();
	if ( pLightmap == NULL ) {
		return false;
	}
	const Vector2i cluster = rPackage->getClusterLocation();
	pLightmap->set_name(
	        String( "Terrain3DLightmapGI_" ) + String::num_int64( cluster.x ) + String( "_" ) + String::num_int64( cluster.y ) );
	pLightmap->set_light_data( rRuntimeData );
	pLightmap->set_shadowmask_mode( ( LightmapGIData::ShadowmaskMode )( rPackage->getShadowmaskMode() ) );
	pLightmap->request_ready();
	m_pTerrain->add_child( pLightmap, false, Node::INTERNAL_MODE_BACK );

	t3dActivePackage_t active;
	active.m_ackage = rPackage;
	active.m_sourceData = rSourceData;
	active.m_runtimeData = rRuntimeData;
	active.m_pNode = pLightmap;
	active.m_nReceiverSignature = nSignature;
	active.m_nReceiverCount = nReceiverCount;
	active.m_bDesired = true;
	m_activePackages.push_back( active );
	m_nResidentEstimatedBytes += incoming_bytes;
	++m_nLoadsLastTick;
	++m_nLoadsTotal;
	return true;
}

bool Terrain3DLightmapStreamer::refreshActive(
        t3dActivePackage_t &rActive, const Ref<LightmapGIData> &rRuntimeData, const uint64_t nSignature, const int nReceiverCount ) {
	if ( m_pTerrain == NULL || rActive.m_ackage.is_null() || rRuntimeData.is_null()
	        || ( nReceiverCount <= 0 && !rActive.m_ackage->hasProbeData() ) ) {
		return false;
	}
	if ( rActive.m_nReceiverSignature == nSignature && rActive.m_nReceiverCount == nReceiverCount && rActive.m_pNode != NULL ) {
		return true;
	}
	releaseNode( rActive.m_pNode );
	rActive.m_pNode = NULL;
	LightmapGI *const pLightmap = acquireNode();
	if ( pLightmap == NULL ) {
		return false;
	}
	const Vector2i cluster = rActive.m_ackage->getClusterLocation();
	pLightmap->set_name(
	        String( "Terrain3DLightmapGI_" ) + String::num_int64( cluster.x ) + String( "_" ) + String::num_int64( cluster.y ) );
	pLightmap->set_light_data( rRuntimeData );
	pLightmap->set_shadowmask_mode( ( LightmapGIData::ShadowmaskMode )( rActive.m_ackage->getShadowmaskMode() ) );
	pLightmap->request_ready();
	m_pTerrain->add_child( pLightmap, false, Node::INTERNAL_MODE_BACK );
	rActive.m_runtimeData = rRuntimeData;
	rActive.m_pNode = pLightmap;
	rActive.m_nReceiverSignature = nSignature;
	rActive.m_nReceiverCount = nReceiverCount;
	++m_nRefreshesLastTick;
	++m_nRefreshesTotal;
	return true;
}

void Terrain3DLightmapStreamer::deactivate( const size_t iIndex ) {
	if ( iIndex >= m_activePackages.size() ) {
		return;
	}
	t3dActivePackage_t &active = m_activePackages[iIndex];
	if ( active.m_pNode != NULL ) {
		releaseNode( active.m_pNode );
		active.m_pNode = NULL;
	}
	const size_t removed_bytes = active.m_ackage.is_valid() ? ( size_t )( MAX<int64_t>( 0, active.m_ackage->getEstimatedGpuBytes() ) ) : 0u;
	active.m_runtimeData.unref();
	active.m_sourceData.unref();
	active.m_ackage.unref();
	for ( size_t i = iIndex + 1u; i < m_activePackages.size(); ++i ) {
		m_activePackages[i - 1u] = m_activePackages[i];
	}
	m_activePackages.pop_back();
	m_nResidentEstimatedBytes = removed_bytes <= m_nResidentEstimatedBytes ? m_nResidentEstimatedBytes - removed_bytes : 0u;
	++m_nUnloadsLastTick;
	++m_nUnloadsTotal;
}

void Terrain3DLightmapStreamer::markDesired( const Vector2i &rCenter ) {
	m_bHasPendingActivation = false;
	m_nPendingPackagesLastTick = 0;
	m_nCapacityDeferredLastTick = 0;
	for ( size_t iPackage = 0u; iPackage < ( size_t )( ( m_activePackages ).size() ); iPackage++ ) {
		t3dActivePackage_t &active = ( m_activePackages )[iPackage];
		active.m_bDesired = false;
	}
	if ( m_pTerrain == NULL || m_pTerrain->getData() == NULL ) {
		return;
	}

	Terrain3DData *const pData = m_pTerrain->getData();
	Terrain3DNodeStreamer *const pNodeStreamer = m_pTerrain->getNodeStreamer();
	const bool authoring_full_residency = m_pTerrain->isEditorAuthoringMode();
	const Vector2i *pLocations = pData->getRegionSlotsInternal();
	Terrain3DRegion *const *ppRegions = pData->getRegionPtrMapInternal();
	const int slot_count = pData->getRegionSlotCountInternal();
	int remaining_loads = authoring_full_residency ? MAX( 1, m_nLoadBudget ) : m_nLoadBudget;

	eastl::fixed_vector<t3dLightmapCandidate_t, s_nLightmapActiveInstanceLimit, false> candidates;
	int unique_count = 0;
	eastl::bitset<s_nMaxRegionSlots> seen_clusters;
	for ( int slot = 0; slot < slot_count; ++slot ) {
		const Vector2i location = pLocations[slot];
		if ( location == s_v2iMax ) {
			continue;
		}
		const t3dRegionStreamingState_t state = m_pTerrain->getRegionStreamingStateInternal( location );
		if ( !( authoring_full_residency || state >= TERRAIN3D_REGION_STREAMING_COLD
		             || ( pNodeStreamer != NULL && pNodeStreamer->isRegionDesired( location ) ) ) ) {
			continue;
		}
		Terrain3DRegion *const pRegion = ppRegions[slot];
		if ( pRegion == NULL || !pRegion->hasLighting() ) {
			continue;
		}
		const Ref<Terrain3DRegionLighting> package = pRegion->getLighting();
		if ( package.is_null() ) {
			continue;
		}
		const Vector2i package_location = package->getClusterLocation();
		const int package_index = Terrain3DRegionRepository::getRegionMapIndex( package_location );
		if ( package_index >= 0 ) {
			if ( seen_clusters.test( ( size_t )package_index ) ) {
				continue;
			}
			seen_clusters.set( ( size_t )package_index );
		} else {
			bool duplicate = false;
			for ( size_t iCandidate = 0u; iCandidate < ( size_t )( ( candidates ).size() ); iCandidate++ ) {
				const t3dLightmapCandidate_t &candidate = ( candidates )[iCandidate];
				if ( candidate.m_ackage == package ) {
					duplicate = true;
					break;
				}
			}
			if ( duplicate ) {
				continue;
			}
		}
		++unique_count;
		if ( unique_count > s_nLightmapActiveInstanceLimit ) {
			++m_nCapacityDeferredLastTick;
		}
		const Vector2i delta = package_location - rCenter;
		t3dLightmapCandidate_t candidate;
		candidate.m_ackage = package;
		candidate.m_location = package_location;
		candidate.m_nDistance = ( int64_t )delta.x * delta.x + ( int64_t )delta.y * delta.y;
		int insert_at = ( int )( candidates.size() );
		while ( insert_at > 0 && t3dLightmapCandidateLess( candidate, candidates[( size_t )( insert_at - 1 )] ) ) {
			--insert_at;
		}
		if ( candidates.size() < ( size_t )( s_nLightmapActiveInstanceLimit ) ) {
			candidates.push_back( candidate );
			for ( int i = ( int )( candidates.size() ) - 1; i > insert_at; --i ) {
				candidates[( size_t )( i )] = candidates[( size_t )( i - 1 )];
			}
			candidates[( size_t )( insert_at )] = candidate;
		} else if ( insert_at < s_nLightmapActiveInstanceLimit ) {
			for ( int i = s_nLightmapActiveInstanceLimit - 1; i > insert_at; --i ) {
				candidates[( size_t )( i )] = candidates[( size_t )( i - 1 )];
			}
			candidates[( size_t )( insert_at )] = candidate;
		}
	}

	for ( size_t iCandidate = 0u; iCandidate < ( size_t )( ( candidates ).size() ); iCandidate++ ) {
		const t3dLightmapCandidate_t &candidate = ( candidates )[iCandidate];
		const Ref<Terrain3DRegionLighting> package = candidate.m_ackage;
		t3dActivePackage_t *pActive = findActive( package );
		Ref<LightmapGIData> source_data;
		if ( pActive != NULL ) {
			source_data = pActive->m_sourceData;
		} else {
			const t3dSourceLoadResult_t load_result = requestSourceData( package, source_data );
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
		if ( pActive != NULL ) {
			const bool bHasActiveReceivers = collectReceiverState( package, receiver_signature, receiver_count );
			if ( bHasActiveReceivers && pActive->m_pNode != NULL && pActive->m_nReceiverSignature == receiver_signature
			        && pActive->m_nReceiverCount == receiver_count ) {
				pActive->m_bDesired = true;
				continue;
			}
		}
		const bool bHasActiveReceivers = buildRuntimeData( package, source_data, pRuntimeData, receiver_signature, receiver_count );
		if ( pActive != NULL ) {
			pActive->m_bDesired = bHasActiveReceivers;
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
			ERR_PRINT_ONCE( "Terrain3D regional lightmap packages with incompatible shadowmask modes or "
			                "directional-light signatures cannot be active together" );
			continue;
		}
		if ( m_activePackages.full() ) {
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

void Terrain3DLightmapStreamer::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
	m_nLoadsLastTick = 0;
	m_nUnloadsLastTick = 0;
	m_nRefreshesLastTick = 0;
	m_nResourceRequestsLastTick = 0;
	m_nResourceFailuresLastTick = 0;
	m_nCompatibilityFailuresLastTick = 0;
	if ( !m_bEnabled || m_pTerrain == NULL || m_pTerrain->getData() == NULL ) {
		return;
	}
	Terrain3DNodeStreamer *const pNodeStreamer = m_pTerrain->getNodeStreamer();
	const uint64_t node_generation = pNodeStreamer != NULL ? pNodeStreamer->getRuntimeGeneration() : 0u;
	const bool bReceiverSetChanged = node_generation != m_nNodeRuntimeGeneration;
	const Vector2i center = m_pTerrain->getData()->getRegionLocation( rTargetPosition );
	if ( !bForce && center == m_center && !m_bHasPendingActivation && !bReceiverSetChanged ) {
		return;
	}
	m_center = center;
	m_nNodeRuntimeGeneration = node_generation;
	markDesired( center );

	int remaining_unloads = m_pTerrain->isEditorAuthoringMode() ? MAX( 1, m_nUnloadBudget ) : m_nUnloadBudget;
	for ( size_t index = m_activePackages.size(); index > 0u && remaining_unloads > 0; --index ) {
		const size_t active_index = index - 1u;
		if ( m_activePackages[active_index].m_bDesired ) {
			continue;
		}
		deactivate( active_index );
		--remaining_unloads;
	}
}

Dictionary Terrain3DLightmapStreamer::getStatsFast( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bEnabled;
	stats["radius_regions"] = m_nRadiusRegions;
	stats["unified_cold_radius"] = m_pTerrain ? m_pTerrain->getStreamingColdRadius() : 0;
	stats["active_packages"] = ( int )m_activePackages.size();
	stats["max_active_packages"] = s_nLightmapActiveInstanceLimit;
	stats["pending_packages"] = m_nPendingPackagesLastTick;
	stats["capacity_deferred"] = m_nCapacityDeferredLastTick;
	stats["compatibility_failures_last_tick"] = m_nCompatibilityFailuresLastTick;
	stats["compatibility_failures_total"] = ( int64_t )( m_nCompatibilityFailuresTotal );
	stats["estimated_gpu_bytes"] = ( int64_t )( m_nResidentEstimatedBytes );
	stats["residency_budget_mb"] = m_nResidencyBudgetMb;
	stats["effective_residency_budget_mb"] = getEffectiveResidencyBudgetMb();
	stats["loads_last_tick"] = m_nLoadsLastTick;
	stats["unloads_last_tick"] = m_nUnloadsLastTick;
	stats["refreshes_last_tick"] = m_nRefreshesLastTick;
	stats["resource_requests_last_tick"] = m_nResourceRequestsLastTick;
	stats["resource_failures_last_tick"] = m_nResourceFailuresLastTick;
	stats["loads_total"] = ( int64_t )( m_nLoadsTotal );
	stats["unloads_total"] = ( int64_t )( m_nUnloadsTotal );
	stats["refreshes_total"] = ( int64_t )( m_nRefreshesTotal );
	stats["resource_requests_total"] = ( int64_t )( m_nResourceRequestsTotal );
	stats["resource_failures_total"] = ( int64_t )( m_nResourceFailuresTotal );
	return stats;
}

Dictionary Terrain3DLightmapStreamer::getStats( void ) const {
	Dictionary stats;
	int lightmap_textures = 0;
	int shadowmask_textures = 0;
	int active_receivers = 0;
	int64_t estimated_gpu_bytes = 0;
	for ( size_t iPackage = 0u; iPackage < ( size_t )( ( m_activePackages ).size() ); iPackage++ ) {
		const t3dActivePackage_t &active = ( m_activePackages )[iPackage];
		if ( active.m_ackage.is_null() ) {
			continue;
		}
		lightmap_textures += active.m_ackage->getLightmapTextureCount();
		shadowmask_textures += active.m_ackage->getShadowmaskTextureCount();
		active_receivers += active.m_nReceiverCount;
		estimated_gpu_bytes += active.m_ackage->getEstimatedGpuBytes();
	}
	stats["enabled"] = m_bEnabled;
	stats["radius_regions"] = m_nRadiusRegions;
	stats["unified_cold_radius"] = m_pTerrain ? m_pTerrain->getStreamingColdRadius() : 0;
	stats["active_packages"] = ( int )m_activePackages.size();
	stats["max_active_packages"] = s_nLightmapActiveInstanceLimit;
	stats["active_receivers"] = active_receivers;
	stats["pending_packages"] = m_nPendingPackagesLastTick;
	stats["capacity_deferred"] = m_nCapacityDeferredLastTick;
	stats["compatibility_failures_last_tick"] = m_nCompatibilityFailuresLastTick;
	stats["compatibility_failures_total"] = ( int64_t )( m_nCompatibilityFailuresTotal );
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
	stats["loads_total"] = ( int64_t )( m_nLoadsTotal );
	stats["unloads_total"] = ( int64_t )( m_nUnloadsTotal );
	stats["refreshes_total"] = ( int64_t )( m_nRefreshesTotal );
	stats["resource_requests_total"] = ( int64_t )( m_nResourceRequestsTotal );
	stats["resource_failures_total"] = ( int64_t )( m_nResourceFailuresTotal );
	return stats;
}

void Terrain3DLightmapStreamer::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "initialize", "terrain" ), &Terrain3DLightmapStreamer::initialize );
	ClassDB::bind_method( D_METHOD( "destroy" ), &Terrain3DLightmapStreamer::destroy );
	ClassDB::bind_method(
	        D_METHOD( "update_residency", "target_position", "force" ), &Terrain3DLightmapStreamer::updateResidency, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "suspend" ), &Terrain3DLightmapStreamer::suspend );
	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &Terrain3DLightmapStreamer::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &Terrain3DLightmapStreamer::isEnabled );
	ClassDB::bind_method( D_METHOD( "set_radius_regions", "radius" ), &Terrain3DLightmapStreamer::setRadiusRegions );
	ClassDB::bind_method( D_METHOD( "get_radius_regions" ), &Terrain3DLightmapStreamer::getRadiusRegions );
	ClassDB::bind_method( D_METHOD( "set_load_budget", "budget" ), &Terrain3DLightmapStreamer::setLoadBudget );
	ClassDB::bind_method( D_METHOD( "get_load_budget" ), &Terrain3DLightmapStreamer::getLoadBudget );
	ClassDB::bind_method( D_METHOD( "set_unload_budget", "budget" ), &Terrain3DLightmapStreamer::setUnloadBudget );
	ClassDB::bind_method( D_METHOD( "get_unload_budget" ), &Terrain3DLightmapStreamer::getUnloadBudget );
	ClassDB::bind_method( D_METHOD( "set_residency_budget_mb", "budget_mb" ), &Terrain3DLightmapStreamer::setResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_residency_budget_mb" ), &Terrain3DLightmapStreamer::getResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_active_package_count" ), &Terrain3DLightmapStreamer::getActivePackageCount );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &Terrain3DLightmapStreamer::getStats );

	ADD_GROUP( "Residency", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "enabled" ), "set_enabled", "is_enabled" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "radius_regions", PROPERTY_HINT_RANGE, "0,16,1" ), "set_radius_regions", "get_radius_regions" );

	ADD_GROUP( "Work Budgets", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "load_budget", PROPERTY_HINT_RANGE, "0,64,1" ), "set_load_budget", "get_load_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "unload_budget", PROPERTY_HINT_RANGE, "0,64,1" ), "set_unload_budget", "get_unload_budget" );

	ADD_GROUP( "Runtime Status", "" );
	// Runtime counters remain available through getStats(); status fields are read-only.
}
