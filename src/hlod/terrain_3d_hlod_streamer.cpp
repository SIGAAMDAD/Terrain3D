// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "hlod/terrain_3d_hlod_streamer.h"

#include "data/terrain_3d_data.h"
#include "data/terrain_3d_half_buffer.h"
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

typedef struct t3dIndexedNodePolicy_s {
	int m_nRegion;
	t3dNodePolicy_t m_policy;
} t3dIndexedNodePolicy_t;

static bool t3dIndexedNodePolicyLess( const t3dIndexedNodePolicy_t &rA, const t3dIndexedNodePolicy_t &rB ) {
	return rA.m_nRegion < rB.m_nRegion || ( rA.m_nRegion == rB.m_nRegion && rA.m_policy.m_nSpawnId < rB.m_policy.m_nSpawnId );
}

int Terrain3DHLODStreamer::floorDiv( const int value, const int divisor ) {
	if ( divisor <= 0 ) {
		return 0;
	}
	int quotient = value / divisor;
	if ( value < 0 && ( value % divisor ) != 0 ) {
		--quotient;
	}
	return quotient;
}
int Terrain3DHLODStreamer::regionIndex( const Vector2i &rRegion ) {
	return Terrain3DData::getRegionMapIndex( rRegion );
}
Vector2i Terrain3DHLODStreamer::regionFromIndex( const int index ) {
	const int size = s_nRegionMapSize;
	return Vector2i( index % size - size / 2, index / size - size / 2 );
}

void Terrain3DHLODStreamer::markRdPublicationDirty( void ) {
	++m_nRdPublicationGeneration;
	if ( m_nRdPublicationGeneration == 0u ) {
		m_nRdPublicationGeneration = 1u;
	}
}

uint64_t Terrain3DHLODStreamer::rdMeshSourceKey( const int nEntryIndex ) {
	return t3dRdHlodMeshSourceKey( ( uint32_t )( nEntryIndex ) );
}

static uint32_t t3dPackHlodSnormPair( const float x, const float y ) {
	const int16_t px = ( int16_t )( lround( CLAMP( x, -1.0f, 1.0f ) * 32767.0f ) );
	const int16_t py = ( int16_t )( lround( CLAMP( y, -1.0f, 1.0f ) * 32767.0f ) );
	return ( uint32_t )( ( uint16_t )( px ) ) | ( ( uint32_t )( ( uint16_t )( py ) ) << 16u );
}

static t3dRdInstanceRecord_t t3dPackHlodInstanceRecord( const Transform3D &rTransform ) {
	t3dRdInstanceRecord_t record;
	record.m_szPosition[0] = ( float )( rTransform.origin.x );
	record.m_szPosition[1] = ( float )( rTransform.origin.y );
	record.m_szPosition[2] = ( float )( rTransform.origin.z );
	const Vector3 scale = rTransform.basis.get_scale();
	const Quaternion rotation = rTransform.basis.orthonormalized().get_rotation_quaternion().normalized();
	record.m_nQuaternion01 = t3dPackHlodSnormPair( ( float )( rotation.x ), ( float )( rotation.y ) );
	record.m_nQuaternion23 = t3dPackHlodSnormPair( ( float )( rotation.z ), ( float )( rotation.w ) );
	record.m_nScale01 =
	        ( uint32_t )( t3dHalfEncode( ( float )( scale.x ) ) ) | ( ( uint32_t )( t3dHalfEncode( ( float )( scale.y ) ) ) << 16u );
	record.m_nScale2Flags = ( uint32_t )( t3dHalfEncode( ( float )( scale.z ) ) );
	record.m_nColorRgba8 = 0xFFFFFFFFu;
	return record;
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
	m_instancerMinLevels.set_allocator( allocator );
	m_nodePolicies.set_allocator( allocator );
	return true;
}

void Terrain3DHLODStreamer::initialize( Terrain3D *const pTerrain ) {
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
	uint8_t old[s_nRegionSlots];
	memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	for ( int i = 0; i < ( int )m_entries.size(); ++i ) {
		deactivate( i );
		m_entries[( size_t )i].m_bDesired = false;
		m_entries[( size_t )i].m_bRetiring = false;
		m_entries[( size_t )i].m_nRetireReadyFrames = 0;
	}
	memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	if ( m_pTerrain ) {
		notifyRepresentationChanges( old );
	}
}
void Terrain3DHLODStreamer::clearRuntime( const bool bNotify, const bool bReleaseStorage ) {
	uint8_t old[s_nRegionSlots];
	memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		deactivate( i );
	}
	if ( bReleaseStorage ) {
		t3dVectorRelease( m_entries );
		t3dVectorRelease( m_roots );
		t3dVectorRelease( m_instancerMinLevels );
		t3dVectorRelease( m_nodePolicies );
	} else {
		m_entries.clear();
		m_roots.clear();
		m_instancerMinLevels.clear();
		m_nodePolicies.clear();
	}
	eastl::fill_n( m_nodePolicyOffsets, s_nRegionSlots + 1u, 0u );
	memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	m_managedRegions.reset();
	m_detailRequiredRegions.reset();
	m_nResidentEstimatedBytes = 0;
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
		const Vector2i parentCluster( floorDiv( entry.getCluster().x, 2 ), floorDiv( entry.getCluster().y, 2 ) );
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
	eastl::fill_n( m_nodePolicyOffsets, s_nRegionSlots + 1u, 0u );
	const PackedInt32Array nodeRegions = rManifest->getNodeRegionIndices();
	const PackedInt64Array nodeIds = rManifest->getNodeSpawnIds();
	const PackedByteArray nodeLevels = rManifest->getNodeMinLevels();
	const int nodeCount = MIN( nodeRegions.size(), MIN( nodeIds.size(), nodeLevels.size() ) );
	eastl::vector<t3dIndexedNodePolicy_t, Terrain3DEastlAllocator> policies;
	policies.reserve( ( size_t )nodeCount );
	for ( int i = 0; i < nodeCount; ++i ) {
		const int region = nodeRegions[i];
		if ( region < 0 || region >= s_nRegionSlots || nodeLevels[i] == 0 ) {
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
	for ( int region = 0; region < s_nRegionSlots; ++region ) {
		m_nodePolicyOffsets[( size_t )region + 1u] += m_nodePolicyOffsets[( size_t )region];
	}
	return true;
}

bool Terrain3DHLODStreamer::compilePackedManifest( const Ref<Terrain3DHLODManifest> &rManifest ) {
	const PackedByteArray blob = rManifest->getRuntimeBlob();
	if ( blob.is_empty() ) {
		return false;
	}
	t3dHlodRuntimeData_t runtime;
	if ( !Terrain3DHLODRuntimeCodec::decode( blob, runtime ) ) {
		return false;
	}
	if ( !t3dContainerReserveCold( m_entries, runtime.m_entries.size() ) ) {
		return false;
	}
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( runtime.m_entries ).size() ); iEntry++ ) {
		const t3dHlodRuntimeEntryRecord_t &source = ( runtime.m_entries )[iEntry];
		t3dEntry_t entry;
		entry.m_nLevel = source.m_nLevel;
		entry.m_cluster = source.m_cluster;
		entry.m_regionMin = source.m_regionMin;
		entry.m_regionMax = source.m_regionMax;
		entry.m_localBounds = source.m_localBounds;
		entry.m_packagePath = source.m_packagePath;
		entry.m_fGeometricError = source.m_fGeometricError;
		entry.m_nSourceSignature = source.m_nSourceSignature;
		entry.m_nBuildSignature = source.m_nBuildSignature;
		entry.m_nEstimatedGpuBytes = source.m_nEstimatedGpuBytes;
		entry.m_nParent = source.m_nParent;
		memcpy( entry.m_szChildren, source.m_szChildren, sizeof( entry.m_szChildren ) );
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
		const t3dHlodRuntimeNodePolicyRecord_t &policy = ( runtime.m_nodePolicies )[iPolicy];
		if ( !t3dVectorPushBackWithinCapacity( m_nodePolicies, t3dNodePolicy_t{ policy.m_nSpawnId, policy.m_nMinLevel } ) ) {
			return false;
		}
	}
	eastl::copy_n( runtime.m_szNodePolicyOffsets, s_nRegionSlots + 1, m_nodePolicyOffsets );
	m_manifest = rManifest;
	return true;
}

bool Terrain3DHLODStreamer::compileManifest( const Ref<Terrain3DHLODManifest> &rManifest ) {
	++m_nColdRebuildCount;
	if ( rManifest.is_null() || !m_pTerrain || rManifest->getFormat() != 3 ) {
		return false;
	}
	if ( compilePackedManifest( rManifest ) ) {
		return true;
	}
	m_entries.clear();
	m_roots.clear();
	m_instancerMinLevels.clear();
	m_nodePolicies.clear();
	m_managedRegions.reset();
	m_detailRequiredRegions.reset();
	eastl::fill_n( m_nodePolicyOffsets, s_nRegionSlots + 1u, 0u );
	if ( !m_pTerrain->isEditorAuthoringMode() ) {
		return false;
	}

	const Array &entries = rManifest->getEntriesInternal();
	if ( !t3dContainerReserveCold( m_entries, ( size_t )entries.size() ) ) {
		return false;
	}
	for ( int i = 0; i < entries.size(); ++i ) {
		if ( Variant( entries[i] ).get_type() != Variant::DICTIONARY ) {
			continue;
		}
		const Dictionary data = entries[i];
		if ( !data.has( "level" ) || !data.has( "cluster" ) || !data.has( "region_min" ) || !data.has( "region_max" )
		        || !data.has( "package_path" ) ) {
			continue;
		}
		t3dEntry_t entry;
		entry.m_nLevel = CLAMP( ( int )data["level"], 1, 6 );
		entry.setCluster( data["cluster"] );
		entry.setRegionMin( data["region_min"] );
		entry.setRegionMax( data["region_max"] );
		entry.m_localBounds = data.get( "local_bounds", AABB() );
		entry.m_packagePath = String( data["package_path"] );
		entry.m_fGeometricError = MAX( ( real_t )data.get( "geometric_error", 1.0 ), 0.001f );
		entry.m_nSourceSignature = ( int64_t )data.get( "source_signature", 0 );
		entry.m_nBuildSignature = ( int64_t )data.get( "build_signature", 0 );
		entry.m_nEstimatedGpuBytes = MAX<int64_t>( ( int64_t )data.get( "estimated_gpu_bytes", 0 ), 0 );
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
	const int expected = s_nRegionSlots * s_nMaxMeshAssets;
	if ( !t3dVectorResizeCold( m_instancerMinLevels, ( size_t )expected ) ) {
		return false;
	}
	for ( int i = 0; i < expected; ++i ) {
		m_instancerMinLevels[( size_t )i] = i < meshPolicy.size() ? meshPolicy[i] : 0;
	}
	if ( !compileLegacyNodePolicies( rManifest ) || !buildHierarchy() ) {
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

bool Terrain3DHLODStreamer::packageAcceptable( const t3dEntry_t &rEntry, const Vector3 &rLocalTarget ) const {
	if ( rEntry.m_nLevel > m_nMaxLevel ) {
		return false;
	}
	if ( m_nForcedDebugLevel >= 0 ) {
		return rEntry.m_nLevel == m_nForcedDebugLevel;
	}
	const real_t regionWorld = ( real_t )( s_nRegionWorldSizeMeters );
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
		m_entries[( size_t )child].m_bDesired = false;
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
			// publishRepresentationMap contains only active, desired, non-retiring
			// replacements. A proxy at any level is therefore sufficient for this
			// region; otherwise the detailed publishers must acknowledge readiness.
			if ( m_szRegionRepresentationLevel[region_index] > 0 ) {
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
		entry.m_bDesired = true;
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
	if ( entry.m_bActive || entry.m_bLoadFailed || entry.m_packagePath.is_empty() || !m_pTerrain || !m_pTerrain->is_inside_tree() ) {
		return entry.m_bActive;
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
		++m_nCapacityDeferredLastTick;
		return false;
	}
	if ( m_nResidentEstimatedBytes > budget - bytes ) {
		// No-hole parent promotion sometimes requires the incoming parent and its
		// active fallback descendants to coexist briefly. Permit one such
		// transition package above the steady-state budget; never overcommit for
		// unrelated residency or while a previous transition is still over budget.
		if ( m_nResidentEstimatedBytes > budget || m_nTransitionOvercommitBytesLastTick > 0 || m_nUnloadBudget <= 0
		        || !hasActiveDescendant( index ) ) {
			++m_nCapacityDeferredLastTick;
			return false;
		}
		m_nTransitionOvercommitBytesLastTick = m_nResidentEstimatedBytes + bytes - budget;
	}
	entry.m_bActive = true;
	entry.m_bRetiring = false;
	entry.m_nRetireReadyFrames = 0;
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
void Terrain3DHLODStreamer::deactivate( const int index ) {
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
	entry.m_bActive = false;
	entry.m_bRetiring = false;
	entry.m_nRetireReadyFrames = 0;
	entry.m_ackage.unref();
	entry.m_bRequestStarted = false;
	m_bSelectionDirty = true;
	if ( wasPublished ) {
		markRdPublicationDirty();
	}
}

void Terrain3DHLODStreamer::publishRepresentationMap( void ) {
	uint8_t old[s_nRegionSlots];
	memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
		const t3dEntry_t &entry = ( m_entries )[iEntry];
		if ( !entry.m_bActive || entry.m_bRetiring || !entry.m_bDesired ) {
			continue;
		}
		for ( int z = entry.getRegionMin().y; z <= entry.getRegionMax().y; ++z ) {
			for ( int x = entry.getRegionMin().x; x <= entry.getRegionMax().x; ++x ) {
				const int region = regionIndex( Vector2i( x, z ) );
				if ( region >= 0 ) {
					m_szRegionRepresentationLevel[region] =
					        MAX<uint8_t>( m_szRegionRepresentationLevel[region], ( uint8_t )entry.m_nLevel );
					m_detailRequiredRegions.reset( ( size_t )region );
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
	for ( int i = 0; i < s_nRegionSlots; ++i ) {
		if ( pOldLevels[i] != m_szRegionRepresentationLevel[i] ) {
			m_pTerrain->onHlodRegionRepresentationChanged( regionFromIndex( i ) );
		}
	}
}

void Terrain3DHLODStreamer::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
	m_nLoadsLastTick = m_nUnloadsLastTick = m_nRequestsLastTick = m_nFailuresLastTick = m_nCapacityDeferredLastTick = 0;
	m_nTransitionOvercommitBytesLastTick = 0;
	if ( !m_bEnabled || !m_pTerrain || m_manifest.is_null() ) {
		suspend();
		return;
	}

	const Vector3 localTarget = m_pTerrain->get_global_transform().affine_inverse().xform( rTargetPosition );
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

	if ( rebuildSelection ) {
		for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
			t3dEntry_t &entry = ( m_entries )[iEntry];
			entry.m_bDesired = false;
		}
		for ( size_t iRoot = 0u; iRoot < ( size_t )( ( m_roots ).size() ); iRoot++ ) {
			const int root = ( m_roots )[iRoot];
			selectRecursive( root, localTarget );
		}
		m_lastSelectionTarget = localTarget;
		m_fLastSelectionProjectionValue = projectionValue;
		m_fLastSelectionViewportHeight = viewportHeight;
		m_nLastSelectionProjection = projection;
		m_bHasSelectionContext = true;
		m_bSelectionDirty = false;
	} else {
		bool hasTransitionWork = false;
		for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
			const t3dEntry_t &entry = ( m_entries )[iEntry];
			if ( ( entry.m_bDesired && !entry.m_bActive && !entry.m_bLoadFailed ) || entry.m_bRetiring
			        || ( entry.m_bActive && !entry.m_bDesired ) ) {
				hasTransitionWork = true;
				break;
			}
		}
		if ( !hasTransitionWork ) {
			return;
		}
	}

	bool representationDirty = rebuildSelection;
	int loadBudget = m_nLoadBudget;
	for ( int i = 0; i < ( int )m_entries.size() && loadBudget > 0; ++i ) {
		t3dEntry_t &entry = m_entries[( size_t )i];
		if ( entry.m_bDesired && !entry.m_bActive && !entry.m_bLoadFailed ) {
			const bool wasRequested = entry.m_bRequestStarted;
			const bool active = requestOrActivate( i );
			if ( active ) {
				representationDirty = true;
			}
			if ( active || ( !wasRequested && entry.m_bRequestStarted ) ) {
				--loadBudget;
			}
		}
	}

	// Mark retirement/cancellation first, then publish once. The representation
	// map must exclude newly retiring proxies before replacementReady() is tested.
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
		t3dEntry_t &entry = ( m_entries )[iEntry];
		if ( !entry.m_bActive || entry.m_bDesired ) {
			if ( entry.m_bRetiring ) {
				representationDirty = true;
			}
			entry.m_bRetiring = false;
			entry.m_nRetireReadyFrames = 0;
			continue;
		}
		if ( !entry.m_bRetiring ) {
			entry.m_bRetiring = true;
			entry.m_nRetireReadyFrames = 0;
			representationDirty = true;
		}
	}
	if ( representationDirty ) {
		publishRepresentationMap();
	}

	int unloadBudget = m_nUnloadBudget;
	for ( int i = 0; i < ( int )m_entries.size() && unloadBudget > 0; ++i ) {
		t3dEntry_t &entry = m_entries[( size_t )i];
		if ( !entry.m_bActive || !entry.m_bRetiring ) {
			continue;
		}
		if ( replacementReady( entry ) ) {
			entry.m_nRetireReadyFrames = MIN<uint8_t>( ( uint8_t )( entry.m_nRetireReadyFrames + 1 ), 3 );
			if ( entry.m_nRetireReadyFrames >= 2 ) {
				deactivate( i );
				--unloadBudget;
			}
		} else {
			entry.m_nRetireReadyFrames = 0;
		}
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
bool Terrain3DHLODStreamer::fillRdMeshSources( eastl::vector<t3dRdExternalMeshSource_t, Terrain3DEastlAllocator> &rSources ) const {
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
		t3dRdExternalMeshSource_t source;
		source.m_nSourceKey = rdMeshSourceKey( i );
		source.m_mesh = entry.m_ackage->getMesh();
		if ( !t3dVectorPushBackWithinCapacity( rSources, source ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DHLODStreamer::buildRdInstanceSnapshot( t3dRdInstanceSceneSnapshot_t &rSnapshot ) const {
	rSnapshot.m_instances.reset();
	rSnapshot.m_cells.reset();
	rSnapshot.m_nGeneration = m_nRdPublicationGeneration;
	if ( !m_pTerrain ) {
		return true;
	}
	if ( rSnapshot.m_instances.capacity() < m_entries.size() || rSnapshot.m_cells.capacity() < m_entries.size() ) {
		++m_nCapacityMissCount;
		return false;
	}

	const real_t regionWorld = ( real_t )( s_nRegionWorldSizeMeters );
	const Transform3D terrainTransform = m_pTerrain->get_global_transform();
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		const t3dEntry_t &entry = m_entries[( size_t )( i )];
		if ( !entry.m_bActive || entry.m_ackage.is_null() || entry.m_ackage->getMesh().is_null() ) {
			continue;
		}

		Transform3D local;
		local.origin = Vector3( entry.getRegionMin().x * regionWorld, 0.0f, entry.getRegionMin().y * regionWorld );
		const Transform3D transform = terrainTransform * local;
		const uint32_t firstInstance = ( uint32_t )( rSnapshot.m_instances.size() );
		if ( !rSnapshot.m_instances.pushBack( t3dPackHlodInstanceRecord( transform ) ) ) {
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

Dictionary Terrain3DHLODStreamer::getStats( void ) const {
	Dictionary d;
	int active = 0, desired = 0, failed = 0, retiring = 0;
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_entries ).size() ); iEntry++ ) {
		const t3dEntry_t &e = ( m_entries )[iEntry];
		active += e.m_bActive;
		desired += e.m_bDesired;
		failed += e.m_bLoadFailed;
		retiring += e.m_bRetiring;
	}
	d["entries"] = ( int )m_entries.size();
	d["active"] = active;
	d["desired"] = desired;
	d["failed"] = failed;
	d["retiring"] = retiring;
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
		d["desired"] = e.m_bDesired;
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
