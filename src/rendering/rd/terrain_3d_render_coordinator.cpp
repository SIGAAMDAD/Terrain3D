// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_render_coordinator.h"

#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <cstring>

Terrain3DRenderCoordinator::~Terrain3DRenderCoordinator( void ) { shutdown(); }

uint64_t Terrain3DRenderCoordinator::hashBytes( const void *const pData, const size_t nBytes ) const {
	uint64_t hash = 1469598103934665603ULL;
	const uint8_t *const bytes = ( const uint8_t * )( pData );
	for ( size_t i = 0u; i < nBytes; ++i ) {
		hash ^= ( uint64_t )( bytes[i] );
		hash *= 1099511628211ULL;
	}
	return hash;
}

void Terrain3DRenderCoordinator::buildMaterialState( t3dRdFrameSnapshot_t &rSnapshot ) const {
	const Ref<Terrain3DAssets> assets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	if ( assets.is_null() ) {
		rSnapshot.m_nMaterialLayerCount = 1u;
		rSnapshot.m_nMaterialGeneration = hashBytes( rSnapshot.m_szMaterials, sizeof( rSnapshot.m_szMaterials ) );
		return;
	}
	const PackedColorArray colors = assets->getTextureColors();
	const PackedFloat32Array normal_depths = assets->getTextureNormalDepths();
	const PackedFloat32Array ao_strengths = assets->getTextureAoStrengths();
	const PackedFloat32Array ao_affects = assets->getTextureAoLightAffects();
	const PackedFloat32Array roughness = assets->getTextureRoughnessMods();
	const PackedFloat32Array metallic = assets->getTextureMetallics();
	const PackedFloat32Array uv_scales = assets->getTextureUvScales();
	const PackedVector2Array detiles = assets->getTextureDetiles();
	const PackedVector2Array displacements = assets->getTextureDisplacements();
	const PackedInt32Array layer_map = assets->getTextureLayerMap();
	const uint32_t count = ( uint32_t )( CLAMP( assets->getTextureCount(), 1, ( int )( s_nRdMaterialLayerCount ) ) );
	rSnapshot.m_nMaterialLayerCount = count;
	for ( uint32_t i = 0u; i < s_nRdMaterialLayerCount; ++i ) {
		t3dRdMaterialLayer_t &layer = rSnapshot.m_szMaterials[i];
		const Color color = i < ( uint32_t )( colors.size() ) ? colors[( int64_t )( i )] : Color( 1.0, 1.0, 1.0, 1.0 );
		layer.m_fColorR = ( float )( color.r );
		layer.m_fColorG = ( float )( color.g );
		layer.m_fColorB = ( float )( color.b );
		layer.m_fColorA = ( float )( color.a );
		layer.m_fUvScale = i < ( uint32_t )( uv_scales.size() ) ? uv_scales[( int64_t )( i )] : 0.1f;
		layer.m_fNormalDepth = i < ( uint32_t )( normal_depths.size() ) ? normal_depths[( int64_t )( i )] : 1.0f;
		layer.m_fRoughness = i < ( uint32_t )( roughness.size() ) ? roughness[( int64_t )( i )] : 0.0f;
		layer.m_fMetallic = i < ( uint32_t )( metallic.size() ) ? metallic[( int64_t )( i )] : 0.0f;
		layer.m_fAoStrength = i < ( uint32_t )( ao_strengths.size() ) ? ao_strengths[( int64_t )( i )] : 1.0f;
		layer.m_fAoLightAffect = i < ( uint32_t )( ao_affects.size() ) ? ao_affects[( int64_t )( i )] : 0.0f;
		const Vector2 detile = i < ( uint32_t )( detiles.size() ) ? detiles[( int64_t )( i )] : Vector2();
		layer.m_fDetileRotation = ( float )( detile.x );
		layer.m_fDetileShift = ( float )( detile.y );
		const Vector2 displacement = i < ( uint32_t )( displacements.size() ) ? displacements[( int64_t )( i )] : Vector2();
		layer.m_fDisplacementOffset = ( float )( displacement.x );
		layer.m_fDisplacementScale = ( float )( displacement.y );
		const int physical = i < ( uint32_t )( layer_map.size() ) ? layer_map[( int64_t )( i )] : ( int )( i );
		layer.m_fPhysicalLayer = ( float )( MAX( physical, 0 ) );
		layer.m_fFlags = ( assets->usesPackedPbr() ? 1.0f : 0.0f ) + ( physical >= 0 ? 2.0f : 0.0f );
	}
	rSnapshot.m_nMaterialGeneration = hashBytes( rSnapshot.m_szMaterials, sizeof( rSnapshot.m_szMaterials ) );
}

void Terrain3DRenderCoordinator::publishSnapshot( void ) {
	if ( !m_pTerrain || !m_pTerrain->getMesher() || !m_pTerrain->getData() ) return;
	t3dRdFrameSnapshot_t snapshot{};
	snapshot.m_nRegionSize = ( uint32_t )( m_pTerrain->getRegionSize() );
	snapshot.m_nRegionMapSize = ( uint32_t )( m_pTerrain->getData()->getRegionMapSizeInternal() );
	snapshot.m_nMeshSize = ( uint32_t )( m_pTerrain->getMeshSize() );
	snapshot.m_fVertexSpacing = ( float )( m_pTerrain->getVertexSpacing() );
	snapshot.m_targetPosition = m_pTerrain->getClipmapTargetPosition();
	snapshot.m_fSubdiv = ( float )( 1 << m_pTerrain->getTessellationLevel() );
	snapshot.m_fHeightMin = s_fRegionHeightMin;
	snapshot.m_fHeightRange = s_fRegionHeightRange;
	snapshot.m_fDisplacementScale = ( float )( m_pTerrain->getDisplacementScale() );
	snapshot.m_bDisplacementEnabled = m_pTerrain->getTessellationLevel() > 0;
	snapshot.m_nPatchCount = ( uint32_t )( m_pTerrain->getMesher()->fillRdPatches( snapshot.m_szPatches, s_nMaxClipmapLevels ) );
	snapshot.m_nGeneration = hashBytes( snapshot.m_szPatches, sizeof( snapshot.m_szPatches ) );
	buildMaterialState( snapshot );
	DirectionalLight3D *const light = Object::cast_to<DirectionalLight3D>( m_pTerrain->getLightTarget() );
	if ( light ) {
		snapshot.m_lightColor = light->get_color() * light->get_param( DirectionalLight3D::PARAM_ENERGY );
		snapshot.m_lightDirection = light->get_global_basis().get_column( 2 ).normalized();
	}
	const Ref<World3D> world = m_pTerrain->get_world_3d();
	const Ref<Environment> environment = world.is_valid() ? world->get_environment() : Ref<Environment>();
	if ( environment.is_valid() ) {
		const Color ambient = environment->get_ambient_light_color();
		snapshot.m_fAmbient = ( float )( environment->get_ambient_light_energy() * MAX( ambient.get_luminance(), 0.0f ) );
		if ( environment->is_fog_enabled() ) {
			snapshot.m_fogColor = environment->get_fog_light_color() * environment->get_fog_light_energy();
			snapshot.m_fFogDensity = environment->get_fog_density();
			snapshot.m_fFogStart = environment->get_fog_depth_begin();
			snapshot.m_fFogEnd = environment->get_fog_depth_end();
			snapshot.m_fFogCurve = environment->get_fog_depth_curve();
			snapshot.m_nFogMode = ( uint32_t )( environment->get_fog_mode() );
		}
	}

	Terrain3DData *const data = m_pTerrain->getData();
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
	const uint64_t region_topology_generation = data->getRegionTopologyGenerationInternal();
	PackedByteArray region_bytes;
	bool bRegionChanged = false;
	if ( region_topology_generation != m_nRegionTopologyGeneration ) {
		const PackedInt32Array region_map = data->getRegionMap();
		region_bytes.resize( region_map.size() * ( int64_t )( sizeof( int32_t ) ) );
		if ( !region_bytes.is_empty() ) std::memcpy( region_bytes.ptrw(), region_map.ptr(), ( size_t )( region_bytes.size() ) );
		bRegionChanged = true;
	}

	std::lock_guard<std::mutex> lock( m_snapshotMutex );
	m_snapshot = snapshot;
	m_hHeightTextureRs = data->getHeightMapsRid();
	m_hControlTextureRs = data->getControlMapsRid();
	m_hColorTextureRs = data->getColorMapsRid();
	m_hAlbedoTextureRs = assets.is_valid() ? assets->getAlbedoArrayRid() : RID();
	m_hNormalTextureRs = assets.is_valid() ? assets->getNormalArrayRid() : RID();
	m_hSurfaceTextureRs = assets.is_valid() ? assets->getSurfaceArrayRid() : RID();
	m_hDisplacementTextureRs = RID();
	const Ref<Terrain3DMaterial> material = m_pTerrain->getMaterial();
	RenderingServer *const server = RenderingServer::get_singleton();
	if ( snapshot.m_bDisplacementEnabled && material.is_valid() && material->getMaterialRid().is_valid() && server ) {
		const Variant displacement = server->material_get_param( material->getMaterialRid(), "_displacement_buffer" );
		if ( displacement.get_type() == Variant::RID ) m_hDisplacementTextureRs = displacement;
	}
	if ( !m_hDisplacementTextureRs.is_valid() ) m_snapshot.m_bDisplacementEnabled = false;
	if ( bRegionChanged ) {
		m_regionMapBytes = region_bytes;
		m_nRegionTopologyGeneration = region_topology_generation;
		++m_nRegionMapGeneration;
	}
}

void Terrain3DRenderCoordinator::updateNativeProxy( void ) {
	if ( !m_pTerrain || !m_pTerrain->getMesher() ) return;
	const t3dRenderBackend_t backend = m_backend.load( std::memory_order_acquire );
	const t3dRdNativeProxyMode_t proxy_mode = m_proxyMode.load( std::memory_order_acquire );
	const bool bRd = backend != T3D_RENDER_BACKEND_RENDERING_SERVER;
	const bool bFullNative = !bRd || backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID || proxy_mode == T3D_RD_NATIVE_PROXY_FULL;
	const bool bShadowProxy = bRd && !bFullNative && proxy_mode == T3D_RD_NATIVE_PROXY_SHADOWS_ONLY;
	m_pTerrain->getMesher()->setRdPresentation( bFullNative, bShadowProxy );
}

void Terrain3DRenderCoordinator::attachEffectToActiveCamera( void ) {
	if ( !m_pTerrain || m_effect.is_null() ||
		m_backend.load( std::memory_order_acquire ) == T3D_RENDER_BACKEND_RENDERING_SERVER ) return;
	Camera3D *camera = nullptr;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const viewport = editor ? editor->get_editor_viewport_3d( 0 ) : nullptr;
		camera = viewport ? viewport->get_camera_3d() : nullptr;
	} else if ( m_pTerrain->get_viewport() ) {
		camera = m_pTerrain->get_viewport()->get_camera_3d();
	}
	if ( !camera ) return;
	if ( m_nAttachedCameraId == camera->get_instance_id() && m_attachedCompositor.is_valid() && camera->get_compositor() == m_attachedCompositor ) return;
	detachEffect();
	Ref<Compositor> compositor = camera->get_compositor();
	m_bCreatedCompositor = compositor.is_null();
	if ( compositor.is_null() ) {
		compositor.instantiate();
		camera->set_compositor( compositor );
	}
	TypedArray<Ref<CompositorEffect>> effects = compositor->get_compositor_effects();
	bool bFound = false;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		if ( effects[i] == m_effect ) { bFound = true; break; }
	}
	if ( !bFound ) {
		effects.push_back( m_effect );
		compositor->set_compositor_effects( effects );
	}
	m_attachedCompositor = compositor;
	m_nAttachedCameraId = camera->get_instance_id();
}

void Terrain3DRenderCoordinator::detachEffect( void ) {
	if ( m_effect.is_null() || m_attachedCompositor.is_null() ) {
		m_attachedCompositor.unref();
		m_nAttachedCameraId = 0u;
		m_bCreatedCompositor = false;
		return;
	}
	TypedArray<Ref<CompositorEffect>> effects = m_attachedCompositor->get_compositor_effects();
	TypedArray<Ref<CompositorEffect>> remaining;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		Ref<CompositorEffect> effect = effects[i];
		if ( effect != m_effect ) remaining.push_back( effect );
	}
	m_attachedCompositor->set_compositor_effects( remaining );
	if ( m_bCreatedCompositor && remaining.is_empty() && m_nAttachedCameraId != 0u ) {
		Object *const object = ObjectDB::get_instance( m_nAttachedCameraId );
		Camera3D *const camera = Object::cast_to<Camera3D>( object );
		if ( camera && camera->get_compositor() == m_attachedCompositor ) camera->set_compositor( Ref<Compositor>() );
	}
	m_attachedCompositor.unref();
	m_nAttachedCameraId = 0u;
	m_bCreatedCompositor = false;
}

bool Terrain3DRenderCoordinator::initializeRendererOnRenderThread( void ) {
	if ( m_bRendererInitialized.load( std::memory_order_acquire ) ) return true;
	const bool ready = m_shaderLibrary.isReady() && m_renderer.initialize( m_shaderLibrary.getNear(),
			m_shaderLibrary.getMiddle(),
			m_shaderLibrary.getFar(),
			m_shaderLibrary.getCull() );
	m_bRendererInitialized.store( ready, std::memory_order_release );
	m_bRendererFailed.store( !ready, std::memory_order_release );
	if ( !ready ) {
		m_renderer.shutdown();
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.isReady() ? m_renderer.getLastError() : m_shaderLibrary.getLastError();
	}
	return ready;
}

void Terrain3DRenderCoordinator::initialize( Terrain3D *const pTerrain ) {
	shutdown();
	m_pTerrain = pTerrain;
	if ( !m_pTerrain ) return;
	m_backend.store( m_pTerrain->getRenderBackend(), std::memory_order_release );
	m_proxyMode.store( m_pTerrain->getRdNativeProxyMode(), std::memory_order_release );
	m_bRendererFailed.store( false, std::memory_order_release );
	m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
	m_bFallbackActive = false;
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = String();
	}
	m_effect.instantiate();
	m_effect->setCoordinator( this );
	if ( !m_shaderLibrary.load() ) {
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
	}
	if ( !m_pTerrain->getMesher() || !m_renderer.prepareTopologies( m_pTerrain->getMesher()->getClipmapGeometry( LOD0_MESH ),
			m_pTerrain->getMesher()->getClipmapGeometry( OUTER_RING_MESH ) ) ) {
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = "Failed to prepare packed RenderingDevice clipmap topology";
	}
	publishSnapshot();
	updateNativeProxy();
	m_bRenderEnabled.store( m_backend.load( std::memory_order_acquire ) != T3D_RENDER_BACKEND_RENDERING_SERVER,
			std::memory_order_release );
	attachEffectToActiveCamera();
}

void Terrain3DRenderCoordinator::shutdown( void ) {
	m_bRenderEnabled.store( false, std::memory_order_release );
	if ( m_effect.is_valid() ) m_effect->set_enabled( false );
	RenderingServer *const server = RenderingServer::get_singleton();
	if ( m_bRendererInitialized.load( std::memory_order_acquire ) ) {
		if ( server && server->is_on_render_thread() ) shutdownRendererOnRenderThread();
		else if ( server && m_effect.is_valid() ) {
			server->call_on_render_thread( callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::shutdownRendererOnRenderThread ) );
			server->force_sync();
		} else {
			m_renderer.shutdown();
			m_bRendererInitialized.store( false, std::memory_order_release );
		}
	}
	if ( m_effect.is_valid() ) m_effect->setCoordinator( nullptr );
	detachEffect();
	m_effect.unref();
	if ( m_pTerrain && m_pTerrain->getMesher() ) m_pTerrain->getMesher()->setRdPresentation( true, false );
	m_shaderLibrary.clear();
	{
		std::lock_guard<std::mutex> lock( m_snapshotMutex );
		m_snapshot = t3dRdFrameSnapshot_t{};
		m_regionMapBytes.clear();
		m_nRegionTopologyGeneration = 0u;
		m_nRegionMapGeneration = 0u;
		m_hHeightTextureRs = RID();
		m_hControlTextureRs = RID();
		m_hColorTextureRs = RID();
		m_hAlbedoTextureRs = RID();
		m_hNormalTextureRs = RID();
		m_hSurfaceTextureRs = RID();
		m_hDisplacementTextureRs = RID();
	}
	m_pTerrain = nullptr;
}

void Terrain3DRenderCoordinator::update( void ) {
	if ( !m_pTerrain ) return;
	publishSnapshot();
	if ( m_bRendererFailed.load( std::memory_order_acquire ) && !m_bFallbackActive ) {
		m_bFallbackActive = true;
		m_bRenderEnabled.store( false, std::memory_order_release );
		if ( m_effect.is_valid() ) m_effect->set_enabled( false );
		if ( m_pTerrain->getMesher() ) m_pTerrain->getMesher()->setRdPresentation( true, false );
	}
	if ( m_backend.load( std::memory_order_acquire ) != T3D_RENDER_BACKEND_RENDERING_SERVER && !m_bFallbackActive )
		attachEffectToActiveCamera();
}

void Terrain3DRenderCoordinator::setBackend( const t3dRenderBackend_t backend ) {
	if ( m_backend.load( std::memory_order_acquire ) == backend ) return;
	m_backend.store( backend, std::memory_order_release );
	if ( backend != T3D_RENDER_BACKEND_RENDERING_SERVER ) {
		m_bRendererFailed.store( false, std::memory_order_release );
		m_bFallbackActive = false;
	}
	m_bRenderEnabled.store( backend != T3D_RENDER_BACKEND_RENDERING_SERVER, std::memory_order_release );
	if ( m_effect.is_valid() ) m_effect->set_enabled( backend != T3D_RENDER_BACKEND_RENDERING_SERVER );
	updateNativeProxy();
	if ( backend == T3D_RENDER_BACKEND_RENDERING_SERVER ) detachEffect();
	else attachEffectToActiveCamera();
}

void Terrain3DRenderCoordinator::setProxyMode( const t3dRdNativeProxyMode_t mode ) {
	if ( m_proxyMode.load( std::memory_order_acquire ) == mode ) return;
	m_proxyMode.store( mode, std::memory_order_release );
	updateNativeProxy();
}

void Terrain3DRenderCoordinator::shutdownRendererOnRenderThread( void ) {
	m_renderer.shutdown();
	m_bRendererInitialized.store( false, std::memory_order_release );
}

void Terrain3DRenderCoordinator::render( RenderData *const pRenderData ) {
	if ( !m_bRenderEnabled.load( std::memory_order_acquire ) || !pRenderData ) return;
	++m_nCallbackCount;
	if ( !initializeRendererOnRenderThread() ) return;
	t3dRdFrameSnapshot_t snapshot{};
	PackedByteArray region_map;
	uint64_t region_generation = 0u;
	RID height, control, color, albedo, normal, surface, displacement;
	{
		std::lock_guard<std::mutex> lock( m_snapshotMutex );
		snapshot = m_snapshot;
		region_map = m_regionMapBytes;
		region_generation = m_nRegionMapGeneration;
		height = m_hHeightTextureRs;
		control = m_hControlTextureRs;
		color = m_hColorTextureRs;
		albedo = m_hAlbedoTextureRs;
		normal = m_hNormalTextureRs;
		surface = m_hSurfaceTextureRs;
		displacement = m_hDisplacementTextureRs;
	}
	if ( snapshot.m_nPatchCount == 0u || region_map.is_empty() ) {
		m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
		return;
	}
	const t3dRenderBackend_t backend = m_backend.load( std::memory_order_acquire );
	const t3dRdNativeProxyMode_t proxy_mode = m_proxyMode.load( std::memory_order_acquire );
	const bool bNativeSurface = backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID || proxy_mode == T3D_RD_NATIVE_PROXY_FULL;
	const bool bColorWrite = !bNativeSurface;
	if ( m_renderer.render( pRenderData,
			 snapshot,
			 region_map,
			 region_generation,
			 height,
			 control,
			 color,
			 albedo,
			 normal,
			 surface,
			 displacement,
			 bColorWrite ) ) {
		m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
	} else {
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
		if ( failures >= 8u ) m_bRendererFailed.store( true, std::memory_order_release );
	}
}

Dictionary Terrain3DRenderCoordinator::getDiagnostics( void ) const {
	Dictionary result;
	const Dictionary renderer = m_renderer.getDiagnostics();
	const t3dRenderBackend_t backend = m_backend.load( std::memory_order_acquire );
	const t3dRdNativeProxyMode_t proxy_mode = m_proxyMode.load( std::memory_order_acquire );
	result["backend"] = ( int )( backend );
	result["proxy_mode"] = ( int )( proxy_mode );
	result["render_enabled"] = m_bRenderEnabled.load( std::memory_order_acquire );
	result["renderer_initialized"] = m_bRendererInitialized.load( std::memory_order_acquire );
	result["fallback_active"] = m_bFallbackActive;
	result["callbacks"] = ( int64_t )( m_nCallbackCount.load( std::memory_order_acquire ) );
	result["consecutive_render_failures"] = ( int64_t )( m_nConsecutiveRenderFailures.load( std::memory_order_acquire ) );
	result["renderer"] = renderer;
	result["shaders"] = m_shaderLibrary.getDiagnostics();
	result["frames_rendered"] = renderer.get( "frames_rendered", 0 );
	result["draws_rendered"] = renderer.get( "draws_rendered", 0 );
	result["indirect_draws"] = renderer.get( "indirect_draws", 0 );
	result["cull_dispatches"] = renderer.get( "cull_dispatches", 0 );
	result["patch_uploads"] = renderer.get( "patch_uploads", 0 );
	result["topology_bytes"] = renderer.get( "topology_bytes", 0 );
	result["lod0_index_bits"] = renderer.get( "lod0_index_bits", 0 );
	result["outer_index_bits"] = renderer.get( "outer_index_bits", 0 );
	result["gpu_culling"] = renderer.get( "gpu_culling", false );
	result["resource_creates"] = renderer.get( "resource_creates", 0 );
	result["resource_frees"] = renderer.get( "resource_frees", 0 );
	result["live_buffer_bytes"] = renderer.get( "live_buffer_bytes", 0 );
	result["peak_buffer_bytes"] = renderer.get( "peak_buffer_bytes", 0 );
	result["live_texture_bytes"] = renderer.get( "live_texture_bytes", 0 );
	result["peak_texture_bytes"] = renderer.get( "peak_texture_bytes", 0 );
	result["buffer_updates"] = renderer.get( "buffer_updates", 0 );
	result["bytes_uploaded"] = renderer.get( "bytes_uploaded", 0 );
	result["last_render_error"] = renderer.get( "last_error", "" );
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		result["initialization_error"] = m_initializationError;
	}
	result["native_depth_prepass_participation"] = false;
	result["native_shadow_proxy_available"] = true;
	result["multiview_supported"] = true;
	result["single_view_only"] = false;
	const bool bNativeSurface = backend == T3D_RENDER_BACKEND_RENDERING_SERVER ||
			backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID || proxy_mode == T3D_RD_NATIVE_PROXY_FULL;
	result["native_surface_enabled"] = bNativeSurface;
	result["color_write_enabled"] = backend != T3D_RENDER_BACKEND_RENDERING_SERVER && !bNativeSurface;
	result["hybrid_depth_only"] = backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID;
	return result;
}
