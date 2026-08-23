#include "rendering/rd/terrain_3d_render_coordinator.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "authoring/terrain_3d_editor.h"
#include "core/logger.h"
#include "core/terrain_3d_integration_helpers.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_lighting_policy.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_zone_allocator.h"
#include "rendering/terrain_3d_compute.h"
#include "simd/terrain_3d_parallel.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "streaming/terrain_3d_performance_profile.h"
#include "streaming/terrain_3d_streaming_policy.h"
#include "terrain_3d.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <limits>
#include <mutex>
#include <numeric>
#include <utility>

void Terrain3D::syncLightShaderParameters( void ) {
	DirectionalLight3D *const pLight =
			m_lightTarget.isValid() ? cast_to<DirectionalLight3D>( m_lightTarget.ptr() ) : nullptr;
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
	if ( m_oceanMaterial.is_valid() ) {
		ShaderMaterial *const pOceanShaderMaterial = Object::cast_to<ShaderMaterial>( m_oceanMaterial.ptr() );
		if ( pOceanShaderMaterial ) {
			if ( bColorChanged ) {
				pOceanShaderMaterial->set_shader_parameter( "_light_color", color );
			}
			if ( bDirectionChanged ) {
				pOceanShaderMaterial->set_shader_parameter( "_light_direction", direction );
			}
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
		m_pCollision = nullptr;
	}
}

void Terrain3D::setupTerrainMesher( void ) {
	if ( !m_pTerrainMesher ) {
		LOG( DEBUG, "Creating mesher" );
		m_pTerrainMesher = memnew( Terrain3DMesher );
	}
	const eastl::array<RID, 3> materials = {
		m_material->getMaterialRidForTier( SHADER_TIER_NEAR ),
		m_material->getMaterialRidForTier( SHADER_TIER_MIDDLE ),
		m_material->getMaterialRidForTier( SHADER_TIER_FAR ),
	};
	const eastl::array<RID, 3> shadow_materials = {
		m_material->getShadowMaterialRidForTier( SHADER_TIER_NEAR ),
		m_material->getShadowMaterialRidForTier( SHADER_TIER_MIDDLE ),
		m_material->getShadowMaterialRidForTier( SHADER_TIER_FAR ),
	};
	const bool bTieringActive = isRuntimeShaderTieringActive();
	const int clipmap_levels = CLAMP( m_nMeshLods + m_nTessellationLevel, 1, s_nMaxClipmapLevels );
	const int near_lods = bTieringActive ? m_nRuntimeNearShaderLods : clipmap_levels;
	const int middle_lods = bTieringActive ? m_nRuntimeMiddleShaderLods : 0;
	const int update_budget = bTieringActive ? m_nRuntimeClipmapUpdateBudget : s_nMaxClipmapLevels;
	const bool bStateMeshBatching = m_material.is_valid() && !m_material->isShaderOverrideEnabled();
	const eastl::array<bool, 3> tier_shadow_enabled = {
		m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled
	};
	const eastl::array<int, 3> tier_shadow_lod_counts = {
		m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount
	};
	m_pTerrainMesher->initialize( this,
			m_nMeshSize,
			m_nMeshLods,
			m_nTessellationLevel,
			m_fVertexSpacing,
			materials,
			shadow_materials,
			near_lods,
			middle_lods,
			update_budget,
			m_nRenderLayers,
			m_castShadows,
			m_giMode,
			m_nShadowLodCount,
			tier_shadow_enabled,
			tier_shadow_lod_counts,
			bStateMeshBatching,
			isGpuRegionCookedPagesEnabled() && !isEditorAuthoringModeInternal() );
	if ( m_pRenderCoordinator && m_bInitialized ) {
		m_pRenderCoordinator->initialize( this );
	}
}

bool Terrain3D::cookRuntimeClipmapGeometryPack( void ) const {
	return m_pTerrainMesher && m_pTerrainMesher->writeRuntimeGeometryPack( getDataDirectory() );
}

void Terrain3D::destroyTerrainMesher( const bool bFinal ) {
	LOG( INFO, "Destroying terrain mesher" );
	if ( !m_pTerrainMesher ) {
		return;
	}
	if ( bFinal ) {
		memdelete( m_pTerrainMesher );
		m_pTerrainMesher = nullptr;
	} else {
		m_pTerrainMesher->destroy();
	}
}

void Terrain3D::setupOceanMesher( void ) {
	if ( m_bOceanEnabled ) {
		m_bLightStateValid = false;
		// settings can be deserialized before the default material is loaded. Build
		// the requested compile-time variant before the mesher captures its RID.
		updateOceanShaderVariant();
		if ( !m_pOceanMesher ) {
			LOG( DEBUG, "Creating mesher" );
			m_pOceanMesher = memnew( Terrain3DMesher );
		}
		const RID ocean_rid = m_oceanMaterial.is_valid() ? m_oceanMaterial->get_rid() : RID();
		const eastl::array<RID, 3> ocean_materials = { ocean_rid, ocean_rid, ocean_rid };
		m_pOceanMesher->initialize( this,
				m_nOceanMeshSize,
				m_nOceanMeshLods,
				m_nOceanTessellationLevel,
				m_fOceanVertexSpacing,
				ocean_materials,
				ocean_materials,
				m_nOceanMeshLods + m_nOceanTessellationLevel,
				0,
				s_nMaxClipmapLevels,
				m_nOceanRenderLayers,
				m_oceanCastShadows,
				m_oceanGiMode,
				m_nOceanMeshLods + m_nOceanTessellationLevel,
				{ true, true, true },
				{ -1, -1, -1 },
				false );
		m_pOceanMesher->updateAabbs( m_fOceanCullMargin, s_v2Zero );
		if ( m_oceanMaterial.is_valid() ) {
			ShaderMaterial *const pOceanShaderMat = Object::cast_to<ShaderMaterial>( m_oceanMaterial.ptr() );
			if ( pOceanShaderMat ) {
				pOceanShaderMat->set_shader_parameter( "_mesh_size", m_nOceanMeshSize );
				pOceanShaderMat->set_shader_parameter( "_vertex_spacing", m_fOceanVertexSpacing );
				pOceanShaderMat->set_shader_parameter( "_vertex_density", 1.0f / m_fOceanVertexSpacing );
				pOceanShaderMat->set_shader_parameter( "_subdiv", pow( 2.f, ( real_t )( m_nOceanTessellationLevel ) ) );
			}
		}
	}
}

void Terrain3D::destroyOceanMesher( const bool bFinal ) {
	LOG( INFO, "Destroying ocean mesher" );
	if ( !m_pOceanMesher ) {
		return;
	}
	if ( bFinal ) {
		memdelete( m_pOceanMesher );
		m_pOceanMesher = nullptr;
	} else {
		m_pOceanMesher->destroy();
	}
}

void Terrain3D::setupDisplacementBuffer( void ) {
	if ( !is_inside_tree() ) {
		LOG( ERROR, "Not inside the tree, skipping displacement buffer setup" );
		return;
	}
	destroyDisplacementBuffer();
	LOG( INFO, "Setting up displacement buffer" );
	m_pDBufferVp = memnew( SubViewport );
	m_pDBufferVp->set_name( "DBufferViewport" );
	add_child( m_pDBufferVp, true );
	m_pDBufferVp->set_size( Vector2i( 2, 2 ) );
	m_pDBufferVp->set_disable_3d( true );
	m_pDBufferVp->set_use_hdr_2d( true );
	m_pDBufferVp->set_update_mode( SubViewport::UPDATE_ONCE );
	m_pDBufferVp->set_disable_input( true );
	m_pDBufferVp->set_default_canvas_item_texture_filter( Viewport::DEFAULT_CANVAS_ITEM_TEXTURE_FILTER_NEAREST );

	m_pDBufferRect = memnew( ColorRect );
	m_pDBufferRect->set_name( "DBufferRect" );
	m_pDBufferVp->add_child( m_pDBufferRect, true );
	m_pDBufferRect->set_anchors_preset( Control::PRESET_FULL_RECT );
}

void Terrain3D::updateDisplacementBuffer( void ) {
	if ( !m_pDBufferVp ) {
		return;
	}
	if ( m_nTessellationLevel == 0 ) {
		invalidateComputeDisplacementRequest();
		m_pDBufferVp->set_size( s_v2iZero );
		m_pDBufferRect->set_size( s_v2iZero );
	} else {
		m_pDBufferVp->set_size( Vector2i( m_nMeshSize * 4 * m_nTessellationLevel, m_nMeshSize * 4 ) );
		m_pDBufferRect->set_size( Vector2i( m_nMeshSize * 4 * m_nTessellationLevel, m_nMeshSize * 4 ) );
		LOG( INFO, "Updating displacement buffer to Size: ", m_pDBufferVp->get_size() );
		if ( m_material.is_valid() && m_material->getMaterialRid().is_valid() ) {
			// Keep a valid raster fallback bound until the asynchronous render-thread
			// compute dispatch publishes its renderer-owned storage texture.
			RS->canvas_item_set_material( m_pDBufferRect->get_canvas_item(), m_material->getBufferMaterialRid() );
			RS->material_set_param(
					m_material->getMaterialRid(), "_displacement_buffer", m_pDBufferVp->get_texture()->get_rid() );
			tryUpdateComputeDisplacement( getClipmapTargetPosition() );
		}
	}
}

bool Terrain3D::tryUpdateComputeDisplacement( const Vector3 &rTargetPosition ) {
	m_bComputeDisplacementActive.store( false, std::memory_order_release );
	if ( !m_bComputeDisplacementEnabled.load( std::memory_order_acquire ) ||
			m_bComputeDisplacementFailed.load( std::memory_order_acquire ) ||
			m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) || !m_pData || m_material.is_null() ||
			!m_material->getMaterialRid().is_valid() ) {
		return false;
	}
	Terrain3DCompute *const pCompute = m_pData->getComputeBackend();
	t3dDisplacementRequest_t request;
	if ( !pCompute || !pCompute->prepareDisplacementRequest( this, rTargetPosition, request ) ) {
		invalidateComputeDisplacementRequest();
		return false;
	}
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		m_computeDisplacementRequest = std::move( request );
		++m_nComputeDisplacementRequestVersion;
	}
	queueComputeDisplacementRender();
	return true;
}

void Terrain3D::invalidateComputeDisplacementRequest( void ) {
	m_bComputeDisplacementActive.store( false, std::memory_order_release );
	std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
	m_computeDisplacementRequest = t3dDisplacementRequest_t{};
	m_hComputeDisplacementTextureRd = RID();
	++m_nComputeDisplacementRequestVersion;
}

RID Terrain3D::getComputeDisplacementTextureRid( void ) const {
	std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
	return m_hComputeDisplacementTextureRd;
}

void Terrain3D::queueComputeDisplacementRender( void ) {
	if ( m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		return;
	}
	bool bExpected = false;
	if ( !m_bComputeDisplacementDispatchPending.compare_exchange_strong(
				 bExpected, true, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
		return;
	}
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) {
		m_bComputeDisplacementDispatchPending.store( false, std::memory_order_release );
		m_bComputeDisplacementFailed.store( true, std::memory_order_release );
		m_bComputeDisplacementFallbackRequested.store( true, std::memory_order_release );
		return;
	}
	if ( pServer->is_on_render_thread() ) {
		renderUpdateComputeDisplacement();
	} else {
		pServer->call_on_render_thread( callable_mp( this, &Terrain3D::renderUpdateComputeDisplacement ) );
	}
}

void Terrain3D::renderUpdateComputeDisplacement( void ) {
	t3dDisplacementRequest_t request;
	uint64_t request_version = 0u;
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		request = m_computeDisplacementRequest;
		request_version = m_nComputeDisplacementRequestVersion;
	}

	bool bSuccess = false;
	RID texture;
	Terrain3DCompute *const pCompute = m_pData ? m_pData->getComputeBackendIfCreated() : nullptr;
	if ( !m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) && pCompute ) {
		bSuccess = pCompute->dispatchDisplacementRequest( request, texture ) && texture.is_valid();
	}
	bool bRequestCurrent = false;
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		bRequestCurrent = m_nComputeDisplacementRequestVersion == request_version;
		if ( bSuccess && bRequestCurrent && m_bComputeDisplacementEnabled.load( std::memory_order_acquire ) &&
				!m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
			m_hComputeDisplacementTextureRd = texture;
		} else if ( bRequestCurrent ) {
			m_hComputeDisplacementTextureRd = RID();
		}
	}
	if ( bSuccess && bRequestCurrent && m_bComputeDisplacementEnabled.load( std::memory_order_acquire ) &&
			!m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		m_bComputeDisplacementActive.store( true, std::memory_order_release );
	} else if ( !bSuccess && !m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		m_bComputeDisplacementActive.store( false, std::memory_order_release );
		m_bComputeDisplacementFailed.store( true, std::memory_order_release );
		m_bComputeDisplacementFallbackRequested.store( true, std::memory_order_release );
	}

	m_bComputeDisplacementDispatchPending.store( false, std::memory_order_release );
	bool bNewerRequest = false;
	bool bNewerRequestValid = false;
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		bNewerRequest = m_nComputeDisplacementRequestVersion != request_version;
		bNewerRequestValid = m_computeDisplacementRequest.valid();
	}
	if ( bNewerRequest && bNewerRequestValid && m_bComputeDisplacementEnabled.load( std::memory_order_acquire ) &&
			!m_bComputeDisplacementFailed.load( std::memory_order_acquire ) &&
			!m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		queueComputeDisplacementRender();
	}
}

void Terrain3D::renderShutdownComputeDisplacement( void ) {
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		m_hComputeDisplacementTextureRd = RID();
	}
	Terrain3DCompute *const pCompute = m_pData ? m_pData->getComputeBackendIfCreated() : nullptr;
	if ( pCompute ) {
		pCompute->shutdownDisplacementResources();
	}
}

void Terrain3D::setComputeDisplacementEnabled( const bool bEnabled ) {
	const bool bOldEnabled = m_bComputeDisplacementEnabled.exchange( bEnabled, std::memory_order_acq_rel );
	if ( bOldEnabled == bEnabled ) {
		return;
	}
	invalidateComputeDisplacementRequest();
	m_bComputeDisplacementFailed.store( false, std::memory_order_release );
	m_bComputeDisplacementFallbackRequested.store( false, std::memory_order_release );
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
	for ( const Variant &var : labels ) {
		Node *const pLabel = cast_to<Node>( var );
		if ( pLabel ) {
			memdelete( pLabel );
		}
	}
}

void Terrain3D::destroyDisplacementBuffer( void ) {
	LOG( INFO, "[compute] Draining displacement work and releasing buffer resources" );
	m_bComputeDisplacementTearingDown.store( true, std::memory_order_release );
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && m_pData && m_pData->getComputeBackendIfCreated() ) {
		if ( pServer->is_on_render_thread() ) {
			renderShutdownComputeDisplacement();
		} else {
			pServer->call_on_render_thread( callable_mp( this, &Terrain3D::renderShutdownComputeDisplacement ) );
			// Lifecycle-only synchronization: all queued displacement work and its
			// renderer-owned resources must be gone before the compute backend or node dies.
			pServer->force_sync();
		}
	}
	m_bComputeDisplacementDispatchPending.store( false, std::memory_order_release );
	m_bComputeDisplacementActive.store( false, std::memory_order_release );
	m_bComputeDisplacementFailed.store( false, std::memory_order_release );
	m_bComputeDisplacementFallbackRequested.store( false, std::memory_order_release );
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		m_computeDisplacementRequest = t3dDisplacementRequest_t{};
		m_hComputeDisplacementTextureRd = RID();
		++m_nComputeDisplacementRequestVersion;
	}
	m_bComputeDisplacementTearingDown.store( false, std::memory_order_release );
	LOG( DEBUG, "Freeing d_buffer_rect" );
	if ( m_pDBufferRect ) {
		memdelete( m_pDBufferRect );
		m_pDBufferRect = nullptr;
	}
	LOG( DEBUG, "Freeing d_buffer_vp" );
	if ( m_pDBufferVp ) {
		memdelete( m_pDBufferVp );
		m_pDBufferVp = nullptr;
	}
}

void Terrain3D::setupMousePicking( void ) {
	if ( !is_inside_tree() ) {
		LOG( ERROR, "Not inside the tree, skipping mouse setup" );
		return;
	}
	LOG( INFO, "Setting up mouse picker and get_intersection viewport, camera & screen quad" );
	m_pMouseVp = memnew( SubViewport );
	m_pMouseVp->set_name( "MouseViewport" );
	add_child( m_pMouseVp, true );
	m_pMouseVp->set_size( V2I( 2 ) );
	m_pMouseVp->set_scaling_3d_mode( Viewport::SCALING_3D_MODE_BILINEAR );
	m_pMouseVp->set_update_mode( SubViewport::UPDATE_ONCE );
	m_pMouseVp->set_disable_input( true );
	m_pMouseVp->set_canvas_cull_mask( 0 );
	m_pMouseVp->set_use_hdr_2d( true );
	m_pMouseVp->set_anisotropic_filtering_level( Viewport::ANISOTROPY_DISABLED );
	m_pMouseVp->set_default_canvas_item_texture_filter( Viewport::DEFAULT_CANVAS_ITEM_TEXTURE_FILTER_NEAREST );
	m_pMouseVp->set_positional_shadow_atlas_size( 0 );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 0, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 1, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 2, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 3, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );

	m_pMouseCam = memnew( Camera3D );
	m_pMouseCam->set_name( "MouseCamera" );
	m_pMouseVp->add_child( m_pMouseCam, true );
	Ref<Environment> pEnv;
	pEnv.instantiate();
	pEnv->set_tonemapper( Environment::TONE_MAPPER_LINEAR );
	m_pMouseCam->set_environment( pEnv );
	Ref<Compositor> pComp;
	pComp.instantiate();
	m_pMouseCam->set_compositor( pComp );
	m_pMouseCam->set_projection( Camera3D::PROJECTION_ORTHOGONAL );
	m_pMouseCam->set_size( 0.1f );
	m_pMouseCam->set_far( 100000.f );

	m_pMouseQuad = memnew( MeshInstance3D );
	m_pMouseQuad->set_name( "MouseQuad" );
	m_pMouseCam->add_child( m_pMouseQuad, true );
	Ref<QuadMesh> pQuad;
	pQuad.instantiate();
	pQuad->set_size( V2( 0.1f ) );
	m_pMouseQuad->set_mesh( pQuad );
	const String shader_code = String(
#include "shaders/gpu_depth.glsl"
	);
	Ref<Shader> pShader;
	pShader.instantiate();
	pShader->set_code( shader_code );
	Ref<ShaderMaterial> pShaderMaterial;
	pShaderMaterial.instantiate();
	pShaderMaterial->set_shader( pShader );
	m_pMouseQuad->set_surface_override_material( 0, pShaderMaterial );
	m_pMouseQuad->set_position( Vector3( 0.f, 0.f, -0.5f ) );

	// Set terrain, terrain shader, mouse camera, and screen quad to mouse layer
	const uint32_t force_update_layer = m_nMouseLayer;
	m_nMouseLayer = 0u;
	setMouseLayer( force_update_layer );
}

void Terrain3D::destroyMousePicking( void ) {
	LOG( DEBUG, "Freeing mouse_quad" );
	if ( m_pMouseQuad ) {
		memdelete( m_pMouseQuad );
		m_pMouseQuad = nullptr;
	}
	LOG( DEBUG, "Freeing mouse_cam" );
	if ( m_pMouseCam ) {
		memdelete( m_pMouseCam );
		m_pMouseCam = nullptr;
	}
	LOG( DEBUG, "Freeing mouse_vp" );
	if ( m_pMouseVp ) {
		memdelete( m_pMouseVp );
		m_pMouseVp = nullptr;
	}
}

void Terrain3D::destroyInstancer( void ) {
	LOG( INFO, "Destroying Instancer" );
	if ( m_pInstancer ) {
		memdelete( m_pInstancer );
		m_pInstancer = nullptr;
	}
}

void Terrain3D::destroyNodeStreamer( const bool bCaptureState ) {
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->destroy( bCaptureState );
		memdelete( m_pNodeStreamer );
		m_pNodeStreamer = nullptr;
	}
}

void Terrain3D::destroyLightmapStreamer( void ) {
	if ( m_pLightmapStreamer ) {
		m_pLightmapStreamer->destroy();
		memdelete( m_pLightmapStreamer );
		m_pLightmapStreamer = nullptr;
	}
}

void Terrain3D::destroyHlodStreamer( void ) {
	if ( m_pHlodStreamer ) {
		m_pHlodStreamer->destroy();
		memdelete( m_pHlodStreamer );
		m_pHlodStreamer = nullptr;
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
		for ( const Vector2i region_loc : region_locations ) {
			Label3D *const pLabel = memnew( Label3D );
			const String text = region_loc;
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
			Vector3 pos = Vector3( ( real_t )( region_loc.x ) + .5f, 0.f, ( real_t )( region_loc.y ) + .5f ) *
					m_nRegionSize * m_fVertexSpacing;
			const real_t fHeight = m_pData->getHeight( pos );
			pos.y = ( std::isnan( fHeight ) ) ? 0.f : fHeight;
			pLabel->set_position( pos );
		}
	}
}

void Terrain3D::setCamera( Camera3D *const pCamera ) {
	if ( m_hCamera.ptr() != pCamera ) {
		LOG( EXTREME, "Setting camera: ", pCamera );
		m_hCamera.setTarget( pCamera );
		if ( m_clipmapTarget.isValid() ) {
			set_physics_process( true );
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

	uint32_t mask = m_nEnvironmentShadowExternalMask | s_nLightingLayerRegional;
	if ( m_environmentShadowQuality >= ENVIRONMENT_SHADOW_NORMAL ) {
		mask |= s_nLightingLayerLarge;
	}
	if ( m_environmentShadowQuality >= ENVIRONMENT_SHADOW_ULTRA ) {
		mask |= s_nLightingLayerFoliage;
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
	const uint32_t mask = m_nEnvironmentDirectionalExternalMask | s_nDirectionalReceiverLayer;
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
		m_material->setEffectiveShaderVariantSourceBudgetMb( m_nEffectiveShaderCacheBudgetMb );
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
	const int count = CLAMP( nCount, 1, s_nMaxClipmapLevels );
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
		m_pData->configureGpuRegionCache( true,
				m_nEffectiveGpuRegionCacheBudgetMb,
				m_nGpuRegionUploadBudgetLayers,
				m_nGpuRegionUploadBudgetMb,
				m_nGpuRegionSparePages,
				m_nGpuRegionRetireFrames );
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
	if ( ( old_level == 0 ) != ( m_nTessellationLevel == 0 ) && m_assets.is_valid() && m_assets->isInitialized() &&
			m_assets->usesPackedPbr() ) {
		m_assets->updateTextureList();
	}
	if ( m_pTerrainMesher && m_material.is_valid() ) {
		m_material->update( FULL_REBUILD );
		setupTerrainMesher();
		updateDisplacementBuffer();
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		m_pData->configureGpuRegionCache( true,
				m_nEffectiveGpuRegionCacheBudgetMb,
				m_nGpuRegionUploadBudgetLayers,
				m_nGpuRegionUploadBudgetMb,
				m_nGpuRegionSparePages,
				m_nGpuRegionRetireFrames );
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
		m_pData->configureGpuRegionCache( true,
				m_nEffectiveGpuRegionCacheBudgetMb,
				m_nGpuRegionUploadBudgetLayers,
				m_nGpuRegionUploadBudgetMb,
				m_nGpuRegionSparePages,
				m_nGpuRegionRetireFrames );
	}
}

void Terrain3D::setVertexSpacing( const real_t fSpacing ) {
	if ( !Math::is_equal_approx( fSpacing, ( real_t )( 1.0f ) ) ) {
		LOG( ERROR,
				"This Terrain3D fork requires 1 metre vertex spacing so every 128-sample region is exactly 128 metres "
				"wide" );
		return;
	}
	if ( Math::is_equal_approx( m_fVertexSpacing, ( real_t )( 1.0f ) ) ) {
		return;
	}
	m_fVertexSpacing = 1.0f;
	LOG( INFO, "Setting fixed vertex spacing: 1 metre" );
	if ( m_pCollision && m_pData && m_pInstancer && m_material.is_valid() ) {
		m_pInstancer->updateVertexSpacing( m_fVertexSpacing );
		m_pData->setVertexSpacingInternal( m_fVertexSpacing );
		updateRegionLabels();
		m_material->update();
		setupTerrainMesher();
		m_pCollision->destroy();
		m_pCollision->build();
		updateDisplacementBuffer();
		if ( effectiveGpuRegionCacheEnabled() ) {
			m_pData->configureGpuRegionCache( true,
					m_nEffectiveGpuRegionCacheBudgetMb,
					m_nGpuRegionUploadBudgetLayers,
					m_nGpuRegionUploadBudgetMb,
					m_nGpuRegionSparePages,
					m_nGpuRegionRetireFrames );
		}
	}
}

void Terrain3D::setCullMargin( const real_t fMargin ) {
	SET_IF_DIFF( m_fCullMargin, CLAMP( fMargin, 0.f, 100000.f ) );
	LOG( INFO, "Setting extra cull margin: ", m_fCullMargin );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->updateAabbs();
	}
}

void Terrain3D::setCastShadows( const RenderingServer::ShadowCastingSetting castShadows ) {
	SET_IF_DIFF( m_castShadows, castShadows );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setCastShadows( m_castShadows );
	}
}

void Terrain3D::setShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nShadowLodCount, CLAMP( nCount, 0, s_nMaxClipmapLevels ) );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setShadowLodCount( m_nShadowLodCount );
	}
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
	SET_IF_DIFF( m_nNearShadowLodCount, CLAMP( nCount, -1, s_nMaxClipmapLevels ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts(
				{ m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void Terrain3D::setMiddleShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nMiddleShadowLodCount, CLAMP( nCount, -1, s_nMaxClipmapLevels ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts(
				{ m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void Terrain3D::setFarShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nFarShadowLodCount, CLAMP( nCount, -1, s_nMaxClipmapLevels ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts(
				{ m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void Terrain3D::setGiMode( const GeometryInstance3D::GIMode giMode ) {
	SET_IF_DIFF( m_giMode, giMode );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setGiMode( m_giMode );
	}
}

void Terrain3D::setRenderLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_nRenderLayers, nLayers );
	LOG( INFO, "Setting terrain render layers to: ", nLayers );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setRenderLayers( m_nRenderLayers );
	}
}

void Terrain3D::updateOceanShaderVariant( void ) {
	ShaderMaterial *const pMaterial = Object::cast_to<ShaderMaterial>( m_oceanMaterial.ptr() );
	if ( !pMaterial || pMaterial->get_shader().is_null() ) {
		return;
	}
	const String current = pMaterial->get_shader()->get_code();
	if ( m_oceanVariantSource.is_empty() && current.contains( "T3D_OCEAN_VARIANT_SOURCE" ) ) {
		m_oceanVariantSource = current;
	}
	if ( m_oceanVariantSource.is_empty() ) {
		return; // Custom ocean material: do not rewrite user shader code.
	}
	String code = m_oceanVariantSource;
	code = code.replace( "#define T3D_OCEAN_VERTEX_ITERATIONS 1",
			String( "#define T3D_OCEAN_VERTEX_ITERATIONS " ) + String::num_int64( m_nOceanVertexWaveIterations ) );
	code = code.replace( "#define T3D_OCEAN_FRAGMENT_ITERATIONS 3",
			String( "#define T3D_OCEAN_FRAGMENT_ITERATIONS " ) + String::num_int64( m_nOceanFragmentWaveIterations ) );
	code = code.replace( "#define T3D_OCEAN_REFRACTION_MODE 2",
			String( "#define T3D_OCEAN_REFRACTION_MODE " ) + String::num_int64( m_oceanRefractionMode ) );
	code = code.replace(
			"#define T3D_OCEAN_FOAM_MODE 2", String( "#define T3D_OCEAN_FOAM_MODE " ) + String::num_int64( m_oceanFoamMode ) );
	code = code.replace( "#define T3D_OCEAN_SCATTERING_MODE 2",
			String( "#define T3D_OCEAN_SCATTERING_MODE " ) + String::num_int64( m_oceanScatteringMode ) );
	code = code.replace( "#define T3D_OCEAN_SHADING_MODE 2",
			String( "#define T3D_OCEAN_SHADING_MODE " ) + String::num_int64( m_oceanShadingMode ) );
	String render_mode = "render_mode cull_back, depth_draw_always, diffuse_burley, specular_schlick_ggx, "
						 "shadows_disabled, skip_vertex_transform;";
	switch ( m_oceanShadingMode ) {
		case OCEAN_SHADING_LAMBERT_DIFFUSE:
			render_mode = "render_mode cull_back, depth_draw_always, diffuse_lambert, specular_disabled, "
						  "shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_LAMBERT_GGX:
			render_mode = "render_mode cull_back, depth_draw_always, diffuse_lambert, specular_schlick_ggx, "
						  "shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_UNSHADED:
			render_mode =
					"render_mode cull_back, depth_draw_always, unshaded, shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_LAMBERT_VERTEX:
			render_mode = "render_mode cull_back, depth_draw_always, diffuse_lambert, specular_disabled, "
						  "vertex_lighting, shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_BURLEY_GGX:
		default:
			break;
	}
	code = code.replace( "render_mode cull_back, depth_draw_always, diffuse_burley, specular_schlick_ggx, "
						 "shadows_disabled, skip_vertex_transform;",
			render_mode );
	if ( m_oceanVariantShader.is_null() ) {
		m_oceanVariantShader.instantiate();
	}
	m_oceanVariantShader->set_code( code );
	pMaterial->set_shader( m_oceanVariantShader );
}

void Terrain3D::setOceanPerformancePreset( const t3dOceanPerformancePreset_t preset ) {
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
	updateOceanShaderVariant();
}

void Terrain3D::setOceanVertexWaveIterations( const int nIterations ) {
	const int value = CLAMP( nIterations, 0, 3 );
	if ( m_nOceanVertexWaveIterations == value ) {
		return;
	}
	m_nOceanVertexWaveIterations = value;
	m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;
	updateOceanShaderVariant();
}
void Terrain3D::setOceanFragmentWaveIterations( const int nIterations ) {
	const int value = CLAMP( nIterations, 0, 4 );
	if ( m_nOceanFragmentWaveIterations == value ) {
		return;
	}
	m_nOceanFragmentWaveIterations = value;
	m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;
	updateOceanShaderVariant();
}
void Terrain3D::setOceanEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bOceanEnabled, bEnabled );
	LOG( INFO, "Setting ocean enabled: ", m_bOceanEnabled );
	if ( m_bOceanEnabled ) {
		if ( m_oceanMaterial.is_null() ) {
			const String ocean_mat_path = ProjectSettings::get_singleton()->globalize_path( OCEAN_MATERIAL_PATH );
			ResourceLoader *const pRl = ResourceLoader::get_singleton();
			if ( pRl->exists( ocean_mat_path ) ) {
				const Ref<ShaderMaterial> pOceanMat = pRl->load( ocean_mat_path );
				if ( pOceanMat.is_valid() ) {
					const Ref<ShaderMaterial> pLocalOcean = pOceanMat->duplicate( true );
					m_oceanMaterial = pLocalOcean.is_valid() ? pLocalOcean : pOceanMat;
				}
			}
		}
		setupOceanMesher();
	} else {
		destroyOceanMesher( false );
	}
	notify_property_list_changed();
}

void Terrain3D::setOceanMeshLods( const int nCount ) {
	SET_IF_DIFF( m_nOceanMeshLods, CLAMP( nCount, 1, 10 ) );
	LOG( INFO, "Setting ocean mesh levels: ", m_nOceanMeshLods );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanTessellationLevel( const int nLevel ) {
	SET_IF_DIFF( m_nOceanTessellationLevel, CLAMP( nLevel, 0, 6 ) );
	LOG( INFO, "Setting ocean tessellation level: ", nLevel );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanMeshSize( const int nSize ) {
	SET_IF_DIFF( m_nOceanMeshSize, CLAMP( nSize & ~1, 8, 256 ) ); // Ensure even
	LOG( INFO, "Setting ocean mesh size: ", m_nOceanMeshSize );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanVertexSpacing( const real_t fSpacing ) {
	SET_IF_DIFF( m_fOceanVertexSpacing, CLAMP( fSpacing, 0.25f, 100.0f ) );
	LOG( INFO, "Setting ocean vertex spacing: ", m_fOceanVertexSpacing );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setOceanCullMargin( const real_t fMargin ) {
	SET_IF_DIFF( m_fOceanCullMargin, CLAMP( fMargin, 0.f, 100000.f ) );
	LOG( INFO, "Setting extra cull margin: ", m_fOceanCullMargin );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->updateAabbs( m_fOceanCullMargin, s_v2Zero );
	}
}

void Terrain3D::setOceanCastShadows( const RenderingServer::ShadowCastingSetting castShadows ) {
	SET_IF_DIFF( m_oceanCastShadows, castShadows );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->setCastShadows( m_oceanCastShadows );
	}
}

void Terrain3D::setOceanGiMode( const GeometryInstance3D::GIMode giMode ) {
	SET_IF_DIFF( m_oceanGiMode, giMode );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->setGiMode( m_oceanGiMode );
	}
}

void Terrain3D::setOceanRenderLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_nOceanRenderLayers, nLayers );
	LOG( INFO, "Setting ocean render layers to: ", nLayers );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->setRenderLayers( m_nOceanRenderLayers );
	}
}

void Terrain3D::setOceanMaterial( const Ref<Material> &rMaterial ) {
	SET_IF_DIFF( m_oceanMaterial, rMaterial );
	m_oceanVariantSource = String();
	m_oceanVariantShader.unref();
	m_bLightStateValid = false;
	LOG( INFO, "Setting ocean material" );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void Terrain3D::setMouseLayer( const uint32_t nLayer ) {
	SET_IF_DIFF( m_nMouseLayer, CLAMP( nLayer, 21u, 32u ) );
	const uint32_t mouse_mask = 1 << ( m_nMouseLayer - 1 );
	LOG( INFO,
			"Setting mouse layer: ",
			m_nMouseLayer,
			" (",
			mouse_mask,
			") on terrain mesh, material, mouse camera, mouse quad" );

	// Set terrain meshes to mouse layer
	// Mask off editor render layers by ORing user layers 1-20 and current mouse layer
	setRenderLayers( ( m_nRenderLayers & 0xFFFFF ) | mouse_mask );
	// Set terrain shader to exclude mouse camera from showing holes
	if ( m_material.is_valid() ) {
		m_material->setShaderParam( "_mouse_layer", mouse_mask );
	}
	// Set mouse camera to see only mouse layer
	if ( m_pMouseCam ) {
		m_pMouseCam->set_cull_mask( mouse_mask );
	}
	// Set screenquad to mouse layer
	if ( m_pMouseQuad ) {
		m_pMouseQuad->set_layer_mask( mouse_mask );
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
		// Get depth from perspective camera snapshot
		if ( !m_pMouseCam ) {
			LOG( ERROR, "Invalid mouse camera" );
			return s_v3Nan;
		}
		// Position mouse cam one unit behind the requested position
		m_pMouseCam->set_global_position( rSrcPos - direction );

		// If looking straight down, then we're not in a region, set rotation directly as look_at() doesn't work
		if ( direction.y < -.99999f ) {
			m_pMouseCam->set_rotation_degrees( Vector3( -90.f, 0.f, 0.f ) );
		} else {
			m_pMouseCam->look_at( m_pMouseCam->get_global_position() + direction, s_v3Up );
		}

		m_pMouseVp->set_update_mode( SubViewport::UPDATE_ONCE );
		const Ref<ViewportTexture> pVpTex = m_pMouseVp->get_texture();
		const Ref<Image> pVpImg = pVpTex->get_image();

		// Read the depth pixel from the camera viewport
		const Color screen_depth = pVpImg->get_pixel( 0, 0 );

		// Get position from depth packed in RGB - unpack back to float.
		// Forward+ is 16bit, mobile and compatibility is 10bit.
		// Compatibility also has precision loss for values below 0.5, so
		// we use only the top half of the range, for 21bit depth encoded.
		const real_t fR = floor( ( screen_depth.r * 256.f ) - 128.f );
		const real_t fG = floor( ( screen_depth.g * 256.f ) - 128.f );
		const real_t fB = floor( ( screen_depth.b * 256.f ) - 128.f );

		// Decode the full depth value
		real_t fDecodedDepth = ( fR + fG / 127.f + fB / ( 127.f * 127.f ) ) / 127.f;

		// Near-plane noise filter, or no hit (sky, underside, far clip)
		if ( fDecodedDepth < 0.00001f || fDecodedDepth > 1.f ) {
			// Catch editor ortho camera with src_pos.y at some random value around 500k
			if ( direction.y < -.99999f && rSrcPos.y >= 100000.f ) {
				return Vector3( rSrcPos.x, 0.f, rSrcPos.z );
			}
			return s_v3Max;
		}

		// Necessary for a near-far precision on hits
		if ( fDecodedDepth > 0.99999f ) {
			fDecodedDepth = 1.f;
		}

		// Denormalize distance to get real depth and terrain position.
		fDecodedDepth *= m_pMouseCam->get_far();

		// Project the camera position by the depth value to get the intersection point.
		return m_pMouseCam->get_global_position() + direction * fDecodedDepth;
	}
}

void Terrain3D::setupRenderCoordinator( void ) {
	if ( !m_pRenderCoordinator ) {
		m_pRenderCoordinator = memnew( Terrain3DRenderCoordinator );
	}
	m_pRenderCoordinator->initialize( this );
}

void Terrain3D::destroyRenderCoordinator( const bool bFinal ) {
	if ( !m_pRenderCoordinator ) {
		return;
	}
	m_pRenderCoordinator->shutdown();
	if ( bFinal ) {
		memdelete( m_pRenderCoordinator );
		m_pRenderCoordinator = nullptr;
	}
}

void Terrain3D::setRenderBackend( const t3dRenderBackend_t backend ) {
	const t3dRenderBackend_t clamped = ( t3dRenderBackend_t )( CLAMP(
			( int )( backend ), ( int )( T3D_RENDER_BACKEND_RENDERING_SERVER ),
			( int )( T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID ) ) );
	if ( m_renderBackend == clamped ) {
		return;
	}
	m_renderBackend = clamped;
	if ( m_pRenderCoordinator ) {
		m_pRenderCoordinator->setBackend( m_renderBackend );
	}
	notify_property_list_changed();
}

void Terrain3D::setRdNativeProxyMode( const t3dRdNativeProxyMode_t mode ) {
	const t3dRdNativeProxyMode_t clamped = ( t3dRdNativeProxyMode_t )( CLAMP(
			( int )( mode ), ( int )( T3D_RD_NATIVE_PROXY_DISABLED ), ( int )( T3D_RD_NATIVE_PROXY_FULL ) ) );
	if ( m_rdNativeProxyMode == clamped ) {
		return;
	}
	m_rdNativeProxyMode = clamped;
	if ( m_pRenderCoordinator ) {
		m_pRenderCoordinator->setProxyMode( m_rdNativeProxyMode );
	}
}

Dictionary Terrain3D::getRdRendererDiagnostics( void ) const {
	return m_pRenderCoordinator ? m_pRenderCoordinator->getDiagnostics() : Dictionary();
}
