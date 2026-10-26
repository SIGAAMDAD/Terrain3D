// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypeSupport.h"
#include "physics/runtime/terrain_3d_collision.h"

#include "Terrain3DConstants.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_util.h"
#include "data/model/terrain_3d_data.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "data/sampling/terrain_3d_sampler.h"
#include "terrain_3d.h"

#include <math.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/height_map_shape3d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

// -----------------------------------------------------------------------------
// Private functions

bool Terrain3DCollision::isAuthoringFullResidencyMode( void ) const {
	return IS_EDITOR && m_pTerrain && m_pTerrain->getEditorExecutionMode() == EDITOR_EXECUTION_AUTHORING;
}

bool Terrain3DCollision::usesEditorBackend( void ) const {
	return IS_EDITOR && ( isEditorMode() || isAuthoringFullResidencyMode() );
}

bool Terrain3DCollision::usesDynamicShapePool( void ) const {
	return isDynamicMode() && !isAuthoringFullResidencyMode();
}

bool Terrain3DCollision::regionAllowsCollision( const Vector2i &rRegionLoc ) const {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || rRegionLoc == s_v2iMax ) {
		return false;
	}
	if ( isAuthoringFullResidencyMode() ) {
		return m_pTerrain->dataObject()->hasRegion( rRegionLoc );
	}
	return m_pTerrain->getRegionStreamingStateInternal( rRegionLoc ) >= TERRAIN3D_REGION_STREAMING_HOT;
}

bool Terrain3DCollision::shapeAllowsCollision( const Vector2i &rPosition, const int nSize ) const {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return false;
	}
	if ( isAuthoringFullResidencyMode() ) {
		return true;
	}
	const int region_size = T3D_REGION_SIZE;
	if ( region_size <= 0 ) {
		return false;
	}
	const int min_region_x = ( int )( Math::floor( ( double )( rPosition.x ) / ( double )( region_size ) ) );
	const int min_region_z = ( int )( Math::floor( ( double )( rPosition.y ) / ( double )( region_size ) ) );
	const int max_region_x = ( int )( Math::floor( ( double )( rPosition.x + nSize ) / ( double )( region_size ) ) );
	const int max_region_z = ( int )( Math::floor( ( double )( rPosition.y + nSize ) / ( double )( region_size ) ) );
	for ( int z = min_region_z; z <= max_region_z; ++z ) {
		for ( int x = min_region_x; x <= max_region_x; ++x ) {
			const Vector2i region( x, z );
			if ( m_pTerrain->dataObject()->hasRegion( region ) && !regionAllowsCollision( region ) ) {
				return false;
			}
		}
	}
	return true;
}

// Calculates shape data from a top-left, descaled, grid-snapped position.
// The caller selects either a dynamic record buffer or the shared full-collision
// upload scratch buffer.
bool Terrain3DCollision::canUseBakedDynamicCollision( void ) const {
	return usesDynamicShapePool() && m_pTerrain && m_pTerrain->dataObject() && m_pTerrain->isGpuRegionCookedPagesEnabled()
	        && !( IS_EDITOR && m_pTerrain->getEditorExecutionMode() == EDITOR_EXECUTION_AUTHORING );
}

int Terrain3DCollision::findBakedRegionCache( const Vector2i &rLocation ) const {
	const int map_index = Terrain3DData::getRegionMapIndex( rLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int index = m_bakedRegionByMap[( size_t )( map_index )];
	return index >= 0 && index < m_nBakedRegionCacheCount && m_bakedRegionCache[( size_t )( index )].m_location == rLocation ? index : -1;
}

void Terrain3DCollision::clearBakedRegionCache( const bool bReleasePayloads ) {
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		m_bakedRegionByMap[i] = ( int16_t )( -1 );
	}
	if ( bReleasePayloads ) {
		for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_bakedRegionCache ); iRegion++ ) {
			t3dCollisionRegionCacheEntry_t &entry = ( m_bakedRegionCache )[iRegion];
			entry = t3dCollisionRegionCacheEntry_t{};
		}
	} else {
		for ( int index = 0; index < m_nBakedRegionCacheCount; ++index ) {
			m_bakedRegionCache[( size_t )( index )].m_location = s_v2iMax;
			m_bakedRegionCache[( size_t )( index )].m_nWidth = 0;
			m_bakedRegionCache[( size_t )( index )].m_nLastUseTick = 0u;
		}
	}
	m_nBakedRegionCacheCount = 0;
}

void Terrain3DCollision::eraseBakedRegionCacheAt( const int nIndex ) {
	if ( nIndex < 0 || nIndex >= m_nBakedRegionCacheCount ) {
		return;
	}
	t3dCollisionRegionCacheEntry_t &dead = m_bakedRegionCache[( size_t )( nIndex )];
	const int dead_map_index = Terrain3DData::getRegionMapIndex( dead.m_location );
	if ( dead_map_index >= 0 ) {
		m_bakedRegionByMap[( size_t )( dead_map_index )] = -1;
	}
	const int last = m_nBakedRegionCacheCount - 1;
	if ( nIndex != last ) {
		eastl::swap( m_bakedRegionCache[( size_t )( nIndex )], m_bakedRegionCache[( size_t )( last )] );
		const int moved_map_index = Terrain3DData::getRegionMapIndex( m_bakedRegionCache[( size_t )( nIndex )].m_location );
		if ( moved_map_index >= 0 ) {
			m_bakedRegionByMap[( size_t )( moved_map_index )] = ( int16_t )nIndex;
		}
	}
	// The now-inactive tail retains its height allocation for the next cache fill.
	m_bakedRegionCache[( size_t )( last )].m_location = s_v2iMax;
	m_bakedRegionCache[( size_t )( last )].m_nWidth = 0;
	m_bakedRegionCache[( size_t )( last )].m_nLastUseTick = 0u;
	--m_nBakedRegionCacheCount;
}

void Terrain3DCollision::refreshBakedRegionCache( const Vector2i &rCenter ) {
	if ( !canUseBakedDynamicCollision() ) {
		clearBakedRegionCache( false );
		return;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const int region_size = T3D_REGION_SIZE;
	if ( region_size <= 0 ) {
		clearBakedRegionCache( false );
		return;
	}
	++m_nBakedRegionCacheTick;
	const int margin = ( int )( m_nRadius ) + ( int )( m_nShapeSize ) + region_size;
	const Vector2i min_region = V2I_DIVIDE_FLOOR( rCenter - V2I( margin ), region_size );
	const Vector2i max_region = V2I_DIVIDE_FLOOR( rCenter + V2I( margin ), region_size );

	for ( int index = m_nBakedRegionCacheCount - 1; index >= 0; --index ) {
		const Vector2i location = m_bakedRegionCache[( size_t )( index )].m_location;
		const Terrain3DRegion *const pRegion = pData->getRegionPtr( location );
		const bool outside =
		        location.x < min_region.x || location.x > max_region.x || location.y < min_region.y || location.y > max_region.y;
		if ( outside || !pRegion || pRegion->isDeleted() || pRegion->isModified() ) {
			eraseBakedRegionCacheAt( index );
		}
	}

	for ( int z = min_region.y; z <= max_region.y; ++z ) {
		for ( int x = min_region.x; x <= max_region.x; ++x ) {
			const Vector2i location( x, z );
			Terrain3DRegion *const pRegion = pData->getRegionPtr( location );
			if ( !pRegion || pRegion->isDeleted() || pRegion->isModified() || !regionAllowsCollision( location ) ) {
				continue;
			}
			const int cached_index = findBakedRegionCache( location );
			if ( cached_index >= 0 ) {
				m_bakedRegionCache[( size_t )( cached_index )].m_nLastUseTick = m_nBakedRegionCacheTick;
				continue;
			}
			if ( m_nBakedRegionCacheCount >= T3D_BAKED_CACHE_CAPACITY ) {
				LOG( ERROR, "Baked collision region cache exceeded fixed geometric capacity" );
				return;
			}
			pData->requestCookedCollisionTile( location, 1u );
			PackedByteArray height_blob;
			PackedByteArray bounds_blob;
			PackedByteArray hole_blob;
			if ( !pData->takeCookedCollisionTile( location, height_blob, bounds_blob, hole_blob ) ) {
				continue;
			}
			t3dCollisionRegionCacheEntry_t &entry = m_bakedRegionCache[( size_t )( m_nBakedRegionCacheCount )];
			int width = 0;
			float min_height = 0.0f;
			float max_height = 0.0f;
			int bounds_width = 0;
			if ( !Terrain3DRuntimeBakeCodec::decodeCollisionHeight( height_blob, entry.m_heights, width )
			        || !Terrain3DRuntimeBakeCodec::decodeCollisionBounds( bounds_blob, min_height, max_height, bounds_width )
			        || width != region_size + 1 || bounds_width != width ) {
				continue;
			}
			entry.m_location = location;
			entry.m_nWidth = width;
			entry.m_nLastUseTick = m_nBakedRegionCacheTick;
			const int map_index = Terrain3DData::getRegionMapIndex( location );
			if ( map_index >= 0 ) {
				m_bakedRegionByMap[( size_t )( map_index )] = ( int16_t )m_nBakedRegionCacheCount;
			}
			++m_nBakedRegionCacheCount;
		}
	}
}

bool Terrain3DCollision::copyBakedTile(
        const Vector2i &rPosition, const int nSize, t3dCollisionShapeRecord_t &rRecord, PackedRealArray &rHeights ) {
	if ( !canUseBakedDynamicCollision() ) {
		return false;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const int region_size = T3D_REGION_SIZE;
	const int width = nSize + 1;
	const int sample_count = width * width;
	if ( width <= 0 || width > T3D_MAX_COLLISION_AXIS_SAMPLES ) {
		return false;
	}
	if ( rHeights.size() != sample_count ) {
		LOG( ERROR, "Baked collision height capacity mismatch; collision rebuild required" );
		return false;
	}
	real_t *const output = rHeights.ptrw();
	float min_height = FLT_MAX;
	float max_height = -FLT_MAX;

	for ( int x = 0; x < width; ++x ) {
		const int world_x = rPosition.x + x;
		for ( int z = 0; z < width; ++z ) {
			const int world_z = rPosition.y + z;
			const Vector2i region_location = V2I_DIVIDE_FLOOR( Vector2i( world_x, world_z ), region_size );
			const Terrain3DRegion *const pRegion = pData->getRegionPtr( region_location );
			if ( !pRegion || pRegion->isDeleted() || pRegion->isModified() ) {
				++m_nBakedCacheMissesLastUpdate;
				return false;
			}
			const int cache_index = findBakedRegionCache( region_location );
			if ( cache_index < 0 ) {
				pData->requestCookedCollisionTile( region_location, 0u );
				++m_nBakedCacheMissesLastUpdate;
				return false;
			}
			t3dCollisionRegionCacheEntry_t &entry = m_bakedRegionCache[( size_t )( cache_index )];
			entry.m_nLastUseTick = m_nBakedRegionCacheTick;
			const int local_x = world_x - region_location.x * region_size;
			const int local_z = world_z - region_location.y * region_size;
			if ( local_x < 0 || local_x >= entry.m_nWidth || local_z < 0 || local_z >= entry.m_nWidth ) {
				++m_nBakedCacheMissesLastUpdate;
				return false;
			}
			const float height = entry.m_heights[local_x * entry.m_nWidth + ( entry.m_nWidth - 1 - local_z )];
			const int destination = x * width + ( width - 1 - z );
			output[destination] = ( real_t )( height );
			if ( !isnan( height ) ) {
				min_height = MIN( min_height, height );
				max_height = MAX( max_height, height );
			}
		}
	}
	if ( min_height == FLT_MAX ) {
		return false;
	}

	t3dCollisionShapeData_t &data = rRecord.m_data;
	data.m_nWidth = width;
	data.m_nDepth = width;
	data.m_transform = Transform3D( Basis( s_v3Up, Math::PI * 0.5f ), t3dVector2iToVector3( rPosition + V2I( nSize / 2 ) ) );
	data.m_fMinHeight = min_height;
	data.m_fMaxHeight = max_height;
	data.m_bValid = true;
	++m_nBakedCacheHitsLastUpdate;
	return true;
}

// Calculates shape data from a top-left, descaled, grid-snapped position.
// This is the canonical live fallback used by both dynamic and full collision.
bool Terrain3DCollision::getShapeData(
        const Vector2i &rPosition, const int nSize, t3dCollisionShapeRecord_t &rRecord, PackedRealArray &rHeights ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) { LOG( ERROR, "Terrain not initialized" ); return false; }
	const Ref<Terrain3DMaterial> material = m_pTerrain->materialResource();
	float min_height = 0.0f;
	float max_height = 0.0f;
	if ( !m_collisionSurfaceBuilder.build(
	             m_pTerrain->dataObject(), material, rPosition, nSize, m_collisionSurfaceScratch, min_height, max_height ) ) {
		rRecord.m_data.m_bValid = false;
		return false;
	}
	if ( rHeights.size() != m_collisionSurfaceScratch.size() ) {
		LOG( ERROR, "Collision height staging capacity mismatch; collision rebuild required" );
		rRecord.m_data.m_bValid = false;
		return false;
	}
	real_t *const output = rHeights.ptrw();
	const float *const input = m_collisionSurfaceScratch.ptr();
	bool has_solid_sample = false;
	for ( int index = 0; index < m_collisionSurfaceScratch.size(); ++index ) {
		output[index] = ( real_t )( input[index] );
		has_solid_sample = has_solid_sample || !isnan( input[index] );
	}
	if ( !has_solid_sample ) {
		rRecord.m_data.m_bValid = false;
		return false;
	}
	const int width = nSize + 1;
	rRecord.m_data.m_nWidth = width;
	rRecord.m_data.m_nDepth = width;
	rRecord.m_data.m_transform = Transform3D( Basis( s_v3Up, Math::PI * 0.5f ), t3dVector2iToVector3( rPosition + V2I( nSize / 2 ) ) );
	rRecord.m_data.m_fMinHeight = min_height;
	rRecord.m_data.m_fMaxHeight = max_height;
	rRecord.m_data.m_bValid = true;
	return true;
}

void Terrain3DCollision::shapeSetDisabled( const int nShapeId, const bool bDisabled ) {
	if ( nShapeId < 0 || nShapeId >= ( int )( m_nShapeRecordCount ) ) {
		return;
	}
	t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( nShapeId )];
	if ( record.m_bDisabled == bDisabled ) {
		return;
	}
	record.m_bDisabled = bDisabled;
	if ( usesEditorBackend() ) {
		if ( !record.m_pNode ) {
			return;
		}
		record.m_pNode->set_disabled( bDisabled );
		record.m_pNode->set_visible( !bDisabled );
	} else if ( m_hStaticBodyRid.is_valid() ) {
		PS->body_set_shape_disabled( m_hStaticBodyRid, nShapeId, bDisabled );
	}
}

void Terrain3DCollision::shapeSetTransform( const int nShapeId, const Transform3D &rXform ) {
	if ( nShapeId < 0 || nShapeId >= ( int )( m_nShapeRecordCount ) ) {
		return;
	}
	t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( nShapeId )];
	if ( record.m_transform.is_equal_approx( rXform ) ) {
		return;
	}
	record.m_transform = rXform;
	if ( usesEditorBackend() ) {
		if ( record.m_pNode ) {
			record.m_pNode->set_transform( rXform );
		}
	} else if ( m_hStaticBodyRid.is_valid() ) {
		PS->body_set_shape_transform( m_hStaticBodyRid, nShapeId, rXform );
	}
}

Vector3 Terrain3DCollision::shapeGetPosition( const int nShapeId ) const {
	if ( nShapeId < 0 || nShapeId >= ( int )( m_nShapeRecordCount ) ) {
		return s_v3Max;
	}
	return m_szShapeRecords[( size_t )( nShapeId )].m_transform.origin;
}

void Terrain3DCollision::shapeSetData( const int nShapeId, const PackedRealArray &rHeights ) {
	if ( nShapeId < 0 || nShapeId >= ( int )( m_nShapeRecordCount ) ) {
		return;
	}
	t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( nShapeId )];
	if ( !record.m_data.m_bValid ) {
		return;
	}
	if ( usesEditorBackend() ) {
		if ( !record.m_pNode ) {
			return;
		}
		Ref<HeightMapShape3D> pHeightShape = record.m_pNode->get_shape();
		if ( pHeightShape.is_valid() ) {
			pHeightShape->set_map_data( rHeights );
		}
	} else if ( record.m_hRid.is_valid() ) {
		Dictionary upload_data;
		upload_data["width"] = record.m_data.m_nWidth;
		upload_data["depth"] = record.m_data.m_nDepth;
		upload_data["heights"] = rHeights;
		upload_data["min_height"] = record.m_data.m_fMinHeight;
		upload_data["max_height"] = record.m_data.m_fMaxHeight;
		PS->shape_set_data( record.m_hRid, upload_data );
	}
}

void Terrain3DCollision::deactivateShape( const int nShapeId ) {
	if ( nShapeId < 0 || nShapeId >= ( int )( m_nShapeRecordCount ) ) {
		return;
	}
	t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( nShapeId )];
	record.m_gridPosition = s_v2iMax;
	record.m_data.m_bValid = false;
	shapeSetDisabled( nShapeId, true );
	shapeSetTransform( nShapeId, Transform3D( Basis(), s_v3Max ) );
}

void Terrain3DCollision::reloadPhysicsMaterial( void ) {
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_physics_material_override( m_physicsMaterial );
		}
	} else {
		if ( m_hStaticBodyRid.is_valid() ) {
			if ( m_physicsMaterial.is_null() ) {
				PS->body_set_param( m_hStaticBodyRid, PhysicsServer3D::BODY_PARAM_BOUNCE, 0.f );
				PS->body_set_param( m_hStaticBodyRid, PhysicsServer3D::BODY_PARAM_FRICTION, 1.f );
			} else {
				const real_t fComputedBounce = m_physicsMaterial->get_bounce() * ( m_physicsMaterial->is_absorbent() ? -1.f : 1.f );
				const real_t fComputedFriction = m_physicsMaterial->get_friction() * ( m_physicsMaterial->is_rough() ? -1.f : 1.f );
				PS->body_set_param( m_hStaticBodyRid, PhysicsServer3D::BODY_PARAM_BOUNCE, fComputedBounce );
				PS->body_set_param( m_hStaticBodyRid, PhysicsServer3D::BODY_PARAM_FRICTION, fComputedFriction );
			}
		}
	}
	if ( m_physicsMaterial.is_valid() ) {
		LOG( DEBUG,
		        "Setting PhysicsMaterial bounce: ",
		        m_physicsMaterial->get_bounce(),
		        ", friction: ",
		        m_physicsMaterial->get_friction() );
	}
}

void Terrain3DCollision::updateFullShape( const int nShapeId, const Vector2i &rRegionLoc ) {
	++m_nFullRegionsUpdatedLastUpdate;
	if ( nShapeId < 0 || nShapeId >= ( int )( m_nShapeRecordCount ) ) {
		return;
	}
	if ( rRegionLoc == s_v2iMax ) {
		deactivateShape( nShapeId );
		++m_nShapesDeactivatedLastUpdate;
		return;
	}
	if ( !regionAllowsCollision( rRegionLoc ) ) {
		deactivateShape( nShapeId );
		++m_nShapesDeactivatedLastUpdate;
		return;
	}

	const int region_size = T3D_REGION_SIZE;
	const Vector2i shape_position = rRegionLoc * region_size;
	t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( nShapeId )];
	const bool strict_cooked = m_pTerrain->isGpuRegionCookedPagesEnabled()
	        && !( IS_EDITOR && m_pTerrain->getEditorExecutionMode() == EDITOR_EXECUTION_AUTHORING );
	bool shape_ready = false;
	const uint64_t preparation_started = Time::get_singleton()->get_ticks_usec();
	if ( strict_cooked ) {
		PackedByteArray height_blob;
		PackedByteArray bounds_blob;
		PackedByteArray hole_blob;
		int baked_width = 0;
		int bounds_width = 0;
		float min_height = 0.0f;
		float max_height = 0.0f;
		if ( m_pTerrain->dataObject()->loadCookedCollisionTileNow( rRegionLoc, height_blob, bounds_blob, hole_blob )
		        && Terrain3DRuntimeBakeCodec::decodeCollisionHeightInto( height_blob, m_fullBakedHeightScratch, baked_width )
		        && Terrain3DRuntimeBakeCodec::decodeCollisionBounds( bounds_blob, min_height, max_height, bounds_width )
		        && baked_width == region_size + 1 && bounds_width == baked_width ) {
			if ( m_fullHeightScratch.size() != m_fullBakedHeightScratch.size() ) {
				LOG( ERROR, "Full collision staging capacity mismatch; collision rebuild required" );
				return;
			}
			real_t *const pWrite = m_fullHeightScratch.ptrw();
			const float *const pRead = m_fullBakedHeightScratch.ptr();
			bool has_solid_sample = false;
			for ( int index = 0; index < m_fullBakedHeightScratch.size(); ++index ) {
				pWrite[index] = ( real_t )pRead[index];
				has_solid_sample = has_solid_sample || !isnan( pRead[index] );
			}
			if ( has_solid_sample ) {
				record.m_data.m_nWidth = baked_width;
				record.m_data.m_nDepth = baked_width;
				record.m_data.m_transform =
				        Transform3D( Basis( s_v3Up, Math::PI * 0.5f ), t3dVector2iToVector3( shape_position + V2I( region_size / 2 ) ) );
				record.m_data.m_fMinHeight = min_height;
				record.m_data.m_fMaxHeight = max_height;
				record.m_data.m_bValid = true;
				shape_ready = true;
			}
		}
	} else {
		shape_ready = getShapeData( shape_position, region_size, record, m_fullHeightScratch );
	}
	m_nPreparationUsecLast += Time::get_singleton()->get_ticks_usec() - preparation_started;
	if ( !shape_ready ) {
		if ( strict_cooked ) {
			LOG( ERROR, "Missing/invalid baked full-collision tile for region ", rRegionLoc );
		}
		deactivateShape( nShapeId );
		++m_nShapesDeactivatedLastUpdate;
		return;
	}

	Transform3D transform = record.m_data.m_transform;
	transform.scale( Vector3( s_fRegionVertexSpacing, 1.0f, s_fRegionVertexSpacing ) );
	record.m_gridPosition = shape_position;
	shapeSetTransform( nShapeId, transform );
	const uint64_t upload_started = Time::get_singleton()->get_ticks_usec();
	shapeSetData( nShapeId, m_fullHeightScratch );
	m_nUploadUsecLast += Time::get_singleton()->get_ticks_usec() - upload_started;
	shapeSetDisabled( nShapeId, false );
	++m_nShapesRebuiltLastUpdate;
	m_nHeightSamplesUploadedLastUpdate += m_fullHeightScratch.size();
}

// -----------------------------------------------------------------------------
// Public functions

void Terrain3DCollision::initialize( Terrain3DCollisionHost *const pTerrain ) {
	if ( pTerrain ) {
		m_pTerrain = pTerrain;
	} else {
		return;
	}
	if ( !IS_EDITOR && isEditorMode() ) {
		LOG( WARN, "Change collision mode to a non-editor mode for releases" );
	}
	build();
}

void Terrain3DCollision::onRegionMapChanged( void ) {
	// The region-map transition is serviced by Terrain3D::physicsProcess().
	// Collision no longer depends on renderer frame callbacks.
	m_bRegionMapUpdatePending = true;
}

void Terrain3DCollision::processRegionMapUpdate( void ) {
	m_bRegionMapUpdatePending = false;

	if ( !m_bInitialized ) {
		m_bColdRebuildPending = true;
		return;
	}

	if ( usesDynamicShapePool() ) {
		// Dynamic collision owns a fixed-size local shape pool. A region layout
		// change only requires repopulating that pool.
		update( s_v2iMax, true );
	} else {
		const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
		m_nUpdateUsecLast = 0u;
		m_nShapesRebuiltLastUpdate = 0;
		m_nShapesReusedLastUpdate = 0;
		m_nShapesDeactivatedLastUpdate = 0;
		m_nHeightSamplesUploadedLastUpdate = 0;
		m_nFullRegionsUpdatedLastUpdate = 0;
		m_nPreparationUsecLast = 0u;
		m_nUploadUsecLast = 0u;
		m_nBakedCacheHitsLastUpdate = 0;
		m_nBakedCacheMissesLastUpdate = 0;
		m_bUpdateSkippedUnchangedLast = false;
		m_bRebuildRequestedLast = false;
		const t3dRegionCoord_t *pSlots = m_pTerrain->dataObject()->getRegionSlotsInternal();
		const int slot_count = m_pTerrain->dataObject()->getRegionSlotCountInternal();
		const int shape_count = ( int )( m_nShapeRecordCount );
		if ( shape_count != slot_count || m_nFullRegionSlotCount != slot_count ) {
			m_bColdRebuildPending = true;
			return;
		}

		// Stable terrain slots let full collision update only the shapes whose
		// region assignment changed. Removing a region disables one shape; adding
		// one repopulates one shape.
		for ( int i = 0; i < slot_count; ++i ) {
			const Vector2i new_location = t3dUnpackRegionCoord( pSlots[i] );
			const Vector2i old_location = t3dUnpackRegionCoord( m_szFullRegionSlots[( size_t )( i )] );
			if ( new_location != old_location ) {
				updateFullShape( i, new_location );
				m_szFullStreamingResident[( size_t )( i )] = regionAllowsCollision( new_location ) ? 1u : 0u;
			}
		}
		for ( int i = 0; i < slot_count; ++i ) {
			m_szFullRegionSlots[( size_t )( i )] = pSlots[i];
		}
		m_nFullRegionSlotCount = slot_count;
		m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	}
}

void Terrain3DCollision::build( void ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) { return; }
	if ( !m_pTerrain->isInsideWorld() ) {
		LOG( ERROR, "Terrain isn't inside world. Returning." );
		return;
	}

	// Clear collision as the user might change modes in the editor
	destroy();

	// Build only in applicable modes
	if ( !isEnabled() ) {
		return;
	}

	// Create StaticBody3D
	if ( usesEditorBackend() ) {
		LOG( INFO, "Building editor collision" );
		m_pStaticBody = memnew( StaticBody3D );
		m_pStaticBody->set_name( "StaticBody3D" );
		m_pStaticBody->set_as_top_level( true );
		m_pTerrain->sceneNode().add_child( m_pStaticBody, true );
		m_pStaticBody->set_owner( &m_pTerrain->sceneNode() );
		m_pStaticBody->set_collision_mask( m_nMask );
		m_pStaticBody->set_collision_layer( m_nLayer );
		m_pStaticBody->set_collision_priority( m_fPriority );
	} else {
		LOG( INFO, "Building collision with Physics Server" );
		m_hStaticBodyRid = PS->body_create();
		PS->body_set_mode( m_hStaticBodyRid, PhysicsServer3D::BODY_MODE_STATIC );
		PS->body_set_space( m_hStaticBodyRid, m_pTerrain->sceneNode().get_world_3d()->get_space() );
		PS->body_attach_object_instance_id( m_hStaticBodyRid, m_pTerrain->sceneNode().get_instance_id() );
		PS->body_set_collision_mask( m_hStaticBodyRid, m_nMask );
		PS->body_set_collision_layer( m_hStaticBodyRid, m_nLayer );
		PS->body_set_collision_priority( m_hStaticBodyRid, m_fPriority );
	}
	reloadPhysicsMaterial();

	// Create CollisionShape3Ds
	int shape_count;
	int hshape_size;
	if ( usesDynamicShapePool() ) {
		int grid_width = m_nRadius * 2 / m_nShapeSize;
		grid_width = t3dIntCeilPow2( grid_width, 4 );
		shape_count = grid_width * grid_width;
		hshape_size = m_nShapeSize + 1;
		LOG( DEBUG, "Grid width: ", grid_width );
	} else {
		// Full collision mirrors stable texture-array slots, including reusable
		// empty slots, so ordinary add/remove operations do not resize the pool.
		shape_count = m_pTerrain->dataObject()->getRegionSlotCountInternal();
		hshape_size = T3D_REGION_SIZE + 1;
	}
	if ( shape_count < 0 || shape_count > T3D_MAX_COLLISION_SHAPE_RECORDS ) {
		LOG( ERROR, "Collision shape count exceeds fixed shape-record capacity: ", shape_count );
		return;
	}
	const int sample_count = hshape_size * hshape_size;
	if ( sample_count <= 0 || !m_collisionSurfaceBuilder.prepareRuntimeScratch( hshape_size ) ) {
		LOG( ERROR, "Collision surface scratch capacity is invalid for axis size: ", hshape_size );
		return;
	}
	m_collisionSurfaceScratch.resize( sample_count );
	if ( usesDynamicShapePool() ) {
		m_fullHeightScratch.clear();
		m_fullBakedHeightScratch.clear();
	} else {
		m_fullHeightScratch.resize( sample_count );
		m_fullBakedHeightScratch.resize( sample_count );
	}
	m_nShapeRecordCount = shape_count;
	m_inactiveShapeIds.clear();
	eastl::fill_n( m_gridScratch, shape_count, -1 );
	LOG( DEBUG, "Shape count: ", shape_count );
	LOG( DEBUG, "Shape size: ", m_nShapeSize, ", hshape_size: ", hshape_size );
	const Transform3D inactiveTransform( Basis(), s_v3Max );
	for ( int i = 0; i < shape_count; ++i ) {
		t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( i )];
		record.m_transform = inactiveTransform;
		record.m_bDisabled = true;
		record.m_gridPosition = s_v2iMax;
		record.m_data.m_nWidth = hshape_size;
		record.m_data.m_nDepth = hshape_size;
		if ( usesDynamicShapePool() ) {
			for ( size_t iBuffer = 0u; iBuffer < T3D_ARRAY_COUNT( record.m_dynamicHeightBuffers ); iBuffer++ ) {
				PackedRealArray &height_buffer = ( record.m_dynamicHeightBuffers )[iBuffer];
				height_buffer.resize( sample_count );
			}
			record.m_nUploadedHeightBuffer = 0;
			record.m_nPendingHeightBuffer = 1;
		}
		if ( usesEditorBackend() ) {
			record.m_pNode = memnew( CollisionShape3D );
			record.m_pNode->set_name( "CollisionShape3D" );
			record.m_pNode->set_disabled( true );
			record.m_pNode->set_visible( false );
			record.m_pNode->set_enable_debug_fill( false );
			Ref<HeightMapShape3D> pHeightShape;
			pHeightShape.instantiate();
			pHeightShape->set_map_width( hshape_size );
			pHeightShape->set_map_depth( hshape_size );
			record.m_pNode->set_shape( pHeightShape );
			m_pStaticBody->add_child( record.m_pNode, true );
			record.m_pNode->set_owner( m_pStaticBody );
			record.m_pNode->set_transform( inactiveTransform );
		} else {
			record.m_hRid = PS->heightmap_shape_create();
			PS->body_add_shape( m_hStaticBodyRid, record.m_hRid, inactiveTransform, true );
			LOG( DEBUG, "Adding shape: ", i, ", rid: ", record.m_hRid.get_id() );
		}
	}

	m_bInitialized = true;
	m_bColdRebuildPending = false;
	update();
}

void Terrain3DCollision::update( const Vector2i &rRegionLoc, const bool bRebuild ) {
	IS_INIT( VOID );
	if ( !m_bInitialized ) {
		return;
	}
	const uint64_t start_time = Time::get_singleton()->get_ticks_usec();
	m_nUpdateUsecLast = 0u;
	m_nShapesRebuiltLastUpdate = 0;
	m_nShapesReusedLastUpdate = 0;
	m_nShapesDeactivatedLastUpdate = 0;
	m_nHeightSamplesUploadedLastUpdate = 0;
	m_nFullRegionsUpdatedLastUpdate = 0;
	m_nPreparationUsecLast = 0u;
	m_nUploadUsecLast = 0u;
	m_nBakedCacheHitsLastUpdate = 0;
	m_nBakedCacheMissesLastUpdate = 0;
	m_bUpdateSkippedUnchangedLast = false;
	m_bRebuildRequestedLast = bRebuild;
	const real_t fSpacing = s_fRegionVertexSpacing;

	if ( usesDynamicShapePool() ) {
		// Collision consumes the same authoritative target as terrain residency.
		// The collision radius remains a local physics-detail budget inside the
		// ACTIVE/HOT envelope instead of creating a second streaming footprint.
		const Vector2i snapped_position = snapToGrid( m_pTerrain->getClipmapTargetPosition() / fSpacing );
		LOG( EXTREME, "Updating collision at ", snapped_position );
		refreshBakedRegionCache( snapped_position );
		if ( !bRebuild && m_lastSnappedPos == snapped_position ) {
			m_bUpdateSkippedUnchangedLast = true;
			m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - start_time;
			return;
		}

		int grid_width = m_nRadius * 2 / m_nShapeSize;
		grid_width = t3dIntCeilPow2( grid_width, 4 );
		const int grid_count = grid_width * grid_width;
		if ( grid_count != ( int )( m_nShapeRecordCount ) ) {
			m_bColdRebuildPending = true;
			return;
		}
		if ( grid_count > T3D_MAX_COLLISION_SHAPE_RECORDS ) {
			LOG( ERROR, "Collision grid exceeds fixed shape-record capacity: ", grid_count );
			return;
		}
		eastl::fill_n( m_gridScratch, grid_count, -1 );
		m_inactiveShapeIds.clear();

		const Vector2i grid_offset = -V2I( grid_width / 2 );
		const Vector2i grid_corner = snapped_position + grid_offset * m_nShapeSize;
		const Vector2i shape_offset = V2I( m_nShapeSize / 2 );
		const int64_t radius_squared = ( int64_t )( m_nRadius ) * ( int64_t )( m_nRadius );

		// Retain all still-resident shapes and collect only the records that can
		// be reassigned. No PhysicsServer state reads and no per-update arrays.
		for ( int shape_id = 0; shape_id < ( int )( m_nShapeRecordCount ); ++shape_id ) {
			t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( shape_id )];
			if ( bRebuild || record.m_gridPosition == s_v2iMax ) {
				deactivateShape( shape_id );
				++m_nShapesDeactivatedLastUpdate;
				m_inactiveShapeIds.push_back( shape_id );
				continue;
			}

			const Vector2i shape_center = record.m_gridPosition + shape_offset;
			if ( shape_center.distance_squared_to( snapped_position ) <= radius_squared
			        && shapeAllowsCollision( record.m_gridPosition, m_nShapeSize ) ) {
				const Vector2i gridLocation = ( record.m_gridPosition - grid_corner ) / m_nShapeSize;
				const int grid_index = gridLocation.y * grid_width + gridLocation.x;
				if ( grid_index >= 0 && grid_index < grid_count && m_gridScratch[( size_t )( grid_index )] < 0 ) {
					m_gridScratch[( size_t )( grid_index )] = shape_id;
					shapeSetDisabled( shape_id, false );
					++m_nShapesReusedLastUpdate;
					continue;
				}
			}

			deactivateShape( shape_id );
			++m_nShapesDeactivatedLastUpdate;
			m_inactiveShapeIds.push_back( shape_id );
		}

		for ( int grid_index = 0; grid_index < grid_count; ++grid_index ) {
			if ( !bRebuild && m_gridScratch[( size_t )( grid_index )] >= 0 ) {
				continue;
			}
			const Vector2i gridLocation( grid_index % grid_width, grid_index / grid_width );
			const Vector2i shape_position = grid_corner + gridLocation * m_nShapeSize;
			const Vector2i shape_center = shape_position + shape_offset;
			if ( shape_center.distance_squared_to( snapped_position ) > radius_squared ) {
				continue;
			}
			if ( !shapeAllowsCollision( shape_position, m_nShapeSize ) ) {
				continue;
			}
			if ( m_inactiveShapeIds.empty() ) {
				LOG( ERROR, "No more unused collision shapes; aborting grid update" );
				break;
			}

			const int shape_id = m_inactiveShapeIds.back();
			t3dCollisionShapeRecord_t &record = m_szShapeRecords[( size_t )( shape_id )];
			record.m_nPendingHeightBuffer = ( uint8_t )( record.m_nUploadedHeightBuffer ^ 1u );
			PackedRealArray &height_buffer = record.m_dynamicHeightBuffers[record.m_nPendingHeightBuffer];
			const uint64_t preparation_started = Time::get_singleton()->get_ticks_usec();
			bool shape_ready = copyBakedTile( shape_position, m_nShapeSize, record, height_buffer );
			if ( !shape_ready ) {
				shape_ready = getShapeData( shape_position, m_nShapeSize, record, height_buffer );
			}
			m_nPreparationUsecLast += Time::get_singleton()->get_ticks_usec() - preparation_started;
			if ( !shape_ready ) {
				continue;
			}
			m_inactiveShapeIds.pop_back();

			Transform3D transform = record.m_data.m_transform;
			transform.scale( Vector3( fSpacing, 1.0f, fSpacing ) );
			record.m_gridPosition = shape_position;
			shapeSetTransform( shape_id, transform );
			const uint64_t upload_started = Time::get_singleton()->get_ticks_usec();
			shapeSetData( shape_id, height_buffer );
			m_nUploadUsecLast += Time::get_singleton()->get_ticks_usec() - upload_started;
			record.m_nUploadedHeightBuffer = record.m_nPendingHeightBuffer;
			shapeSetDisabled( shape_id, false );
			m_gridScratch[( size_t )( grid_index )] = shape_id;
			++m_nShapesRebuiltLastUpdate;
			m_nHeightSamplesUploadedLastUpdate += height_buffer.size();
		}
		m_lastSnappedPos = snapped_position;
	} else {
		const t3dRegionCoord_t *pRegionSlots = m_pTerrain->dataObject()->getRegionSlotsInternal();
		const int region_slot_count = m_pTerrain->dataObject()->getRegionSlotCountInternal();
		if ( ( int )( m_nShapeRecordCount ) != region_slot_count ) {
			m_bColdRebuildPending = true;
			return;
		}
		const uint64_t streaming_generation = m_pTerrain->getRegionStreamingGeneration();
		if ( !bRebuild && rRegionLoc == s_v2iMax && streaming_generation == m_nStreamingGenerationLast ) {
			m_bUpdateSkippedUnchangedLast = true;
			m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - start_time;
			return;
		}
		if ( rRegionLoc != s_v2iMax ) {
			const int region_id = m_pTerrain->dataObject()->getRegionId( rRegionLoc );
			if ( region_id >= 0 ) {
				updateFullShape( region_id, rRegionLoc );
				m_szFullStreamingResident[( size_t )( region_id )] = regionAllowsCollision( rRegionLoc ) ? 1u : 0u;
			}
		} else {
			for ( int i = 0; i < region_slot_count; ++i ) {
				const Vector2i region = t3dUnpackRegionCoord( pRegionSlots[i] );
				const uint8_t desired = regionAllowsCollision( region ) ? 1u : 0u;
				if ( bRebuild || desired != m_szFullStreamingResident[( size_t )( i )] || m_nFullRegionSlotCount != region_slot_count
				        || !t3dRegionCoordEquals( m_szFullRegionSlots[( size_t )( i )], region ) ) {
					updateFullShape( i, region );
					m_szFullStreamingResident[( size_t )( i )] = desired;
				} else {
					++m_nShapesReusedLastUpdate;
				}
			}
		}
		for ( int i = 0; i < region_slot_count; ++i ) {
			m_szFullRegionSlots[( size_t )( i )] = pRegionSlots[i];
		}
		m_nFullRegionSlotCount = region_slot_count;
		m_nStreamingGenerationLast = streaming_generation;
	}
	LOG( EXTREME,
	        "Collision update time (sampling + full backend heightmap ingestion): ",
	        Time::get_singleton()->get_ticks_usec() - start_time,
	        " us" );
	m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - start_time;
}

void Terrain3DCollision::requestColdRebuild( void ) {
	m_bColdRebuildPending = true;
}

void Terrain3DCollision::serviceColdRebuild( void ) {
	if ( !m_bColdRebuildPending ) {
		return;
	}
	m_bColdRebuildServiceActive = true;
	++m_nColdRebuildCount;
	build();
	if ( m_bColdRebuildPending ) {
		++m_nColdRebuildFailures;
	}
	m_bColdRebuildServiceActive = false;
}

void Terrain3DCollision::servicePendingRegionMapUpdate( void ) {
	if ( m_bRegionMapUpdatePending ) {
		processRegionMapUpdate();
	}
}

void Terrain3DCollision::destroy( void ) {
	m_bInitialized = false;
	m_lastSnappedPos = s_v2iMax;
	m_bRegionMapUpdatePending = false;
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		m_szFullRegionSlots[i] = t3dInvalidRegionCoord();
	}
	m_nFullRegionSlotCount = 0;
	m_nStreamingGenerationLast = 0u;
	memset( m_szFullStreamingResident, 0, sizeof( m_szFullStreamingResident ) );
	m_inactiveShapeIds.clear();
	m_fullHeightScratch.clear();
	m_fullBakedHeightScratch.clear();
	m_collisionSurfaceScratch.clear();
	clearBakedRegionCache( true );
	for ( int recordIndex = 0; recordIndex < m_nShapeRecordCount; ++recordIndex ) {
		t3dCollisionShapeRecord_t &record = m_szShapeRecords[recordIndex];
		if ( record.m_pNode ) {
			LOG( DEBUG, "Freeing CollisionShape3D ", record.m_pNode->get_name() );
			t3dRemoveFromTree( record.m_pNode );
			memdelete( record.m_pNode );
			record.m_pNode = NULL;
		}
		if ( record.m_hRid.is_valid() ) {
			LOG( DEBUG, "Freeing CollisionShape RID ", record.m_hRid );
			PS->free_rid( record.m_hRid );
			record.m_hRid = RID();
		}
	}
	for ( int i = 0; i < m_nShapeRecordCount; ++i ) {
		t3dCollisionShapeRecord_t &record = m_szShapeRecords[i];
		record.m_dynamicHeightBuffers[0].clear();
		record.m_dynamicHeightBuffers[1].clear();
		record.m_nUploadedHeightBuffer = 0u;
		record.m_nPendingHeightBuffer = 1u;
		record.m_transform = Transform3D( Basis(), s_v3Max );
		record.m_gridPosition = s_v2iMax;
		record.m_data = t3dCollisionShapeData_t{};
		record.m_bDisabled = true;
	}
	m_nShapeRecordCount = 0;
	m_bColdRebuildPending = false;

	if ( m_hStaticBodyRid.is_valid() ) {
		LOG( DEBUG, "Freeing StaticBody RID" );
		PS->free_rid( m_hStaticBodyRid );
		m_hStaticBodyRid = RID();
	}
	if ( m_pStaticBody ) {
		LOG( DEBUG, "Freeing StaticBody3D" );
		t3dRemoveFromTree( m_pStaticBody );
		memdelete( m_pStaticBody );
		m_pStaticBody = NULL;
	}
}

void Terrain3DCollision::setMode( const t3dCollisionMode_t mode ) {
	SET_IF_DIFF( m_mode, mode );
	LOG( INFO, "Setting collision mode: ", mode );
	if ( isEnabled() ) {
		requestColdRebuild();
	} else {
		destroy();
	}
}

void Terrain3DCollision::setShapeSize( const uint16_t nSize ) {
	uint16_t size = CLAMP( nSize, 8, 64 );
	size = t3dIntRoundMult( size, ( uint16_t )( 8 ) );
	SET_IF_DIFF( m_nShapeSize, size );
	LOG( INFO, "Setting collision dynamic shape size: ", m_nShapeSize );
	// Ensure size:radius always results in at least one valid shape
	if ( m_nShapeSize > m_nRadius - 8 ) {
		setRadius( m_nShapeSize + 16 );
	} else if ( isDynamicMode() ) {
		requestColdRebuild();
	}
}

void Terrain3DCollision::setRadius( const uint16_t nRadius ) {
	uint16_t radius = CLAMP( nRadius, 16, 256 );
	radius = t3dIntCeilPow2( radius, ( uint16_t )( 16 ) );
	SET_IF_DIFF( m_nRadius, radius );
	LOG( INFO, "Setting collision dynamic radius: ", m_nRadius );
	// Ensure size:radius always results in at least one valid shape
	if ( m_nRadius < m_nShapeSize + 8 ) {
		setShapeSize( m_nRadius - 8 );
	} else if ( m_nShapeSize < 16 && m_nRadius > 128 ) {
		setShapeSize( 16 );
	} else if ( isDynamicMode() ) {
		requestColdRebuild();
	}
}

void Terrain3DCollision::setLayer( const uint32_t nLayers ) {
	SET_IF_DIFF( m_nLayer, nLayers );
	LOG( INFO, "Setting collision layers: ", nLayers );
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_collision_layer( m_nLayer );
		}
	} else {
		if ( m_hStaticBodyRid.is_valid() ) {
			PS->body_set_collision_layer( m_hStaticBodyRid, m_nLayer );
		}
	}
}

void Terrain3DCollision::setMask( const uint32_t nMask ) {
	SET_IF_DIFF( m_nMask, nMask );
	LOG( INFO, "Setting collision mask: ", nMask );
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_collision_mask( m_nMask );
		}
	} else {
		if ( m_hStaticBodyRid.is_valid() ) {
			PS->body_set_collision_mask( m_hStaticBodyRid, m_nMask );
		}
	}
}

void Terrain3DCollision::setPriority( const real_t fPriority ) {
	SET_IF_DIFF( m_fPriority, fPriority );
	LOG( INFO, "Setting collision priority: ", fPriority );
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_collision_priority( m_fPriority );
		}
	} else {
		if ( m_hStaticBodyRid.is_valid() ) {
			PS->body_set_collision_priority( m_hStaticBodyRid, m_fPriority );
		}
	}
}

void Terrain3DCollision::setPhysicsMaterial( const Ref<PhysicsMaterial> &rMat ) {
	if ( m_physicsMaterial == rMat ) {
		return;
	}
	if ( m_physicsMaterial.is_valid() ) {
		if ( m_physicsMaterial->is_connected( "changed", callable_mp( this, &Terrain3DCollision::reloadPhysicsMaterial ) ) ) {
			LOG( DEBUG, "Disconnecting _physics_material::m_bChanged signal to _reload_physics_material()" );
			m_physicsMaterial->disconnect( "changed", callable_mp( this, &Terrain3DCollision::reloadPhysicsMaterial ) );
		}
	}
	m_physicsMaterial = rMat;
	LOG( INFO, "Setting physics material: ", rMat );
	if ( m_physicsMaterial.is_valid() ) {
		LOG( DEBUG, "Connecting _physics_material::m_bChanged signal to _reload_physics_material()" );
		m_physicsMaterial->connect( "changed", callable_mp( this, &Terrain3DCollision::reloadPhysicsMaterial ) );
	}
	reloadPhysicsMaterial();
}

RID Terrain3DCollision::getRid( void ) const {
	if ( !usesEditorBackend() ) {
		return m_hStaticBodyRid;
	} else {
		if ( m_pStaticBody ) {
			return m_pStaticBody->get_rid();
		}
	}
	return RID();
}

// -----------------------------------------------------------------------------
// Protected functions

size_t Terrain3DCollision::getRetainedBufferBytes( void ) const {
	size_t bytes = ( size_t )( m_nShapeRecordCount ) * sizeof( t3dCollisionShapeRecord_t );
	bytes += ( size_t )( m_fullHeightScratch.size() ) * sizeof( real_t );
	bytes += ( size_t )( m_fullBakedHeightScratch.size() ) * sizeof( float );
	bytes += ( size_t )( m_collisionSurfaceScratch.size() ) * sizeof( float );
	bytes += m_collisionSurfaceBuilder.getRetainedBufferBytes();
	bytes += sizeof( m_bakedRegionCache ) + sizeof( m_bakedRegionByMap );
	for ( size_t iRegion = 0u; iRegion < T3D_ARRAY_COUNT( m_bakedRegionCache ); iRegion++ ) {
		const t3dCollisionRegionCacheEntry_t &entry = ( m_bakedRegionCache )[iRegion];
		bytes += ( size_t )( entry.m_heights.size() ) * sizeof( float );
	}
	for ( int recordIndex = 0; recordIndex < m_nShapeRecordCount; ++recordIndex ) {
		const t3dCollisionShapeRecord_t &record = m_szShapeRecords[recordIndex];
		bytes += ( size_t )( record.m_dynamicHeightBuffers[0].size() ) * sizeof( real_t );
		bytes += ( size_t )( record.m_dynamicHeightBuffers[1].size() ) * sizeof( real_t );
	}
	return bytes;
}

Dictionary Terrain3DCollision::getStats( void ) const {
	Dictionary stats;
	int active_shapes = 0;
	int active_shapes_outside_hot = 0;
	int valid_shapes = 0;
	int height_samples = 0;
	for ( int recordIndex = 0; recordIndex < m_nShapeRecordCount; ++recordIndex ) {
		const t3dCollisionShapeRecord_t &record = m_szShapeRecords[recordIndex];
		if ( !record.m_bDisabled ) {
			++active_shapes;
			if ( !isAuthoringFullResidencyMode() && record.m_gridPosition != s_v2iMax
			        && !shapeAllowsCollision(
			                record.m_gridPosition, usesDynamicShapePool() ? ( int )( m_nShapeSize ) : ( int )( T3D_REGION_SIZE ) ) ) {
				++active_shapes_outside_hot;
			}
		}
		if ( record.m_data.m_bValid ) {
			++valid_shapes;
			height_samples += MAX( record.m_data.m_nWidth, 0 ) * MAX( record.m_data.m_nDepth, 0 );
		}
	}
	stats["initialized"] = m_bInitialized;
	stats["enabled"] = isEnabled();
	stats["mode"] = ( int )( m_mode );
	stats["dynamic_mode"] = isDynamicMode();
	stats["effective_dynamic_shape_pool"] = usesDynamicShapePool();
	stats["authoring_full_residency"] = isAuthoringFullResidencyMode();
	stats["streaming_generation_last"] = ( int64_t )( m_nStreamingGenerationLast );
	stats["editor_mode"] = isEditorMode();
	stats["shape_size"] = ( int )( m_nShapeSize );
	stats["radius"] = ( int )( m_nRadius );
	stats["shape_records"] = ( int )( m_nShapeRecordCount );
	stats["active_shapes"] = active_shapes;
	stats["active_shapes_outside_hot"] = active_shapes_outside_hot;
	stats["disabled_shapes"] = ( int )( m_nShapeRecordCount )-active_shapes;
	stats["valid_shapes"] = valid_shapes;
	stats["height_samples"] = height_samples;
	stats["last_snapped_position"] = m_lastSnappedPos;
	stats["region_map_update_pending"] = m_bRegionMapUpdatePending;
	stats["cold_rebuild_pending"] = m_bColdRebuildPending;
	stats["cold_rebuild_service_active"] = m_bColdRebuildServiceActive;
	stats["cold_rebuild_count"] = ( int64_t )( m_nColdRebuildCount );
	stats["cold_rebuild_failures"] = ( int64_t )( m_nColdRebuildFailures );
	stats["update_usec_last"] = ( int64_t )( m_nUpdateUsecLast );
	stats["preparation_usec_last"] = ( int64_t )( m_nPreparationUsecLast );
	stats["physics_upload_usec_last"] = ( int64_t )( m_nUploadUsecLast );
	stats["baked_cache_regions"] = m_nBakedRegionCacheCount;
	stats["baked_cache_capacity"] = T3D_BAKED_CACHE_CAPACITY;
	stats["baked_cache_hits_last_update"] = m_nBakedCacheHitsLastUpdate;
	stats["baked_cache_misses_last_update"] = m_nBakedCacheMissesLastUpdate;
	stats["shapes_rebuilt_last_update"] = m_nShapesRebuiltLastUpdate;
	stats["shapes_reused_last_update"] = m_nShapesReusedLastUpdate;
	stats["shapes_deactivated_last_update"] = m_nShapesDeactivatedLastUpdate;
	stats["height_samples_uploaded_last_update"] = m_nHeightSamplesUploadedLastUpdate;
	stats["full_regions_updated_last_update"] = m_nFullRegionsUpdatedLastUpdate;
	stats["update_skipped_unchanged_last"] = m_bUpdateSkippedUnchangedLast;
	stats["rebuild_requested_last"] = m_bRebuildRequestedLast;
	stats["full_region_slots"] = m_nFullRegionSlotCount;
	stats["physics_layer"] = ( int64_t )( m_nLayer );
	stats["physics_mask"] = ( int64_t )( m_nMask );
	stats["priority"] = m_fPriority;
	return stats;
}

void Terrain3DCollision::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( COLLISION_DISABLED );
	BIND_ENUM_CONSTANT( DYNAMIC_GAME );
	BIND_ENUM_CONSTANT( DYNAMIC_EDITOR );
	BIND_ENUM_CONSTANT( FULL_GAME );
	BIND_ENUM_CONSTANT( FULL_EDITOR );

	ClassDB::bind_method( D_METHOD( "build" ), &Terrain3DCollision::build );
	ClassDB::bind_method(
	        D_METHOD( "update", "region_location", "rebuild" ), &Terrain3DCollision::update, DEFVAL( s_v2iMax ), DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "destroy" ), &Terrain3DCollision::destroy );
	ClassDB::bind_method( D_METHOD( "set_mode", "mode" ), &Terrain3DCollision::setMode );
	ClassDB::bind_method( D_METHOD( "get_mode" ), &Terrain3DCollision::getMode );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &Terrain3DCollision::isEnabled );
	ClassDB::bind_method( D_METHOD( "is_editor_mode" ), &Terrain3DCollision::isEditorMode );
	ClassDB::bind_method( D_METHOD( "is_dynamic_mode" ), &Terrain3DCollision::isDynamicMode );

	ClassDB::bind_method( D_METHOD( "set_shape_size", "size" ), &Terrain3DCollision::setShapeSize );
	ClassDB::bind_method( D_METHOD( "get_shape_size" ), &Terrain3DCollision::getShapeSize );
	ClassDB::bind_method( D_METHOD( "set_radius", "radius" ), &Terrain3DCollision::setRadius );
	ClassDB::bind_method( D_METHOD( "get_radius" ), &Terrain3DCollision::getRadius );
	ClassDB::bind_method( D_METHOD( "set_layer", "layers" ), &Terrain3DCollision::setLayer );
	ClassDB::bind_method( D_METHOD( "get_layer" ), &Terrain3DCollision::getLayer );
	ClassDB::bind_method( D_METHOD( "set_mask", "mask" ), &Terrain3DCollision::setMask );
	ClassDB::bind_method( D_METHOD( "get_mask" ), &Terrain3DCollision::getMask );
	ClassDB::bind_method( D_METHOD( "set_priority", "priority" ), &Terrain3DCollision::setPriority );
	ClassDB::bind_method( D_METHOD( "get_priority" ), &Terrain3DCollision::getPriority );
	ClassDB::bind_method( D_METHOD( "set_physics_material", "material" ), &Terrain3DCollision::setPhysicsMaterial );
	ClassDB::bind_method( D_METHOD( "get_physics_material" ), &Terrain3DCollision::getPhysicsMaterial );
	ClassDB::bind_method( D_METHOD( "get_rid" ), &Terrain3DCollision::getRid );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &Terrain3DCollision::getStats );

	ADD_GROUP( "Collision Geometry & Range", "" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "mode", PROPERTY_HINT_ENUM, "Disabled,Dynamic / Game,Dynamic / Editor,Full / Game,Full / Editor" ),
	        "set_mode",
	        "get_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shape_size", PROPERTY_HINT_RANGE, "8,64,8" ), "set_shape_size", "get_shape_size" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "radius", PROPERTY_HINT_RANGE, "16,256,16" ), "set_radius", "get_radius" );

	ADD_GROUP( "Physics Layers", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "layer", PROPERTY_HINT_LAYERS_3D_PHYSICS ), "set_layer", "get_layer" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mask", PROPERTY_HINT_LAYERS_3D_PHYSICS ), "set_mask", "get_mask" );

	ADD_GROUP( "Physics Material", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "priority", PROPERTY_HINT_RANGE, "0.1,256,.1" ), "set_priority", "get_priority" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial" ),
	        "set_physics_material",
	        "get_physics_material" );
}
