#ifndef TERRAIN3D_SHARED_VT_RUNTIME_H
#define TERRAIN3D_SHARED_VT_RUNTIME_H

#include "Terrain3DTypes.h"

#include "rendering/rd/vt/gpu/terrain_3d_vt_resources.h"
#include "rendering/rd/vt/runtime/terrain_3d_virtual_texture.h"
#include "rendering/rd/vt/gpu/terrain_3d_vt_shared_bindings.h"
#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <godot_cpp/variant/callable.hpp>
#include "memory/arena/terrain_3d_memory_zone.h"


// t3dVTSharedResourceClass_e centralized in Terrain3DTypes.h.


// Shared VT ownership retained after removal of the World scene renderer.
class Terrain3DSharedVTRuntime {
public:
	Terrain3DSharedVTRuntime();
	~Terrain3DSharedVTRuntime();
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone );
	bool bindVirtualTexture( Terrain3DVirtualTexture *pVirtualTexture );
	bool prepare_shared_vt_cpu_cold( t3dVTSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_PRODUCTION,
	        uint32_t nPreviewMaterialCapacity = 0u, uint32_t nPreviewVirtualPageCapacity = 0u,
	        const t3dWorldResourceLimits_t *pProductionWorldLimits = NULL );
	bool prepare_shared_vt_storage_cold( Terrain3DRdContext &rContext );
	bool service_shared_vt_storage_cold( Terrain3DRdContext &rContext, uint32_t nMaxWorkUnits );
	bool bind_shared_vt_executable_cold();
	bool service_shared_vt_executable_cold( uint32_t nMaxWorkUnits );
	bool publish_shared_vt_cold();
	bool service_shared_vt_publication_cold( uint64_t nMaxBytes, uint32_t nMaxWorkUnits );
	bool ensure_shared_resources_cold( Terrain3DRdContext &rContext,
	        t3dVTSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_PRODUCTION,
	        uint32_t nPreviewMaterialCapacity = 0u, uint32_t nPreviewVirtualPageCapacity = 0u,
	        const t3dWorldResourceLimits_t *pProductionWorldLimits = NULL );
	void shutdown_cold( bool bPreserveTailStaging = false );
	bool are_shared_resources_ready() const;
	t3dVTSharedResourceClass_t get_shared_resource_class() const;
	const t3dWorldLimits_t &get_limits() const;
	const Terrain3DVTResourceImage &get_resources() const;
	const t3dSharedVTBindings_t &get_shared_vt_bindings() const;
	bool snapshot_shared_vt_bindings_hot( t3dSharedVTBindings_t &rBindings ) const;

private:
	bool refresh_shared_vt_bindings_hot();
	Terrain3DVTResourceImage m_Resources;
	t3dSharedVTBindings_t m_SharedVTBindings = {};
	t3dWorldLimits_t m_Limits = {};
	t3dVTGpuProfile_t m_Profile = {};
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	Terrain3DVirtualTexture *m_pVirtualTexture = NULL;
	t3dVTSharedResourceClass_t m_eSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	t3dVTSharedResourceClass_t m_ePreparingSharedResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	bool m_bSharedResourcesInitialized = false;
};

#endif // TERRAIN3D_SHARED_VT_RUNTIME_H
