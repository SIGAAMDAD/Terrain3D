// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_render_coordinator.h"

#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "rendering/rd/terrain_3d_rd_compositor_effect.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>

#include <cstring>

Terrain3DRenderCoordinator::~Terrain3DRenderCoordinator( void ) {
	shutdown();
}

uint64_t Terrain3DRenderCoordinator::hashRegionMap( const PackedInt32Array &rMap ) const {
	uint64_t hash = 1469598103934665603ULL;
	const uint8_t *const bytes = ( const uint8_t * )( rMap.ptr() );
	const size_t byte_count = ( size_t )( rMap.size() ) * sizeof( int32_t );
	for ( size_t i = 0u; i < byte_count; ++i ) {
		hash ^= ( uint64_t )( bytes[i] );
		hash *= 1099511628211ULL;
	}
	return hash;
}

void Terrain3DRenderCoordinator::buildTopologyCopies( void ) {
	m_lod0Vertices.clear();
	m_lod0Indices.clear();
	m_outerVertices.clear();
	m_outerIndices.clear();
	m_nLod0VertexCount = 0u;
	m_nLod0IndexCount = 0u;
	m_nOuterVertexCount = 0u;
	m_nOuterIndexCount = 0u;
	if ( !m_pTerrain || !m_pTerrain->getMesher() ) {
		return;
	}
	Terrain3DMesher *const mesher = m_pTerrain->getMesher();
	auto copy_topology = []( const t3dClipmapGeometry_t &rGeometry,
			PackedByteArray &rVertexBytes,
			uint32_t &rVertexCount,
			PackedByteArray &rIndexBytes,
			uint32_t &rIndexCount ) {
		if ( rGeometry.m_vertices.size() != rGeometry.m_metadata.size() || rGeometry.m_vertices.is_empty() ||
				rGeometry.m_indices.is_empty() ) {
			return;
		}
		rVertexCount = ( uint32_t )( rGeometry.m_vertices.size() );
		rIndexCount = ( uint32_t )( rGeometry.m_indices.size() );
		rVertexBytes.resize( ( int64_t )( rVertexCount ) * ( int64_t )( sizeof( t3dRdClipmapVertex_t ) ) );
		t3dRdClipmapVertex_t *const vertices = ( t3dRdClipmapVertex_t * )( rVertexBytes.ptrw() );
		for ( uint32_t i = 0u; i < rVertexCount; ++i ) {
			const Vector3 position = rGeometry.m_vertices[( int64_t )( i )];
			const Vector2 metadata = rGeometry.m_metadata[( int64_t )( i )];
			vertices[i].m_fX = ( float )( position.x );
			vertices[i].m_fY = ( float )( position.y );
			vertices[i].m_fZ = ( float )( position.z );
			vertices[i].m_fPartCode = ( float )( metadata.x );
		}
		rIndexBytes.resize( ( int64_t )( rIndexCount ) * ( int64_t )( sizeof( uint32_t ) ) );
		uint32_t *const indices = ( uint32_t * )( rIndexBytes.ptrw() );
		for ( uint32_t i = 0u; i < rIndexCount; ++i ) {
			indices[i] = ( uint32_t )( rGeometry.m_indices[( int64_t )( i )] );
		}
	};
	copy_topology( mesher->getClipmapGeometry( LOD0_MESH ),
			m_lod0Vertices,
			m_nLod0VertexCount,
			m_lod0Indices,
			m_nLod0IndexCount );
	copy_topology( mesher->getClipmapGeometry( OUTER_RING_MESH ),
			m_outerVertices,
			m_nOuterVertexCount,
			m_outerIndices,
			m_nOuterIndexCount );
}

void Terrain3DRenderCoordinator::publishSnapshot( void ) {
	if ( !m_pTerrain || !m_pTerrain->getMesher() || !m_pTerrain->getData() ) {
		return;
	}
	t3dRdFrameSnapshot_t snapshot{};
	snapshot.m_nGeneration = m_snapshot.m_nGeneration + 1u;
	snapshot.m_nRegionSize = ( uint32_t )( m_pTerrain->getRegionSize() );
	snapshot.m_nRegionMapSize = ( uint32_t )( m_pTerrain->getData()->getRegionMapSizeInternal() );
	snapshot.m_nMeshSize = ( uint32_t )( m_pTerrain->getMeshSize() );
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
	const RID albedo_rid = assets.is_valid() ? assets->getAlbedoArrayRid() : RID();
	snapshot.m_nMaterialLayerCount = albedo_rid.is_valid()
			? ( uint32_t )( MAX( assets->getTextureCount(), 1 ) )
			: 1u;
	snapshot.m_fVertexSpacing = ( float )( m_pTerrain->getVertexSpacing() );
	snapshot.m_targetPosition = m_pTerrain->getClipmapTargetPosition();
	snapshot.m_fSubdiv = ( float )( 1 << m_pTerrain->getTessellationLevel() );
	snapshot.m_fHeightMin = s_fRegionHeightMin;
	snapshot.m_fHeightRange = s_fRegionHeightRange;
	snapshot.m_nPatchCount = ( uint32_t )( m_pTerrain->getMesher()->fillRdPatches(
			snapshot.m_szPatches, s_nMaxClipmapLevels ) );
	DirectionalLight3D *const light = Object::cast_to<DirectionalLight3D>( m_pTerrain->getLightTarget() );
	if ( light ) {
		snapshot.m_lightColor = light->get_color() * light->get_param( DirectionalLight3D::PARAM_ENERGY );
		snapshot.m_lightDirection = light->get_global_basis().get_column( 2 ).normalized();
	}

	Terrain3DData *const data = m_pTerrain->getData();
	const PackedInt32Array region_map = data->getRegionMap();
	const uint64_t region_hash = hashRegionMap( region_map );
	PackedByteArray region_bytes;
	bool bRegionChanged = false;
	if ( region_hash != m_nRegionMapHash ) {
		region_bytes.resize( region_map.size() * ( int64_t )( sizeof( int32_t ) ) );
		if ( !region_bytes.is_empty() ) {
			std::memcpy( region_bytes.ptrw(), region_map.ptr(), ( size_t )( region_bytes.size() ) );
		}
		bRegionChanged = true;
	}

	std::lock_guard<std::mutex> lock( m_snapshotMutex );
	m_snapshot = snapshot;
	m_hHeightTextureRs = data->getHeightMapsRid();
	m_hControlTextureRs = data->getControlMapsRid();
	m_hColorTextureRs = data->getColorMapsRid();
	m_hAlbedoTextureRs = albedo_rid;
	if ( bRegionChanged ) {
		m_regionMapBytes = region_bytes;
		m_nRegionMapHash = region_hash;
		++m_nRegionMapGeneration;
	}
}

void Terrain3DRenderCoordinator::updateNativeProxy( void ) {
	if ( !m_pTerrain || !m_pTerrain->getMesher() ) {
		return;
	}
	const bool bRd = m_backend != T3D_RENDER_BACKEND_RENDERING_SERVER;
	const bool bFullNative = !bRd || m_backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID ||
			m_proxyMode == T3D_RD_NATIVE_PROXY_FULL;
	const bool bShadowProxy = bRd && !bFullNative && m_proxyMode == T3D_RD_NATIVE_PROXY_SHADOWS_ONLY;
	m_pTerrain->getMesher()->setRdPresentation( bFullNative, bShadowProxy );
}

void Terrain3DRenderCoordinator::attachEffectToActiveCamera( void ) {
	if ( !m_pTerrain || m_effect.is_null() || m_backend == T3D_RENDER_BACKEND_RENDERING_SERVER ) {
		return;
	}
	Camera3D *camera = nullptr;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const viewport = editor ? editor->get_editor_viewport_3d( 0 ) : nullptr;
		camera = viewport ? viewport->get_camera_3d() : nullptr;
	} else if ( m_pTerrain->get_viewport() ) {
		camera = m_pTerrain->get_viewport()->get_camera_3d();
	}
	if ( !camera ) {
		return;
	}
	if ( m_nAttachedCameraId == camera->get_instance_id() && m_attachedCompositor.is_valid() &&
			camera->get_compositor() == m_attachedCompositor ) {
		return;
	}
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
		if ( effects[i] == m_effect ) {
			bFound = true;
			break;
		}
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
		if ( effect != m_effect ) {
			remaining.push_back( effect );
		}
	}
	m_attachedCompositor->set_compositor_effects( remaining );
	if ( m_bCreatedCompositor && remaining.is_empty() && m_nAttachedCameraId != 0u ) {
		Object *const object = ObjectDB::get_instance( m_nAttachedCameraId );
		Camera3D *const camera = Object::cast_to<Camera3D>( object );
		if ( camera && camera->get_compositor() == m_attachedCompositor ) {
			camera->set_compositor( Ref<Compositor>() );
		}
	}
	m_attachedCompositor.unref();
	m_nAttachedCameraId = 0u;
	m_bCreatedCompositor = false;
}

bool Terrain3DRenderCoordinator::initializeRendererOnRenderThread( void ) {
	if ( m_bRendererInitialized.load( std::memory_order_acquire ) ) {
		return true;
	}
	const bool ready = m_renderer.initialize( m_shaderSpirv,
			m_lod0Vertices,
			m_nLod0VertexCount,
			m_lod0Indices,
			m_nLod0IndexCount,
			m_outerVertices,
			m_nOuterVertexCount,
			m_outerIndices,
			m_nOuterIndexCount );
	m_bRendererInitialized.store( ready, std::memory_order_release );
	m_bRendererFailed.store( !ready, std::memory_order_release );
	if ( !ready ) {
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = m_renderer.getLastError();
	}
	return ready;
}

void Terrain3DRenderCoordinator::initialize( Terrain3D *const pTerrain ) {
	shutdown();
	m_pTerrain = pTerrain;
	if ( !m_pTerrain ) {
		return;
	}
	m_backend = m_pTerrain->getRenderBackend();
	m_proxyMode = m_pTerrain->getRdNativeProxyMode();
	m_bRendererFailed.store( false, std::memory_order_release );
	m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
	m_bFallbackActive = false;
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = String();
	}
	m_effect.instantiate();
	m_effect->setCoordinator( this );
	Ref<RDShaderFile> shader_file;
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( loader ) {
		shader_file = loader->load( "res://addons/terrain_3d/rd/terrain_clipmap.glsl" );
	}
	m_shaderSpirv = shader_file.is_valid() ? shader_file->get_spirv() : Ref<RDShaderSPIRV>();
	if ( m_shaderSpirv.is_null() ) {
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = "RD experiment shader failed to load or compile";
	}
	buildTopologyCopies();
	publishSnapshot();
	updateNativeProxy();
	m_bRenderEnabled.store( m_backend != T3D_RENDER_BACKEND_RENDERING_SERVER, std::memory_order_release );
	attachEffectToActiveCamera();
}

void Terrain3DRenderCoordinator::shutdown( void ) {
	m_bRenderEnabled.store( false, std::memory_order_release );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}

	RenderingServer *const server = RenderingServer::get_singleton();
	if ( m_bRendererInitialized.load( std::memory_order_acquire ) ) {
		if ( server && server->is_on_render_thread() ) {
			shutdownRendererOnRenderThread();
		} else if ( server && m_effect.is_valid() ) {
			server->call_on_render_thread( callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::shutdownRendererOnRenderThread ) );
			server->force_sync();
		} else {
		m_renderer.shutdown();
		m_bRendererInitialized.store( false, std::memory_order_release );
		}
	}

	if ( m_effect.is_valid() ) {
		m_effect->setCoordinator( nullptr );
	}
	detachEffect();
	m_effect.unref();
	if ( m_pTerrain && m_pTerrain->getMesher() ) {
		m_pTerrain->getMesher()->setRdPresentation( true, false );
	}
	m_shaderSpirv.unref();
	m_pTerrain = nullptr;
}

void Terrain3DRenderCoordinator::update( void ) {
	if ( !m_pTerrain ) {
		return;
	}
	publishSnapshot();
	if ( m_bRendererFailed.load( std::memory_order_acquire ) && !m_bFallbackActive ) {
		m_bFallbackActive = true;
		m_bRenderEnabled.store( false, std::memory_order_release );
		if ( m_effect.is_valid() ) {
			m_effect->set_enabled( false );
		}
		if ( m_pTerrain->getMesher() ) {
			m_pTerrain->getMesher()->setRdPresentation( true, false );
		}
	}
	if ( m_backend != T3D_RENDER_BACKEND_RENDERING_SERVER && !m_bFallbackActive ) {
		attachEffectToActiveCamera();
	}
}

void Terrain3DRenderCoordinator::setBackend( const t3dRenderBackend_t backend ) {
	if ( m_backend == backend ) {
		return;
	}
	m_backend = backend;
	if ( m_backend != T3D_RENDER_BACKEND_RENDERING_SERVER ) {
		m_bRendererFailed.store( false, std::memory_order_release );
		m_bFallbackActive = false;
	}
	m_bRenderEnabled.store( m_backend != T3D_RENDER_BACKEND_RENDERING_SERVER, std::memory_order_release );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( m_backend != T3D_RENDER_BACKEND_RENDERING_SERVER );
	}
	updateNativeProxy();
	if ( m_backend == T3D_RENDER_BACKEND_RENDERING_SERVER ) {
		detachEffect();
	} else {
		attachEffectToActiveCamera();
	}
}

void Terrain3DRenderCoordinator::setProxyMode( const t3dRdNativeProxyMode_t mode ) {
	if ( m_proxyMode == mode ) {
		return;
	}
	m_proxyMode = mode;
	updateNativeProxy();
}

void Terrain3DRenderCoordinator::shutdownRendererOnRenderThread( void ) {
	m_renderer.shutdown();
	m_bRendererInitialized.store( false, std::memory_order_release );
}

void Terrain3DRenderCoordinator::render( RenderData *const pRenderData ) {
	if ( !m_bRenderEnabled.load( std::memory_order_acquire ) || !pRenderData ) {
		return;
	}
	++m_nCallbackCount;
	if ( !initializeRendererOnRenderThread() ) {
		return;
	}
	t3dRdFrameSnapshot_t snapshot{};
	PackedByteArray region_map;
	uint64_t region_generation = 0u;
	RID height;
	RID control;
	RID color;
	RID albedo;
	{
		std::lock_guard<std::mutex> lock( m_snapshotMutex );
		snapshot = m_snapshot;
		region_map = m_regionMapBytes;
		region_generation = m_nRegionMapGeneration;
		height = m_hHeightTextureRs;
		control = m_hControlTextureRs;
		color = m_hColorTextureRs;
		albedo = m_hAlbedoTextureRs;
	}
	if ( snapshot.m_nPatchCount == 0u || region_map.is_empty() ) {
		m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
		return;
	}
	if ( m_renderer.render( pRenderData, snapshot, region_map, region_generation, height, control, color, albedo ) ) {
		m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
	} else {
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
		if ( failures >= 8u ) {
			m_bRendererFailed.store( true, std::memory_order_release );
		}
	}
}

Dictionary Terrain3DRenderCoordinator::getDiagnostics( void ) const {
	Dictionary result;
	result["backend"] = ( int )( m_backend );
	result["proxy_mode"] = ( int )( m_proxyMode );
	result["render_enabled"] = m_bRenderEnabled.load( std::memory_order_acquire );
	result["renderer_initialized"] = m_bRendererInitialized.load( std::memory_order_acquire );
	result["fallback_active"] = m_bFallbackActive;
	result["callbacks"] = ( int64_t )( m_nCallbackCount.load( std::memory_order_acquire ) );
	result["consecutive_render_failures"] = ( int64_t )( m_nConsecutiveRenderFailures.load( std::memory_order_acquire ) );
	result["frames_rendered"] = ( int64_t )( m_renderer.getFramesRendered() );
	result["draws_rendered"] = ( int64_t )( m_renderer.getDrawsRendered() );
	result["topology_bytes"] = ( int64_t )( m_renderer.getTopologyBytes() );
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		result["initialization_error"] = m_initializationError;
	}
	result["last_render_error"] = m_renderer.getLastError();
	result["single_view_only"] = true;
	result["native_depth_prepass_participation"] = false;
	return result;
}
