// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <algorithm>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/height_map_shape3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include <godot_cpp/classes/scene_tree.hpp>

#include "Terrain3DConstants.h"
#include "logger.h"
#include "terrain_3d.h"
#include "terrain_3d_collision.h"
#include "terrain_3d_data.h"
#include "terrain_3d_sampler.h"
#include "terrain_3d_util.h"

///////////////////////////
// Private Functions
///////////////////////////

bool T3DCollision::isAuthoringFullResidencyMode( void ) const {
	return IS_EDITOR && m_pTerrain &&
		m_pTerrain->getEditorExecutionMode() == T3DTerrain::EDITOR_EXECUTION_AUTHORING;
}

bool T3DCollision::usesEditorBackend( void ) const {
	return IS_EDITOR && ( isEditorMode() || isAuthoringFullResidencyMode() );
}

bool T3DCollision::usesDynamicShapePool( void ) const {
	return isDynamicMode() && !isAuthoringFullResidencyMode();
}

bool T3DCollision::regionAllowsCollision( const Vector2i &rRegionLoc ) const {
	if ( !m_pTerrain || !m_pTerrain->getData() || rRegionLoc == V2I_MAX ) {
		return false;
	}
	if ( isAuthoringFullResidencyMode() ) {
		return m_pTerrain->getData()->hasRegion( rRegionLoc );
	}
	return m_pTerrain->getRegionStreamingStateInternal( rRegionLoc ) >= TERRAIN3D_REGION_STREAMING_HOT;
}

bool T3DCollision::shapeAllowsCollision( const Vector2i &rPosition, const int nSize ) const {
	if ( !m_pTerrain || !m_pTerrain->getData() ) {
		return false;
	}
	if ( isAuthoringFullResidencyMode() ) {
		return true;
	}
	const int region_size = m_pTerrain->getRegionSize();
	if ( region_size <= 0 ) {
		return false;
	}
	const int min_region_x = ( int ) ( Math::floor( ( double ) ( rPosition.x ) / ( double ) ( region_size ) ) );
	const int min_region_z = ( int ) ( Math::floor( ( double ) ( rPosition.y ) / ( double ) ( region_size ) ) );
	const int max_region_x = ( int ) ( Math::floor( ( double ) ( rPosition.x + nSize ) / ( double ) ( region_size ) ) );
	const int max_region_z = ( int ) ( Math::floor( ( double ) ( rPosition.y + nSize ) / ( double ) ( region_size ) ) );
	for ( int z = min_region_z; z <= max_region_z; ++z ) {
		for ( int x = min_region_x; x <= max_region_x; ++x ) {
			const Vector2i region( x, z );
			if ( m_pTerrain->getData()->hasRegion( region ) && !regionAllowsCollision( region ) ) {
				return false;
			}
		}
	}
	return true;
}

void T3DCollision::refreshRegionBlendCurve( const int nRegionSize, const real_t fRegionBlend ) {
	if ( nRegionSize <= 0 ) {
		m_fRegionBlendCurve.fill( 0.0f );
		m_nRegionBlendCurveSize = 0;
		m_fRegionBlendCurveSetting = -1.0f;
		return;
	}
	if ( m_nRegionBlendCurveSize == nRegionSize &&
		Math::is_equal_approx( m_fRegionBlendCurveSetting, fRegionBlend ) ) {
		return;
	}
	if ( nRegionSize > TERRAIN3D_MAX_REGION_BLEND_SAMPLES ) {
		LOG( T3DTerrain::ERROR, "Region blend curve size ", nRegionSize, " exceeds fixed capacity ",
			TERRAIN3D_MAX_REGION_BLEND_SAMPLES );
		m_nRegionBlendCurveSize = 0;
		return;
	}
	const real_t fBlendFactor = 2.0f + 126.0f * ( 1.0f - fRegionBlend );
	for ( int local = 0; local < nRegionSize; ++local ) {
		const real_t fBiased = ( real_t ) ( local ) / ( real_t ) ( nRegionSize ) - 0.5011f;
		real_t fFraction = fBiased - Math::floor( fBiased );
		fFraction = Math::clamp( fFraction, ( real_t ) ( 1e-8f ), ( real_t ) ( 1.0f - 1e-8f ) );
		m_fRegionBlendCurve[( size_t ) ( local )] =
			1.0f / ( 1.0f + Math::exp( fBlendFactor * Math::log( ( 1.0f - fFraction ) / fFraction ) ) );
	}
	m_nRegionBlendCurveSize = nRegionSize;
	m_fRegionBlendCurveSetting = fRegionBlend;
}

real_t T3DCollision::getRegionBlendWeight( const int64_t nGridCoordinate ) const {
	if ( m_nRegionBlendCurveSize <= 0 ) {
		return 0.0f;
	}
	int64_t local = nGridCoordinate % ( int64_t ) ( m_nRegionBlendCurveSize );
	if ( local < 0 ) {
		local += m_nRegionBlendCurveSize;
	}
	return m_fRegionBlendCurve[( size_t ) ( local )];
}

real_t T3DCollision::getRegionBlend(
	const int iX, const int iZ, const int iMinRegionX,
	const int iMinRegionZ, const int nPresenceWidth
) const {
	const int32_t x0 = m_szBlendRegionX0[( size_t ) ( iX )] - iMinRegionX;
	const int32_t x1 = m_szBlendRegionX1[( size_t ) ( iX )] - iMinRegionX;
	const int32_t z0 = m_szBlendRegionZ0[( size_t ) ( iZ )] - iMinRegionZ;
	const int32_t z1 = m_szBlendRegionZ1[( size_t ) ( iZ )] - iMinRegionZ;
	const uint8_t *presence = m_regionPresenceScratch.data();
	const real_t fD = ( real_t ) ( presence[( size_t ) ( z0 ) * ( size_t ) ( nPresenceWidth ) + ( size_t ) ( x0 )] );
	const real_t fC = ( real_t ) ( presence[( size_t ) ( z0 ) * ( size_t ) ( nPresenceWidth ) + ( size_t ) ( x1 )] );
	const real_t fA = ( real_t ) ( presence[( size_t ) ( z1 ) * ( size_t ) ( nPresenceWidth ) + ( size_t ) ( x0 )] );
	const real_t fB = ( real_t ) ( presence[( size_t ) ( z1 ) * ( size_t ) ( nPresenceWidth ) + ( size_t ) ( x1 )] );
	const real_t fWeightX = m_fBlendWeightX[( size_t ) ( iX )];
	const real_t fWeightZ = m_fBlendWeightZ[( size_t ) ( iZ )];
	const real_t fBlend = Math::lerp( Math::lerp( fD, fC, fWeightX ), Math::lerp( fA, fB, fWeightX ), fWeightZ );
	return ( 1.0f - fBlend ) * 2.0f;
}

// Calculates shape data from a top-left, descaled, grid-snapped position.
// The caller selects either a dynamic record buffer or the shared full-collision
// upload scratch buffer.
bool T3DCollision::getShapeData(
	const Vector2i &rPosition, const int nSize,
	t3dcollisionShapeRecord_t &rRecord, PackedRealArray &rHeights
) {
	IS_DATA_INIT_MESG( "Terrain not initialized", false );
	t3dcollisionShapeData_t &rData = rRecord.data;
	const T3DData *data = m_pTerrain->getData();
	const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
	rData.bValid = false;
	if ( !pMaterial.is_valid() ) {
		return false;
	}

	const int region_size = m_pTerrain->getRegionSize();
	const Vector2i region_location = V2I_DIVIDE_FLOOR( rPosition, region_size );
	const T3DRegion *pOriginRegion = data->getRegionPtr( region_location );
	if ( !pOriginRegion || pOriginRegion->isDeleted() ) {
		LOG( T3DTerrain::EXTREME, "Region not found at: ", region_location, ". Returning blank" );
		return false;
	}

	const T3DMaterial::t3dworldBackground_t background_mode = pMaterial->getWorldBackground();
	const bool bBlendBackground = background_mode == T3DMaterial::t3dworldBackground_t::FLAT ||
								  background_mode == T3DMaterial::t3dworldBackground_t::NOISE;
	const real_t fGroundLevel = pMaterial->get( "ground_level" );
	const real_t fRegionBlend = pMaterial->get( "region_blend" );
	const real_t fRegionTexelSize = 1.0f / ( real_t ) ( region_size );
	if ( bBlendBackground ) {
		refreshRegionBlendCurve( region_size, fRegionBlend );
	}

	const int height_shape_size = nSize + 1;
	if ( height_shape_size > TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES ) {
		LOG( T3DTerrain::ERROR, "Collision shape axis size ", height_shape_size, " exceeds fixed blend scratch capacity ",
			TERRAIN3D_MAX_COLLISION_AXIS_SAMPLES );
		return false;
	}

	int32_t min_blend_region_x = INT32_MAX;
	int32_t max_blend_region_x = INT32_MIN;
	int32_t min_blend_region_z = INT32_MAX;
	int32_t max_blend_region_z = INT32_MIN;
	if ( bBlendBackground ) {
		// Resolve the two blend-region coordinates and the logistic weight once per
		// axis sample. The old inner loop rebuilt four Vector2 region queries for
		// every height. A 65x65 shape could therefore perform 16,900 region lookups
		// even though it overlaps only a handful of region-presence bits.
		for ( int i = 0; i < height_shape_size; ++i ) {
			const int64_t sample_x = ( int64_t ) ( rPosition.x ) + i;
			const int64_t sample_z = ( int64_t ) ( rPosition.y ) + i;
			const int32_t x0 = ( int32_t ) ( Math::floor( ( double ) ( sample_x ) * ( double ) ( fRegionTexelSize ) - 0.5011 ) );
			const int32_t z0 = ( int32_t ) ( Math::floor( ( double ) ( sample_z ) * ( double ) ( fRegionTexelSize ) - 0.5011 ) );
			m_szBlendRegionX0[( size_t ) ( i )] = x0;
			m_szBlendRegionX1[( size_t ) ( i )] = x0 + 1;
			m_szBlendRegionZ0[( size_t ) ( i )] = z0;
			m_szBlendRegionZ1[( size_t ) ( i )] = z0 + 1;
			m_fBlendWeightX[( size_t ) ( i )] = getRegionBlendWeight( sample_x );
			m_fBlendWeightZ[( size_t ) ( i )] = getRegionBlendWeight( sample_z );
			min_blend_region_x = MIN( min_blend_region_x, x0 );
			max_blend_region_x = MAX( max_blend_region_x, x0 + 1 );
			min_blend_region_z = MIN( min_blend_region_z, z0 );
			max_blend_region_z = MAX( max_blend_region_z, z0 + 1 );
		}
		const int presence_width = max_blend_region_x - min_blend_region_x + 1;
		const int presence_height = max_blend_region_z - min_blend_region_z + 1;
		m_regionPresenceScratch.resize( ( size_t ) ( presence_width ) * ( size_t ) ( presence_height ) );
		for ( int z = 0; z < presence_height; ++z ) {
			uint8_t *presence_row = m_regionPresenceScratch.data() + ( size_t ) ( z ) * ( size_t ) ( presence_width );
			for ( int x = 0; x < presence_width; ++x ) {
				const T3DRegion *pRegion =
					data->getRegionPtr( Vector2i( min_blend_region_x + x, min_blend_region_z + z ) );
				presence_row[x] = pRegion && !pRegion->isDeleted() ? ( uint8_t ) ( 1 ) : ( uint8_t ) ( 0 );
			}
		}
	}

	const int sample_count = height_shape_size * height_shape_size;
	if ( rHeights.size() != sample_count ) {
		rHeights.resize( sample_count );
	}
	real_t *pHeightWrite = rHeights.ptrw();
	real_t min_height = FLT_MAX;
	real_t max_height = -FLT_MAX;
	T3DSampler sampler( data );
	m_fHeightRowScratch.resize( ( size_t ) ( sample_count ) );
	float *const pRowMajorHeight = m_fHeightRowScratch.data();
	// Decode terrain rows in contiguous spans. The physics server needs a rotated
	// layout, so perform that cheap transpose only after the resident height planes
	// have been consumed sequentially.
	sampler.copyHeightRect( rPosition.x, rPosition.y, height_shape_size, height_shape_size,
		pRowMajorHeight, height_shape_size, true );

	// HeightMapShape3D wants a -90 degree rotation. Write each destination
	// column as one contiguous run; the compact source snapshot remains L1-sized
	// even though reading one source column at a time is strided.
	for ( int x = 0; x < height_shape_size; ++x ) {
		const int destination_base = x * height_shape_size;
		for ( int z = height_shape_size - 1, destination = 0; z >= 0; --z, ++destination ) {
			const int index = destination_base + destination;
			real_t fHeight = pRowMajorHeight[( size_t ) ( z ) * ( size_t ) ( height_shape_size ) + ( size_t ) ( x )];
			if ( !std::isnan( fHeight ) && bBlendBackground ) {
				fHeight = Math::lerp( fHeight, fGroundLevel, smoothstep( 0.0f, 1.0f, getRegionBlend( x, z, min_blend_region_x, min_blend_region_z,
					max_blend_region_x - min_blend_region_x + 1 ) ) );
			}
			pHeightWrite[index] = fHeight;
			if ( !std::isnan( fHeight ) ) {
				min_height = MIN( min_height, fHeight );
				max_height = MAX( max_height, fHeight );
			}
		}
	}

	rData.nWidth = height_shape_size;
	rData.depth = height_shape_size;
	rData.transform = Transform3D( Basis( V3_UP, Math_PI * 0.5f ), v2iv3( rPosition + V2I( nSize / 2 ) ) );
	rData.fMinHeight = min_height;
	rData.fMaxHeight = max_height;
	rData.bValid = min_height != FLT_MAX;
	return rData.bValid;
}

void T3DCollision::shapeSetDisabled( const int nShapeId, const bool bDisabled ) {
	if ( nShapeId < 0 || nShapeId >= ( int ) ( m_shapeRecords.size() ) ) {
		return;
	}
	t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( nShapeId )];
	if ( record.bDisabled == bDisabled ) {
		return;
	}
	record.bDisabled = bDisabled;
	if ( usesEditorBackend() ) {
		if ( !record.pNode ) {
			return;
		}
		record.pNode->set_disabled( bDisabled );
		record.pNode->set_visible( !bDisabled );
	} else if ( m_staticBodyRid.is_valid() ) {
		PS->body_set_shape_disabled( m_staticBodyRid, nShapeId, bDisabled );
	}
}

void T3DCollision::shapeSetTransform( const int nShapeId, const Transform3D &rXform ) {
	if ( nShapeId < 0 || nShapeId >= ( int ) ( m_shapeRecords.size() ) ) {
		return;
	}
	t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( nShapeId )];
	if ( record.transform.is_equal_approx( rXform ) ) {
		return;
	}
	record.transform = rXform;
	if ( usesEditorBackend() ) {
		if ( record.pNode ) {
			record.pNode->set_transform( rXform );
		}
	} else if ( m_staticBodyRid.is_valid() ) {
		PS->body_set_shape_transform( m_staticBodyRid, nShapeId, rXform );
	}
}

Vector3 T3DCollision::shapeGetPosition( const int nShapeId ) const {
	if ( nShapeId < 0 || nShapeId >= ( int ) ( m_shapeRecords.size() ) ) {
		return V3_MAX;
	}
	return m_shapeRecords[( size_t ) ( nShapeId )].transform.origin;
}

void T3DCollision::shapeSetData( const int nShapeId, const PackedRealArray &rHeights ) {
	if ( nShapeId < 0 || nShapeId >= ( int ) ( m_shapeRecords.size() ) ) {
		return;
	}
	t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( nShapeId )];
	if ( !record.data.bValid ) {
		return;
	}
	if ( usesEditorBackend() ) {
		if ( !record.pNode ) {
			return;
		}
		Ref<HeightMapShape3D> pHeightShape = record.pNode->get_shape();
		if ( pHeightShape.is_valid() ) {
			pHeightShape->set_map_data( rHeights );
		}
	} else if ( record.rid.is_valid() ) {
		Dictionary upload_data;
		upload_data["width"] = record.data.nWidth;
		upload_data["depth"] = record.data.depth;
		upload_data["heights"] = rHeights;
		upload_data["min_height"] = record.data.fMinHeight;
		upload_data["max_height"] = record.data.fMaxHeight;
		PS->shape_set_data( record.rid, upload_data );
	}
}

void T3DCollision::deactivateShape( const int nShapeId ) {
	if ( nShapeId < 0 || nShapeId >= ( int ) ( m_shapeRecords.size() ) ) {
		return;
	}
	t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( nShapeId )];
	record.gridPosition = V2I_MAX;
	record.data.bValid = false;
	shapeSetDisabled( nShapeId, true );
	shapeSetTransform( nShapeId, Transform3D( Basis(), V3_MAX ) );
}

void T3DCollision::reloadPhysicsMaterial( void ) {
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_physics_material_override( m_pPhysicsMaterial );
		}
	} else {
		if ( m_staticBodyRid.is_valid() ) {
			if ( m_pPhysicsMaterial.is_null() ) {
				PS->body_set_param( m_staticBodyRid, PhysicsServer3D::BODY_PARAM_BOUNCE, 0.f );
				PS->body_set_param( m_staticBodyRid, PhysicsServer3D::BODY_PARAM_FRICTION, 1.f );
			} else {
				const real_t fComputedBounce =
					m_pPhysicsMaterial->get_bounce() * ( m_pPhysicsMaterial->is_absorbent() ? -1.f : 1.f );
				const real_t fComputedFriction =
					m_pPhysicsMaterial->get_friction() * ( m_pPhysicsMaterial->is_rough() ? -1.f : 1.f );
				PS->body_set_param( m_staticBodyRid, PhysicsServer3D::BODY_PARAM_BOUNCE, fComputedBounce );
				PS->body_set_param( m_staticBodyRid, PhysicsServer3D::BODY_PARAM_FRICTION, fComputedFriction );
			}
		}
	}
	if ( m_pPhysicsMaterial.is_valid() ) {
		LOG( T3DTerrain::DEBUG, "t3dsetting_t PhysicsMaterial bounce: ", m_pPhysicsMaterial->get_bounce(),
			", friction: ", m_pPhysicsMaterial->get_friction() );
	}
}

void T3DCollision::updateFullShape( const int nShapeId, const Vector2i &rRegionLoc ) {
	++m_nFullRegionsUpdatedLastUpdate;
	if ( nShapeId < 0 || nShapeId >= ( int ) ( m_shapeRecords.size() ) ) {
		return;
	}
	if ( rRegionLoc == V2I_MAX ) {
		deactivateShape( nShapeId );
		++m_nShapesDeactivatedLastUpdate;
		return;
	}
	if ( !regionAllowsCollision( rRegionLoc ) ) {
		deactivateShape( nShapeId );
		++m_nShapesDeactivatedLastUpdate;
		return;
	}

	const int region_size = m_pTerrain->getRegionSize();
	const Vector2i shape_position = rRegionLoc * region_size;
	t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( nShapeId )];
	if ( !getShapeData( shape_position, region_size, record, m_fullHeightScratch ) ) {
		deactivateShape( nShapeId );
		++m_nShapesDeactivatedLastUpdate;
		return;
	}

	Transform3D transform = record.data.transform;
	transform.scale( Vector3( m_pTerrain->getVertexSpacing(), 1.0f, m_pTerrain->getVertexSpacing() ) );
	record.gridPosition = shape_position;
	shapeSetTransform( nShapeId, transform );
	shapeSetData( nShapeId, m_fullHeightScratch );
	shapeSetDisabled( nShapeId, false );
	++m_nShapesRebuiltLastUpdate;
	m_nHeightSamplesUploadedLastUpdate += m_fullHeightScratch.size();
}

///////////////////////////
// Public Functions
///////////////////////////

void T3DCollision::initialize( T3DTerrain *const pTerrain ) {
	if ( pTerrain ) {
		m_pTerrain = pTerrain;
	} else {
		return;
	}
	if ( !IS_EDITOR && isEditorMode() ) {
		LOG( T3DTerrain::WARN, "Change collision mode to a non-editor mode for releases" );
	}
	build();
}

void T3DCollision::onRegionMapChanged( void ) {
	if ( m_bRegionMapUpdatePending ) {
		return;
	}
	m_bRegionMapUpdatePending = true;
	const Callable callback = callable_mp( this, &T3DCollision::processRegionMapUpdate );
	if ( !RS->is_connected( "frame_pre_draw", callback ) ) {
		RS->connect( "frame_pre_draw", callback );
	}
}

void T3DCollision::processRegionMapUpdate( void ) {
	const Callable callback = callable_mp( this, &T3DCollision::processRegionMapUpdate );
	if ( RS->is_connected( "frame_pre_draw", callback ) ) {
		RS->disconnect( "frame_pre_draw", callback );
	}
	m_bRegionMapUpdatePending = false;

	if ( !m_bInitialized ) {
		build();
		return;
	}

	if ( usesDynamicShapePool() ) {
		// Dynamic collision owns a fixed-size local shape pool. A region layout
		// change only requires repopulating that pool.
		update( V2I_MAX, true );
	} else {
		const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
		m_nUpdateUsecLast = 0u;
		m_nShapesRebuiltLastUpdate = 0;
		m_nShapesReusedLastUpdate = 0;
		m_nShapesDeactivatedLastUpdate = 0;
		m_nHeightSamplesUploadedLastUpdate = 0;
		m_nFullRegionsUpdatedLastUpdate = 0;
		m_bUpdateSkippedUnchangedLast = false;
		m_bRebuildRequestedLast = false;
		const Vector2i *pSlots = m_pTerrain->getData()->getRegionSlotsInternal();
		const int slot_count = m_pTerrain->getData()->getRegionSlotCountInternal();
		const int shape_count = ( int ) ( m_shapeRecords.size() );
		if ( shape_count != slot_count || m_fullRegionSlots.size() != slot_count ) {
			build();
			return;
		}

		// Stable terrain slots let full collision update only the shapes whose
		// region assignment changed. Removing a region disables one shape; adding
		// one repopulates one shape.
		for ( int i = 0; i < slot_count; ++i ) {
			const Vector2i new_location = pSlots[i];
			const Vector2i old_location = m_fullRegionSlots[i];
			if ( new_location != old_location ) {
				updateFullShape( i, new_location );
				m_szFullStreamingResident[( size_t ) ( i )] = regionAllowsCollision( new_location ) ? 1u : 0u;
			}
		}
		m_fullRegionSlots = m_pTerrain->getData()->getRegionSlots();
		m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	}
}

void T3DCollision::build( void ) {
	IS_DATA_INIT( VOID );
	if ( !m_pTerrain->isInsideWorld() ) {
		LOG( T3DTerrain::ERROR, "Terrain isn't inside world. Returning." );
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
		LOG( T3DTerrain::INFO, "Building editor collision" );
		m_pStaticBody = memnew( StaticBody3D );
		m_pStaticBody->set_name( "StaticBody3D" );
		m_pStaticBody->set_as_top_level( true );
		m_pTerrain->add_child( m_pStaticBody, true );
		m_pStaticBody->set_owner( m_pTerrain );
		m_pStaticBody->set_collision_mask( m_nMask );
		m_pStaticBody->set_collision_layer( m_layer );
		m_pStaticBody->set_collision_priority( m_fPriority );
	} else {
		LOG( T3DTerrain::INFO, "Building collision with Physics Server" );
		m_staticBodyRid = PS->body_create();
		PS->body_set_mode( m_staticBodyRid, PhysicsServer3D::BODY_MODE_STATIC );
		PS->body_set_space( m_staticBodyRid, m_pTerrain->get_world_3d()->get_space() );
		PS->body_attach_object_instance_id( m_staticBodyRid, m_pTerrain->get_instance_id() );
		PS->body_set_collision_mask( m_staticBodyRid, m_nMask );
		PS->body_set_collision_layer( m_staticBodyRid, m_layer );
		PS->body_set_collision_priority( m_staticBodyRid, m_fPriority );
	}
	reloadPhysicsMaterial();

	// Create CollisionShape3Ds
	int shape_count;
	int hshape_size;
	if ( usesDynamicShapePool() ) {
		int grid_width = m_nRadius * 2 / m_nShapeSize;
		grid_width = intCeilPow2( grid_width, 4 );
		shape_count = grid_width * grid_width;
		hshape_size = m_nShapeSize + 1;
		LOG( T3DTerrain::DEBUG, "Grid width: ", grid_width );
	} else {
		// Full collision mirrors stable texture-array slots, including reusable
		// empty slots, so ordinary add/remove operations do not resize the pool.
		shape_count = m_pTerrain->getData()->getRegionSlots().size();
		hshape_size = m_pTerrain->getRegionSize() + 1;
	}
	m_shapeRecords.resize( ( size_t ) ( shape_count ) );
	m_gridScratch.assign( ( size_t ) ( shape_count ), -1 );
	m_inactiveShapeIds.clear();
	m_inactiveShapeIds.reserve( ( size_t ) ( shape_count ) );
	LOG( T3DTerrain::DEBUG, "Shape count: ", shape_count );
	LOG( T3DTerrain::DEBUG, "Shape size: ", m_nShapeSize, ", hshape_size: ", hshape_size );
	const Transform3D inactiveTransform( Basis(), V3_MAX );
	for ( int i = 0; i < shape_count; ++i ) {
		t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( i )];
		record.transform = inactiveTransform;
		record.bDisabled = true;
		record.gridPosition = V2I_MAX;
		record.data.nWidth = hshape_size;
		record.data.depth = hshape_size;
		if ( usesDynamicShapePool() ) {
			for ( PackedRealArray &height_buffer : record.szDynamicHeightBuffers ) {
				height_buffer.resize( hshape_size * hshape_size );
			}
			record.uploadedHeightBuffer = 0;
			record.pendingHeightBuffer = 1;
		}
		if ( usesEditorBackend() ) {
			record.pNode = memnew( CollisionShape3D );
			record.pNode->set_name( "CollisionShape3D" );
			record.pNode->set_disabled( true );
			record.pNode->set_visible( false );
			record.pNode->set_enable_debug_fill( false );
			Ref<HeightMapShape3D> pHeightShape;
			pHeightShape.instantiate();
			pHeightShape->set_map_width( hshape_size );
			pHeightShape->set_map_depth( hshape_size );
			record.pNode->set_shape( pHeightShape );
			m_pStaticBody->add_child( record.pNode, true );
			record.pNode->set_owner( m_pStaticBody );
			record.pNode->set_transform( inactiveTransform );
		} else {
			record.rid = PS->heightmap_shape_create();
			PS->body_add_shape( m_staticBodyRid, record.rid, inactiveTransform, true );
			LOG( T3DTerrain::DEBUG, "Adding shape: ", i, ", rid: ", record.rid.get_id() );
		}
	}

	m_bInitialized = true;
	update();
}

void T3DCollision::update( const Vector2i &rRegionLoc, const bool bRebuild ) {
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
	m_bUpdateSkippedUnchangedLast = false;
	m_bRebuildRequestedLast = bRebuild;
	const real_t fSpacing = m_pTerrain->getVertexSpacing();

	if ( usesDynamicShapePool() ) {
		// Collision consumes the same authoritative target as terrain residency.
		// The collision radius remains a local physics-detail budget inside the
		// ACTIVE/HOT envelope instead of creating a second streaming footprint.
		const Vector2i snapped_position = snapToGrid( m_pTerrain->getClipmapTargetPosition() / fSpacing );
		LOG( T3DTerrain::EXTREME, "Updating collision at ", snapped_position );
		if ( !bRebuild && m_lastSnappedPos == snapped_position ) {
			m_bUpdateSkippedUnchangedLast = true;
			m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - start_time;
			return;
		}

		int grid_width = m_nRadius * 2 / m_nShapeSize;
		grid_width = intCeilPow2( grid_width, 4 );
		const int grid_count = grid_width * grid_width;
		if ( grid_count != ( int ) ( m_shapeRecords.size() ) ) {
			build();
			return;
		}
		if ( ( int ) ( m_gridScratch.size() ) != grid_count ) {
			m_gridScratch.resize( ( size_t ) ( grid_count ) );
		}
		std::fill( m_gridScratch.begin(), m_gridScratch.end(), -1 );
		m_inactiveShapeIds.clear();

		const Vector2i grid_offset = -V2I( grid_width / 2 );
		const Vector2i grid_corner = snapped_position + grid_offset * m_nShapeSize;
		const Vector2i shape_offset = V2I( m_nShapeSize / 2 );
		const int64_t radius_squared = ( int64_t ) ( m_nRadius ) * ( int64_t ) ( m_nRadius );

		// Retain all still-resident shapes and collect only the records that can
		// be reassigned. No PhysicsServer state reads and no per-update arrays.
		for ( int shape_id = 0; shape_id < ( int ) ( m_shapeRecords.size() ); ++shape_id ) {
			t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( shape_id )];
			if ( bRebuild || record.gridPosition == V2I_MAX ) {
				deactivateShape( shape_id );
				++m_nShapesDeactivatedLastUpdate;
				m_inactiveShapeIds.push_back( shape_id );
				continue;
			}

			const Vector2i shape_center = record.gridPosition + shape_offset;
			if ( shape_center.distance_squared_to( snapped_position ) <= radius_squared &&
				shapeAllowsCollision( record.gridPosition, m_nShapeSize ) ) {
				const Vector2i gridLocation = ( record.gridPosition - grid_corner ) / m_nShapeSize;
				const int grid_index = gridLocation.y * grid_width + gridLocation.x;
				if ( grid_index >= 0 && grid_index < grid_count && m_gridScratch[( size_t ) ( grid_index )] < 0 ) {
					m_gridScratch[( size_t ) ( grid_index )] = shape_id;
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
			if ( !bRebuild && m_gridScratch[( size_t ) ( grid_index )] >= 0 ) {
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
				LOG( T3DTerrain::ERROR, "No more unused collision shapes; aborting grid update" );
				break;
			}

			const int shape_id = m_inactiveShapeIds.back();
			t3dcollisionShapeRecord_t &record = m_shapeRecords[( size_t ) ( shape_id )];
			record.pendingHeightBuffer = ( uint8_t ) ( record.uploadedHeightBuffer ^ 1u );
			PackedRealArray &height_buffer = record.szDynamicHeightBuffers[record.pendingHeightBuffer];
			if ( !getShapeData( shape_position, m_nShapeSize, record, height_buffer ) ) {
				continue;
			}
			m_inactiveShapeIds.pop_back();

			Transform3D transform = record.data.transform;
			transform.scale( Vector3( fSpacing, 1.0f, fSpacing ) );
			record.gridPosition = shape_position;
			shapeSetTransform( shape_id, transform );
			shapeSetData( shape_id, height_buffer );
			record.uploadedHeightBuffer = record.pendingHeightBuffer;
			shapeSetDisabled( shape_id, false );
			m_gridScratch[( size_t ) ( grid_index )] = shape_id;
			++m_nShapesRebuiltLastUpdate;
			m_nHeightSamplesUploadedLastUpdate += height_buffer.size();
		}
		m_lastSnappedPos = snapped_position;
	} else {
		const Vector2i *pRegionSlots = m_pTerrain->getData()->getRegionSlotsInternal();
		const int region_slot_count = m_pTerrain->getData()->getRegionSlotCountInternal();
		if ( ( int ) ( m_shapeRecords.size() ) != region_slot_count ) {
			build();
			return;
		}
		const uint64_t streaming_generation = m_pTerrain->getRegionStreamingGeneration();
		if ( !bRebuild && rRegionLoc == V2I_MAX && streaming_generation == m_nStreamingGenerationLast ) {
			m_bUpdateSkippedUnchangedLast = true;
			m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - start_time;
			return;
		}
		if ( rRegionLoc != V2I_MAX ) {
			const int region_id = m_pTerrain->getData()->getRegionId( rRegionLoc );
			if ( region_id >= 0 ) {
				updateFullShape( region_id, rRegionLoc );
				m_szFullStreamingResident[( size_t ) ( region_id )] = regionAllowsCollision( rRegionLoc ) ? 1u : 0u;
			}
		} else {
			for ( int i = 0; i < region_slot_count; ++i ) {
				const Vector2i region = Vector2i( pRegionSlots[i] );
				const uint8_t desired = regionAllowsCollision( region ) ? 1u : 0u;
				if ( bRebuild || desired != m_szFullStreamingResident[( size_t ) ( i )] ||
					m_fullRegionSlots.size() != region_slot_count || m_fullRegionSlots[i] != region ) {
					updateFullShape( i, region );
					m_szFullStreamingResident[( size_t ) ( i )] = desired;
				} else {
					++m_nShapesReusedLastUpdate;
				}
			}
		}
		m_fullRegionSlots = m_pTerrain->getData()->getRegionSlots();
		m_nStreamingGenerationLast = streaming_generation;
	}
	LOG( T3DTerrain::EXTREME, "Collision update time (sampling + full backend heightmap ingestion): ",
		Time::get_singleton()->get_ticks_usec() - start_time, " us" );
	m_nUpdateUsecLast = Time::get_singleton()->get_ticks_usec() - start_time;
}

void T3DCollision::destroy( void ) {
	m_bInitialized = false;
	m_lastSnappedPos = V2I_MAX;
	m_bRegionMapUpdatePending = false;
	m_fullRegionSlots.clear();
	m_nStreamingGenerationLast = 0u;
	m_szFullStreamingResident.fill( 0u );
	m_gridScratch.clear();
	m_inactiveShapeIds.clear();
	m_fullHeightScratch.clear();
	m_fRegionBlendCurve.fill( 0.0f );
	m_nRegionBlendCurveSize = 0;
	m_fRegionBlendCurveSetting = -1.0f;
	const Callable region_callback = callable_mp( this, &T3DCollision::processRegionMapUpdate );
	if ( RS->is_connected( "frame_pre_draw", region_callback ) ) {
		RS->disconnect( "frame_pre_draw", region_callback );
	}

	for ( t3dcollisionShapeRecord_t &record : m_shapeRecords ) {
		if ( record.pNode ) {
			LOG( T3DTerrain::DEBUG, "Freeing CollisionShape3D ", record.pNode->get_name() );
			removeFromTree( record.pNode );
			memdelete( record.pNode );
			record.pNode = nullptr;
		}
		if ( record.rid.is_valid() ) {
			LOG( T3DTerrain::DEBUG, "Freeing CollisionShape RID ", record.rid );
			PS->free_rid( record.rid );
			record.rid = RID();
		}
	}
	m_shapeRecords.clear();

	if ( m_staticBodyRid.is_valid() ) {
		LOG( T3DTerrain::DEBUG, "Freeing StaticBody RID" );
		PS->free_rid( m_staticBodyRid );
		m_staticBodyRid = RID();
	}
	if ( m_pStaticBody ) {
		LOG( T3DTerrain::DEBUG, "Freeing StaticBody3D" );
		removeFromTree( m_pStaticBody );
		memdelete( m_pStaticBody );
		m_pStaticBody = nullptr;
	}
}

void T3DCollision::setMode( const t3dcollisionMode_t mode ) {
	SET_IF_DIFF( m_mode, mode );
	LOG( T3DTerrain::INFO, "t3dsetting_t collision mode: ", mode );
	if ( isEnabled() ) {
		build();
	} else {
		destroy();
	}
}

void T3DCollision::setShapeSize( const uint16_t nSize ) {
	uint16_t size = CLAMP( nSize, 8, 64 );
	size = intRoundMult( size, ( uint16_t ) ( 8 ) );
	SET_IF_DIFF( m_nShapeSize, size );
	LOG( T3DTerrain::INFO, "t3dsetting_t collision dynamic shape size: ", m_nShapeSize );
	// Ensure size:radius always results in at least one valid shape
	if ( m_nShapeSize > m_nRadius - 8 ) {
		setRadius( m_nShapeSize + 16 );
	} else if ( isDynamicMode() ) {
		build();
	}
}

void T3DCollision::setRadius( const uint16_t nRadius ) {
	uint16_t radius = CLAMP( nRadius, 16, 256 );
	radius = intCeilPow2( radius, ( uint16_t ) ( 16 ) );
	SET_IF_DIFF( m_nRadius, radius );
	LOG( T3DTerrain::INFO, "t3dsetting_t collision dynamic radius: ", m_nRadius );
	// Ensure size:radius always results in at least one valid shape
	if ( m_nRadius < m_nShapeSize + 8 ) {
		setShapeSize( m_nRadius - 8 );
	} else if ( m_nShapeSize < 16 && m_nRadius > 128 ) {
		setShapeSize( 16 );
	} else if ( isDynamicMode() ) {
		build();
	}
}

void T3DCollision::setLayer( const uint32_t nLayers ) {
	SET_IF_DIFF( m_layer, nLayers );
	LOG( T3DTerrain::INFO, "t3dsetting_t collision layers: ", nLayers );
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_collision_layer( m_layer );
		}
	} else {
		if ( m_staticBodyRid.is_valid() ) {
			PS->body_set_collision_layer( m_staticBodyRid, m_layer );
		}
	}
}

void T3DCollision::setMask( const uint32_t nMask ) {
	SET_IF_DIFF( m_nMask, nMask );
	LOG( T3DTerrain::INFO, "t3dsetting_t collision mask: ", nMask );
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_collision_mask( m_nMask );
		}
	} else {
		if ( m_staticBodyRid.is_valid() ) {
			PS->body_set_collision_mask( m_staticBodyRid, m_nMask );
		}
	}
}

void T3DCollision::setPriority( const real_t fPriority ) {
	SET_IF_DIFF( m_fPriority, fPriority );
	LOG( T3DTerrain::INFO, "t3dsetting_t collision priority: ", fPriority );
	if ( usesEditorBackend() ) {
		if ( m_pStaticBody ) {
			m_pStaticBody->set_collision_priority( m_fPriority );
		}
	} else {
		if ( m_staticBodyRid.is_valid() ) {
			PS->body_set_collision_priority( m_staticBodyRid, m_fPriority );
		}
	}
}

void T3DCollision::setPhysicsMaterial( const Ref<PhysicsMaterial> &rMat ) {
	if ( m_pPhysicsMaterial == rMat ) {
		return;
	}
	if ( m_pPhysicsMaterial.is_valid() ) {
		if ( m_pPhysicsMaterial->is_connected( "changed",
											callable_mp( this, &T3DCollision::reloadPhysicsMaterial ) ) ) {
			LOG( T3DTerrain::DEBUG, "Disconnecting _physics_material::changed signal to _reload_physics_material()" );
			m_pPhysicsMaterial->disconnect( "changed", callable_mp( this, &T3DCollision::reloadPhysicsMaterial ) );
		}
	}
	m_pPhysicsMaterial = rMat;
	LOG( T3DTerrain::INFO, "t3dsetting_t physics material: ", rMat );
	if ( m_pPhysicsMaterial.is_valid() ) {
		LOG( T3DTerrain::DEBUG, "Connecting _physics_material::changed signal to _reload_physics_material()" );
		m_pPhysicsMaterial->connect( "changed", callable_mp( this, &T3DCollision::reloadPhysicsMaterial ) );
	}
	reloadPhysicsMaterial();
}

RID T3DCollision::getRid( void ) const {
	if ( !usesEditorBackend() ) {
		return m_staticBodyRid;
	} else {
		if ( m_pStaticBody ) {
			return m_pStaticBody->get_rid();
		}
	}
	return RID();
}

///////////////////////////
// Protected Functions
///////////////////////////

size_t T3DCollision::getRetainedBufferBytes( void ) const {
	size_t bytes = m_shapeRecords.capacity() * sizeof( t3dcollisionShapeRecord_t );
	bytes += m_gridScratch.capacity() * sizeof( int32_t );
	bytes += m_inactiveShapeIds.capacity() * sizeof( int32_t );
	bytes += m_fHeightRowScratch.capacity() * sizeof( float );
	bytes += m_regionPresenceScratch.capacity() * sizeof( uint8_t );
	bytes += ( size_t ) ( m_fullHeightScratch.size() ) * sizeof( real_t );
	for ( const t3dcollisionShapeRecord_t &record : m_shapeRecords ) {
		bytes += ( size_t ) ( record.szDynamicHeightBuffers[0].size() ) * sizeof( real_t );
		bytes += ( size_t ) ( record.szDynamicHeightBuffers[1].size() ) * sizeof( real_t );
	}
	return bytes;
}

Dictionary T3DCollision::getStats( void ) const {
	Dictionary stats;
	int active_shapes = 0;
	int active_shapes_outside_hot = 0;
	int valid_shapes = 0;
	int height_samples = 0;
	for ( const t3dcollisionShapeRecord_t &record : m_shapeRecords ) {
		if ( !record.bDisabled ) {
			++active_shapes;
			if ( !isAuthoringFullResidencyMode() && record.gridPosition != V2I_MAX &&
				!shapeAllowsCollision( record.gridPosition,
					usesDynamicShapePool() ? ( int ) ( m_nShapeSize ) : ( int ) ( m_pTerrain->getRegionSize() ) ) ) {
				++active_shapes_outside_hot;
			}
		}
		if ( record.data.bValid ) {
			++valid_shapes;
			height_samples += MAX( record.data.nWidth, 0 ) * MAX( record.data.depth, 0 );
		}
	}
	stats["initialized"] = m_bInitialized;
	stats["enabled"] = isEnabled();
	stats["mode"] = ( int ) ( m_mode );
	stats["dynamic_mode"] = isDynamicMode();
	stats["effective_dynamic_shape_pool"] = usesDynamicShapePool();
	stats["authoring_full_residency"] = isAuthoringFullResidencyMode();
	stats["streaming_generation_last"] = ( int64_t ) ( m_nStreamingGenerationLast );
	stats["editor_mode"] = isEditorMode();
	stats["shape_size"] = ( int ) ( m_nShapeSize );
	stats["radius"] = ( int ) ( m_nRadius );
	stats["shape_records"] = ( int ) ( m_shapeRecords.size() );
	stats["active_shapes"] = active_shapes;
	stats["active_shapes_outside_hot"] = active_shapes_outside_hot;
	stats["disabled_shapes"] = ( int ) ( m_shapeRecords.size() ) - active_shapes;
	stats["valid_shapes"] = valid_shapes;
	stats["height_samples"] = height_samples;
	stats["last_snapped_position"] = m_lastSnappedPos;
	stats["region_map_update_pending"] = m_bRegionMapUpdatePending;
	stats["update_usec_last"] = ( int64_t ) ( m_nUpdateUsecLast );
	stats["shapes_rebuilt_last_update"] = m_nShapesRebuiltLastUpdate;
	stats["shapes_reused_last_update"] = m_nShapesReusedLastUpdate;
	stats["shapes_deactivated_last_update"] = m_nShapesDeactivatedLastUpdate;
	stats["height_samples_uploaded_last_update"] = m_nHeightSamplesUploadedLastUpdate;
	stats["full_regions_updated_last_update"] = m_nFullRegionsUpdatedLastUpdate;
	stats["update_skipped_unchanged_last"] = m_bUpdateSkippedUnchangedLast;
	stats["rebuild_requested_last"] = m_bRebuildRequestedLast;
	stats["full_region_slots"] = m_fullRegionSlots.size();
	stats["physics_layer"] = ( int64_t ) ( m_layer );
	stats["physics_mask"] = ( int64_t ) ( m_nMask );
	stats["priority"] = m_fPriority;
	return stats;
}

void T3DCollision::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( DISABLED );
	BIND_ENUM_CONSTANT( DYNAMIC_GAME );
	BIND_ENUM_CONSTANT( DYNAMIC_EDITOR );
	BIND_ENUM_CONSTANT( FULL_GAME );
	BIND_ENUM_CONSTANT( FULL_EDITOR );

	ClassDB::bind_method( D_METHOD( "build" ), &T3DCollision::build );
	ClassDB::bind_method( D_METHOD( "update", "region_location", "rebuild" ), &T3DCollision::update, DEFVAL( V2I_MAX ),
						 DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "destroy" ), &T3DCollision::destroy );
	ClassDB::bind_method( D_METHOD( "set_mode", "mode" ), &T3DCollision::setMode );
	ClassDB::bind_method( D_METHOD( "get_mode" ), &T3DCollision::getMode );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &T3DCollision::isEnabled );
	ClassDB::bind_method( D_METHOD( "is_editor_mode" ), &T3DCollision::isEditorMode );
	ClassDB::bind_method( D_METHOD( "is_dynamic_mode" ), &T3DCollision::isDynamicMode );

	ClassDB::bind_method( D_METHOD( "set_shape_size", "size" ), &T3DCollision::setShapeSize );
	ClassDB::bind_method( D_METHOD( "get_shape_size" ), &T3DCollision::getShapeSize );
	ClassDB::bind_method( D_METHOD( "set_radius", "radius" ), &T3DCollision::setRadius );
	ClassDB::bind_method( D_METHOD( "get_radius" ), &T3DCollision::getRadius );
	ClassDB::bind_method( D_METHOD( "set_layer", "layers" ), &T3DCollision::setLayer );
	ClassDB::bind_method( D_METHOD( "get_layer" ), &T3DCollision::getLayer );
	ClassDB::bind_method( D_METHOD( "set_mask", "mask" ), &T3DCollision::setMask );
	ClassDB::bind_method( D_METHOD( "get_mask" ), &T3DCollision::getMask );
	ClassDB::bind_method( D_METHOD( "set_priority", "priority" ), &T3DCollision::setPriority );
	ClassDB::bind_method( D_METHOD( "get_priority" ), &T3DCollision::getPriority );
	ClassDB::bind_method( D_METHOD( "set_physics_material", "material" ), &T3DCollision::setPhysicsMaterial );
	ClassDB::bind_method( D_METHOD( "get_physics_material" ), &T3DCollision::getPhysicsMaterial );
	ClassDB::bind_method( D_METHOD( "get_rid" ), &T3DCollision::getRid );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &T3DCollision::getStats );

	ADD_GROUP( "Collision Geometry & Range", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mode", PROPERTY_HINT_ENUM,
							  "Disabled,Dynamic / Game,Dynamic / Editor,Full / Game,Full / Editor" ),
				 "set_mode", "get_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shape_size", PROPERTY_HINT_RANGE, "8,64,8" ), "set_shape_size",
				 "get_shape_size" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "radius", PROPERTY_HINT_RANGE, "16,256,16" ), "set_radius", "get_radius" );

	ADD_GROUP( "Physics Layers", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "layer", PROPERTY_HINT_LAYERS_3D_PHYSICS ), "set_layer", "get_layer" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mask", PROPERTY_HINT_LAYERS_3D_PHYSICS ), "set_mask", "get_mask" );

	ADD_GROUP( "Physics Material", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "priority", PROPERTY_HINT_RANGE, "0.1,256,.1" ), "set_priority",
				 "get_priority" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial" ),
				 "set_physics_material", "get_physics_material" );

}
