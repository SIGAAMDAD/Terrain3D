// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RENDER_COORDINATOR_H
#define TERRAIN3D_RENDER_COORDINATOR_H

#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/terrain_3d_rd_shader_library.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <cstddef>

#include <atomic>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <mutex>

class Terrain3D;

class Terrain3DRenderCoordinator {
	CLASS_NAME_STATIC( "Terrain3DRenderCoordinator" );

private:
	Terrain3D *m_pTerrain = nullptr;
	Ref<Terrain3DRdCompositorEffect> m_effect;
	Ref<Compositor> m_attachedCompositor;
	uint64_t m_nAttachedCameraId = 0u;
	bool m_bCreatedCompositor = false;
	std::atomic<t3dRenderBackend_t> m_backend{ T3D_RENDER_BACKEND_RENDERING_SERVER };
	std::atomic<t3dRdNativeProxyMode_t> m_proxyMode{ T3D_RD_NATIVE_PROXY_SHADOWS_ONLY };

	mutable std::mutex m_snapshotMutex;
	t3dRdFrameSnapshot_t m_snapshot;
	PackedByteArray m_regionMapBytes;
	uint64_t m_nRegionTopologyGeneration = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	RID m_hHeightTextureRs;
	RID m_hControlTextureRs;
	RID m_hColorTextureRs;
	RID m_hAlbedoTextureRs;
	RID m_hNormalTextureRs;
	RID m_hSurfaceTextureRs;
	RID m_hDisplacementTextureRs;

	Terrain3DRdShaderLibrary m_shaderLibrary;
	Terrain3DRdClipmapRenderer m_renderer;
	std::atomic_bool m_bRendererInitialized{ false };
	std::atomic_bool m_bRenderEnabled{ false };
	std::atomic_bool m_bRendererFailed{ false };
	bool m_bFallbackActive = false;
	std::atomic_uint64_t m_nCallbackCount{ 0u };
	std::atomic_uint32_t m_nConsecutiveRenderFailures{ 0u };
	mutable std::mutex m_diagnosticsMutex;
	String m_initializationError;

	uint64_t hashBytes( const void *pData, size_t nBytes ) const;
	void buildMaterialState( t3dRdFrameSnapshot_t &rSnapshot ) const;
	void publishSnapshot( void );
	void updateNativeProxy( void );
	void attachEffectToActiveCamera( void );
	void detachEffect( void );
	bool initializeRendererOnRenderThread( void );

public:
	Terrain3DRenderCoordinator( void ) = default;
	~Terrain3DRenderCoordinator( void );

	void initialize( Terrain3D *pTerrain );
	void shutdown( void );
	void update( void );
	void setBackend( t3dRenderBackend_t backend );
	void setProxyMode( t3dRdNativeProxyMode_t mode );
	void render( RenderData *pRenderData );
	void shutdownRendererOnRenderThread( void );
	Dictionary getDiagnostics( void ) const;

	EA_FORCE_INLINE t3dRenderBackend_t getBackend( void ) const { return m_backend.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE t3dRdNativeProxyMode_t getProxyMode( void ) const { return m_proxyMode.load( std::memory_order_acquire ); }
};

#endif // TERRAIN3D_RENDER_COORDINATOR_H
