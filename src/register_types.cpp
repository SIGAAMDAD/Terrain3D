// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef GDEXTENSION
#include <gdextension_interface.h>
#endif

#include "assets/terrain_3d_packed_pbr_texture.h"
#include "assets/terrain_3d_world_import_profile.h"
#include "assets/terrain_3d_world_material_resource.h"
#include "assets/terrain_3d_rendering_material.h"
#include "authoring/world/terrain_3d_world_controller.h"
#include "instances/terrain_3d_world_instance_3d.h"
#include "authoring/terrain_3d_autoshader_baker.h"
#include "authoring/terrain_3d_editor.h"
#include "authoring/terrain_3d_erosion_job.h"
#include "authoring/terrain_3d_erosion_settings.h"
#include "authoring/terrain_3d_landform_baker.h"
#include "diagnostics/terrain_3d_diagnostics.h"
#include "diagnostics/unit_testing.h"
#include "hlod/terrain_3d_hlod_builder.h"
#include "hlod/terrain_3d_hlod_manifest.h"
#include "hlod/terrain_3d_hlod_package.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "rendering/rd/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_forward_plus_bridge.h"
#include "rendering/rd/terrain_3d_rd_texture_manager.h"
#include "register_types.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "terrain_3d.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/os.hpp>

#include <stdlib.h>

void t3dInitializeTerrain3DModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
	ClassDB::register_class<Terrain3D>();
	ClassDB::register_class<Terrain3DWorldMaterial>();
	ClassDB::register_class<Terrain3DRenderingMaterial>();
	ClassDB::register_class<Terrain3DWorldImportProfile>();
	ClassDB::register_class<Terrain3DWorldInstance3D>();
	ClassDB::register_class<Terrain3DWorldController>();
	ClassDB::register_class<Terrain3DRdCompositorEffect>();
	ClassDB::register_internal_class<Terrain3DRdTextureManager>();
	Terrain3DRdTextureManager::initializeSingleton();
	ClassDB::register_internal_class<Terrain3DRdForwardPlusBridgeAdapter>();
	Terrain3DRdForwardPlusBridgeAdapter::initializeSingleton();
	ClassDB::register_class<Terrain3DMemoryStats>();
	ClassDB::register_class<Terrain3DShaderTierStats>();
	ClassDB::register_class<Terrain3DCacheStats>();
	ClassDB::register_class<Terrain3DFrameStats>();
	ClassDB::register_class<Terrain3DAutoShaderBaker>();
	ClassDB::register_class<Terrain3DAssets>();
	ClassDB::register_class<Terrain3DErosionSettings>();
	ClassDB::register_class<Terrain3DData>();
	ClassDB::register_class<Terrain3DErosionJob>();
	ClassDB::register_class<Terrain3DEditor>();
	ClassDB::register_class<Terrain3DLandformBaker>();
	ClassDB::register_class<Terrain3DCollision>();
	ClassDB::register_class<Terrain3DInstancer>();
	ClassDB::register_class<Terrain3DLightmapStreamer>();
	ClassDB::register_class<Terrain3DHLODBuilder>();
	ClassDB::register_class<Terrain3DHLODPackage>();
	ClassDB::register_class<Terrain3DHLODManifest>();
	ClassDB::register_class<Terrain3DHLODStreamer>();
	ClassDB::register_class<Terrain3DNodeStreamer>();
	ClassDB::register_class<Terrain3DMaterial>();
	ClassDB::register_class<Terrain3DMeshAsset>();
	ClassDB::register_class<Terrain3DRegion>();
	ClassDB::register_class<Terrain3DRegionLighting>();
	ClassDB::register_class<Terrain3DPackedPBRTexture>();
	ClassDB::register_class<Terrain3DTextureAsset>();
	ClassDB::register_class<Terrain3DUtil>();
	OS *const os = OS::get_singleton();
	if ( os && os->get_environment( "TERRAIN3D_RUN_RD_CPU_TESTS" ) == "1" && !t3dRunRdCpuTests() ) {
		exit( EXIT_FAILURE );
	}
}

void t3dUninitializeTerrain3DModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
	Terrain3DRdForwardPlusBridgeAdapter::shutdownSingleton();
	Terrain3DRdTextureManager::shutdownSingleton();
	Terrain3DRdContext::shutdownShared();
}

#ifdef GDEXTENSION
extern "C" {
// Initialization.
GDExtensionBool GDE_EXPORT t3dInit( GDExtensionInterfaceGetProcAddress getProcAddress,
        GDExtensionClassLibraryPtr library,
        GDExtensionInitialization *pInitialization ) {
	GDExtensionBinding::InitObject initObj( getProcAddress, library, pInitialization );

	initObj.register_initializer( t3dInitializeTerrain3DModule );
	initObj.register_terminator( t3dUninitializeTerrain3DModule );
	initObj.set_minimum_library_initialization_level( MODULE_INITIALIZATION_LEVEL_SERVERS );

	return initObj.init();
}
}
#endif /* GDEXTENSION */
