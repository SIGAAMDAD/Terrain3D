// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "hlod/terrain_3d_hlod_streamer.h"
#include <cmath>
#include "eastl_config.h"
#include <EASTL/sort.h>
#include <cstring>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include "terrain_3d.h"
#include "data/terrain_3d_data.h"


int T3DHLODStreamer::floorDiv( const int value, const int divisor ) {
	if ( divisor <= 0 ) { return 0; }
	int quotient = value / divisor;
	if ( value < 0 && ( value % divisor ) != 0 ) { --quotient; }
	return quotient;
}
int T3DHLODStreamer::regionIndex( const Vector2i &rRegion ) {
	return T3DData::getRegionMapIndex( rRegion );
}
Vector2i T3DHLODStreamer::regionFromIndex( const int index ) {
	const int size = TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
	return Vector2i( index % size - size / 2, index / size - size / 2 );
}

void T3DHLODStreamer::initialize( T3DTerrain *const pTerrain ) {
	if ( m_pTerrain == pTerrain ) { return; }
	destroy();
	m_pTerrain = pTerrain;
	reloadManifest();
}
void T3DHLODStreamer::destroy( void ) {
	clearRuntime( true, true );
	m_pTerrain = nullptr;
	m_manifest.unref();
}
void T3DHLODStreamer::suspend( void ) {
	uint8_t old[TERRAIN3D_MAX_REGION_SLOTS];
	std::memcpy( old, m_regionRepresentationLevel, sizeof( old ) );
	for ( int i = 0; i < ( int ) m_entries.size(); ++i ) {
		deactivate( i );
		m_entries[( size_t ) i].desired = false;
		m_entries[( size_t ) i].retiring = false;
		m_entries[( size_t ) i].retireReadyFrames = 0;
	}
	std::memset( m_regionRepresentationLevel, 0, sizeof( m_regionRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	if ( m_pTerrain ) { notifyRepresentationChanges( old ); }
}
void T3DHLODStreamer::clearRuntime( const bool bNotify, const bool bReleaseStorage ) {
	uint8_t old[TERRAIN3D_MAX_REGION_SLOTS];
	std::memcpy( old, m_regionRepresentationLevel, sizeof( old ) );
	for ( int i = 0; i < ( int ) ( m_entries.size() ); ++i ) { deactivate( i ); }
	if ( bReleaseStorage ) {
		eastl::vector<t3dentry_t>().swap( m_entries );
		eastl::vector<int>().swap( m_roots );
		eastl::vector<uint8_t>().swap( m_instancerMinLevels );
		eastl::vector<t3dnodePolicy_t>().swap( m_nodePolicies );
	} else {
		m_entries.clear();
		m_roots.clear();
		m_instancerMinLevels.clear();
		m_nodePolicies.clear();
	}
	m_nodePolicyOffsets.fill( 0u );
	std::memset( m_regionRepresentationLevel, 0, sizeof( m_regionRepresentationLevel ) );
	m_managedRegions.reset();
	m_detailRequiredRegions.reset();
	m_nResidentEstimatedBytes = 0;
	m_bHasSelectionContext = false;
	m_bSelectionDirty = true;
	if ( bNotify && m_pTerrain ) { notifyRepresentationChanges( old ); }
}

int T3DHLODStreamer::findEntry( const int level, const Vector2i &rCluster ) const {
	for ( int i = 0; i < ( int ) ( m_entries.size() ); ++i ) {
		if ( m_entries[( size_t ) i].level == level && m_entries[( size_t ) i].cluster == rCluster ) { return i; }
	}
	return -1;
}
void T3DHLODStreamer::buildHierarchy( void ) {
	m_roots.clear();
	for ( t3dentry_t &entry : m_entries ) { entry.parent = -1; for ( int &child : entry.children ) { child = -1; } }
	for ( int i = 0; i < ( int ) m_entries.size(); ++i ) {
		t3dentry_t &entry = m_entries[( size_t ) i];
		const Vector2i parentCluster( floorDiv( entry.cluster.x, 2 ), floorDiv( entry.cluster.y, 2 ) );
		const int parent = findEntry( entry.level + 1, parentCluster );
		entry.parent = parent;
		if ( parent < 0 ) { m_roots.push_back( i ); continue; }
		const int localX = entry.cluster.x - parentCluster.x * 2;
		const int localY = entry.cluster.y - parentCluster.y * 2;
		const int child = CLAMP( localY, 0, 1 ) * 2 + CLAMP( localX, 0, 1 );
		m_entries[( size_t ) parent].children[child] = i;
	}
}

void T3DHLODStreamer::compileLegacyNodePolicies( const Ref<T3DHLODManifest> &rManifest ) {
	m_nodePolicies.clear();
	m_nodePolicyOffsets.fill( 0u );
	const PackedInt32Array nodeRegions = rManifest->getNodeRegionIndices();
	const PackedInt64Array nodeIds = rManifest->getNodeSpawnIds();
	const PackedByteArray nodeLevels = rManifest->getNodeMinLevels();
	const int nodeCount = MIN( nodeRegions.size(), MIN( nodeIds.size(), nodeLevels.size() ) );
	struct IndexedPolicy { int region = 0; t3dnodePolicy_t policy; };
	eastl::vector<IndexedPolicy> policies;
	policies.reserve( ( size_t ) nodeCount );
	for ( int i = 0; i < nodeCount; ++i ) {
		const int region = nodeRegions[i];
		if ( region < 0 || region >= TERRAIN3D_MAX_REGION_SLOTS || nodeLevels[i] == 0 ) continue;
		policies.push_back( {region, {nodeIds[i], nodeLevels[i]}} );
	}
	eastl::sort( policies.begin(), policies.end(), []( const IndexedPolicy &a, const IndexedPolicy &b ) {
		return a.region < b.region || ( a.region == b.region && a.policy.spawnId < b.policy.spawnId );
	} );
	m_nodePolicies.reserve( policies.size() );
	for ( const IndexedPolicy &indexed : policies ) {
		++m_nodePolicyOffsets[( size_t ) indexed.region + 1u];
		m_nodePolicies.push_back( indexed.policy );
	}
	for ( int region = 0; region < TERRAIN3D_MAX_REGION_SLOTS; ++region ) {
		m_nodePolicyOffsets[( size_t ) region + 1u] += m_nodePolicyOffsets[( size_t ) region];
	}
}

bool T3DHLODStreamer::compilePackedManifest( const Ref<T3DHLODManifest> &rManifest ) {
	const PackedByteArray blob = rManifest->getRuntimeBlob();
	if ( blob.is_empty() ) return false;
	T3DHLODRuntimeData runtime;
	if ( !T3DHLODRuntimeCodec::decode( blob, runtime ) ) return false;
	m_entries.reserve( runtime.entries.size() );
	for ( const T3DHLODRuntimeEntryRecord &source : runtime.entries ) {
		t3dentry_t entry;
		entry.level = source.level; entry.cluster = source.cluster; entry.regionMin = source.regionMin; entry.regionMax = source.regionMax;
		entry.localBounds = source.localBounds; entry.packagePath = source.packagePath; entry.geometricError = source.geometricError;
		entry.sourceSignature = source.sourceSignature; entry.buildSignature = source.buildSignature; entry.estimatedGpuBytes = source.estimatedGpuBytes;
		entry.parent = source.parent; for ( int child = 0; child < 4; ++child ) entry.children[child] = source.children[child];
		m_entries.push_back( entry );
		for ( int z = entry.regionMin.y; z <= entry.regionMax.y; ++z ) for ( int x = entry.regionMin.x; x <= entry.regionMax.x; ++x ) {
			const int index = regionIndex( Vector2i( x, z ) ); if ( index >= 0 ) m_managedRegions.set( ( size_t ) index );
		}
	}
	m_roots = eastl::move( runtime.roots );
	m_instancerMinLevels = eastl::move( runtime.instancerMinLevels );
	m_nodePolicies.reserve( runtime.nodePolicies.size() );
	for ( const T3DHLODRuntimeNodePolicyRecord &policy : runtime.nodePolicies ) m_nodePolicies.push_back( {policy.spawnId, policy.minLevel} );
	m_nodePolicyOffsets = runtime.nodePolicyOffsets;
	m_manifest = rManifest;
	return true;
}

bool T3DHLODStreamer::compileManifest( const Ref<T3DHLODManifest> &rManifest ) {
	if ( rManifest.is_null() || !m_pTerrain || rManifest->getFormat() != 1 ||
		rManifest->getRegionMapSize() != TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ||
		rManifest->getRegionSize() != ( int ) m_pTerrain->getRegionSize() ||
		!Math::is_equal_approx( rManifest->getVertexSpacing(), m_pTerrain->getVertexSpacing() ) ) {
		return false;
	}
	if ( compilePackedManifest( rManifest ) ) return true;
	if ( !m_pTerrain->isEditorAuthoringMode() ) return false;

	const Array &entries = rManifest->getEntriesInternal();
	m_entries.reserve( ( size_t ) entries.size() );
	for ( int i = 0; i < entries.size(); ++i ) {
		if ( Variant( entries[i] ).get_type() != Variant::DICTIONARY ) continue;
		const Dictionary data = entries[i];
		if ( !data.has( "level" ) || !data.has( "cluster" ) || !data.has( "region_min" ) || !data.has( "region_max" ) || !data.has( "package_path" ) ) continue;
		t3dentry_t entry;
		entry.level = CLAMP( ( int ) data["level"], 1, 6 ); entry.cluster = data["cluster"]; entry.regionMin = data["region_min"]; entry.regionMax = data["region_max"];
		entry.localBounds = data.get( "local_bounds", AABB() ); entry.packagePath = String( data["package_path"] ); entry.geometricError = MAX( ( real_t ) data.get( "geometric_error", 1.0 ), 0.001f );
		entry.sourceSignature = ( int64_t ) data.get( "source_signature", 0 ); entry.buildSignature = ( int64_t ) data.get( "build_signature", 0 ); entry.estimatedGpuBytes = MAX<int64_t>( ( int64_t ) data.get( "estimated_gpu_bytes", 0 ), 0 );
		m_entries.push_back( entry );
		for ( int z = entry.regionMin.y; z <= entry.regionMax.y; ++z ) for ( int x = entry.regionMin.x; x <= entry.regionMax.x; ++x ) { const int index = regionIndex( Vector2i( x, z ) ); if ( index >= 0 ) m_managedRegions.set( ( size_t ) index ); }
	}
	const PackedByteArray meshPolicy = rManifest->getInstancerMinLevels();
	const int expected = TERRAIN3D_MAX_REGION_SLOTS * TERRAIN3D_MAX_MESH_ASSETS;
	m_instancerMinLevels.resize( ( size_t ) expected );
	for ( int i = 0; i < expected; ++i ) m_instancerMinLevels[( size_t ) i] = i < meshPolicy.size() ? meshPolicy[i] : 0;
	compileLegacyNodePolicies( rManifest );
	buildHierarchy();
	m_manifest = rManifest;
	return true;
}

bool T3DHLODStreamer::reloadManifest( void ) {
	clearRuntime( true );
	m_manifest.unref();
	if ( !m_bEnabled || !m_pTerrain || m_manifestPath.is_empty() || !ResourceLoader::get_singleton()->exists( m_manifestPath ) ) { return false; }
	const Ref<Resource> resource = ResourceLoader::get_singleton()->load( m_manifestPath, "T3DHLODManifest", ResourceLoader::CACHE_MODE_REPLACE );
	const Ref<T3DHLODManifest> manifest = resource;
	return compileManifest( manifest );
}

bool T3DHLODStreamer::packageAcceptable( const t3dentry_t &rEntry, const Vector3 &rLocalTarget ) const {
	if ( rEntry.level > m_nMaxLevel ) { return false; }
	if ( m_nForcedDebugLevel >= 0 ) { return rEntry.level == m_nForcedDebugLevel; }
	const real_t regionWorld = ( real_t ) m_pTerrain->getRegionSize() * m_pTerrain->getVertexSpacing();
	const Vector3 clusterOrigin( rEntry.regionMin.x * regionWorld, 0.0f, rEntry.regionMin.y * regionWorld );
	const AABB worldBounds( clusterOrigin + rEntry.localBounds.position, rEntry.localBounds.size );
	const Vector3 nearest(
		CLAMP( rLocalTarget.x, worldBounds.position.x, worldBounds.get_end().x ),
		CLAMP( rLocalTarget.y, worldBounds.position.y, worldBounds.get_end().y ),
		CLAMP( rLocalTarget.z, worldBounds.position.z, worldBounds.get_end().z ) );
	const real_t distance = MAX( rLocalTarget.distance_to( nearest ), 1.0f );
	real_t projectionScale = 540.0f;
	real_t projectedError = 0.0f;
	if ( Camera3D *camera = m_pTerrain->getCamera() ) {
		real_t height = 1080.0f;
		if ( camera->get_viewport() ) { height = MAX( camera->get_viewport()->get_visible_rect().size.y, 1.0f ); }
		if ( camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL ) {
			projectedError = rEntry.geometricError * height / MAX( camera->get_size(), 0.001f );
		} else {
			projectionScale = height / MAX( 2.0f * std::tan( Math::deg_to_rad( camera->get_fov() ) * 0.5f ), 0.001f );
			projectedError = rEntry.geometricError * projectionScale / distance;
		}
	} else {
		projectedError = rEntry.geometricError * projectionScale / distance;
	}
	const real_t threshold = rEntry.active ? m_fTargetErrorPixels * ( 1.0f + m_fHysteresis ) : m_fTargetErrorPixels * MAX( 0.05f, 1.0f - m_fHysteresis );
	return projectedError <= threshold;
}

void T3DHLODStreamer::selectFallbackChildren( const int index, const Vector3 &rLocalTarget ) {
	const t3dentry_t &entry = m_entries[( size_t ) index];
	// A leaf has no HLOD fallback. Leaving it undesired deliberately publishes
	// detailed residency while the still-active proxy remains visible until the
	// detailed publishers acknowledge readiness. Re-marking an active leaf as
	// desired here would permanently trap the world at HLOD1 after first use.
	for ( const int child : entry.children ) {
		if ( child >= 0 ) {
			selectRecursive( child, rLocalTarget );
		}
	}
}
void T3DHLODStreamer::clearDesiredDescendants( const int index ) {
	if ( index < 0 || index >= ( int ) m_entries.size() ) { return; }
	for ( const int child : m_entries[( size_t ) index].children ) {
		if ( child < 0 ) { continue; }
		m_entries[( size_t ) child].desired = false;
		clearDesiredDescendants( child );
	}
}

bool T3DHLODStreamer::hasActiveDescendant( const int index ) const {
	if ( index < 0 || index >= ( int ) m_entries.size() ) { return false; }
	for ( const int child : m_entries[( size_t ) index].children ) {
		if ( child < 0 ) { continue; }
		if ( m_entries[( size_t ) child].active || hasActiveDescendant( child ) ) { return true; }
	}
	return false;
}

bool T3DHLODStreamer::replacementReady( const t3dentry_t &rEntry ) const {
	if ( !m_pTerrain ) { return true; }
	for ( int z = rEntry.regionMin.y; z <= rEntry.regionMax.y; ++z ) {
		for ( int x = rEntry.regionMin.x; x <= rEntry.regionMax.x; ++x ) {
			const Vector2i region( x, z );
			const int region_index = regionIndex( region );
			if ( region_index < 0 ) { continue; }
			// publishRepresentationMap contains only active, desired, non-retiring
			// replacements. A proxy at any level is therefore sufficient for this
			// region; otherwise the detailed publishers must acknowledge readiness.
			if ( m_regionRepresentationLevel[region_index] > 0 ) { continue; }
			if ( !m_pTerrain->isHlodDetailReady( region, region ) ) { return false; }
		}
	}
	return true;
}

void T3DHLODStreamer::selectRecursive( const int index, const Vector3 &rLocalTarget ) {
	if ( index < 0 || index >= ( int ) m_entries.size() ) { return; }
	t3dentry_t &entry = m_entries[( size_t ) index];
	const bool preferred = !entry.loadFailed && packageAcceptable( entry, rLocalTarget );
	if ( preferred ) {
		entry.desired = true;
		if ( entry.active ) { return; }
		// Until the preferred parent is active, retain the best available child representation.
		selectFallbackChildren( index, rLocalTarget );
		return;
	}
	selectFallbackChildren( index, rLocalTarget );
}

bool T3DHLODStreamer::requestOrActivate( const int index ) {
	t3dentry_t &entry = m_entries[( size_t ) index];
	if ( entry.active || entry.loadFailed || entry.packagePath.is_empty() || !m_pTerrain || !m_pTerrain->is_inside_tree() ) { return entry.active; }
	ResourceLoader *loader = ResourceLoader::get_singleton();
	if ( !loader ) { return false; }
	ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status( entry.packagePath );
	if ( status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE && !entry.requestStarted ) {
		const Error error = loader->load_threaded_request( entry.packagePath, "T3DHLODPackage", true, ResourceLoader::CACHE_MODE_REUSE );
		entry.requestStarted = error == OK;
		entry.loadFailed = error != OK;
		if ( error == OK ) { ++m_nRequestsLastTick; } else { ++m_nFailuresLastTick; }
		return false;
	}
	if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS || status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE ) { return false; }
	if ( status != ResourceLoader::THREAD_LOAD_LOADED ) { entry.loadFailed = true; ++m_nFailuresLastTick; return false; }
	const Ref<Resource> loaded = loader->load_threaded_get( entry.packagePath );
	entry.package = loaded;
	if ( entry.package.is_null() || entry.package->getMesh().is_null() ||
		entry.package->getLevel() != entry.level ||
		entry.package->getClusterLocation() != entry.cluster ||
		entry.package->getRegionMin() != entry.regionMin ||
		entry.package->getRegionMax() != entry.regionMax ||
		entry.package->getSourceSignature() != entry.sourceSignature ||
		entry.package->getBuildSignature() != entry.buildSignature ) {
		entry.package.unref();
		entry.loadFailed = true;
		++m_nFailuresLastTick;
		return false;
	}
	const int64_t bytes = MAX<int64_t>( entry.estimatedGpuBytes, entry.package->getEstimatedGpuBytes() );
	const int64_t budget = ( int64_t ) m_nMemoryBudgetMb * 1024LL * 1024LL;
	if ( bytes > budget ) { ++m_nCapacityDeferredLastTick; return false; }
	if ( m_nResidentEstimatedBytes > budget - bytes ) {
		// No-hole parent promotion sometimes requires the incoming parent and its
		// active fallback descendants to coexist briefly. Permit one such
		// transition package above the steady-state budget; never overcommit for
		// unrelated residency or while a previous transition is still over budget.
		if ( m_nResidentEstimatedBytes > budget || m_nTransitionOvercommitBytesLastTick > 0 ||
			m_nUnloadBudget <= 0 || !hasActiveDescendant( index ) ) {
			++m_nCapacityDeferredLastTick;
			return false;
		}
		m_nTransitionOvercommitBytesLastTick = m_nResidentEstimatedBytes + bytes - budget;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if ( !rs || m_pTerrain->get_world_3d().is_null() ) { return false; }
	entry.instance = rs->instance_create();
	rs->instance_set_base( entry.instance, entry.package->getMesh()->get_rid() );
	rs->instance_set_scenario( entry.instance, m_pTerrain->get_world_3d()->get_scenario() );
	const real_t regionWorld = ( real_t ) m_pTerrain->getRegionSize() * m_pTerrain->getVertexSpacing();
	Transform3D local; local.origin = Vector3( entry.regionMin.x * regionWorld, 0.0f, entry.regionMin.y * regionWorld );
	rs->instance_set_transform( entry.instance, m_pTerrain->get_global_transform() * local );
	rs->instance_geometry_set_cast_shadows_setting( entry.instance, entry.level <= m_nMaxShadowLevel ? RenderingServer::SHADOW_CASTING_SETTING_ON : RenderingServer::SHADOW_CASTING_SETTING_OFF );
	entry.active = true; entry.retiring = false; entry.retireReadyFrames = 0;
	m_bSelectionDirty = true;
	// Fallback descendants were marked desired only while this proxy was not
	// ready. Once the replacement exists they must immediately become retirement
	// candidates instead of rendering on top of their parent for another tick.
	clearDesiredDescendants( index );
	m_nResidentEstimatedBytes += bytes;
	++m_nLoadsLastTick;
	return true;
}
void T3DHLODStreamer::deactivate( const int index ) {
	if ( index < 0 || index >= ( int ) m_entries.size() ) { return; }
	t3dentry_t &entry = m_entries[( size_t ) index];
	if ( entry.instance.is_valid() ) { RenderingServer::get_singleton()->free_rid( entry.instance ); entry.instance = RID(); ++m_nUnloadsLastTick; }
	if ( entry.active ) {
		const int64_t bytes = entry.package.is_valid() ? MAX<int64_t>( entry.estimatedGpuBytes, entry.package->getEstimatedGpuBytes() ) : entry.estimatedGpuBytes;
		m_nResidentEstimatedBytes = MAX<int64_t>( m_nResidentEstimatedBytes - MAX<int64_t>( bytes, 0 ), 0 );
	}
	entry.active = false; entry.retiring = false; entry.retireReadyFrames = 0; entry.package.unref(); entry.requestStarted = false;
	m_bSelectionDirty = true;
}

void T3DHLODStreamer::publishRepresentationMap( void ) {
	uint8_t old[TERRAIN3D_MAX_REGION_SLOTS]; std::memcpy( old, m_regionRepresentationLevel, sizeof( old ) );
	std::memset( m_regionRepresentationLevel, 0, sizeof( m_regionRepresentationLevel ) );
	m_detailRequiredRegions = m_managedRegions;
	for ( const t3dentry_t &entry : m_entries ) {
		if ( !entry.active || entry.retiring || !entry.desired ) { continue; }
		for ( int z = entry.regionMin.y; z <= entry.regionMax.y; ++z ) for ( int x = entry.regionMin.x; x <= entry.regionMax.x; ++x ) {
			const int region = regionIndex( Vector2i( x, z ) );
			if ( region >= 0 ) { m_regionRepresentationLevel[region] = MAX<uint8_t>( m_regionRepresentationLevel[region], ( uint8_t ) entry.level ); m_detailRequiredRegions.reset( ( size_t ) region ); }
		}
	}
	notifyRepresentationChanges( old );
}
void T3DHLODStreamer::notifyRepresentationChanges( const uint8_t *pOldLevels ) {
	if ( !m_pTerrain ) { return; }
	for ( int i = 0; i < TERRAIN3D_MAX_REGION_SLOTS; ++i ) {
		if ( pOldLevels[i] != m_regionRepresentationLevel[i] ) { m_pTerrain->onHlodRegionRepresentationChanged( regionFromIndex( i ) ); }
	}
}

void T3DHLODStreamer::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
	m_nLoadsLastTick = m_nUnloadsLastTick = m_nRequestsLastTick = m_nFailuresLastTick = m_nCapacityDeferredLastTick = 0;
	m_nTransitionOvercommitBytesLastTick = 0;
	if ( !m_bEnabled || !m_pTerrain || m_manifest.is_null() ) { suspend(); return; }

	const Vector3 localTarget = m_pTerrain->get_global_transform().affine_inverse().xform( rTargetPosition );
	real_t projectionValue = 0.0f;
	real_t viewportHeight = 1080.0f;
	int projection = -1;
	if ( Camera3D *camera = m_pTerrain->getCamera() ) {
		projection = ( int ) camera->get_projection();
		projectionValue = projection == ( int ) Camera3D::PROJECTION_ORTHOGONAL ? camera->get_size() : camera->get_fov();
		if ( camera->get_viewport() ) { viewportHeight = MAX( camera->get_viewport()->get_visible_rect().size.y, 1.0f ); }
	}
	const bool selectionContextChanged = !m_bHasSelectionContext ||
		localTarget != m_lastSelectionTarget ||
		projection != m_nLastSelectionProjection ||
		!Math::is_equal_approx( projectionValue, m_fLastSelectionProjectionValue ) ||
		!Math::is_equal_approx( viewportHeight, m_fLastSelectionViewportHeight );
	const bool rebuildSelection = bForce || m_bSelectionDirty || selectionContextChanged;

	if ( rebuildSelection ) {
		for ( t3dentry_t &entry : m_entries ) { entry.desired = false; }
		for ( const int root : m_roots ) { selectRecursive( root, localTarget ); }
		m_lastSelectionTarget = localTarget;
		m_fLastSelectionProjectionValue = projectionValue;
		m_fLastSelectionViewportHeight = viewportHeight;
		m_nLastSelectionProjection = projection;
		m_bHasSelectionContext = true;
		m_bSelectionDirty = false;
	} else {
		bool hasTransitionWork = false;
		for ( const t3dentry_t &entry : m_entries ) {
			if ( ( entry.desired && !entry.active && !entry.loadFailed ) || entry.retiring || ( entry.active && !entry.desired ) ) {
				hasTransitionWork = true;
				break;
			}
		}
		if ( !hasTransitionWork ) { return; }
	}

	bool representationDirty = rebuildSelection;
	int loadBudget = m_nLoadBudget;
	for ( int i = 0; i < ( int ) m_entries.size() && loadBudget > 0; ++i ) {
		t3dentry_t &entry = m_entries[( size_t ) i];
		if ( entry.desired && !entry.active && !entry.loadFailed ) {
			const bool wasRequested = entry.requestStarted;
			const bool active = requestOrActivate( i );
			if ( active ) { representationDirty = true; }
			if ( active || ( !wasRequested && entry.requestStarted ) ) { --loadBudget; }
		}
	}

	// Mark retirement/cancellation first, then publish once. The representation
	// map must exclude newly retiring proxies before replacementReady() is tested.
	for ( t3dentry_t &entry : m_entries ) {
		if ( !entry.active || entry.desired ) {
			if ( entry.retiring ) { representationDirty = true; }
			entry.retiring = false;
			entry.retireReadyFrames = 0;
			continue;
		}
		if ( !entry.retiring ) {
			entry.retiring = true;
			entry.retireReadyFrames = 0;
			representationDirty = true;
		}
	}
	if ( representationDirty ) { publishRepresentationMap(); }

	int unloadBudget = m_nUnloadBudget;
	for ( int i = 0; i < ( int ) m_entries.size() && unloadBudget > 0; ++i ) {
		t3dentry_t &entry = m_entries[( size_t ) i];
		if ( !entry.active || !entry.retiring ) { continue; }
		if ( replacementReady( entry ) ) {
			entry.retireReadyFrames = MIN<uint8_t>( ( uint8_t ) ( entry.retireReadyFrames + 1 ), 3 );
			if ( entry.retireReadyFrames >= 2 ) { deactivate( i ); --unloadBudget; }
		} else {
			entry.retireReadyFrames = 0;
		}
	}
}

uint8_t T3DHLODStreamer::getNodeMinimumLevel( const Vector2i &rRegion, const int64_t nSpawnId ) const {
	const int region = regionIndex( rRegion ); if ( region < 0 ) return 0;
	const uint32_t begin = m_nodePolicyOffsets[( size_t ) region];
	const uint32_t end = m_nodePolicyOffsets[( size_t ) region + 1u];
	for ( uint32_t index = begin; index < end && index < m_nodePolicies.size(); ++index ) {
		const t3dnodePolicy_t &policy = m_nodePolicies[index]; if ( policy.spawnId == nSpawnId ) return policy.minLevel;
	}
	return 0;
}
Dictionary T3DHLODStreamer::getStats( void ) const {
	Dictionary d; int active = 0, desired = 0, failed = 0, retiring = 0;
	for ( const t3dentry_t &e : m_entries ) { active += e.active; desired += e.desired; failed += e.loadFailed; retiring += e.retiring; }
	d["entries"] = ( int ) m_entries.size(); d["active"] = active; d["desired"] = desired; d["failed"] = failed; d["retiring"] = retiring;
	d["loads_last_tick"] = m_nLoadsLastTick; d["unloads_last_tick"] = m_nUnloadsLastTick; d["requests_last_tick"] = m_nRequestsLastTick; d["failures_last_tick"] = m_nFailuresLastTick; d["capacity_deferred_last_tick"] = m_nCapacityDeferredLastTick;
	d["resident_estimated_bytes"] = m_nResidentEstimatedBytes; d["memory_budget_mb"] = m_nMemoryBudgetMb; d["transition_overcommit_bytes_last_tick"] = m_nTransitionOvercommitBytesLastTick; d["managed_regions"] = ( int ) m_managedRegions.count(); d["detail_required_regions"] = ( int ) m_detailRequiredRegions.count();
	return d;
}
Array T3DHLODStreamer::getDebugEntries( void ) const {
	Array out;
	for ( const t3dentry_t &e : m_entries ) { Dictionary d; d["level"] = e.level; d["cluster"] = e.cluster; d["region_min"] = e.regionMin; d["region_max"] = e.regionMax; d["local_bounds"] = e.localBounds; d["package_path"] = e.packagePath; d["desired"] = e.desired; d["active"] = e.active; d["retiring"] = e.retiring; d["load_failed"] = e.loadFailed; d["request_started"] = e.requestStarted; out.push_back( d ); }
	return out;
}
void T3DHLODStreamer::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "reload_manifest" ), &T3DHLODStreamer::reloadManifest );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &T3DHLODStreamer::getStats );
	ClassDB::bind_method( D_METHOD( "get_debug_entries" ), &T3DHLODStreamer::getDebugEntries );
	ClassDB::bind_method( D_METHOD( "is_region_suppressed", "region", "minimum_level" ), &T3DHLODStreamer::isRegionSuppressed, DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "get_region_representation_level", "region" ), &T3DHLODStreamer::getRegionRepresentationLevel );
}
