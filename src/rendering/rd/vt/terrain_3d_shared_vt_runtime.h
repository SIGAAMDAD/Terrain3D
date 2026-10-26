#ifndef TERRAIN3D_SHARED_VT_RUNTIME_H
#define TERRAIN3D_SHARED_VT_RUNTIME_H

#include "rendering/rd/vt/terrain_3d_vt_resources.h"
#include "rendering/rd/vt/terrain_3d_virtual_texture.h"
#include "rendering/rd/vt/terrain_3d_vt_shared_bindings.h"
#include "rendering/rd/world/terrain_3d_world_contract.h"

#include <godot_cpp/variant/callable.hpp>

class Terrain3DMemoryZone;

typedef enum t3dVtSharedResourceClass_e {
	T3D_VT_SHARED_RESOURCES_NONE = 0,
	T3D_VT_SHARED_RESOURCES_PREVIEW,
	T3D_VT_SHARED_RESOURCES_PRODUCTION,
} t3dVtSharedResourceClass_t;

// Shared VT ownership retained after removal of the World scene renderer.
class Terrain3DSharedVtRuntime {
public:
	Terrain3DSharedVtRuntime();
	~Terrain3DSharedVtRuntime();
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone );
	bool bindVirtualTexture( Terrain3DVirtualTexture *pVirtualTexture );
	bool prepare_shared_vt_cpu_cold( t3dVtSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_PRODUCTION,
	        uint32_t nPreviewMaterialCapacity = 0u, uint32_t nPreviewVirtualPageCapacity = 0u,
	        const t3dWorldResourceLimits_t *pProductionWorldLimits = NULL );
	bool prepare_shared_vt_storage_cold( Terrain3DRdContext &rContext );
	bool service_shared_vt_storage_cold( Terrain3DRdContext &rContext, uint32_t nMaxWorkUnits );
	bool bind_shared_vt_executable_cold();
	bool service_shared_vt_executable_cold( uint32_t nMaxWorkUnits );
	bool publish_shared_vt_cold();
	bool service_shared_vt_publication_cold( uint64_t nMaxBytes, uint32_t nMaxWorkUnits );
	bool ensure_shared_resources_cold( Terrain3DRdContext &rContext,
	        t3dVtSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_PRODUCTION,
	        uint32_t nPreviewMaterialCapacity = 0u, uint32_t nPreviewVirtualPageCapacity = 0u,
	        const t3dWorldResourceLimits_t *pProductionWorldLimits = NULL );
	void shutdown_cold();
	bool are_shared_resources_ready() const;
	t3dVtSharedResourceClass_t get_shared_resource_class() const;
	const T3DWorldLimits &get_limits() const;
	const Terrain3DVtResourceImage &get_resources() const;
	const T3DTerrainStageASharedVTBindings &get_shared_vt_bindings() const;
	bool snapshot_shared_vt_bindings_hot( T3DTerrainStageASharedVTBindings &rBindings ) const;

private:
	bool refresh_shared_vt_bindings_hot();
	Terrain3DVtResourceImage m_Resources;
	T3DTerrainStageASharedVTBindings m_SharedVTBindings = {};
	T3DWorldLimits m_Limits = {};
	T3DVtGpuProfile m_Profile = {};
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	Terrain3DVirtualTexture *m_pVirtualTexture = NULL;
	t3dVtSharedResourceClass_t m_eSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	t3dVtSharedResourceClass_t m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	bool m_bSharedResourcesInitialized = false;
};

#endif
