// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_numeric.h"
#include "core/utility/terrain_3d_generation.h"
#include "Terrain3DTypes.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_record.h"

#include "data/model/terrain_3d_data.h"
#include "eastl_config.h"
#include "terrain_3d.h"

#include <EASTL/sort.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

// t3dIndexedNodePolicy_s centralized in Terrain3DTypes.h.

static bool t3dIndexedNodePolicyLess( const t3dIndexedNodePolicy_t& rA, const t3dIndexedNodePolicy_t& rB )
{
	return rA.m_nRegion < rB.m_nRegion || ( rA.m_nRegion == rB.m_nRegion && rA.m_policy.m_nSpawnId < rB.m_policy.m_nSpawnId );
}

static const Terrain3DRenderingMaterial *t3dResolveHlodRenderingMaterial( const void *const pContext, const uint32_t nSurface ) {
	const Terrain3DHLODPackage *const pPackage = ( const Terrain3DHLODPackage *const )( pContext );
	return pPackage ? pPackage->getRenderingMaterialPtr( nSurface ) : NULL;
}

int Terrain3DHLODStreamer::regionIndex( const Vector2i& rRegion )
{
	return Terrain3DData::getRegionMapIndex( rRegion );
}
Vector2i Terrain3DHLODStreamer::regionFromIndex( const int index ) {
	const int size = T3D_REGION_MAP_SIZE;
	return Vector2i( index % size - size / 2, index / size - size / 2 );
}

void Terrain3DHLODStreamer::markRdPublicationDirty( void ) {
	m_nRdPublicationGeneration = t3dNextGeneration( m_nRdPublicationGeneration );
}

uint64_t Terrain3DHLODStreamer::rdMeshSourceKey( const int nEntryIndex ) {
	return t3dRdHlodMeshSourceKey( ( uint32_t )( nEntryIndex ) );
}

static AABB t3dTransformHlodBounds( const Transform3D &rTransform, const AABB &rBounds ) {
	const Vector3 minimum = rBounds.position;
	const Vector3 maximum = rBounds.get_end();
	Vector3 first = rTransform.xform( minimum );
	Vector3 outMin = first;
	Vector3 outMax = first;
	for ( int corner = 1; corner < 8; ++corner ) {
		const Vector3 local( ( corner & 1 ) != 0 ? maximum.x : minimum.x,
		        ( corner & 2 ) != 0 ? maximum.y : minimum.y,
		        ( corner & 4 ) != 0 ? maximum.z : minimum.z );
		const Vector3 world = rTransform.xform( local );
		outMin.x = MIN( outMin.x, world.x );
		outMin.y = MIN( outMin.y, world.y );
		outMin.z = MIN( outMin.z, world.z );
		outMax.x = MAX( outMax.x, world.x );
		outMax.y = MAX( outMax.y, world.y );
		outMax.z = MAX( outMax.z, world.z );
	}
	return AABB( outMin, outMax - outMin );
}

bool Terrain3DHLODStreamer::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_entries.set_allocator( allocator );
	m_roots.set_allocator( allocator );
	m_activeEntries.set_allocator( allocator );
	m_loadingEntries.set_allocator( allocator );
	m_retiringEntries.set_allocator( allocator );
	m_loadCandidates.set_allocator( allocator );
	m_instancerMinLevels.set_allocator( allocator );
	m_nodePolicies.set_allocator( allocator );
	return true;
}

void Terrain3DHLODStreamer::initialize( Terrain3DOwner *const pTerrain ) {
	if ( m_pTerrain == pTerrain ) {
		return;
	}
	destroy();
	m_pTerrain = pTerrain;
	reloadManifest();
}
void Terrain3DHLODStreamer::destroy( void ) {
	clearRuntime( true, true );
	m_pTerrain = NULL;
	m_manifest.unref();
}
void Terrain3DHLODStreamer::suspend( void ) {
	uint8_t old[T3D_REGION_SLOTS];
	memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	while ( !m_activeEntries.empty() ) {
		deactivate( m_activeEntries.back() );
	}
	while ( !m_loadingEntries.empty() ) {
		deactivate( m_loadingEntries.back() );
	}
	m_retiringEntries.clear();
	m_loadCandidates.clear();
	beginSelectionGeneration();
	memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	memset( m_szRegionResidentRepresentationLevel, 0, sizeof( m_szRegionResidentRepresentationLevel ) );
	memset( m_szRegionRetiringRepresentationLevel, 0, sizeof( m_szRegionRetiringRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	if ( m_pTerrain ) {
		notifyRepresentationChanges( old );
	}
}
void Terrain3DHLODStreamer::clearRuntime( const bool bNotify, const bool bReleaseStorage ) {
	uint8_t old[T3D_REGION_SLOTS];
	memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		deactivate( i, true );
	}
	if ( bReleaseStorage ) {
		t3dVectorRelease( m_entries );
		t3dVectorRelease( m_roots );
		t3dVectorRelease( m_activeEntries );
		t3dVectorRelease( m_loadingEntries );
		t3dVectorRelease( m_retiringEntries );
		t3dVectorRelease( m_loadCandidates );
		t3dVectorRelease( m_instancerMinLevels );
		t3dVectorRelease( m_nodePolicies );
	} else {
		m_entries.clear();
		m_roots.clear();
		m_activeEntries.clear();
		m_loadingEntries.clear();
		m_retiringEntries.clear();
		m_loadCandidates.clear();
		m_instancerMinLevels.clear();
		m_nodePolicies.clear();
	}
	eastl::fill_n( m_nodePolicyOffsets, T3D_REGION_SLOTS + 1u, 0u );
	memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	memset( m_szRegionResidentRepresentationLevel, 0, sizeof( m_szRegionResidentRepresentationLevel ) );
	memset( m_szRegionRetiringRepresentationLevel, 0, sizeof( m_szRegionRetiringRepresentationLevel ) );
	m_managedRegions.reset();
	m_detailRequiredRegions.reset();
	m_nResidentEstimatedBytes = 0;
	m_nReservedBytes = 0u;
	m_nLoadCandidateCursor = 0u;
	m_nDesiredGeneration = 1u;
	m_nTransitionEntriesExaminedLastTick = 0u;
	m_bHasSelectionContext = false;
	m_bSelectionDirty = true;
	if ( bNotify && m_pTerrain ) {
		notifyRepresentationChanges( old );
	}
}

int Terrain3DHLODStreamer::findEntry( const int level, const Vector2i &rCluster ) const {
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		if ( m_entries[( size_t )i].m_nLevel == level && m_entries[( size_t )i].getCluster() == rCluster ) {
			return i;
		}
	}
	return -1;
}
bool Terrain3DHLODStreamer::buildHierarchy( void ) {
	m_roots.clear();
	if ( !t3dContainerReserveCold( m_roots, m_entries.size() ) ) {
		return false;
	}
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
		t3dEntry_t &entry = ( m_entries )[iEntry];
		entry.m_nParent = -1;
		for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( entry.m_szChildren ); iChild++ ) {
			int &child = ( entry.m_szChildren )[iChild];
			child = -1;
		}
	}
	for ( int i = 0; i < ( int )m_entries.size(); ++i ) {
		t3dEntry_t &entry = m_entries[( size_t )i];
		const Vector2i parentCluster( t3dDivideFloor( entry.getCluster().x, 2 ), t3dDivideFloor( entry.getCluster().y, 2 ) );
		const int parent = findEntry( entry.m_nLevel + 1, parentCluster );
		entry.m_nParent = parent;
		if ( parent < 0 ) {
			if ( !t3dVectorPushBackWithinCapacity( m_roots, i ) ) {
				return false;
			}
			continue;
		}
		const int localX = entry.getCluster().x - parentCluster.x * 2;
		const int localY = entry.getCluster().y - parentCluster.y * 2;
		const int child = CLAMP( localY, 0, 1 ) * 2 + CLAMP( localX, 0, 1 );
		m_entries[( size_t )parent].m_szChildren[child] = i;
	}
	return true;
}

bool Terrain3DHLODStreamer::compileLegacyNodePolicies( const Ref<Terrain3DHLODManifest> &rManifest ) {
	m_nodePolicies.clear();
	eastl::fill_n( m_nodePolicyOffsets, T3D_REGION_SLOTS + 1u, 0u );
	const PackedInt32Array nodeRegions = rManifest->getNodeRegionIndices();
	const PackedInt64Array nodeIds = rManifest->getNodeSpawnIds();
	const PackedByteArray nodeLevels = rManifest->getNodeMinLevels();
	const int nodeCount = MIN( nodeRegions.size(), MIN( nodeIds.size(), nodeLevels.size() ) );
	eastl::vector<t3dIndexedNodePolicy_t, Terrain3DEastlAllocator> policies;
	policies.reserve( ( size_t )nodeCount );
	for ( int i = 0; i < nodeCount; ++i ) {
		const int region = nodeRegions[i];
		if ( region < 0 || region >= T3D_REGION_SLOTS || nodeLevels[i] == 0 ) {
			continue;
		}
		policies.push_back( { region, { nodeIds[i], nodeLevels[i] } } );
	}
	eastl::sort( policies.begin(), policies.end(), t3dIndexedNodePolicyLess );
	if ( !t3dContainerReserveCold( m_nodePolicies, policies.size() ) ) {
		return false;
	}
	for ( size_t iPolicy = 0u; iPolicy < ( size_t )( ( policies ).size() ); iPolicy++ ) {
		const t3dIndexedNodePolicy_t &indexed = ( policies )[iPolicy];
		++m_nodePolicyOffsets[( size_t )indexed.m_nRegion + 1u];
		if ( !t3dVectorPushBackWithinCapacity( m_nodePolicies, indexed.m_policy ) ) {
			return false;
		}
	}
	for ( int region = 0; region < T3D_REGION_SLOTS; ++region ) {
		m_nodePolicyOffsets[( size_t )region + 1u] += m_nodePolicyOffsets[( size_t )region];
	}
	return true;
}

bool Terrain3DHLODStreamer::compilePackedManifest( const Ref<Terrain3DHLODManifest> &rManifest ) {
	const PackedByteArray blob = rManifest->getRuntimeBlob();
	if ( blob.is_empty() ) {
		return false;
	}
	t3dHLODRuntimeData_t runtime;
	if ( !Terrain3DHLODRuntimeCodec::decode( blob, runtime ) ) {
		return false;
	}
	if ( !t3dContainerReserveCold( m_entries, runtime.m_entries.size() ) ) {
		return false;
	}
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( runtime.m_entries ).size() ); iEntry++ ) {
		const t3dHLODEntryDescriptor_t &source = ( runtime.m_entries )[iEntry];
		t3dEntry_t entry;
		( t3dHLODEntryDescriptor_t & )( entry  ) = source;
		if ( !t3dVectorPushBackWithinCapacity( m_entries, entry ) ) {
			return false;
		}
		for ( int z = entry.getRegionMin().y; z <= entry.getRegionMax().y; ++z ) {
			for ( int x = entry.getRegionMin().x; x <= entry.getRegionMax().x; ++x ) {
				const int index = regionIndex( Vector2i( x, z ) );
				if ( index >= 0 ) {
					m_managedRegions.set( ( size_t )index );
				}
			}
		}
	}
	m_roots = eastl::move( runtime.m_roots );
	m_instancerMinLevels = eastl::move( runtime.m_instancerMinLevels );
	if ( !t3dContainerReserveCold( m_nodePolicies, runtime.m_nodePolicies.size() ) ) {
		return false;
	}
	for ( size_t iPolicy = 0u; iPolicy < ( size_t )( ( runtime.m_nodePolicies ).size() ); iPolicy++ ) {
		const t3dHLODRuntimeNodePolicyRecord_t &policy = ( runtime.m_nodePolicies )[iPolicy];
		if ( !t3dVectorPushBackWithinCapacity( m_nodePolicies, t3dNodePolicy_t{ policy.m_nSpawnId, policy.m_nMinLevel } ) ) {
			return false;
		}
	}
	eastl::copy_n( runtime.m_szNodePolicyOffsets, T3D_REGION_SLOTS + 1, m_nodePolicyOffsets );
	m_manifest = rManifest;
	return true;
}

bool Terrain3DHLODStreamer::compileManifest( const Ref<Terrain3DHLODManifest> &rManifest ) {
	++m_nColdRebuildCount;
	if ( rManifest.is_null() || !m_pTerrain || rManifest->getFormat() != 3 ) {
		return false;
	}
	if ( compilePackedManifest( rManifest ) ) {
		return prepareTransitionStorageCold();
	}
	m_entries.clear();
	m_roots.clear();
	m_instancerMinLevels.clear();
	m_nodePolicies.clear();
	m_managedRegions.reset();
	m_detailRequiredRegions.reset();
	eastl::fill_n( m_nodePolicyOffsets, T3D_REGION_SLOTS + 1u, 0u );
	if ( !m_pTerrain->isEditorAuthoringMode() ) {
		return false;
	}

	const Array &entries = rManifest->getEntriesInternal();
	if ( !t3dContainerReserveCold( m_entries, ( size_t )entries.size() ) ) {
		return false;
	}
	for ( int i = 0; i < entries.size(); ++i ) {
		t3dEntry_t entry;
		if ( !Terrain3DHLODRuntimeCodec::decodeEntryDictionary( entries[i], entry, false ) ) {
			continue;
		}
		if ( !t3dVectorPushBackWithinCapacity( m_entries, entry ) ) {
			return false;
		}
		for ( int z = entry.getRegionMin().y; z <= entry.getRegionMax().y; ++z ) {
			for ( int x = entry.getRegionMin().x; x <= entry.getRegionMax().x; ++x ) {
				const int index = regionIndex( Vector2i( x, z ) );
				if ( index >= 0 ) {
					m_managedRegions.set( ( size_t )index );
				}
			}
		}
	}
	const PackedByteArray meshPolicy = rManifest->getInstancerMinLevels();
	const int expected = T3D_REGION_SLOTS * T3D_MAX_MESH_ASSETS;
	if ( !t3dVectorResizeCold( m_instancerMinLevels, ( size_t )expected ) ) {
		return false;
	}
	for ( int i = 0; i < expected; ++i ) {
		m_instancerMinLevels[( size_t )i] = i < meshPolicy.size() ? meshPolicy[i] : 0;
	}
	if ( !compileLegacyNodePolicies( rManifest ) || !buildHierarchy() || !prepareTransitionStorageCold() ) {
		return false;
	}
	m_manifest = rManifest;
	return true;
}

bool Terrain3DHLODStreamer::reloadManifest( void ) {
	clearRuntime( true );
	m_manifest.unref();
	if ( !m_bEnabled || !m_pTerrain || m_manifestPath.is_empty() || !ResourceLoader::get_singleton()->exists( m_manifestPath ) ) {
		return false;
	}
	const Ref<Resource> resource =
	        ResourceLoader::get_singleton()->load( m_manifestPath, "Terrain3DHLODManifest", ResourceLoader::CACHE_MODE_REPLACE );
	const Ref<Terrain3DHLODManifest> manifest = resource;
	return compileManifest( manifest );
}

bool Terrain3DHLODStreamer::prepareTransitionStorageCold( void ) {
	const size_t capacity = m_entries.size();
	return t3dContainerReserveCold( m_activeEntries, capacity ) && t3dContainerReserveCold( m_loadingEntries, capacity )
	        && t3dContainerReserveCold( m_retiringEntries, capacity ) && t3dContainerReserveCold( m_loadCandidates, capacity );
}

void Terrain3DHLODStreamer::beginSelectionGeneration( void ) {
	++m_nDesiredGeneration;
	if ( m_nDesiredGeneration != 0u ) {
		return;
	}
	m_nDesiredGeneration = 1u;
	for ( size_t i = 0u; i < m_entries.size(); ++i ) {
		m_entries[i].m_nDesiredGeneration = 0u;
	}
}

void Terrain3DHLODStreamer::markDesired( const int index ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	t3dEntry_t &entry = m_entries[( size_t )index];
	if ( entry.m_nDesiredGeneration == m_nDesiredGeneration ) {
		return;
	}
	entry.m_nDesiredGeneration = m_nDesiredGeneration;
	if ( !entry.m_bActive && !entry.m_bLoadFailed && entry.m_iLoadingListPosition < 0 ) {
		t3dVectorPushBackWithinCapacity( m_loadCandidates, index );
	}
}

void Terrain3DHLODStreamer::addActiveEntry( const int index ) {
	t3dEntry_t &entry = m_entries[( size_t )index];
	if ( entry.m_iActiveListPosition >= 0 ) {
		return;
	}
	entry.m_iActiveListPosition = ( int )m_activeEntries.size();
	if ( !t3dVectorPushBackWithinCapacity( m_activeEntries, index ) ) {
		entry.m_iActiveListPosition = -1;
		++m_nCapacityMissCount;
	}
}

void Terrain3DHLODStreamer::removeActiveEntry( const int index ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	t3dEntry_t &entry = m_entries[( size_t )index];
	const int position = entry.m_iActiveListPosition;
	if ( position < 0 || position >= ( int )m_activeEntries.size() ) {
		entry.m_iActiveListPosition = -1;
		return;
	}
	const int moved = m_activeEntries.back();
	m_activeEntries.pop_back();
	if ( position < ( int )m_activeEntries.size() ) {
		m_activeEntries[( size_t )position] = moved;
		m_entries[( size_t )moved].m_iActiveListPosition = position;
	}
	entry.m_iActiveListPosition = -1;
}

void Terrain3DHLODStreamer::addLoadingEntry( const int index ) {
	t3dEntry_t &entry = m_entries[( size_t )index];
	if ( entry.m_iLoadingListPosition >= 0 ) {
		return;
	}
	entry.m_iLoadingListPosition = ( int )m_loadingEntries.size();
	if ( !t3dVectorPushBackWithinCapacity( m_loadingEntries, index ) ) {
		entry.m_iLoadingListPosition = -1;
		++m_nCapacityMissCount;
	}
}

void Terrain3DHLODStreamer::removeLoadingEntry( const int index ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	t3dEntry_t &entry = m_entries[( size_t )index];
	const int position = entry.m_iLoadingListPosition;
	if ( position < 0 || position >= ( int )m_loadingEntries.size() ) {
		entry.m_iLoadingListPosition = -1;
		return;
	}
	const int moved = m_loadingEntries.back();
	m_loadingEntries.pop_back();
	if ( position < ( int )m_loadingEntries.size() ) {
		m_loadingEntries[( size_t )position] = moved;
		m_entries[( size_t )moved].m_iLoadingListPosition = position;
	}
	entry.m_iLoadingListPosition = -1;
}

void Terrain3DHLODStreamer::addRetiringEntry( const int index ) {
	t3dEntry_t &entry = m_entries[( size_t )index];
	if ( entry.m_iRetiringListPosition >= 0 ) {
		return;
	}
	entry.m_bRetiring = true;
	entry.m_nRetireReadyFrames = 0;
	entry.m_iRetiringListPosition = ( int )m_retiringEntries.size();
	if ( !t3dVectorPushBackWithinCapacity( m_retiringEntries, index ) ) {
		entry.m_bRetiring = false;
		entry.m_iRetiringListPosition = -1;
		++m_nCapacityMissCount;
	}
}

void Terrain3DHLODStreamer::removeRetiringEntry( const int index ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	t3dEntry_t &entry = m_entries[( size_t )index];
	const int position = entry.m_iRetiringListPosition;
	if ( position >= 0 && position < ( int )m_retiringEntries.size() ) {
		const int moved = m_retiringEntries.back();
		m_retiringEntries.pop_back();
		if ( position < ( int )m_retiringEntries.size() ) {
			m_retiringEntries[( size_t )position] = moved;
			m_entries[( size_t )moved].m_iRetiringListPosition = position;
		}
	}
	entry.m_iRetiringListPosition = -1;
	entry.m_bRetiring = false;
	entry.m_nRetireReadyFrames = 0;
}

void Terrain3DHLODStreamer::refreshRetiringFromActive( void ) {
	for ( size_t position = 0u; position < m_activeEntries.size(); ++position ) {
		const int index = m_activeEntries[position];
		++m_nTransitionEntriesExaminedLastTick;
		t3dEntry_t &entry = m_entries[( size_t )index];
		if ( isDesired( entry ) ) {
			removeRetiringEntry( index );
		} else {
			addRetiringEntry( index );
		}
	}
}

bool Terrain3DHLODStreamer::packageAcceptable( const t3dEntry_t &rEntry, const Vector3 &rLocalTarget ) const {
	if ( rEntry.m_nLevel > m_nMaxLevel ) {
		return false;
	}
	if ( m_nForcedDebugLevel >= 0 ) {
		return rEntry.m_nLevel == m_nForcedDebugLevel;
	}
	const real_t regionWorld = ( real_t )( T3D_REGION_WORLD_SIZE_METERS );
	const Vector3 clusterOrigin( rEntry.getRegionMin().x * regionWorld, 0.0f, rEntry.getRegionMin().y * regionWorld );
	const AABB worldBounds( clusterOrigin + rEntry.m_localBounds.position, rEntry.m_localBounds.size );
	const Vector3 nearest( CLAMP( rLocalTarget.x, worldBounds.position.x, worldBounds.get_end().x ),
	        CLAMP( rLocalTarget.y, worldBounds.position.y, worldBounds.get_end().y ),
	        CLAMP( rLocalTarget.z, worldBounds.position.z, worldBounds.get_end().z ) );
	const real_t distance = MAX( rLocalTarget.distance_to( nearest ), 1.0f );
	real_t projectionScale = 540.0f;
	real_t projectedError = 0.0f;
	if ( Camera3D *camera = m_pTerrain->getCamera() ) {
		real_t height = 1080.0f;
		if ( camera->get_viewport() ) {
			height = MAX( camera->get_viewport()->get_visible_rect().size.y, 1.0f );
		}
		if ( camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL ) {
			projectedError = rEntry.m_fGeometricError * height / MAX( camera->get_size(), 0.001f );
		} else {
			projectionScale = height / MAX( 2.0f * tan( Math::deg_to_rad( camera->get_fov() ) * 0.5f ), 0.001f );
			projectedError = rEntry.m_fGeometricError * projectionScale / distance;
		}
	} else {
		projectedError = rEntry.m_fGeometricError * projectionScale / distance;
	}
	const real_t threshold =
	        rEntry.m_bActive ? m_fTargetErrorPixels * ( 1.0f + m_fHysteresis ) : m_fTargetErrorPixels * MAX( 0.05f, 1.0f - m_fHysteresis );
	return projectedError <= threshold;
}

void Terrain3DHLODStreamer::selectFallbackChildren( const int index, const Vector3 &rLocalTarget ) {
	const t3dEntry_t &entry = m_entries[( size_t )index];
	// A leaf has no HLOD fallback. Leaving it undesired deliberately publishes
	// detailed residency while the still-active proxy remains visible until the
	// detailed publishers acknowledge readiness. Re-marking an active leaf as
	// desired here would permanently trap the world at HLOD1 after first use.
	for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( entry.m_szChildren ); iChild++ ) {
		const int child = ( entry.m_szChildren )[iChild];
		if ( child >= 0 ) {
			selectRecursive( child, rLocalTarget );
		}
	}
}
void Terrain3DHLODStreamer::clearDesiredDescendants( const int index ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( m_entries[( size_t )index].m_szChildren ); iChild++ ) {
		const int child = ( m_entries[( size_t )index].m_szChildren )[iChild];
		if ( child < 0 ) {
			continue;
		}
		m_entries[( size_t )child].m_nDesiredGeneration = 0u;
		clearDesiredDescendants( child );
	}
}

bool Terrain3DHLODStreamer::hasActiveDescendant( const int index ) const {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return false;
	}
	for ( size_t iChild = 0u; iChild < T3D_ARRAY_COUNT( m_entries[( size_t )index].m_szChildren ); iChild++ ) {
		const int child = ( m_entries[( size_t )index].m_szChildren )[iChild];
		if ( child < 0 ) {
			continue;
		}
		if ( m_entries[( size_t )child].m_bActive || hasActiveDescendant( child ) ) {
			return true;
		}
	}
	return false;
}

bool Terrain3DHLODStreamer::replacementReady( const t3dEntry_t &rEntry ) const {
	if ( !m_pTerrain ) {
		return true;
	}
	for ( int z = rEntry.getRegionMin().y; z <= rEntry.getRegionMax().y; ++z ) {
		for ( int x = rEntry.getRegionMin().x; x <= rEntry.getRegionMax().x; ++x ) {
			const Vector2i region( x, z );
			const int region_index = regionIndex( region );
			if ( region_index < 0 ) {
				continue;
			}
			// A retiring level may contain sources whose minimum HLOD level is above
			// a finer replacement. Only an equal/coarser active level is guaranteed
			// to contain every source represented by this entry. Otherwise detailed
			// publishers must acknowledge restoration before the proxy disappears.
			if ( m_szRegionRepresentationLevel[region_index] >= ( uint8_t )( rEntry.m_nLevel ) ) {
				continue;
			}
			if ( !m_pTerrain->isHlodDetailReady( region, region ) ) {
				return false;
			}
		}
	}
	return true;
}

void Terrain3DHLODStreamer::selectRecursive( const int index, const Vector3 &rLocalTarget ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	t3dEntry_t &entry = m_entries[( size_t )index];
	const bool preferred = !entry.m_bLoadFailed && packageAcceptable( entry, rLocalTarget );
	if ( preferred ) {
		markDesired( index );
		if ( entry.m_bActive ) {
			return;
		}
		// Until the preferred parent is active, retain the best available child representation.
		selectFallbackChildren( index, rLocalTarget );
		return;
	}
	selectFallbackChildren( index, rLocalTarget );
}

bool Terrain3DHLODStreamer::requestOrActivate( const int index ) {
	t3dEntry_t &entry = m_entries[( size_t )index];
	if ( entry.m_bActive || entry.m_bLoadFailed || entry.m_bCancelPending || entry.m_packagePath.is_empty() || !m_pTerrain || !m_pTerrain->sceneNode().is_inside_tree() ) {
		return entry.m_bActive;
	}
	Terrain3DMemoryAccountScope memory_scope(m_pTerrain->getMemoryAccountInternal());
	if (!entry.m_bRequestStarted && entry.m_memory.bytes() == 0) {
		const uint64_t budget = uint64_t(m_nMemoryBudgetMb) * 1024ull * 1024ull;
		const uint64_t incoming = uint64_t(MAX<int64_t>(0, entry.m_nEstimatedGpuBytes));
		const uint64_t reserved = m_nReservedBytes;
		if (budget == 0 || incoming > budget || reserved > budget - incoming || !entry.m_memory.reserve(incoming)) {
			++m_nCapacityDeferredLastTick;
			return false;
		}
		m_nReservedBytes += entry.m_memory.bytes();
	}
	ResourceLoader *loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return false;
	}
	ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status( entry.m_packagePath );
	if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE && !entry.m_bRequestStarted ) {
		const Error error =
		        loader->load_threaded_request( entry.m_packagePath, "Terrain3DHLODPackage", true, ResourceLoader::CACHE_MODE_REUSE );
		entry.m_bRequestStarted = error == OK;
		entry.m_bLoadFailed = error != OK;
		if ( error == OK ) {
			++m_nRequestsLastTick;
		} else {
			++m_nFailuresLastTick;
		}
		return false;
	}
	if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS || status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) {
		return false;
	}
	if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
		entry.m_bLoadFailed = true;
		++m_nFailuresLastTick;
		return false;
	}
	const Ref<Resource> loaded = loader->load_threaded_get( entry.m_packagePath );
	entry.m_ackage = loaded;
	if ( entry.m_ackage.is_null() || entry.m_ackage->getMesh().is_null() || entry.m_ackage->getLevel() != entry.m_nLevel
	        || entry.m_ackage->getClusterLocation() != entry.getCluster() || entry.m_ackage->getRegionMin() != entry.getRegionMin()
	        || entry.m_ackage->getRegionMax() != entry.getRegionMax() || entry.m_ackage->getSourceSignature() != entry.m_nSourceSignature
	        || entry.m_ackage->getBuildSignature() != entry.m_nBuildSignature ) {
		entry.m_ackage.unref();
		entry.m_bLoadFailed = true;
		++m_nFailuresLastTick;
		return false;
	}
	const int64_t bytes = MAX<int64_t>( entry.m_nEstimatedGpuBytes, entry.m_ackage->getEstimatedGpuBytes() );
	const int64_t budget = ( int64_t )m_nMemoryBudgetMb * 1024LL * 1024LL;
	if ( bytes > budget ) {
		entry.m_ackage.unref();
		++m_nCapacityDeferredLastTick;
		return false;
	}
	if ( m_nResidentEstimatedBytes > budget - bytes ) {
		// Promotion must fit alongside its live fallback. Defer instead of
		// borrowing an unreserved transition package from the total budget.
		entry.m_ackage.unref();
		++m_nCapacityDeferredLastTick;
		return false;
	}
	entry.m_bActive = true;
	removeLoadingEntry( index );
	removeRetiringEntry( index );
	addActiveEntry( index );
	m_bSelectionDirty = true;
	// Fallback descendants were marked desired only while this proxy was not
	// ready. Once the replacement exists they must immediately become retirement
	// candidates instead of rendering on top of their parent for another tick.
	clearDesiredDescendants( index );
	m_nResidentEstimatedBytes += bytes;
	markRdPublicationDirty();
	++m_nLoadsLastTick;
	return true;
}
void Terrain3DHLODStreamer::setMemoryBudgetMb(int nMb) {
	const int requested = CLAMP(nMb, 0, 4096);
	if (requested == m_nMemoryBudgetMb) { return; }
	m_nMemoryBudgetMb = requested;
	const int64_t budget = int64_t(m_nMemoryBudgetMb) * 1024LL * 1024LL;
	while (m_nResidentEstimatedBytes > budget && !m_activeEntries.empty()) {
		deactivate(m_activeEntries.back());
	}
	// A loaded-but-unpublished package must not retain an implicit spare grant.
	for ( int position = ( int )m_loadingEntries.size() - 1; position >= 0; --position ) {
		deactivate( m_loadingEntries[( size_t )position] );
	}
	publishRepresentationMap();
}

void Terrain3DHLODStreamer::deactivate( const int index, const bool bWait ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	t3dEntry_t &entry = m_entries[( size_t )index];
	const bool wasPublished = entry.m_bActive;
	if ( entry.m_bActive ) {
		++m_nUnloadsLastTick;
		const int64_t bytes = entry.m_ackage.is_valid() ? MAX<int64_t>( entry.m_nEstimatedGpuBytes, entry.m_ackage->getEstimatedGpuBytes() )
		                                                : entry.m_nEstimatedGpuBytes;
		m_nResidentEstimatedBytes = MAX<int64_t>( m_nResidentEstimatedBytes - MAX<int64_t>( bytes, 0 ), 0 );
	}
	if (entry.m_bRequestStarted && !entry.m_bActive) {
		ResourceLoader *loader = ResourceLoader::get_singleton();
		if (loader) {
			const auto status = loader->load_threaded_get_status(entry.m_packagePath);
			if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS && !bWait ) {
				// Keep the reservation until Godot's worker finishes, without joining
				// it from a residency or memory-policy update.
				entry.m_bCancelPending = true;
				addLoadingEntry( index );
				return;
			}
			if (status == ResourceLoader::THREAD_LOAD_IN_PROGRESS || status == ResourceLoader::THREAD_LOAD_LOADED) {
				const Ref<Resource> retired = loader->load_threaded_get(entry.m_packagePath);
			}
		}
	}
	entry.m_bActive = false;
	removeActiveEntry( index );
	removeLoadingEntry( index );
	removeRetiringEntry( index );
	entry.m_ackage.unref();
	m_nReservedBytes -= MIN( m_nReservedBytes, entry.m_memory.bytes() );
	entry.m_memory.reset();
	entry.m_bRequestStarted = false;
	entry.m_bCancelPending = false;
	m_bSelectionDirty = true;
	if ( wasPublished ) {
		markRdPublicationDirty();
	}
}

void Terrain3DHLODStreamer::publishRepresentationMap( void ) {
	uint8_t old[T3D_REGION_SLOTS];
	memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	memset( m_szRegionResidentRepresentationLevel, 0, sizeof( m_szRegionResidentRepresentationLevel ) );
	memset( m_szRegionRetiringRepresentationLevel, 0, sizeof( m_szRegionRetiringRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	for ( size_t iActive = 0u; iActive < m_activeEntries.size(); ++iActive ) {
		const t3dEntry_t &entry = m_entries[( size_t )m_activeEntries[iActive]];
		for ( int z = entry.getRegionMin().y; z <= entry.getRegionMax().y; ++z ) {
			for ( int x = entry.getRegionMin().x; x <= entry.getRegionMax().x; ++x ) {
				const int region = regionIndex( Vector2i( x, z ) );
				if ( region >= 0 ) {
					m_szRegionResidentRepresentationLevel[region] =
					        MAX<uint8_t>( m_szRegionResidentRepresentationLevel[region], ( uint8_t )entry.m_nLevel );
					if ( entry.m_bRetiring || !isDesired( entry ) ) {
						m_szRegionRetiringRepresentationLevel[region] =
						        MAX<uint8_t>( m_szRegionRetiringRepresentationLevel[region], ( uint8_t )entry.m_nLevel );
					} else {
						m_szRegionRepresentationLevel[region] =
						        MAX<uint8_t>( m_szRegionRepresentationLevel[region], ( uint8_t )entry.m_nLevel );
						m_detailRequiredRegions.reset( ( size_t )region );
					}
				}
			}
		}
	}
	notifyRepresentationChanges( old );
}
void Terrain3DHLODStreamer::notifyRepresentationChanges( const uint8_t *pOldLevels ) {
	if ( !m_pTerrain ) {
		return;
	}
	bool changed = false;
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		if ( pOldLevels[i] != m_szRegionRepresentationLevel[i] ) {
			changed = true;
			break;
		}
	}
	if ( changed && ++m_nRepresentationGeneration == 0u ) {
		m_nRepresentationGeneration = 1u;
	}
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		if ( pOldLevels[i] != m_szRegionRepresentationLevel[i] ) {
			m_pTerrain->onHlodRegionRepresentationChanged( regionFromIndex( i ) );
		}
	}
}

void Terrain3DHLODStreamer::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
	m_nLoadsLastTick = m_nUnloadsLastTick = m_nRequestsLastTick = m_nFailuresLastTick = m_nCapacityDeferredLastTick = 0;
	m_nTransitionOvercommitBytesLastTick = 0;
	m_nTransitionEntriesExaminedLastTick = 0u;
	if ( !m_bEnabled || !m_pTerrain || m_manifest.is_null() ) {
		suspend();
		return;
	}

	const Vector3 localTarget = m_pTerrain->sceneNode().get_global_transform().affine_inverse().xform( rTargetPosition );
	real_t projectionValue = 0.0f;
	real_t viewportHeight = 1080.0f;
	int projection = -1;
	if ( Camera3D *camera = m_pTerrain->getCamera() ) {
		projection = ( int )camera->get_projection();
		projectionValue = projection == ( int )Camera3D::PROJECTION_ORTHOGONAL ? camera->get_size() : camera->get_fov();
		if ( camera->get_viewport() ) {
			viewportHeight = MAX( camera->get_viewport()->get_visible_rect().size.y, 1.0f );
		}
	}
	const bool selectionContextChanged = !m_bHasSelectionContext || localTarget != m_lastSelectionTarget
	        || projection != m_nLastSelectionProjection || !Math::is_equal_approx( projectionValue, m_fLastSelectionProjectionValue )
	        || !Math::is_equal_approx( viewportHeight, m_fLastSelectionViewportHeight );
	const bool rebuildSelection = bForce || m_bSelectionDirty || selectionContextChanged;

	bool representationDirty = false;
	if ( rebuildSelection ) {
		beginSelectionGeneration();
		m_loadCandidates.clear();
		for ( size_t iRoot = 0u; iRoot < m_roots.size(); ++iRoot ) {
			selectRecursive( m_roots[iRoot], localTarget );
		}
		refreshRetiringFromActive();
		representationDirty = true;
		m_lastSelectionTarget = localTarget;
		m_fLastSelectionProjectionValue = projectionValue;
		m_fLastSelectionViewportHeight = viewportHeight;
		m_nLastSelectionProjection = projection;
		m_bHasSelectionContext = true;
		m_bSelectionDirty = false;
	} else if ( m_loadCandidates.empty() && m_loadingEntries.empty() && m_retiringEntries.empty() ) {
		return;
	}

	int loadBudget = m_nLoadBudget;
	for ( int position = ( int )m_loadingEntries.size() - 1; position >= 0; --position ) {
		const int index = m_loadingEntries[( size_t )position];
		++m_nTransitionEntriesExaminedLastTick;
		t3dEntry_t &entry = m_entries[( size_t )index];
		if ( !isDesired( entry ) || entry.m_bLoadFailed || entry.m_bCancelPending ) {
			deactivate( index );
			continue;
		}
		if ( loadBudget > 0 && requestOrActivate( index ) ) {
			--loadBudget;
			representationDirty = true;
		}
	}

	while ( !m_loadCandidates.empty() && loadBudget > 0 ) {
		const size_t position = m_nLoadCandidateCursor % m_loadCandidates.size();
		const int index = m_loadCandidates[position];
		m_loadCandidates[position] = m_loadCandidates.back();
		m_loadCandidates.pop_back();
		++m_nTransitionEntriesExaminedLastTick;
		--loadBudget;
		t3dEntry_t &entry = m_entries[( size_t )index];
		if ( !isDesired( entry ) || entry.m_bActive || entry.m_bLoadFailed || entry.m_iLoadingListPosition >= 0 ) { continue; }
		const bool active = requestOrActivate( index );
		if ( entry.m_bRequestStarted && !active && !entry.m_bLoadFailed ) { addLoadingEntry( index ); }
		if ( active ) { representationDirty = true; }
		if ( !active && !entry.m_bRequestStarted && !entry.m_bLoadFailed ) {
			// Keep a denied candidate and rotate past it without rebuilding the
			// manifest selection or starving the rest of the candidate queue.
			m_loadCandidates.push_back( index );
			m_nLoadCandidateCursor = position + 1u;
		} else {
			m_nLoadCandidateCursor = position;
		}
	}

	if ( representationDirty ) {
		refreshRetiringFromActive();
		publishRepresentationMap();
	}

	int unloadBudget = m_nUnloadBudget;
	int position = 0;
	bool retirement_representation_dirty = false;
	while ( position < ( int )m_retiringEntries.size() && unloadBudget > 0 ) {
		const int index = m_retiringEntries[( size_t )position];
		++m_nTransitionEntriesExaminedLastTick;
		t3dEntry_t &entry = m_entries[( size_t )index];
		if ( !entry.m_bActive || isDesired( entry ) ) {
			removeRetiringEntry( index );
			continue;
		}
		if ( replacementReady( entry ) ) {
			entry.m_nRetireReadyFrames = MIN<uint8_t>( ( uint8_t )( entry.m_nRetireReadyFrames + 1 ), 3 );
			if ( entry.m_nRetireReadyFrames >= 2 ) {
				deactivate( index );
				retirement_representation_dirty = true;
				--unloadBudget;
				continue;
			}
		} else {
			entry.m_nRetireReadyFrames = 0;
		}
		++position;
	}
	if ( retirement_representation_dirty ) {
		publishRepresentationMap();
	}
}

uint8_t Terrain3DHLODStreamer::getNodeMinimumLevel( const Vector2i &rRegion, const int64_t nSpawnId ) const {
	const int region = regionIndex( rRegion );
	if ( region < 0 ) {
		return 0;
	}
	const uint32_t begin = m_nodePolicyOffsets[( size_t )region];
	const uint32_t end = m_nodePolicyOffsets[( size_t )region + 1u];
	for ( uint32_t index = begin; index < end && index < m_nodePolicies.size(); ++index ) {
		const t3dNodePolicy_t &policy = m_nodePolicies[index];
		if ( policy.m_nSpawnId == nSpawnId ) {
			return policy.m_nMinLevel;
		}
	}
	return 0;
}
bool Terrain3DHLODStreamer::fillRdMeshSources( eastl::vector<t3dRdExternalMeshSource_t, Terrain3DEastlAllocator> &rSources ) {
	rSources.clear();
	if ( rSources.capacity() < m_entries.size() ) {
		++m_nCapacityMissCount;
		return false;
	}
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		const t3dEntry_t &entry = m_entries[( size_t )( i )];
		if ( !entry.m_bActive || entry.m_ackage.is_null() || entry.m_ackage->getMesh().is_null() ) {
			continue;
		}
		if ( !entry.m_ackage->hasCanonicalRenderingMaterials() || !entry.m_ackage->ensureRenderingMaterialStableKeysCold() ) {
			return false;
		}
		t3dRdExternalMeshSource_t source;
		source.m_nSourceKey = rdMeshSourceKey( i );
		source.m_mesh = entry.m_ackage->getMesh();
		source.m_pMaterialContext = entry.m_ackage.ptr();
		source.m_pResolveRenderingMaterial = &t3dResolveHlodRenderingMaterial;
		source.m_nFlags = T3D_RD_EXTERNAL_MESH_REQUIRE_RENDERING_MATERIAL;
		if ( !t3dVectorPushBackWithinCapacity( rSources, source ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DHLODStreamer::buildRdInstanceSnapshot( t3dRdInstanceSceneSnapshot_t &rSnapshot ) const {
	rSnapshot.m_instances.reset();
	rSnapshot.m_cells.reset();
	rSnapshot.m_dirtyRanges.reset();
	rSnapshot.m_dirtyCells.reset();
	rSnapshot.m_bInstanceLayoutChanged = true;
	rSnapshot.m_bTopologyChanged = true;
	rSnapshot.m_nGeneration = m_nRdPublicationGeneration;
	if ( !m_pTerrain ) {
		return true;
	}
	if ( rSnapshot.m_instances.capacity() < m_entries.size() || rSnapshot.m_cells.capacity() < m_entries.size() ) {
		++m_nCapacityMissCount;
		return false;
	}

	const real_t regionWorld = ( real_t )( T3D_REGION_WORLD_SIZE_METERS );
	const Transform3D terrainTransform = m_pTerrain->sceneNode().get_global_transform();
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		const t3dEntry_t &entry = m_entries[( size_t )( i )];
		if ( !entry.m_bActive || entry.m_ackage.is_null() || entry.m_ackage->getMesh().is_null() ) {
			continue;
		}

		Transform3D local;
		local.origin = Vector3( entry.getRegionMin().x * regionWorld, 0.0f, entry.getRegionMin().y * regionWorld );
		const Transform3D transform = terrainTransform * local;
		const uint32_t firstInstance = ( uint32_t )( rSnapshot.m_instances.size() );
		if ( !rSnapshot.m_instances.pushBack( t3dRdPackInstanceRecord( transform ) ) ) {
			return false;
		}

		t3dRdInstanceCell_t cell;
		cell.m_nMeshSourceKey = rdMeshSourceKey( i );
		cell.m_nRegionX = entry.getRegionMin().x;
		cell.m_nRegionZ = entry.getRegionMin().y;
		cell.m_nCellX = ( int16_t )( CLAMP( entry.getCluster().x, ( int )( INT16_MIN ), ( int )( INT16_MAX ) ) );
		cell.m_nCellZ = ( int16_t )( CLAMP( entry.getCluster().y, ( int )( INT16_MIN ), ( int )( INT16_MAX ) ) );
		cell.m_nMeshAssetId = UINT16_MAX;
		cell.m_nLod = ( int16_t )( entry.m_nLevel );
		cell.m_nFirstInstance = firstInstance;
		cell.m_nInstanceCount = 1u;
		cell.m_nRevision = ( uint32_t )( ( uint64_t )( entry.m_nBuildSignature ) & 0xFFFFFFFFull );
		cell.m_nSettingsRevision = ( uint32_t )( m_nRdPublicationGeneration & 0xFFFFFFFFull );
		cell.m_nVisibilityLayers = 1u;
		cell.m_nFlags = T3D_RD_INSTANCE_HLOD;
		if ( entry.m_nLevel <= m_nMaxShadowLevel ) {
			cell.m_nFlags |= T3D_RD_INSTANCE_CAST_SHADOWS;
		}
		cell.m_fLodBegin = 0.0f;
		cell.m_fLodEnd = 0.0f;
		cell.m_fFadeMargin = 0.0f;
		const AABB package_bounds = entry.m_ackage.is_valid() ? entry.m_ackage->getLocalBounds() : entry.m_localBounds;
		cell.m_bounds = t3dTransformHlodBounds( transform, package_bounds );
		if ( !rSnapshot.m_cells.pushBack( cell ) ) {
			return false;
		}
	}
	return true;
}

void Terrain3DHLODStreamer::onInstanceMutation( const t3dInstanceMutationEvent_t &rEvent ) {
	( void )( rEvent );
	++m_nInstanceMutationInvalidations;
	m_bSelectionDirty = true;
}

Dictionary Terrain3DHLODStreamer::getStats( void ) const {
	Dictionary d;
	int active = 0, desired = 0, failed = 0, retiring = 0;
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
		const t3dEntry_t &e = ( m_entries )[iEntry];
		active += e.m_bActive;
		desired += isDesired( e );
		failed += e.m_bLoadFailed;
		retiring += e.m_bRetiring;
	}
	d["entries"] = ( int )m_entries.size();
	d["active"] = active;
	d["desired"] = desired;
	d["failed"] = failed;
	d["retiring"] = retiring;
	d["active_transition_entries"] = ( int )m_activeEntries.size();
	d["loading_transition_entries"] = ( int )m_loadingEntries.size();
	d["retiring_transition_entries"] = ( int )m_retiringEntries.size();
	d["load_candidate_entries"] = ( int )m_loadCandidates.size();
	d["transition_entries_examined_last_tick"] = ( int64_t )m_nTransitionEntriesExaminedLastTick;
	d["loads_last_tick"] = m_nLoadsLastTick;
	d["unloads_last_tick"] = m_nUnloadsLastTick;
	d["requests_last_tick"] = m_nRequestsLastTick;
	d["failures_last_tick"] = m_nFailuresLastTick;
	d["capacity_deferred_last_tick"] = m_nCapacityDeferredLastTick;
	d["resident_estimated_bytes"] = m_nResidentEstimatedBytes;
	d["memory_budget_mb"] = m_nMemoryBudgetMb;
	d["transition_overcommit_bytes_last_tick"] = m_nTransitionOvercommitBytesLastTick;
	d["managed_regions"] = ( int )m_managedRegions.count();
	d["detail_required_regions"] = ( int )m_detailRequiredRegions.count();
	d["rd_publication_generation"] = ( int64_t )( m_nRdPublicationGeneration );
	d["owner_capacity_misses"] = ( int64_t )( m_nCapacityMissCount );
	d["owner_cold_rebuilds"] = ( int64_t )( m_nColdRebuildCount );
	d["instance_mutation_invalidations"] = ( int64_t )( m_nInstanceMutationInvalidations );
	return d;
}
Array Terrain3DHLODStreamer::getDebugEntries( void ) const {
	Array out;
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
		const t3dEntry_t &e = ( m_entries )[iEntry];
		Dictionary d;
		d["level"] = e.m_nLevel;
		d["cluster"] = e.getCluster();
		d["region_min"] = e.getRegionMin();
		d["region_max"] = e.getRegionMax();
		d["local_bounds"] = e.m_localBounds;
		d["package_path"] = e.m_packagePath;
		d["desired"] = isDesired( e );
		d["active"] = e.m_bActive;
		d["retiring"] = e.m_bRetiring;
		d["load_failed"] = e.m_bLoadFailed;
		d["request_started"] = e.m_bRequestStarted;
		out.push_back( d );
	}
	return out;
}
void Terrain3DHLODStreamer::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "reload_manifest" ), &Terrain3DHLODStreamer::reloadManifest );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &Terrain3DHLODStreamer::getStats );
	ClassDB::bind_method( D_METHOD( "get_debug_entries" ), &Terrain3DHLODStreamer::getDebugEntries );
	ClassDB::bind_method(
	        D_METHOD( "is_region_suppressed", "region", "minimum_level" ), &Terrain3DHLODStreamer::isRegionSuppressed, DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "get_region_representation_level", "region" ), &Terrain3DHLODStreamer::getRegionRepresentationLevel );
}

// M40 extracted non-template header implementations.
void Terrain3DHLODStreamer::setEnabled( bool bEnabled ) {
		if ( m_bEnabled == bEnabled ) {
			return;
		}
		m_bEnabled = bEnabled;
		if ( !bEnabled ) {
			suspend();
		} else if ( m_manifest.is_null() || m_entries.empty() ) {
			reloadManifest();
		}
	}

void Terrain3DHLODStreamer::setManifestPath( const String &rPath ) {
		const String path = rPath.strip_edges();
		if ( path == m_manifestPath ) {
			return;
		}
		m_manifestPath = path;
		if ( m_pTerrain ) {
			reloadManifest();
		}
	}

void Terrain3DHLODStreamer::setTargetErrorPixels( real_t fPixels ) {
		const real_t value = CLAMP( fPixels, 0.25f, 64.0f );
		if ( Math::is_equal_approx( m_fTargetErrorPixels, value ) ) {
			return;
		}
		m_fTargetErrorPixels = value;
		m_bSelectionDirty = true;
	}

void Terrain3DHLODStreamer::setHysteresis( real_t fRatio ) {
		const real_t value = CLAMP( fRatio, 0.0f, 0.75f );
		if ( Math::is_equal_approx( m_fHysteresis, value ) ) {
			return;
		}
		m_fHysteresis = value;
		m_bSelectionDirty = true;
	}

void Terrain3DHLODStreamer::setMaxLevel( int nLevel ) {
		const int value = CLAMP( nLevel, 1, 6 );
		if ( m_nMaxLevel == value ) {
			return;
		}
		m_nMaxLevel = value;
		m_bSelectionDirty = true;
	}

void Terrain3DHLODStreamer::setMaxShadowLevel( int nLevel ) {
		const int value = CLAMP( nLevel, 0, 6 );
		if ( m_nMaxShadowLevel == value ) {
			return;
		}
		m_nMaxShadowLevel = value;
		markRdPublicationDirty();
	}

void Terrain3DHLODStreamer::setForcedDebugLevel( int nLevel ) {
		const int value = CLAMP( nLevel, -1, 6 );
		if ( m_nForcedDebugLevel == value ) {
			return;
		}
		m_nForcedDebugLevel = value;
		m_bSelectionDirty = true;
	}

bool Terrain3DHLODStreamer::isRegionManaged( const Vector2i &rRegion ) const {
		const int index = regionIndex( rRegion );
		return index >= 0 && m_managedRegions.test( ( size_t )index );
	}

bool Terrain3DHLODStreamer::isRegionDetailRequired( const Vector2i &rRegion ) const {
		const int index = regionIndex( rRegion );
		return index < 0 || !m_managedRegions.test( ( size_t )index ) || m_detailRequiredRegions.test( ( size_t )index );
	}

uint8_t Terrain3DHLODStreamer::getInstancerMinimumLevel( const Vector2i &rRegion, int nMeshId ) const {
		const int region = regionIndex( rRegion );
		if ( region < 0 || nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS ) {
			return 0;
		}
		const size_t offset = ( size_t )region * T3D_MAX_MESH_ASSETS + ( size_t )nMeshId;
		return offset < m_instancerMinLevels.size() ? m_instancerMinLevels[offset] : 0;
	}

t3dHLODInstancerRepresentationState_t Terrain3DHLODStreamer::getInstancerRepresentationState(
	        const Vector2i &rRegion, const int nMeshId ) const {
		t3dHLODInstancerRepresentationState_t state;
		const int region = regionIndex( rRegion );
		if ( region < 0 || nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS ) {
			return state;
		}
		const size_t offset = ( size_t )region * T3D_MAX_MESH_ASSETS + ( size_t )nMeshId;
		state.m_nMinimumLevel = offset < m_instancerMinLevels.size() ? m_instancerMinLevels[offset] : 0;
		state.m_nActiveLevel = m_szRegionRepresentationLevel[region];
		state.m_nResidentLevel = m_szRegionResidentRepresentationLevel[region];
		state.m_nRetiringLevel = m_szRegionRetiringRepresentationLevel[region];
		return state;
	}

uint8_t Terrain3DHLODStreamer::getRegionRepresentationLevel( const Vector2i &rRegion ) const {
		const int index = regionIndex( rRegion );
		return index >= 0 ? m_szRegionRepresentationLevel[index] : 0;
	}

