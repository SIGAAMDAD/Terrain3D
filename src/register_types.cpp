// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef GDEXTENSION
#include <gdextension_interface.h>
#endif

#include "register_types.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"
#ifdef T3D_TESTS_ENABLED
#include "diagnostics/tests/unit_testing.h"
#endif

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <stdlib.h>

void t3dInitializeTerrain3DModule(
	ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
	UtilityFunctions::print( "[Terrain3D][NativeDLL] selected=", t3dGetNativeBuildFlavor() );
	if ( !Terrain3DRdShaderLibrary::initializeSharedState() ) {
		UtilityFunctions::push_error( "[Terrain3D] Failed to initialize shared RD shader state" );
		return;
	}
	t3dRegisterRuntimeTypes();
#if defined( T3D_EDITOR_NATIVE )
	t3dRegisterEditorTypes();
#endif
#ifdef T3D_TESTS_ENABLED
	OS *const os = OS::get_singleton();
	// RenderingServer is available after module registration. The GPU runner
	// calls this test-only entry point on its render thread after engine startup.
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "run_vt_warm_gpu_tests" ), &t3dRunVTWarmGpuTests );
	ClassDB::bind_static_method( "Terrain3DUtil", D_METHOD( "run_centralization_tests" ), &t3dRunCentralizationTests );
	ClassDB::bind_static_method( "Terrain3DUtil",
								 D_METHOD( "run_system_unit_tests", "suite_filter", "case_filter", "list_only" ),
								 &t3dRunSystemUnitTests );
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

void t3dUninitializeTerrain3DModule(
	ModuleInitializationLevel level ) {
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
GDExtensionBool GDE_EXPORT t3dInit(
	GDExtensionInterfaceGetProcAddress getProcAddress,
	GDExtensionClassLibraryPtr		   library,
	GDExtensionInitialization		  *pInitialization ) {
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
