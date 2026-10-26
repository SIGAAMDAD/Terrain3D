// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_COMPOSITOR_EFFECT_H
#define TERRAIN3D_RD_COMPOSITOR_EFFECT_H

#include "rendering/rd/core/terrain_3d_render_coordinator_access.h"

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

class Terrain3DRdCompositorEffect : public godot::CompositorEffect
{
	GDCLASS( Terrain3DRdCompositorEffect, godot::CompositorEffect );

private:
	Terrain3DRenderCoordinatorAccess *m_pCoordinator = NULL;

protected:
	static void _bind_methods(
		void ) {
	}

public:
	Terrain3DRdCompositorEffect( void );
	void setCoordinator( Terrain3DRenderCoordinatorAccess *pCoordinator );
	void shutdownRendererOnRenderThread( void );
	void shutdownRendererLifecycleOnRenderThread( int64_t nShutdownGeneration );
	void renderPreviewOnRenderThread( void );
	void renderPreviewAsyncOnRenderThread( void );
	void servicePreviewVtRepublishOnRenderThread( void );
	void serviceShaderStartupOnRenderThread( int64_t nStartupGeneration );
	void recoverRendererLifecycleOnRenderThread( int64_t nStartupGeneration, int64_t nRecoveryRevision );
	void acknowledgeStartupCancellationOnRenderThread( int64_t nStartupGeneration );
	void serviceStartupWorldPublicationOnRenderThread( int64_t nStartupGeneration, int64_t nSnapshotGeneration );
	void consumeFragmentFeedbackOnRenderThread( const godot::PackedByteArray &rBytes );
	void consumeDescriptorFenceOnRenderThread( const godot::PackedByteArray &rBytes,
		int64_t nEpoch, int64_t nDeviceGeneration, int64_t nTicket );
	void consumePreviewReadbackOnRenderThread( const godot::PackedByteArray &rReadbackBytes,
											   int32_t						 nSlot,
											   int64_t						 nRequestGeneration );
	void _render_callback( int32_t nEffectCallbackType, godot::RenderData *pRenderData ) override;
};

#endif // TERRAIN3D_RD_COMPOSITOR_EFFECT_H
