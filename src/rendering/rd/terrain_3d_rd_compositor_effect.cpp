// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_compositor_effect.h"

#include "rendering/rd/terrain_3d_render_coordinator.h"

Terrain3DRdCompositorEffect::Terrain3DRdCompositorEffect( void ) {
	set_effect_callback_type( CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_OPAQUE );
	set_access_resolved_color( false );
	set_access_resolved_depth( false );
	set_needs_motion_vectors( false );
	set_needs_normal_roughness( false );
	set_needs_separate_specular( false );
	set_enabled( false );
}

void Terrain3DRdCompositorEffect::setCoordinator( Terrain3DRenderCoordinator *const pCoordinator ) {
	m_pCoordinator = pCoordinator;
}

void Terrain3DRdCompositorEffect::shutdownRendererOnRenderThread( void ) {
	if ( m_pCoordinator ) {
		m_pCoordinator->shutdownRendererOnRenderThread();
	}
}

void Terrain3DRdCompositorEffect::renderPreviewOnRenderThread( void ) {
	if ( m_pCoordinator ) {
		m_pCoordinator->renderPreviewOnRenderThread();
	}
}

void Terrain3DRdCompositorEffect::_render_callback( const int32_t nEffectCallbackType, RenderData *const pRenderData ) {
	if ( nEffectCallbackType != CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_OPAQUE || !m_pCoordinator ) {
		return;
	}
	m_pCoordinator->render( pRenderData );
}
