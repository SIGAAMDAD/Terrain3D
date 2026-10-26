#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#include "core/utility/terrain_3d_generation.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "instances/physics/terrain_3d_instance_occlusion.h"
#include "instances/physics/terrain_3d_instance_physics.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "authoring/editor/terrain_3d_editor.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_integration_helpers.h"
#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "lighting/policy/terrain_3d_lighting_policy.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "simd/core/terrain_3d_parallel.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"
#include "terrain_3d.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

void Terrain3D::syncLightShaderParameters( void ) {
	DirectionalLight3D *const pLight = m_lightTarget.isValid() ? cast_to<DirectionalLight3D>( m_lightTarget.ptr() ) : NULL;
	if ( !pLight ) {
		m_bLightStateValid = false;
		return;
	}
	const Color color = pLight->get_color() * pLight->get_param( DirectionalLight3D::PARAM_ENERGY );
	const Vector3 direction = pLight->get_global_basis().get_column( 2 ).normalized();
	const bool bColorChanged = !m_bLightStateValid || !m_lastLightColor.is_equal_approx( color );
	const bool bDirectionChanged = !m_bLightStateValid || !m_lastLightDirection.is_equal_approx( direction );
	if ( !bColorChanged && !bDirectionChanged ) {
		return;
	}

	if ( m_material.is_valid() ) {
		if ( bColorChanged ) {
			m_material->setShaderParam( "_light_color", color );
		}
		if ( bDirectionChanged ) {
			m_material->setShaderParam( "_light_direction", direction );
		}
	}
	m_lastLightColor = color;
	m_lastLightDirection = direction;
	m_bLightStateValid = true;
}

void Terrain3D::destroyCollision( const bool bFinal ) {
	LOG( INFO, "Destroying Collision" );
	if ( m_pCollision ) {
		m_pCollision->destroy();
	}
	if ( bFinal && m_pCollision ) {
		memdelete( m_pCollision );
		m_pCollision = NULL;
	}
}

void Terrain3D::setupTerrainMesher( void ) {
	setupTerrainMesher( getClipmapTargetPosition() );
}

void Terrain3D::setupTerrainMesher( const Vector3 &rInitialTarget ) {
	if ( !m_pTerrainMesher ) {
		LOG( DEBUG, "Creating procedural clipmap layout producer" );
		m_pTerrainMesher = memnew( Terrain3DMesher );
	}
	const bool bTieringActive = isRuntimeShaderTieringActive();
	const int clipmap_levels = CLAMP( m_nMeshLods + m_nTessellationLevel, 1, T3D_MAX_CLIPMAP_LEVELS );
	const int near_lods = bTieringActive ? m_nRuntimeNearShaderLods : clipmap_levels;
	const int middle_lods = bTieringActive ? m_nRuntimeMiddleShaderLods : 0;
	const int update_budget = bTieringActive ? m_nRuntimeClipmapUpdateBudget : T3D_MAX_CLIPMAP_LEVELS;
	const eastl::array<bool, 3> tier_shadow_enabled = { m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled };
	const eastl::array<int, 3> tier_shadow_lod_counts = { m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount };
	t3dMesherInitialization_t config;
	config.m_nMeshSize = m_nMeshSize;
	config.m_nLods = m_nMeshLods;
	config.m_nTessellationLevel = m_nTessellationLevel;
	config.m_fVertexSpacing = s_fRegionVertexSpacing;
	config.m_nNearShaderLodCount = near_lods;
	config.m_nMiddleShaderLodCount = middle_lods;
	config.m_nClipmapUpdateBudget = update_budget;
	config.m_castShadows = m_castShadows;
	config.m_nShadowLodCount = m_nShadowLodCount;
	config.m_bTierShadowEnabled = tier_shadow_enabled;
	config.m_nTierShadowLodCounts = tier_shadow_lod_counts;
	config.m_initialTarget = rInitialTarget;
	m_pTerrainMesher->initialize( this, config );
}


void Terrain3D::destroyTerrainMesher( const bool bFinal ) {
	LOG( INFO, "Destroying terrain mesher" );
	if ( !m_pTerrainMesher ) {
		return;
	}
	if ( bFinal ) {
		memdelete( m_pTerrainMesher );
		m_pTerrainMesher = NULL;
	} else {
		m_pTerrainMesher->destroy();
	}
}

void Terrain3D::setupOceanMesher( void ) {
	setupOceanMesher( getClipmapTargetPosition() );
}

void Terrain3D::setupOceanMesher( const Vector3 &rInitialTarget ) {
	if ( !m_bOceanEnabled ) {
		return;
	}
	m_bLightStateValid = false;
	if ( !m_pOceanMesher ) {
		LOG( DEBUG, "Creating procedural ocean clipmap layout producer" );
		m_pOceanMesher = memnew( Terrain3DMesher );
	}
	t3dMesherInitialization_t initialization;
	initialization.m_nMeshSize = m_nOceanMeshSize;
	initialization.m_nLods = m_nOceanMeshLods;
	initialization.m_nTessellationLevel = m_nOceanTessellationLevel;
	initialization.m_fVertexSpacing = m_fOceanVertexSpacing;
	initialization.m_nNearShaderLodCount = m_nOceanMeshLods + m_nOceanTessellationLevel;
	initialization.m_castShadows = m_oceanCastShadows;
	initialization.m_nShadowLodCount = m_nOceanMeshLods + m_nOceanTessellationLevel;
	initialization.m_bTierShadowEnabled = { true, true, true };
	initialization.m_nTierShadowLodCounts = { -1, -1, -1 };
	initialization.m_initialTarget = rInitialTarget;
	m_pOceanMesher->initialize( this, initialization );
	m_pOceanMesher->updateAabbs( m_fOceanCullMargin, s_v2Zero );
	if ( m_pRenderCoordinator && m_bInitialized ) {
		m_pRenderCoordinator->initializeOcean();
	}
}

void Terrain3D::destroyOceanMesher( const bool bFinal ) {
	LOG( INFO, "Destroying ocean mesher" );
	if ( !m_pOceanMesher ) {
		return;
	}
	if ( bFinal ) {
		memdelete( m_pOceanMesher );
		m_pOceanMesher = NULL;
	} else {
		m_pOceanMesher->destroy();
	}
}

void Terrain3D::setupDisplacementBuffer( void ) {
	destroyDisplacementBuffer();
	m_nComputeDisplacementPageTableGeneration = UINT64_MAX;
	m_bComputeDisplacementDirty.store( m_nTessellationLevel > 0, T3D_MEMORY_ORDER_RELEASE );
	if ( m_nTessellationLevel > 0 ) {
		LOG( INFO, "Initializing direct RenderingDevice displacement path" );
		const uint64_t nPageTableGeneration = m_pData ? m_pData->getPhysicalRegionMapGenerationInternal() : 0u;
		if ( tryUpdateComputeDisplacement( getClipmapTargetPosition() ) ) {
			m_nComputeDisplacementPageTableGeneration = nPageTableGeneration;
			m_bComputeDisplacementDirty.store( false, T3D_MEMORY_ORDER_RELEASE );
		}
	}
}

void Terrain3D::updateDisplacementBuffer( void ) {
	if ( m_nTessellationLevel == 0 ) {
		m_bComputeDisplacementDirty.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_nComputeDisplacementPageTableGeneration = UINT64_MAX;
		invalidateComputeDisplacementRequest();
		return;
	}
	m_bComputeDisplacementDirty.store( true, T3D_MEMORY_ORDER_RELEASE );
	LOG( INFO, "Updating direct RenderingDevice displacement buffer" );
	const uint64_t nPageTableGeneration = m_pData ? m_pData->getPhysicalRegionMapGenerationInternal() : 0u;
	if ( tryUpdateComputeDisplacement( getClipmapTargetPosition() ) ) {
		m_nComputeDisplacementPageTableGeneration = nPageTableGeneration;
		m_bComputeDisplacementDirty.store( false, T3D_MEMORY_ORDER_RELEASE );
	}
}

bool Terrain3D::tryUpdateComputeDisplacement( const Vector3 &rTargetPosition ) {
	m_bComputeDisplacementActive.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( !m_bComputeDisplacementEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) || m_bComputeDisplacementFailed.load( T3D_MEMORY_ORDER_ACQUIRE )
	        || m_bComputeDisplacementTearingDown.load( T3D_MEMORY_ORDER_ACQUIRE ) || !m_pData || m_material.is_null() ) {
		return false;
	}
	Terrain3DCompute *const pCompute = m_pData->getComputeBackend();
	t3dDisplacementRequest_t request;
	if ( !pCompute || !pCompute->prepareDisplacementRequest( this, rTargetPosition, request ) ) {
		invalidateComputeDisplacementRequest();
		return false;
	}
	{
		Terrain3DScopedLock lock( m_computeDisplacementMutex );
		m_computeDisplacementRequest = eastl::move( request );
		++m_nComputeDisplacementRequestVersion;
	}
	queueComputeDisplacementRender();
	return true;
}

void Terrain3D::invalidateComputeDisplacementRequest( void ) {
	m_bComputeDisplacementActive.store( false, T3D_MEMORY_ORDER_RELEASE );
	Terrain3DScopedLock lock( m_computeDisplacementMutex );
	m_computeDisplacementRequest = t3dDisplacementRequest_t{};
	m_hComputeDisplacementTextureRd = RID();
	++m_nComputeDisplacementRequestVersion;
}

RID Terrain3D::getComputeDisplacementTextureRid( void ) const {
	Terrain3DScopedLock lock( m_computeDisplacementMutex );
	return m_hComputeDisplacementTextureRd;
}

void Terrain3D::queueComputeDisplacementRender( void ) {
	if ( m_bComputeDisplacementTearingDown.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return;
	}
	bool bExpected = false;
	if ( !m_bComputeDisplacementDispatchPending.compare_exchange_strong(
	             bExpected, true, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderUpdateComputeDisplacement();
	} else if ( !Terrain3DRdContext::enqueueOnRenderThread( callable_mp( this, &Terrain3D::renderUpdateComputeDisplacement ) ) ) {
		m_bComputeDisplacementDispatchPending.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bComputeDisplacementFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		m_bComputeDisplacementFallbackRequested.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
}

void Terrain3D::renderUpdateComputeDisplacement( void ) {
	t3dDisplacementRequest_t request;
	uint64_t request_version = 0u;
	{
		Terrain3DScopedLock lock( m_computeDisplacementMutex );
		request = m_computeDisplacementRequest;
		request_version = m_nComputeDisplacementRequestVersion;
	}

	bool bSuccess = false;
	RID texture;
	Terrain3DCompute *const pCompute = m_pData ? m_pData->getComputeBackendIfCreated() : NULL;
	if ( !m_bComputeDisplacementTearingDown.load( T3D_MEMORY_ORDER_ACQUIRE ) && pCompute ) {
		bSuccess = pCompute->dispatchDisplacementRequest( request, texture ) && texture.is_valid();
	}
	bool bRequestCurrent = false;
	{
		Terrain3DScopedLock lock( m_computeDisplacementMutex );
		bRequestCurrent = m_nComputeDisplacementRequestVersion == request_version;
		if ( bSuccess && bRequestCurrent && m_bComputeDisplacementEnabled.load( T3D_MEMORY_ORDER_ACQUIRE )
		        && !m_bComputeDisplacementTearingDown.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
			m_hComputeDisplacementTextureRd = texture;
		} else if ( bRequestCurrent ) {
			m_hComputeDisplacementTextureRd = RID();
		}
	}
	if ( bSuccess && bRequestCurrent && m_bComputeDisplacementEnabled.load( T3D_MEMORY_ORDER_ACQUIRE )
	        && !m_bComputeDisplacementTearingDown.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		m_bComputeDisplacementActive.store( true, T3D_MEMORY_ORDER_RELEASE );
	} else if ( !bSuccess && !m_bComputeDisplacementTearingDown.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		m_bComputeDisplacementActive.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bComputeDisplacementFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		m_bComputeDisplacementFallbackRequested.store( true, T3D_MEMORY_ORDER_RELEASE );
	}

	m_bComputeDisplacementDispatchPending.store( false, T3D_MEMORY_ORDER_RELEASE );
	bool bNewerRequest = false;
	bool bNewerRequestValid = false;
	{
		Terrain3DScopedLock lock( m_computeDisplacementMutex );
		bNewerRequest = m_nComputeDisplacementRequestVersion != request_version;
		bNewerRequestValid = m_computeDisplacementRequest.valid();
	}
	if ( bNewerRequest && bNewerRequestValid && m_bComputeDisplacementEnabled.load( T3D_MEMORY_ORDER_ACQUIRE )
	        && !m_bComputeDisplacementFailed.load( T3D_MEMORY_ORDER_ACQUIRE )
	        && !m_bComputeDisplacementTearingDown.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		queueComputeDisplacementRender();
	}
}

void Terrain3D::renderShutdownComputeDisplacement( void ) {
	{
		Terrain3DScopedLock lock( m_computeDisplacementMutex );
		m_hComputeDisplacementTextureRd = RID();
	}
	Terrain3DCompute *const pCompute = m_pData ? m_pData->getComputeBackendIfCreated() : NULL;
	if ( pCompute ) {
		pCompute->shutdownDisplacementResources();
	}
}

void Terrain3D::setComputeDisplacementEnabled( const bool bEnabled ) {
	const bool bOldEnabled = m_bComputeDisplacementEnabled.exchange( bEnabled, T3D_MEMORY_ORDER_ACQ_REL );
	if ( bOldEnabled == bEnabled ) {
		return;
	}
	invalidateComputeDisplacementRequest();
	m_bComputeDisplacementFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bComputeDisplacementFallbackRequested.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_lastBufferPosition = s_v2Max;
	if ( m_bInitialized ) {
		updateDisplacementBuffer();
	}
}

void Terrain3D::setDisplacementUpdateDistance( const real_t fDistance ) {
	SET_IF_DIFF( m_fDisplacementUpdateDistance, CLAMP( fDistance, 0.0f, 16.0f ) );
	m_lastBufferPosition = s_v2Max;
}

void Terrain3D::destroyLabels( void ) {
	if ( !m_pLabelParent ) {
		return;
	}
	const Array labels = m_pLabelParent->get_children();
	LOG( DEBUG, "Destroying ", labels.size(), " region labels" );
	for ( size_t iLabel = 0u; iLabel < ( size_t )( ( labels ).size() ); iLabel++ ) {
		const Variant &var = ( labels )[iLabel];
		Node *const pLabel = cast_to<Node>( var );
		if ( pLabel ) {
			memdelete( pLabel );
		}
	}
}

void Terrain3D::destroyDisplacementBuffer( void ) {
	LOG( INFO, "[compute] Draining displacement work and releasing buffer resources" );
	m_bComputeDisplacementTearingDown.store( true, T3D_MEMORY_ORDER_RELEASE );
	if ( m_pData && m_pData->getComputeBackendIfCreated() ) {
		if ( Terrain3DRdContext::isOnRenderThread() ) {
			renderShutdownComputeDisplacement();
		} else if ( Terrain3DRdContext::enqueueOnRenderThread( callable_mp( this, &Terrain3D::renderShutdownComputeDisplacement ) ) ) {
			// Lifecycle-only synchronization: all queued displacement work and its
			// renderer-owned resources must be gone before the compute backend or node dies.
			Terrain3DRdContext::forceSync();
		}
	}
	m_bComputeDisplacementDispatchPending.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bComputeDisplacementActive.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bComputeDisplacementFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bComputeDisplacementFallbackRequested.store( false, T3D_MEMORY_ORDER_RELEASE );
	{
		Terrain3DScopedLock lock( m_computeDisplacementMutex );
		m_computeDisplacementRequest = t3dDisplacementRequest_t{};
		m_hComputeDisplacementTextureRd = RID();
		++m_nComputeDisplacementRequestVersion;
	}
	m_bComputeDisplacementTearingDown.store( false, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3D::setupMousePicking( void ) {
	LOG( DEBUG, "Mouse picking uses the direct Terrain3DData heightfield query; no viewport resources are required" );
}

void Terrain3D::destroyMousePicking( void ) {
	// Direct heightfield picking owns no SceneTree or RenderingServer resources.
}

void Terrain3D::destroyInstancePhysics( void ) {
	if ( m_pInstancePhysics ) {
		m_pInstancePhysics->destroy();
		memdelete( m_pInstancePhysics );
		m_pInstancePhysics = NULL;
	}
}

void Terrain3D::destroyInstanceOcclusion( void ) {
	if ( m_pInstanceOcclusion ) {
		m_pInstanceOcclusion->destroy();
		memdelete( m_pInstanceOcclusion );
		m_pInstanceOcclusion = NULL;
	}
}

void Terrain3D::destroyInstancer( void ) {
	LOG( INFO, "Destroying Instancer" );
	if ( m_pInstancer ) {
		memdelete( m_pInstancer );
		m_pInstancer = NULL;
	}
}

void Terrain3D::destroyNodeStreamer( const bool bCaptureState ) {
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->destroy( bCaptureState );
		memdelete( m_pNodeStreamer );
		m_pNodeStreamer = NULL;
	}
}

void Terrain3D::destroyLightmapStreamer( void ) {
	if ( m_pLightmapStreamer ) {
		m_pLightmapStreamer->destroy();
		memdelete( m_pLightmapStreamer );
		m_pLightmapStreamer = NULL;
	}
}

void Terrain3D::destroyHlodStreamer( void ) {
	if ( m_pHlodStreamer ) {
		m_pHlodStreamer->destroy();
		memdelete( m_pHlodStreamer );
		m_pHlodStreamer = NULL;
	}
}

void Terrain3D::setLabelDistance( const real_t fDistance ) {
	SET_IF_DIFF( m_fLabelDistance, CLAMP( fDistance, 0.f, 100000.f ) );
	LOG( INFO, "Setting region label distance: ", m_fLabelDistance );
	updateRegionLabels();
}

void Terrain3D::setLabelSize( const int nSize ) {
	SET_IF_DIFF( m_nLabelSize, CLAMP( nSize, 24, 128 ) );
	LOG( INFO, "Setting region label size: ", m_nLabelSize );
	updateRegionLabels();
}

void Terrain3D::updateRegionLabels( void ) {
	destroyLabels();
	if ( m_pLabelParent && m_fLabelDistance > 0.f && m_pData ) {
		const TypedArray<Vector2i> region_locations = m_pData->getRegionLocations();
		LOG( DEBUG, "Creating ", region_locations.size(), " region labels" );
		for ( size_t iRegion = 0u; iRegion < ( size_t )( ( region_locations ).size() ); iRegion++ ) {
			const Vector2i region_loc = ( region_locations )[iRegion];
			Label3D *const pLabel = memnew( Label3D );
			const String text = UtilityFunctions::str( region_loc );
			pLabel->set_name( String( "Label3D" ) + text.replace( " ", "" ) );
			pLabel->set_pixel_size( .001f );
			pLabel->set_billboard_mode( BaseMaterial3D::BILLBOARD_ENABLED );
			pLabel->set_draw_flag( Label3D::FLAG_DOUBLE_SIDED, true );
			pLabel->set_draw_flag( Label3D::FLAG_DISABLE_DEPTH_TEST, true );
			pLabel->set_draw_flag( Label3D::FLAG_FIXED_SIZE, true );
			pLabel->set_render_priority( 127 );
			pLabel->set_outline_render_priority( 126 );
			pLabel->set_text( text );
			pLabel->set_modulate( Color( 1.f, 1.f, 1.f, .5f ) );
			pLabel->set_outline_modulate( Color( 0.f, 0.f, 0.f, .5f ) );
			pLabel->set_font_size( m_nLabelSize );
			pLabel->set_outline_size( m_nLabelSize / 6 );
			pLabel->set_visibility_range_end( m_fLabelDistance );
			pLabel->set_visibility_range_end_margin( m_fLabelDistance / 10.f );
			pLabel->set_visibility_range_fade_mode( GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF );
			m_pLabelParent->add_child( pLabel, true );
			Vector3 pos = Vector3( ( real_t )( region_loc.x ) + .5f, 0.f, ( real_t )( region_loc.y ) + .5f ) * T3D_REGION_SIZE
			        * s_fRegionVertexSpacing;
			const real_t fHeight = m_pData->getHeight( pos );
			pos.y = ( isnan( fHeight ) ) ? 0.f : fHeight;
			pLabel->set_position( pos );
		}
	}
}

void Terrain3D::setCamera( Camera3D *const pCamera ) {
	if ( m_hCamera.ptr() != pCamera ) {
		LOG( EXTREME, "Setting camera: ", pCamera );
		m_hCamera.setTarget( pCamera );
		// Editor authoring uses the camera itself as the clipmap target. Re-enable
		// servicing when a valid camera arrives even when no explicit target exists.
		if ( m_hCamera.isValid() || m_clipmapTarget.isValid() ) {
			set_physics_process( true );
		}
		// Camera handoff is a topology-position event, not a normal movement event.
		// Publish the new patch origins immediately instead of waiting for a physics
		// tick. This is required in the editor because the first camera reference is
		// commonly supplied by _forward_3d_gui_input() after the terrain has already
		// published its startup snapshot.
		if ( m_bInitialized && ( m_hCamera.isValid() || m_clipmapTarget.isValid() ) ) {
			synchronizeClipmapNow( getClipmapTargetPosition() );
		}
	}
}

void Terrain3D::setClipmapTarget( Node3D *const pNode ) {
	if ( m_clipmapTarget.ptr() != pNode ) {
		LOG( INFO, "Setting clipmap target: ", pNode );
		m_clipmapTarget.setTarget( pNode );
		if ( m_clipmapTarget.isValid() ) {
			set_physics_process( true );
		}
		if ( m_bInitialized && ( m_clipmapTarget.isValid() || m_hCamera.isValid() ) ) {
			synchronizeClipmapNow( getClipmapTargetPosition() );
		}
	}
}

Vector3 Terrain3D::getClipmapTargetPosition( void ) const {
	// In Editor, or no clipmap target, use camera
	if ( IS_EDITOR || !m_clipmapTarget.getTarget() ) {
		if ( Node3D *cam = m_hCamera.getTarget() ) {
			return cam->get_global_position();
		}
	}
	if ( Node3D *target = m_clipmapTarget.getTarget() ) {
		return target->get_global_position();
	}
	return s_v3Zero;
}

void Terrain3D::setCollisionTarget( Node3D *const pNode ) {
	if ( m_collisionTarget.ptr() != pNode ) {
		LOG( INFO, "Setting collision target: ", pNode );
		m_collisionTarget.setTarget( pNode );
		if ( m_collisionTarget.isValid() ) {
			set_physics_process( true );
		}
	}
}

Vector3 Terrain3D::getCollisionTargetPosition( void ) const {
	// In Editor, always prefer camera
	if ( IS_EDITOR ) {
		if ( Node3D *cam = m_hCamera.getTarget() ) {
			return cam->get_global_position();
		}
	}
	if ( Node3D *target = m_collisionTarget.getTarget() ) {
		return target->get_global_position();
	}
	if ( Node3D *target = m_clipmapTarget.getTarget() ) {
		return target->get_global_position();
	}
	if ( Node3D *cam = m_hCamera.getTarget() ) {
		return cam->get_global_position();
	}
	return s_v3Zero;
}

void Terrain3D::updateEnvironmentShadowCasterMask( void ) {
	DirectionalLight3D *const pLight = Object::cast_to<DirectionalLight3D>( m_lightTarget.ptr() );
	if ( !pLight ) {
		return;
	}
	if ( !m_bSemanticShadowPolicyEnabled ) {
		if ( m_bLightShadowMaskCaptured ) {
			pLight->set_shadow_caster_mask( m_nLightShadowCasterMaskBeforeSemantic );
			m_bLightShadowMaskCaptured = false;
		}
		return;
	}
	if ( !m_bLightShadowMaskCaptured ) {
		m_nLightShadowCasterMaskBeforeSemantic = pLight->get_shadow_caster_mask();
		m_bLightShadowMaskCaptured = true;
	}

	uint32_t mask = m_nEnvironmentShadowExternalMask | T3D_LIGHTING_LAYER_REGIONAL;
	if ( m_environmentShadowQuality >= ENVIRONMENT_SHADOW_NORMAL ) {
		mask |= T3D_LIGHTING_LAYER_LARGE;
	}
	if ( m_environmentShadowQuality >= ENVIRONMENT_SHADOW_ULTRA ) {
		mask |= T3D_LIGHTING_LAYER_FOLIAGE;
	}
	pLight->set_shadow_caster_mask( mask );
}

void Terrain3D::setSemanticShadowPolicyEnabled( const bool bEnabled ) {
	if ( m_bSemanticShadowPolicyEnabled == bEnabled ) {
		return;
	}
	m_bSemanticShadowPolicyEnabled = bEnabled;
	updateEnvironmentShadowCasterMask();
}

void Terrain3D::setEnvironmentShadowQuality( const t3dEnvironmentShadowQuality_t eQuality ) {
	const t3dEnvironmentShadowQuality_t quality = ( t3dEnvironmentShadowQuality_t )( CLAMP(
	        ( int )( eQuality ), ( int )( ENVIRONMENT_SHADOW_LOW ), ( int )( ENVIRONMENT_SHADOW_ULTRA ) ) );
	if ( m_environmentShadowQuality == quality ) {
		return;
	}
	m_environmentShadowQuality = quality;
	updateEnvironmentShadowCasterMask();
}

void Terrain3D::setEnvironmentShadowExternalMask( const uint32_t nMask ) {
	const uint32_t mask = nMask & 0xFFFFFu;
	if ( m_nEnvironmentShadowExternalMask == mask ) {
		return;
	}
	m_nEnvironmentShadowExternalMask = mask;
	updateEnvironmentShadowCasterMask();
}

void Terrain3D::updateEnvironmentDirectionalReceiverMask( void ) {
	DirectionalLight3D *const pLight = Object::cast_to<DirectionalLight3D>( m_lightTarget.ptr() );
	if ( !pLight ) {
		return;
	}
	if ( !m_bSemanticDirectionalReceptionEnabled ) {
		if ( m_bLightCullMaskCaptured ) {
			pLight->set_cull_mask( m_nLightCullMaskBeforeSemantic );
			m_bLightCullMaskCaptured = false;
		}
		return;
	}
	if ( !m_bLightCullMaskCaptured ) {
		m_nLightCullMaskBeforeSemantic = pLight->get_cull_mask();
		m_bLightCullMaskCaptured = true;
	}
	const uint32_t mask = m_nEnvironmentDirectionalExternalMask | T3D_DIRECTIONAL_RECEIVER_LAYER;
	pLight->set_cull_mask( mask & 0xFFFFFu );
}

void Terrain3D::setSemanticDirectionalReceptionEnabled( const bool bEnabled ) {
	if ( m_bSemanticDirectionalReceptionEnabled == bEnabled ) {
		return;
	}
	m_bSemanticDirectionalReceptionEnabled = bEnabled;
	updateEnvironmentDirectionalReceiverMask();
}

void Terrain3D::setEnvironmentDirectionalExternalMask( const uint32_t nMask ) {
	const uint32_t mask = nMask & 0xFFFFFu;
	if ( m_nEnvironmentDirectionalExternalMask == mask ) {
		return;
	}
	m_nEnvironmentDirectionalExternalMask = mask;
	updateEnvironmentDirectionalReceiverMask();
}

void Terrain3D::setLightTarget( Node3D *const pNode ) {
	if ( m_lightTarget.ptr() != pNode ) {
		DirectionalLight3D *const pOldLight = Object::cast_to<DirectionalLight3D>( m_lightTarget.ptr() );
		if ( m_bSemanticShadowPolicyEnabled && m_bLightShadowMaskCaptured && pOldLight ) {
			pOldLight->set_shadow_caster_mask( m_nLightShadowCasterMaskBeforeSemantic );
		}
		if ( m_bSemanticDirectionalReceptionEnabled && m_bLightCullMaskCaptured && pOldLight ) {
			pOldLight->set_cull_mask( m_nLightCullMaskBeforeSemantic );
		}
		m_bLightShadowMaskCaptured = false;
		m_bLightCullMaskCaptured = false;
		LOG( INFO, "Setting directional light target: ", pNode );
		m_lightTarget.setTarget( pNode );
		m_bLightStateValid = false;
		updateEnvironmentShadowCasterMask();
		updateEnvironmentDirectionalReceiverMask();
		if ( m_lightTarget.isValid() ) {
			set_physics_process( true );
		}
	}
}

void Terrain3D::synchronizeClipmapNow( const Vector3 &rTargetPosition ) {
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->resetTargetPosition();
		m_pTerrainMesher->snap( rTargetPosition );
	}
	if ( m_bOceanEnabled && m_pOceanMesher ) {
		m_pOceanMesher->resetTargetPosition();
		m_pOceanMesher->snap( rTargetPosition );
	}
	if ( m_pCollision ) {
		m_pCollision->resetTargetPosition();
	}
	if ( m_pInstancer ) {
		m_pInstancer->synchronizeStreamingTarget( rTargetPosition );
	}
	if ( m_nTessellationLevel > 0 ) {
		m_lastBufferPosition = s_v2Max;
	}
	if ( m_bInitialized && m_pRenderCoordinator ) {
		m_pRenderCoordinator->update();
	}
}

void Terrain3D::snap( void ) {
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->resetTargetPosition();
	}
	if ( m_bOceanEnabled && m_pOceanMesher ) {
		m_pOceanMesher->resetTargetPosition();
	}
	if ( m_pCollision ) {
		m_pCollision->resetTargetPosition();
	}
	if ( m_nTessellationLevel > 0 ) {
		m_lastBufferPosition = s_v2Max;
	}
}

void Terrain3D::setMaterial( const Ref<Terrain3DMaterial> &rMaterial ) {
	if ( !t3dDiffers( m_material, rMaterial ) ) {
		return;
	}
	LOG( INFO, "Replacing terrain material" );
	const bool bWasInitialized = m_bInitialized;
	const bool bSuppliedMaterial = rMaterial.is_valid();
	const Ref<Terrain3DMaterial> pOldMaterial = m_material;
	disconnectResourceSignals( m_assets, pOldMaterial );
	if ( pOldMaterial.is_valid() ) {
		pOldMaterial->uninitialize( this );
	}
	m_material = rMaterial;
	if ( m_material.is_null() ) {
		m_material.instantiate();
	}
	if ( m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL && !bSuppliedMaterial ) {
		m_material->setShaderPerformancePreset( SHADER_PRESET_XBOX_360_AUTHENTIC );
	}
	m_bPerformanceProfileMaterialPendingApply = false;
	m_bLightStateValid = false;
	connectResourceSignals( m_assets, m_material );
	applyAutoShaderProfileMode();
	if ( bWasInitialized ) {
		m_material->initialize( this );
		serviceMemoryBudgetPolicy( true );
		setupTerrainMesher();
		updateDisplacementBuffer();
	} else {
		initializeInternal();
	}
	refreshPerformanceProfileConformity();
	emit_signal( "material_changed" );
}

void Terrain3D::setRuntimeShaderTieringEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bRuntimeShaderTieringEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	LOG( INFO, "Setting runtime shader tiering: ", bEnabled );
	if ( bEnabled && m_pData && m_bInitialized && !m_pData->ensureFarAtlases() ) {
		LOG( WARN,
		        "Runtime far shader tier could not prepare coarse terrain atlases; far-tier rendering may use fallback "
		        "textures until terrain data is ready." );
	}
	if ( m_material.is_valid() && m_material->isInitialized() ) {
		m_material->update( FULL_REBUILD );
	}
	if ( m_bInitialized ) {
		setupTerrainMesher();
	}
}

void Terrain3D::setRuntimeNearShaderLods( const int nCount ) {
	const int count = CLAMP( nCount, 0, getTotalClipmapLevels() );
	if ( m_nRuntimeNearShaderLods == count ) {
		return;
	}
	m_nRuntimeNearShaderLods = count;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( true );
	notify_property_list_changed();
}

void Terrain3D::setRuntimeMiddleShaderLods( const int nCount ) {
	const int count = CLAMP( nCount, 0, getTotalClipmapLevels() - m_nRuntimeNearShaderLods );
	if ( m_nRuntimeMiddleShaderLods == count ) {
		return;
	}
	m_nRuntimeMiddleShaderLods = count;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( true );
	notify_property_list_changed();
}

void Terrain3D::setRuntimeNearRangeEndM( const real_t fRangeM ) {
	setRuntimeNearShaderLods( rangeToLodCount( MAX( fRangeM, ( real_t )( 0.0f ) ) ) );
}

real_t Terrain3D::getRuntimeNearRangeEndM( void ) const {
	return lodCountToRange( m_nRuntimeNearShaderLods );
}

void Terrain3D::setRuntimeMiddleRangeEndM( const real_t fRangeM ) {
	const int total_non_far = MAX( m_nRuntimeNearShaderLods, rangeToLodCount( MAX( fRangeM, ( real_t )( 0.0f ) ) ) );
	setRuntimeMiddleShaderLods( total_non_far - m_nRuntimeNearShaderLods );
}

real_t Terrain3D::getRuntimeMiddleRangeEndM( void ) const {
	return lodCountToRange( m_nRuntimeNearShaderLods + m_nRuntimeMiddleShaderLods );
}

void Terrain3D::setRuntimeSyncDetailLods( const bool bEnabled ) {
	if ( m_bRuntimeSyncDetailLods == bEnabled ) {
		return;
	}
	m_bRuntimeSyncDetailLods = bEnabled;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( true );
}

void Terrain3D::setRuntimeClipmapUpdateBudget( const int nCount ) {
	const int count = CLAMP( nCount, 1, T3D_MAX_CLIPMAP_LEVELS );
	if ( m_nRuntimeClipmapUpdateBudget == count ) {
		return;
	}
	m_nRuntimeClipmapUpdateBudget = count;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( false );
}

void Terrain3D::setRuntimePagePrepareThreads( const int nCount ) {
	const int count = CLAMP( nCount, 1, 32 );
	if ( m_nRuntimePagePrepareThreads == count ) {
		return;
	}
	m_nRuntimePagePrepareThreads = count;
	compileRuntimePageNumericConfig();
	refreshPerformanceProfileConformity();
}

void Terrain3D::setMeshLods( const int nCount ) {
	SET_IF_DIFF( m_nMeshLods, CLAMP( nCount, 1, 10 ) );
	refreshPerformanceProfileConformity();
	normalizeRuntimeTierLayout( true );
	LOG( INFO, "Setting mesh levels: ", m_nMeshLods );
	if ( m_pTerrainMesher && m_material.is_valid() ) {
		m_material->update();
		setupTerrainMesher();
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		reconfigureGpuRegionCache();
	}
}

void Terrain3D::setTessellationLevel( const int nLevel ) {
	const int old_level = m_nTessellationLevel;
	SET_IF_DIFF( m_nTessellationLevel, CLAMP( nLevel, 0, 6 ) );
	refreshPerformanceProfileConformity();
	normalizeRuntimeTierLayout( true );
	LOG( INFO, "Setting tessellation level: ", nLevel );
	// The optimized R8 displacement array exists only while tessellation is
	// active. Crossing zero creates or frees it before rebuilding shaders.
	if ( ( old_level == 0 ) != ( m_nTessellationLevel == 0 ) && m_assets.is_valid() && m_assets->isInitialized()
	        && m_assets->usesPackedPbr() ) {
		m_assets->updateTextureList();
	}
	if ( m_pTerrainMesher && m_material.is_valid() ) {
		m_material->update( FULL_REBUILD );
		setupTerrainMesher();
		updateDisplacementBuffer();
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		reconfigureGpuRegionCache();
	}
	notify_property_list_changed();
}

void Terrain3D::setMeshSize( const int nSize ) {
	SET_IF_DIFF( m_nMeshSize, CLAMP( nSize & ~1, 8, 256 ) ); // Ensure even
	refreshPerformanceProfileConformity();
	normalizeRuntimeTierLayout( true );
	LOG( INFO, "Setting mesh size: ", m_nMeshSize );
	if ( m_pTerrainMesher && m_material.is_valid() ) {
		m_material->update();
		setupTerrainMesher();
		updateDisplacementBuffer();
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		reconfigureGpuRegionCache();
	}
}

void Terrain3D::setCullMargin( const real_t fMargin ) {
	SET_IF_DIFF( m_fCullMargin, CLAMP( fMargin, 0.f, 100000.f ) );
	LOG( INFO, "Setting extra cull margin: ", m_fCullMargin );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->updateAabbs();
	}
}

void Terrain3D::setCastShadows( const GeometryInstance3D::ShadowCastingSetting castShadows ) {
	SET_IF_DIFF( m_castShadows, castShadows );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setCastShadows( m_castShadows );
	}
}

void Terrain3D::setShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nShadowLodCount, CLAMP( nCount, 0, T3D_MAX_CLIPMAP_LEVELS ) );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setShadowLodCount( m_nShadowLodCount );
	}
}

void Terrain3D::setDominantSunOnly( const bool bEnabled ) {
	SET_IF_DIFF( m_bDominantSunOnly, bEnabled );
	refreshPerformanceProfileConformity();
	LOG( INFO, "Setting terrain dominant-sun-only Forward+ lighting: ", bEnabled );
}

void Terrain3D::setNearShadowEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bNearShadowEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowEnabled( { m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled } );
	}
}

void Terrain3D::setMiddleShadowEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bMiddleShadowEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowEnabled( { m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled } );
	}
}

void Terrain3D::setFarShadowEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bFarShadowEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowEnabled( { m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled } );
	}
}

void Terrain3D::setNearShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nNearShadowLodCount, CLAMP( nCount, -1, T3D_MAX_CLIPMAP_LEVELS ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts( { m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void Terrain3D::setMiddleShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nMiddleShadowLodCount, CLAMP( nCount, -1, T3D_MAX_CLIPMAP_LEVELS ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts( { m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void Terrain3D::setFarShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nFarShadowLodCount, CLAMP( nCount, -1, T3D_MAX_CLIPMAP_LEVELS ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts( { m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void Terrain3D::setGiMode( const GeometryInstance3D::GIMode giMode ) {
	SET_IF_DIFF( m_giMode, giMode );
	refreshPerformanceProfileConformity();
}

void Terrain3D::setRenderLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_nRenderLayers, nLayers );
	LOG( INFO, "Setting terrain render layers to: ", nLayers );
}

void Terrain3D::setOceanPerformancePreset( const t3dOceanPerformancePreset_t preset ) {
	if ( m_oceanPerformancePreset == preset ) {
		return;
	}
	m_oceanPerformancePreset = preset;
	switch ( preset ) {
		case OCEAN_PRESET_BORDER_MINIMUM:
			m_nOceanVertexWaveIterations = 0;
			m_nOceanFragmentWaveIterations = 0;
			m_oceanRefractionMode = OCEAN_REFRACTION_OFF;
			m_oceanFoamMode = OCEAN_FOAM_OFF;
			m_oceanScatteringMode = OCEAN_SCATTERING_OFF;
			m_oceanShadingMode = OCEAN_SHADING_LAMBERT_DIFFUSE;
			break;
		case OCEAN_PRESET_BORDER_BALANCED:
			m_nOceanVertexWaveIterations = 1;
			m_nOceanFragmentWaveIterations = 1;
			m_oceanRefractionMode = OCEAN_REFRACTION_DEPTH_TINT;
			m_oceanFoamMode = OCEAN_FOAM_SHORE;
			m_oceanScatteringMode = OCEAN_SCATTERING_APPROXIMATE;
			m_oceanShadingMode = OCEAN_SHADING_LAMBERT_GGX;
			break;
		case OCEAN_PRESET_HIGH:
			m_nOceanVertexWaveIterations = 1;
			m_nOceanFragmentWaveIterations = 2;
			m_oceanRefractionMode = OCEAN_REFRACTION_FULL;
			m_oceanFoamMode = OCEAN_FOAM_SHORE;
			m_oceanScatteringMode = OCEAN_SCATTERING_APPROXIMATE;
			m_oceanShadingMode = OCEAN_SHADING_LAMBERT_GGX;
			break;
		case OCEAN_PRESET_CINEMATIC:
			m_nOceanVertexWaveIterations = 2;
			m_nOceanFragmentWaveIterations = 3;
			m_oceanRefractionMode = OCEAN_REFRACTION_FULL;
			m_oceanFoamMode = OCEAN_FOAM_FULL;
			m_oceanScatteringMode = OCEAN_SCATTERING_FULL;
			m_oceanShadingMode = OCEAN_SHADING_BURLEY_GGX;
			break;
		case OCEAN_PRESET_CUSTOM:
		default:
			break;
	}
	markOceanPublicationDirty();
}

void Terrain3D::setOceanVertexWaveIterations( const int nIterations ) {
	const int value = CLAMP( nIterations, 0, 3 );
	if ( m_nOceanVertexWaveIterations == value ) {
		return;
	}
	m_nOceanVertexWaveIterations = value;
	m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;
	markOceanPublicationDirty();
}
void Terrain3D::setOceanFragmentWaveIterations( const int nIterations ) {
	const int value = CLAMP( nIterations, 0, 4 );
	if ( m_nOceanFragmentWaveIterations == value ) {
		return;
	}
	m_nOceanFragmentWaveIterations = value;
	m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;
	markOceanPublicationDirty();
}
void Terrain3D::setOceanEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bOceanEnabled, bEnabled );
	markOceanPublicationDirty();
	LOG( INFO, "Setting ocean enabled: ", m_bOceanEnabled );
	if ( m_bOceanEnabled ) {
		if ( m_oceanSettings.is_null() ) {
			ResourceLoader *const pRl = ResourceLoader::get_singleton();
			if ( pRl ) {
				const Ref<Terrain3DOceanSettings> defaults = pRl->load( OCEAN_SETTINGS_PATH );
				if ( defaults.is_valid() ) {
					const Ref<Resource> duplicate = defaults->duplicate( true );
					const Ref<Terrain3DOceanSettings> local = duplicate;
					m_oceanSettings = local.is_valid() ? local : defaults;
				}
			}
			if ( m_oceanSettings.is_null() ) {
				m_oceanSettings.instantiate();
			}
			connectOceanSettingsPublicationSignal();
		}
		setupOceanMesher();
	} else {
		destroyOceanMesher( false );
	}
	notify_property_list_changed();
}

void Terrain3D::setOceanMeshLods( const int nCount ) {
	SET_IF_DIFF( m_nOceanMeshLods, CLAMP( nCount, 1, 10 ) );
	markOceanPublicationDirty();
	LOG( INFO, "Setting ocean mesh levels: ", m_nOceanMeshLods );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanTessellationLevel( const int nLevel ) {
	SET_IF_DIFF( m_nOceanTessellationLevel, CLAMP( nLevel, 0, 6 ) );
	markOceanPublicationDirty();
	LOG( INFO, "Setting ocean tessellation level: ", nLevel );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanMeshSize( const int nSize ) {
	SET_IF_DIFF( m_nOceanMeshSize, CLAMP( nSize & ~1, 8, 256 ) ); // Ensure even
	markOceanPublicationDirty();
	LOG( INFO, "Setting ocean mesh size: ", m_nOceanMeshSize );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanVertexSpacing( const real_t fSpacing ) {
	SET_IF_DIFF( m_fOceanVertexSpacing, CLAMP( fSpacing, 0.25f, 100.0f ) );
	markOceanPublicationDirty();
	LOG( INFO, "Setting ocean vertex spacing: ", m_fOceanVertexSpacing );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanCullMargin( const real_t fMargin ) {
	SET_IF_DIFF( m_fOceanCullMargin, CLAMP( fMargin, 0.f, 100000.f ) );
	markOceanPublicationDirty();
	LOG( INFO, "Setting extra cull margin: ", m_fOceanCullMargin );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->updateAabbs( m_fOceanCullMargin, s_v2Zero );
	}
}

void Terrain3D::setOceanCastShadows( const GeometryInstance3D::ShadowCastingSetting castShadows ) {
	SET_IF_DIFF( m_oceanCastShadows, castShadows );
	markOceanPublicationDirty();
	if ( m_pOceanMesher ) {
		m_pOceanMesher->setCastShadows( m_oceanCastShadows );
	}
}

void Terrain3D::setOceanGiMode( const GeometryInstance3D::GIMode giMode ) {
	SET_IF_DIFF( m_oceanGiMode, giMode );
	markOceanPublicationDirty();
}

void Terrain3D::setOceanRenderLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_nOceanRenderLayers, nLayers );
	markOceanPublicationDirty();
	LOG( INFO, "Setting ocean render layers to: ", nLayers );
}

void Terrain3D::setOceanSettings( const Ref<Terrain3DOceanSettings> &rSettings ) {
	if ( m_oceanSettings == rSettings ) {
		return;
	}
	disconnectOceanSettingsPublicationSignal();
	m_oceanSettings = rSettings;
	connectOceanSettingsPublicationSignal();
	markOceanPublicationDirty();
	LOG( INFO, "Setting ocean settings" );
}

void Terrain3D::markOceanPublicationDirty( void ) {
	m_nOceanPublicationGeneration = t3dNextGeneration( m_nOceanPublicationGeneration );
}

void Terrain3D::connectOceanSettingsPublicationSignal( void ) {
	if ( m_oceanSettings.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &Terrain3D::markOceanPublicationDirty );
	if ( !m_oceanSettings->is_connected( "changed", callback ) ) {
		m_oceanSettings->connect( "changed", callback );
	}
}

void Terrain3D::disconnectOceanSettingsPublicationSignal( void ) {
	if ( m_oceanSettings.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &Terrain3D::markOceanPublicationDirty );
	if ( m_oceanSettings->is_connected( "changed", callback ) ) {
		m_oceanSettings->disconnect( "changed", callback );
	}
}

Vector3 Terrain3D::getIntersection( const Vector3 &rSrcPos, const Vector3 &rDirection, const bool bGpuMode ) {
	if ( rDirection.is_zero_approx() || !rDirection.is_finite() ) {
		LOG( ERROR, "Invalid direction vector: ", rDirection );
		return s_v3Nan;
	}
	if ( !rSrcPos.is_finite() ) {
		LOG( ERROR, "Invalid source vector: ", rSrcPos );
		return s_v3Nan;
	}

	Vector3 direction = rDirection.normalized();
	// Looking straight down inside a region can use getHeight() directly.
	if ( direction.y < -.99999f ) {
		const real_t fHeight = m_pData->getHeight( rSrcPos );
		if ( std ::isfinite( fHeight ) ) {
			return Vector3( rSrcPos.x, fHeight, rSrcPos.z );
		}
	}

	// Raymarching mode
	if ( !bGpuMode ) {
		// Must start above terrain if in a region
		real_t fHeight = m_pData->getHeight( rSrcPos );
		if ( fHeight > rSrcPos.y ) { // False if Nan
			return s_v3Max;
		}
		// Raymarch down the ray in small increments until we find the terrain height
		Vector3 point = rSrcPos;
		for ( int i = 0; i < 4000; i++ ) {
			fHeight = m_pData->getHeight( point );
			if ( point.y - fHeight <= 0.f ) { // Nan comparison is false, which continues loop
				return point;
			}
			point += direction;
		}
		return s_v3Max;

	} else {
		// Compatibility GPU mode now selects the precise world-query path. The
		// heightfield intersection is solved per traversed grid cell against the
		// same bilinear CPU sampling surface used by authoring, avoiding viewport
		// rendering, shader compilation and synchronous texture readback.
		Vector3 hitPosition;
		if ( m_pData->intersectHeightfieldRay( rSrcPos, direction, 100000.0f, hitPosition ) ) {
			return hitPosition;
		}
		// Preserve the editor orthographic fallback used when the camera origin is
		// intentionally parked at an extreme altitude over empty terrain.
		if ( direction.y < -.99999f && rSrcPos.y >= 100000.f ) {
			return Vector3( rSrcPos.x, 0.f, rSrcPos.z );
		}
		return s_v3Max;
	}
}

void Terrain3D::setupRenderCoordinator( void ) {
	const uint64_t nStartedUsec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][CoordinatorSetup] BEGIN generation=", m_nStartupGeneration );
	if ( !m_pRenderCoordinator ) {
		m_pRenderCoordinator = memnew( Terrain3DRenderCoordinator );
		if ( !m_pRenderCoordinator->bindRuntimeAllocator( getRuntimeMemoryZoneInternal(), getRuntimeAllocatorOverflowPolicyInternal() ) ) {
			LOG( ERROR,
			        "[Terrain3D][Startup][CoordinatorSetup] FAIL generation=",
			        m_nStartupGeneration,
			        " reason=allocator_bind" );
			return;
		}
	}
	if ( !m_pRenderCoordinator->prepareStartupCold( m_nStartupGeneration ) ) {
		LOG( ERROR,
		        "[Terrain3D][Startup][CoordinatorSetup] FAIL generation=",
		        m_nStartupGeneration,
		        " reason=prepare" );
		return;
	}
	LOG( INFO,
	        "[Terrain3D][Startup][CoordinatorSetup] END generation=",
	        m_nStartupGeneration,
	        " elapsed=",
	        Time::get_singleton()->get_ticks_usec() - nStartedUsec,
	        " us" );
}

void Terrain3D::destroyRenderCoordinator( const bool bFinal ) {
	if ( !m_pRenderCoordinator ) {
		return;
	}
	m_pRenderCoordinator->shutdown();
	if ( bFinal ) {
		memdelete( m_pRenderCoordinator );
		m_pRenderCoordinator = NULL;
	}
}

Dictionary Terrain3D::getRdRendererDiagnostics( void ) const {
	return m_pRenderCoordinator ? m_pRenderCoordinator->getDiagnostics() : Dictionary();
}

Dictionary Terrain3D::getGpuUploadDiagnostics(void) const {
	return Terrain3DRdContext::getShared().getGpuUploadDiagnostics();
}

Dictionary Terrain3D::bakeVirtualTextures( void ) {
	Dictionary result;
	result["complete"] = false;
	if ( !m_pRenderCoordinator ) {
		result["error"] = "Terrain3D RD coordinator is unavailable";
		return result;
	}
	String error;
	const bool bComplete = m_pRenderCoordinator->bakePreviewVirtualTexturesCold( &error );
	result["complete"] = bComplete;
	result["error"] = error;
	const Dictionary diagnostics = m_pRenderCoordinator->getDiagnostics();
	result["dirty_allocations"] = diagnostics.get( "vt_editor_dirty_allocations", 0 );
	result["cache_hits"] = diagnostics.get( "vt_editor_cache_hits", 0 );
	result["cache_misses"] = diagnostics.get( "vt_editor_cache_misses", 0 );
	result["package_bytes"] = diagnostics.get( "preview_vt_authoring_package_bytes", 0 );
	result["package_generation"] = diagnostics.get( "preview_vt_authoring_package_generation", 0 );
	result["republish_pending"] = diagnostics.get( "preview_vt_republish_pending", false );
	result["republish_queued"] = diagnostics.get( "preview_vt_republish_queued", false );
	result["cook_ms_extract"] = diagnostics.get( "vt_editor_cook_ms_extract", 0.0 );
	result["cook_ms_filter"] = diagnostics.get( "vt_editor_cook_ms_filter", 0.0 );
	result["cook_ms_page"] = diagnostics.get( "vt_editor_cook_ms_page", 0.0 );
	result["cook_ms_encode"] = diagnostics.get( "vt_editor_cook_ms_encode", 0.0 );
	result["cook_ms_pack"] = diagnostics.get( "vt_editor_cook_ms_pack", 0.0 );
	return result;
}

Ref<Image> Terrain3D::createRdMeshPreview( const uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = T3D_RD_PREVIEW_SYSTEMIC_RD;
	}
	return m_pRenderCoordinator ? m_pRenderCoordinator->createMeshPreview( nAssetId, rSize, pFailure ) : Ref<Image>();
}

bool Terrain3D::requestRdMeshPreviewAsync( const uint16_t nAssetId,
        const Vector2i &rSize,
        const uint64_t nCompletionOwnerId,
        const uint64_t nCompletionGeneration,
        t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = T3D_RD_PREVIEW_SYSTEMIC_RD;
	}
	return m_pRenderCoordinator
	        && m_pRenderCoordinator->requestMeshPreviewAsync( nAssetId, rSize, nCompletionOwnerId, nCompletionGeneration, pFailure );
}

Dictionary Terrain3D::forceRebakeVirtualTextures( void ) {
	if ( m_pRenderCoordinator ) { return m_pRenderCoordinator->forceRebakeVirtualTexturesCold(); }
	Dictionary result; result["state"] = "failed"; result["error"] = "Terrain3D RD coordinator is unavailable"; return result;
}
Dictionary Terrain3D::getForceRebakeVirtualTexturesStatus( void ) const {
	return m_pRenderCoordinator ? m_pRenderCoordinator->getForceRebakeVirtualTexturesStatusCold() : Dictionary();
}
