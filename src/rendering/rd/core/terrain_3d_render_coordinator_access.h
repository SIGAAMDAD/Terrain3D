#ifndef TERRAIN3D_RENDER_COORDINATOR_ACCESS_H
#define TERRAIN3D_RENDER_COORDINATOR_ACCESS_H
#include <stdint.h>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
// Borrowed render-thread lifecycle endpoint. Attachment owns its lifetime.
class Terrain3DRenderCoordinatorAccess {
protected:
	~Terrain3DRenderCoordinatorAccess( void ) = default;
public:
	virtual void render( godot::RenderData *pRenderData ) = 0;
	virtual void shutdownRendererOnRenderThread( void ) = 0;
	virtual void shutdownRendererLifecycleOnRenderThread( uint32_t nGeneration ) = 0;
	virtual void recoverRendererLifecycleOnRenderThread( uint32_t nGeneration, uint64_t nRevision ) = 0;
	virtual void renderPreviewOnRenderThread( void ) = 0;
	virtual void renderPreviewAsyncOnRenderThread( void ) = 0;
	virtual void servicePreviewVtRepublishOnRenderThread( void ) = 0;
	virtual void serviceShaderStartupOnRenderThread( uint32_t nGeneration ) = 0;
	virtual void acknowledgeStartupCancellationOnRenderThread( uint32_t nGeneration ) = 0;
	virtual void serviceStartupWorldPublicationOnRenderThread( uint32_t nGeneration, uint64_t nSnapshotGeneration ) = 0;
	virtual void consumePreviewReadbackOnRenderThread( uint32_t nSlot, uint64_t nRequestGeneration,
	        const godot::PackedByteArray &rReadbackBytes ) = 0;
};
#endif // TERRAIN3D_RENDER_COORDINATOR_ACCESS_H
