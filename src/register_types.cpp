// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef GDEXTENSION
#include <gdextension_interface.h>
#endif

#include "assets/resources/terrain_3d_packed_pbr_texture.h"
#include "assets/import/terrain_3d_world_import_profile.h"
#include "assets/resources/terrain_3d_world_material_resource.h"
#include "assets/resources/terrain_3d_rendering_material.h"
#include "authoring/world/core/terrain_3d_world_controller.h"
#include "instances/runtime/terrain_3d_world_instance_3d.h"
#include "authoring/baking/terrain_3d_autoshader_baker.h"
#include "authoring/editor/terrain_3d_editor.h"
#include "authoring/erosion/terrain_3d_erosion_job.h"
#include "authoring/erosion/terrain_3d_erosion_settings.h"
#include "authoring/baking/terrain_3d_landform_baker.h"
#include "diagnostics/core/terrain_3d_diagnostics.h"
#ifdef T3D_TESTS_ENABLED
#include "diagnostics/tests/unit_testing.h"
#endif
#include "hlod/build/terrain_3d_hlod_builder.h"
#include "hlod/format/terrain_3d_hlod_manifest.h"
#include "hlod/format/terrain_3d_hlod_package.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"
#include "rendering/rd/compositor/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"
#include "rendering/surface/terrain_3d_ocean_settings.h"
#include "register_types.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "terrain_3d.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <stdlib.h>


void t3dInitializeTerrain3DModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
	if ( !Terrain3DRdShaderLibrary::initializeSharedState() ) {
		UtilityFunctions::push_error( "[Terrain3D] Failed to initialize shared RD shader state" );
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
	ClassDB::register_class<Terrain3DOceanSettings>();
	ClassDB::register_class<Terrain3DMeshAsset>();
	ClassDB::register_class<Terrain3DRegion>();
	ClassDB::register_class<Terrain3DRegionLighting>();
	ClassDB::register_class<Terrain3DPackedPBRTexture>();
	ClassDB::register_class<Terrain3DTextureAsset>();
	ClassDB::register_class<Terrain3DUtil>();
#ifdef T3D_TESTS_ENABLED
	OS *const os = OS::get_singleton();
	// RenderingServer is available after module registration. The GPU runner
	// calls this test-only entry point on its render thread after engine startup.
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "run_vt_warm_gpu_tests" ), &t3dRunVTWarmGpuTests );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "run_centralization_tests" ), &t3dRunCentralizationTests );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "run_system_unit_tests", "suite_filter", "case_filter", "list_only" ), &t3dRunSystemUnitTests );
	if ( os && os->get_environment( "TERRAIN3D_RUN_VT_WARM_TESTS" ) == "1" && !t3dRunVTWarmRepublishTests() ) {
		exit( EXIT_FAILURE );
	}
	if ( os && os->get_environment( "TERRAIN3D_RUN_MEGAVT_CACHE_TESTS" ) == "1" && !t3dRunMegaVTCacheTests() ) {
		exit( EXIT_FAILURE );
	}
	if ( os && os->get_environment( "TERRAIN3D_RUN_RD_CPU_TESTS" ) == "1" && !t3dRunRdCpuTests() ) {
		exit( EXIT_FAILURE );
	}
#endif
}

void t3dUninitializeTerrain3DModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
	Terrain3DRdForwardPlusBridgeAdapter::shutdownSingleton();
	Terrain3DRdTextureManager::shutdownSingleton();
	Terrain3DRdShaderLibrary::shutdownSharedState();
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

	const GDExtensionBool bInitialized = initObj.init();
#ifdef T3D_DOC_DATA_ENABLED
	if ( bInitialized ) {
		t3dRegisterGeneratedDocData();
	}
#endif
	return bInitialized;
}
}
#endif /* GDEXTENSION */
