// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/compositor/terrain_3d_rd_compositor_effect.h"

Terrain3DRdCompositorEffect::Terrain3DRdCompositorEffect(
	void ) {
	set_effect_callback_type( CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_OPAQUE );
	set_access_resolved_color( false );
	set_access_resolved_depth( false );
	set_needs_motion_vectors( false );
	set_needs_normal_roughness( false );
	set_needs_separate_specular( false );
	set_enabled( false );
}

void Terrain3DRdCompositorEffect::setCoordinator(
	Terrain3DRenderCoordinatorAccess *const pCoordinator ) {
	m_pCoordinator = pCoordinator;
}

void Terrain3DRdCompositorEffect::shutdownRendererOnRenderThread(
	void ) {
	if ( m_pCoordinator ) {
		m_pCoordinator->shutdownRendererOnRenderThread();
	}
}

void Terrain3DRdCompositorEffect::shutdownRendererLifecycleOnRenderThread(
	const int64_t nShutdownGeneration ) {
	if ( m_pCoordinator && nShutdownGeneration >= 0 && nShutdownGeneration <= UINT32_MAX ) {
		m_pCoordinator->shutdownRendererLifecycleOnRenderThread( ( uint32_t ) nShutdownGeneration );
	}
}

#if defined( T3D_EDITOR_NATIVE )
void Terrain3DRdCompositorEffect::initializeEditorBrushOnRenderThread(
	const int64_t nStrokeId ) {
	if ( m_pCoordinator && nStrokeId > 0 ) {
		m_pCoordinator->initializeEditorBrushOnRenderThread( ( uint64_t ) nStrokeId );
	}
}
void Terrain3DRdCompositorEffect::uploadEditorBrushTileOnRenderThread(
    const int64_t nStrokeId, const int64_t nTile, const godot::PackedByteArray &rSource ) {
    if ( m_pCoordinator && nStrokeId > 0 && nTile >= 0 )
        m_pCoordinator->uploadEditorBrushTileOnRenderThread( ( uint64_t ) nStrokeId,
            ( uint32_t ) nTile, rSource );
}
void Terrain3DRdCompositorEffect::executeEditorBrushCommandOnRenderThread(
    const int64_t nStrokeId, const int64_t nCommand ) {
    if ( m_pCoordinator && nStrokeId > 0 && nCommand >= 0 )
        m_pCoordinator->executeEditorBrushCommandOnRenderThread( ( uint64_t ) nStrokeId, ( uint32_t ) nCommand );
}
void Terrain3DRdCompositorEffect::sealEditorBrushOnRenderThread(
	const int64_t nStrokeId ) {
	if ( m_pCoordinator && nStrokeId > 0 ) {
		m_pCoordinator->sealEditorBrushOnRenderThread( ( uint64_t ) nStrokeId );
	}
}
void Terrain3DRdCompositorEffect::discardEditorBrushOnRenderThread(
	const int64_t nStrokeId ) {
	if ( m_pCoordinator && nStrokeId > 0 ) {
		m_pCoordinator->discardEditorBrushOnRenderThread( ( uint64_t ) nStrokeId );
	}
}
void Terrain3DRdCompositorEffect::consumeEditorBrushReadbackOnRenderThread(
	const godot::PackedByteArray &rBytes, const int64_t nStrokeId,
	const int64_t nOwnerGeneration, const int64_t nDeviceGeneration ) {
	if ( m_pCoordinator && nStrokeId > 0 && nOwnerGeneration > 0 &&
         nOwnerGeneration <= UINT32_MAX && nDeviceGeneration > 0 ) {
		m_pCoordinator->consumeEditorBrushReadbackOnRenderThread( rBytes,
			( uint64_t ) nStrokeId, ( uint32_t ) nOwnerGeneration, ( uint64_t ) nDeviceGeneration );
	}
}
#endif

void Terrain3DRdCompositorEffect::renderPreviewOnRenderThread(
	void ) {
	if ( m_pCoordinator ) {
		m_pCoordinator->renderPreviewOnRenderThread();
	}
}

void Terrain3DRdCompositorEffect::renderPreviewAsyncOnRenderThread(
	void ) {
	if ( m_pCoordinator ) {
		m_pCoordinator->renderPreviewAsyncOnRenderThread();
	}
}

void Terrain3DRdCompositorEffect::servicePreviewVtRepublishOnRenderThread(
	void ) {
	if ( m_pCoordinator ) {
		m_pCoordinator->servicePreviewVtRepublishOnRenderThread();
	}
}

void Terrain3DRdCompositorEffect::serviceShaderStartupOnRenderThread(
	const int64_t nStartupGeneration ) {
	if ( m_pCoordinator && nStartupGeneration > 0 && nStartupGeneration <= UINT32_MAX ) {
		m_pCoordinator->serviceShaderStartupOnRenderThread( ( uint32_t ) nStartupGeneration );
	}
}

void Terrain3DRdCompositorEffect::recoverRendererLifecycleOnRenderThread(
	const int64_t nStartupGeneration,
	const int64_t nRecoveryRevision ) {
	if ( m_pCoordinator && nStartupGeneration > 0 && nStartupGeneration <= UINT32_MAX && nRecoveryRevision > 0 ) {
		m_pCoordinator->recoverRendererLifecycleOnRenderThread( ( uint32_t ) nStartupGeneration,
																( uint64_t ) nRecoveryRevision );
	}
}

void Terrain3DRdCompositorEffect::acknowledgeStartupCancellationOnRenderThread(
	const int64_t nStartupGeneration ) {
	if ( m_pCoordinator && nStartupGeneration > 0 && nStartupGeneration <= UINT32_MAX ) {
		m_pCoordinator->acknowledgeStartupCancellationOnRenderThread( ( uint32_t ) nStartupGeneration );
	}
}

void Terrain3DRdCompositorEffect::serviceStartupWorldPublicationOnRenderThread(
	const int64_t nStartupGeneration,
	const int64_t nSnapshotGeneration ) {
	if ( m_pCoordinator && nStartupGeneration > 0 && nStartupGeneration <= UINT32_MAX && nSnapshotGeneration > 0 ) {
		m_pCoordinator->serviceStartupWorldPublicationOnRenderThread( ( uint32_t ) nStartupGeneration,
																	  ( uint64_t ) nSnapshotGeneration );
	}
}

void Terrain3DRdCompositorEffect::consumePreviewReadbackOnRenderThread(
	const godot::PackedByteArray &rReadbackBytes,
	const int32_t				  nSlot,
	const int64_t				  nRequestGeneration ) {
	if ( m_pCoordinator != NULL && nSlot >= 0 ) {
		m_pCoordinator->consumePreviewReadbackOnRenderThread( ( uint32_t ) nSlot,
															  ( uint64_t ) nRequestGeneration,
															  rReadbackBytes );
	}
}

void Terrain3DRdCompositorEffect::_render_callback(
	const int32_t			 nEffectCallbackType,
	godot::RenderData *const pRenderData ) {
	if ( nEffectCallbackType != CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_OPAQUE || !m_pCoordinator ) {
		return;
	}
	m_pCoordinator->render( pRenderData );
}

void Terrain3DRdCompositorEffect::consumeFragmentFeedbackOnRenderThread(
	const godot::PackedByteArray &rBytes ) {
	if ( m_pCoordinator ) {
		m_pCoordinator->consumeFragmentFeedbackOnRenderThread( rBytes );
	}
}

void Terrain3DRdCompositorEffect::consumeDescriptorFenceOnRenderThread(
	const godot::PackedByteArray &rBytes, const int64_t nEpoch,
	const int64_t nDeviceGeneration, const int64_t nTicket ) {
	if ( m_pCoordinator && nEpoch > 0 && nDeviceGeneration > 0 && nTicket > 0 ) {
		m_pCoordinator->consumeDescriptorFenceOnRenderThread( rBytes,
			( uint64_t ) nEpoch, ( uint64_t ) nDeviceGeneration, ( uint64_t ) nTicket );
	}
}
