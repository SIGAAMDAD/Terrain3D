// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "hlod/terrain_3d_hlod_streamer.h"

#include "data/terrain_3d_data.h"
#include "data/terrain_3d_half_buffer.h"
#include "eastl_config.h"
#include "terrain_3d.h"

#include <EASTL/sort.h>
#include <cmath>
#include <cstring>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>

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
	const int size = s_nActiveMaxRegionMapSize;
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
	const int16_t px = ( int16_t )( std::lround( CLAMP( x, -1.0f, 1.0f ) * 32767.0f ) );
	const int16_t py = ( int16_t )( std::lround( CLAMP( y, -1.0f, 1.0f ) * 32767.0f ) );
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
	record.m_nScale01 = ( uint32_t )( t3dHalfEncode( ( float )( scale.x ) ) ) |
			( ( uint32_t )( t3dHalfEncode( ( float )( scale.y ) ) ) << 16u );
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
	m_pTerrain = nullptr;
	m_manifest.unref();
}
void Terrain3DHLODStreamer::suspend( void ) {
	uint8_t old[s_nMaxRegionSlots];
	std::memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	for ( int i = 0; i < ( int )m_entries.size(); ++i ) {
		deactivate( i );
		m_entries[( size_t )i].m_bDesired = false;
		m_entries[( size_t )i].m_bRetiring = false;
		m_entries[( size_t )i].m_nRetireReadyFrames = 0;
	}
	std::memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	if ( m_pTerrain ) {
		notifyRepresentationChanges( old );
	}
}
void Terrain3DHLODStreamer::clearRuntime( const bool bNotify, const bool bReleaseStorage ) {
	uint8_t old[s_nMaxRegionSlots];
	std::memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		deactivate( i );
	}
	if ( bReleaseStorage ) {
		eastl::vector<t3dEntry_t>().swap( m_entries );
		eastl::vector<int>().swap( m_roots );
		eastl::vector<uint8_t>().swap( m_instancerMinLevels );
		eastl::vector<t3dNodePolicy_t>().swap( m_nodePolicies );
	} else {
		m_entries.clear();
		m_roots.clear();
		m_instancerMinLevels.clear();
		m_nodePolicies.clear();
	}
	m_nodePolicyOffsets.fill( 0u );
	std::memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
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
		if ( m_entries[( size_t )i].m_nLevel == level && m_entries[( size_t )i].m_cluster == rCluster ) {
			return i;
		}
	}
	return -1;
}
void Terrain3DHLODStreamer::buildHierarchy( void ) {
	m_roots.clear();
	for ( t3dEntry_t &entry : m_entries ) {
		entry.m_nParent = -1;
		for ( int &child : entry.m_szChildren ) {
			child = -1;
		}
	}
	for ( int i = 0; i < ( int )m_entries.size(); ++i ) {
		t3dEntry_t &entry = m_entries[( size_t )i];
		const Vector2i parentCluster( floorDiv( entry.m_cluster.x, 2 ), floorDiv( entry.m_cluster.y, 2 ) );
		const int parent = findEntry( entry.m_nLevel + 1, parentCluster );
		entry.m_nParent = parent;
		if ( parent < 0 ) {
			m_roots.push_back( i );
			continue;
		}
		const int localX = entry.m_cluster.x - parentCluster.x * 2;
		const int localY = entry.m_cluster.y - parentCluster.y * 2;
		const int child = CLAMP( localY, 0, 1 ) * 2 + CLAMP( localX, 0, 1 );
		m_entries[( size_t )parent].m_szChildren[child] = i;
	}
}

void Terrain3DHLODStreamer::compileLegacyNodePolicies( const Ref<Terrain3DHLODManifest> &rManifest ) {
	m_nodePolicies.clear();
	m_nodePolicyOffsets.fill( 0u );
	const PackedInt32Array nodeRegions = rManifest->getNodeRegionIndices();
	const PackedInt64Array nodeIds = rManifest->getNodeSpawnIds();
	const PackedByteArray nodeLevels = rManifest->getNodeMinLevels();
	const int nodeCount = MIN( nodeRegions.size(), MIN( nodeIds.size(), nodeLevels.size() ) );
	typedef struct t3dIndexedNodePolicy_s {
		int m_nRegion = 0;
		t3dNodePolicy_t m_policy;
	} t3dIndexedNodePolicy_t;
	eastl::vector<t3dIndexedNodePolicy_t> policies;
	policies.reserve( ( size_t )nodeCount );
	for ( int i = 0; i < nodeCount; ++i ) {
		const int region = nodeRegions[i];
		if ( region < 0 || region >= s_nMaxRegionSlots || nodeLevels[i] == 0 ) {
			continue;
		}
		policies.push_back( { region, { nodeIds[i], nodeLevels[i] } } );
	}
	eastl::sort(
			policies.begin(), policies.end(), []( const t3dIndexedNodePolicy_t &a, const t3dIndexedNodePolicy_t &b ) {
				return a.m_nRegion < b.m_nRegion ||
						( a.m_nRegion == b.m_nRegion && a.m_policy.m_nSpawnId < b.m_policy.m_nSpawnId );
			} );
	m_nodePolicies.reserve( policies.size() );
	for ( const t3dIndexedNodePolicy_t &indexed : policies ) {
		++m_nodePolicyOffsets[( size_t )indexed.m_nRegion + 1u];
		m_nodePolicies.push_back( indexed.m_policy );
	}
	for ( int region = 0; region < s_nMaxRegionSlots; ++region ) {
		m_nodePolicyOffsets[( size_t )region + 1u] += m_nodePolicyOffsets[( size_t )region];
	}
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
	m_entries.reserve( runtime.m_entries.size() );
	for ( const t3dHlodRuntimeEntryRecord_t &source : runtime.m_entries ) {
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
		for ( int child = 0; child < 4; ++child ) {
			entry.m_szChildren[child] = source.m_szChildren[child];
		}
		m_entries.push_back( entry );
		for ( int z = entry.m_regionMin.y; z <= entry.m_regionMax.y; ++z ) {
			for ( int x = entry.m_regionMin.x; x <= entry.m_regionMax.x; ++x ) {
				const int index = regionIndex( Vector2i( x, z ) );
				if ( index >= 0 ) {
					m_managedRegions.set( ( size_t )index );
				}
			}
		}
	}
	m_roots = eastl::move( runtime.m_roots );
	m_instancerMinLevels = eastl::move( runtime.m_instancerMinLevels );
	m_nodePolicies.reserve( runtime.m_nodePolicies.size() );
	for ( const t3dHlodRuntimeNodePolicyRecord_t &policy : runtime.m_nodePolicies ) {
		m_nodePolicies.push_back( { policy.m_nSpawnId, policy.m_nMinLevel } );
	}
	m_nodePolicyOffsets = runtime.m_szNodePolicyOffsets;
	m_manifest = rManifest;
	return true;
}

bool Terrain3DHLODStreamer::compileManifest( const Ref<Terrain3DHLODManifest> &rManifest ) {
	if ( rManifest.is_null() || !m_pTerrain || rManifest->getFormat() != 1 ||
			rManifest->getRegionMapSize() != s_nActiveMaxRegionMapSize ||
			rManifest->getRegionSize() != ( int )m_pTerrain->getRegionSize() ||
			!Math::is_equal_approx( rManifest->getVertexSpacing(), m_pTerrain->getVertexSpacing() ) ) {
		return false;
	}
	if ( compilePackedManifest( rManifest ) ) {
		return true;
	}
	if ( !m_pTerrain->isEditorAuthoringMode() ) {
		return false;
	}

	const Array &entries = rManifest->getEntriesInternal();
	m_entries.reserve( ( size_t )entries.size() );
	for ( int i = 0; i < entries.size(); ++i ) {
		if ( Variant( entries[i] ).get_type() != Variant::DICTIONARY ) {
			continue;
		}
		const Dictionary data = entries[i];
		if ( !data.has( "level" ) || !data.has( "cluster" ) || !data.has( "region_min" ) || !data.has( "region_max" ) ||
				!data.has( "package_path" ) ) {
			continue;
		}
		t3dEntry_t entry;
		entry.m_nLevel = CLAMP( ( int )data["level"], 1, 6 );
		entry.m_cluster = data["cluster"];
		entry.m_regionMin = data["region_min"];
		entry.m_regionMax = data["region_max"];
		entry.m_localBounds = data.get( "local_bounds", AABB() );
		entry.m_packagePath = String( data["package_path"] );
		entry.m_fGeometricError = MAX( ( real_t )data.get( "geometric_error", 1.0 ), 0.001f );
		entry.m_nSourceSignature = ( int64_t )data.get( "source_signature", 0 );
		entry.m_nBuildSignature = ( int64_t )data.get( "build_signature", 0 );
		entry.m_nEstimatedGpuBytes = MAX<int64_t>( ( int64_t )data.get( "estimated_gpu_bytes", 0 ), 0 );
		m_entries.push_back( entry );
		for ( int z = entry.m_regionMin.y; z <= entry.m_regionMax.y; ++z ) {
			for ( int x = entry.m_regionMin.x; x <= entry.m_regionMax.x; ++x ) {
				const int index = regionIndex( Vector2i( x, z ) );
				if ( index >= 0 ) {
					m_managedRegions.set( ( size_t )index );
				}
			}
		}
	}
	const PackedByteArray meshPolicy = rManifest->getInstancerMinLevels();
	const int expected = s_nMaxRegionSlots * s_nMaxMeshAssets;
	m_instancerMinLevels.resize( ( size_t )expected );
	for ( int i = 0; i < expected; ++i ) {
		m_instancerMinLevels[( size_t )i] = i < meshPolicy.size() ? meshPolicy[i] : 0;
	}
	compileLegacyNodePolicies( rManifest );
	buildHierarchy();
	m_manifest = rManifest;
	return true;
}

bool Terrain3DHLODStreamer::reloadManifest( void ) {
	clearRuntime( true );
	m_manifest.unref();
	if ( !m_bEnabled || !m_pTerrain || m_manifestPath.is_empty() ||
			!ResourceLoader::get_singleton()->exists( m_manifestPath ) ) {
		return false;
	}
	const Ref<Resource> resource = ResourceLoader::get_singleton()->load(
			m_manifestPath, "Terrain3DHLODManifest", ResourceLoader::CACHE_MODE_REPLACE );
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
	const real_t regionWorld = ( real_t )m_pTerrain->getRegionSize() * m_pTerrain->getVertexSpacing();
	const Vector3 clusterOrigin( rEntry.m_regionMin.x * regionWorld, 0.0f, rEntry.m_regionMin.y * regionWorld );
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
			projectionScale = height / MAX( 2.0f * std::tan( Math::deg_to_rad( camera->get_fov() ) * 0.5f ), 0.001f );
			projectedError = rEntry.m_fGeometricError * projectionScale / distance;
		}
	} else {
		projectedError = rEntry.m_fGeometricError * projectionScale / distance;
	}
	const real_t threshold = rEntry.m_bActive ? m_fTargetErrorPixels * ( 1.0f + m_fHysteresis )
											  : m_fTargetErrorPixels * MAX( 0.05f, 1.0f - m_fHysteresis );
	return projectedError <= threshold;
}

void Terrain3DHLODStreamer::selectFallbackChildren( const int index, const Vector3 &rLocalTarget ) {
	const t3dEntry_t &entry = m_entries[( size_t )index];
	// A leaf has no HLOD fallback. Leaving it undesired deliberately publishes
	// detailed residency while the still-active proxy remains visible until the
	// detailed publishers acknowledge readiness. Re-marking an active leaf as
	// desired here would permanently trap the world at HLOD1 after first use.
	for ( const int child : entry.m_szChildren ) {
		if ( child >= 0 ) {
			selectRecursive( child, rLocalTarget );
		}
	}
}
void Terrain3DHLODStreamer::clearDesiredDescendants( const int index ) {
	if ( index < 0 || index >= ( int )m_entries.size() ) {
		return;
	}
	for ( const int child : m_entries[( size_t )index].m_szChildren ) {
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
	for ( const int child : m_entries[( size_t )index].m_szChildren ) {
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
	for ( int z = rEntry.m_regionMin.y; z <= rEntry.m_regionMax.y; ++z ) {
		for ( int x = rEntry.m_regionMin.x; x <= rEntry.m_regionMax.x; ++x ) {
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
	if ( entry.m_bActive || entry.m_bLoadFailed || entry.m_packagePath.is_empty() || !m_pTerrain ||
			!m_pTerrain->is_inside_tree() ) {
		return entry.m_bActive;
	}
	ResourceLoader *loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return false;
	}
	ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status( entry.m_packagePath );
	if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE && !entry.m_bRequestStarted ) {
		const Error error = loader->load_threaded_request(
				entry.m_packagePath, "Terrain3DHLODPackage", true, ResourceLoader::CACHE_MODE_REUSE );
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
	if ( entry.m_ackage.is_null() || entry.m_ackage->getMesh().is_null() ||
			entry.m_ackage->getLevel() != entry.m_nLevel || entry.m_ackage->getClusterLocation() != entry.m_cluster ||
			entry.m_ackage->getRegionMin() != entry.m_regionMin ||
			entry.m_ackage->getRegionMax() != entry.m_regionMax ||
			entry.m_ackage->getSourceSignature() != entry.m_nSourceSignature ||
			entry.m_ackage->getBuildSignature() != entry.m_nBuildSignature ) {
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
		if ( m_nResidentEstimatedBytes > budget || m_nTransitionOvercommitBytesLastTick > 0 || m_nUnloadBudget <= 0 ||
				!hasActiveDescendant( index ) ) {
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
		const int64_t bytes = entry.m_ackage.is_valid()
				? MAX<int64_t>( entry.m_nEstimatedGpuBytes, entry.m_ackage->getEstimatedGpuBytes() )
				: entry.m_nEstimatedGpuBytes;
		m_nResidentEstimatedBytes = MAX<int64_t>( m_nResidentEstimatedBytes - MAX<int64_t>( bytes, 0 ), 0 );
	}
	entry.m_bActive = false;
	entry.m_bRetiring = false;
	entry.m_nRetireReadyFrames = 0;
	entry.m_ackage.unref();
	entry.m_bRequestStarted = false;
	m_bSelectionDirty = true;
	if ( wasPublished ) markRdPublicationDirty();
}

void Terrain3DHLODStreamer::publishRepresentationMap( void ) {
	uint8_t old[s_nMaxRegionSlots];
	std::memcpy( old, m_szRegionRepresentationLevel, sizeof( old ) );
	std::memset( m_szRegionRepresentationLevel, 0, sizeof( m_szRegionRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	for ( const t3dEntry_t &entry : m_entries ) {
		if ( !entry.m_bActive || entry.m_bRetiring || !entry.m_bDesired ) {
			continue;
		}
		for ( int z = entry.m_regionMin.y; z <= entry.m_regionMax.y; ++z ) {
			for ( int x = entry.m_regionMin.x; x <= entry.m_regionMax.x; ++x ) {
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
	for ( int i = 0; i < s_nMaxRegionSlots; ++i ) {
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
	const bool selectionContextChanged = !m_bHasSelectionContext || localTarget != m_lastSelectionTarget ||
			projection != m_nLastSelectionProjection ||
			!Math::is_equal_approx( projectionValue, m_fLastSelectionProjectionValue ) ||
			!Math::is_equal_approx( viewportHeight, m_fLastSelectionViewportHeight );
	const bool rebuildSelection = bForce || m_bSelectionDirty || selectionContextChanged;

	if ( rebuildSelection ) {
		for ( t3dEntry_t &entry : m_entries ) {
			entry.m_bDesired = false;
		}
		for ( const int root : m_roots ) {
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
		for ( const t3dEntry_t &entry : m_entries ) {
			if ( ( entry.m_bDesired && !entry.m_bActive && !entry.m_bLoadFailed ) || entry.m_bRetiring ||
					( entry.m_bActive && !entry.m_bDesired ) ) {
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
	for ( t3dEntry_t &entry : m_entries ) {
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
void Terrain3DHLODStreamer::fillRdMeshSources( std::vector<t3dRdExternalMeshSource_t> &rSources ) const {
	rSources.clear();
	rSources.reserve( m_entries.size() );
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		const t3dEntry_t &entry = m_entries[( size_t )( i )];
		if ( !entry.m_bActive || entry.m_ackage.is_null() || entry.m_ackage->getMesh().is_null() ) {
			continue;
		}
		t3dRdExternalMeshSource_t source;
		source.m_nSourceKey = rdMeshSourceKey( i );
		source.m_mesh = entry.m_ackage->getMesh();
		rSources.push_back( source );
	}
}

std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> Terrain3DHLODStreamer::buildRdInstanceSnapshot( void ) const {
	std::shared_ptr<t3dRdInstanceSceneSnapshot_t> snapshot = std::make_shared<t3dRdInstanceSceneSnapshot_t>();
	snapshot->m_nGeneration = m_nRdPublicationGeneration;
	if ( !m_pTerrain ) {
		return snapshot;
	}

	const real_t regionWorld = ( real_t )( m_pTerrain->getRegionSize() ) * m_pTerrain->getVertexSpacing();
	const Transform3D terrainTransform = m_pTerrain->get_global_transform();
	snapshot->m_instances.reserve( m_entries.size() );
	snapshot->m_cells.reserve( m_entries.size() );
	for ( int i = 0; i < ( int )( m_entries.size() ); ++i ) {
		const t3dEntry_t &entry = m_entries[( size_t )( i )];
		if ( !entry.m_bActive || entry.m_ackage.is_null() || entry.m_ackage->getMesh().is_null() ) {
			continue;
		}

		Transform3D local;
		local.origin = Vector3( entry.m_regionMin.x * regionWorld, 0.0f, entry.m_regionMin.y * regionWorld );
		const Transform3D transform = terrainTransform * local;
		const uint32_t firstInstance = ( uint32_t )( snapshot->m_instances.size() );
		snapshot->m_instances.push_back( t3dPackHlodInstanceRecord( transform ) );

		t3dRdInstanceCell_t cell;
		cell.m_nMeshSourceKey = rdMeshSourceKey( i );
		cell.m_nRegionX = entry.m_regionMin.x;
		cell.m_nRegionZ = entry.m_regionMin.y;
		cell.m_nCellX = ( int16_t )( CLAMP( entry.m_cluster.x, ( int )( INT16_MIN ), ( int )( INT16_MAX ) ) );
		cell.m_nCellZ = ( int16_t )( CLAMP( entry.m_cluster.y, ( int )( INT16_MIN ), ( int )( INT16_MAX ) ) );
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
		snapshot->m_cells.push_back( cell );
	}
	return snapshot;
}

Dictionary Terrain3DHLODStreamer::getStats( void ) const {
	Dictionary d;
	int active = 0, desired = 0, failed = 0, retiring = 0;
	for ( const t3dEntry_t &e : m_entries ) {
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
	return d;
}
Array Terrain3DHLODStreamer::getDebugEntries( void ) const {
	Array out;
	for ( const t3dEntry_t &e : m_entries ) {
		Dictionary d;
		d["level"] = e.m_nLevel;
		d["cluster"] = e.m_cluster;
		d["region_min"] = e.m_regionMin;
		d["region_max"] = e.m_regionMax;
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
	ClassDB::bind_method( D_METHOD( "is_region_suppressed", "region", "minimum_level" ),
			&Terrain3DHLODStreamer::isRegionSuppressed,
			DEFVAL( 1 ) );
	ClassDB::bind_method(
			D_METHOD( "get_region_representation_level", "region" ), &Terrain3DHLODStreamer::getRegionRepresentationLevel );
}
