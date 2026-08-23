// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_COMPOSITOR_EFFECT_H
#define TERRAIN3D_RD_COMPOSITOR_EFFECT_H

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>

class Terrain3DRenderCoordinator;

class Terrain3DRdCompositorEffect : public godot::CompositorEffect {
	GDCLASS( Terrain3DRdCompositorEffect, godot::CompositorEffect );

private:
	Terrain3DRenderCoordinator *m_pCoordinator = nullptr;

protected:
	static void _bind_methods( void ) {}

public:
	Terrain3DRdCompositorEffect( void );
	void setCoordinator( Terrain3DRenderCoordinator *pCoordinator );
	void shutdownRendererOnRenderThread( void );
	void renderPreviewOnRenderThread( void );
	void _render_callback( int32_t nEffectCallbackType, godot::RenderData *pRenderData ) override;
};

#endif // TERRAIN3D_RD_COMPOSITOR_EFFECT_H
