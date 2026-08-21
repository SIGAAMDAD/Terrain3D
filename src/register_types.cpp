// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef GDEXTENSION
#include <gdextension_interface.h>
#endif

#include <godot_cpp/core/class_db.hpp>

#include "register_types.h"
#include "terrain_3d.h"
#include "core/terrain_3d_diagnostics.h"
#include "authoring/terrain_3d_autoshader_baker.h"
#include "authoring/terrain_3d_editor.h"
#include "authoring/terrain_3d_erosion_job.h"
#include "authoring/terrain_3d_erosion_settings.h"
#include "authoring/terrain_3d_landform_baker.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "hlod/terrain_3d_hlod_builder.h"
#include "hlod/terrain_3d_hlod_manifest.h"
#include "hlod/terrain_3d_hlod_package.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "assets/terrain_3d_packed_pbr_texture.h"

void t3dInitializeTerrain3DModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
	ClassDB::register_class<T3DTerrain>();
	ClassDB::register_class<T3DMemoryStats>();
	ClassDB::register_class<T3DShaderTierStats>();
	ClassDB::register_class<T3DCacheStats>();
	ClassDB::register_class<T3DFrameStats>();
	ClassDB::register_class<T3DAutoShaderBaker>();
	ClassDB::register_class<T3DAssets>();
	ClassDB::register_class<T3DErosionSettings>();
	ClassDB::register_class<T3DData>();
	ClassDB::register_class<T3DErosionJob>();
	ClassDB::register_class<T3DEditor>();
	ClassDB::register_class<T3DLandformBaker>();
	ClassDB::register_class<T3DCollision>();
	ClassDB::register_class<T3DInstancer>();
	ClassDB::register_class<T3DLightmapStreamer>();
	ClassDB::register_class<T3DHLODBuilder>();
	ClassDB::register_class<T3DHLODPackage>();
	ClassDB::register_class<T3DHLODManifest>();
	ClassDB::register_class<T3DHLODStreamer>();
	ClassDB::register_class<T3DNodeStreamer>();
	ClassDB::register_class<T3DMaterial>();
	ClassDB::register_class<T3DMeshAsset>();
	ClassDB::register_class<T3DRegion>();
	ClassDB::register_class<T3DRegionLighting>();
	ClassDB::register_class<T3DPackedPBRTexture>();
	ClassDB::register_class<T3DTextureAsset>();
	ClassDB::register_class<T3DUtil>();
}

void t3dUninitializeTerrain3DModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
}

#ifdef GDEXTENSION
extern "C" {
// Initialization.
GDExtensionBool GDE_EXPORT t3dInit(
	GDExtensionInterfaceGetProcAddress getProcAddress,
	GDExtensionClassLibraryPtr library,
	GDExtensionInitialization *pInitialization
) {
	GDExtensionBinding::InitObject initObj( getProcAddress, library, pInitialization );

	initObj.register_initializer( t3dInitializeTerrain3DModule );
	initObj.register_terminator( t3dUninitializeTerrain3DModule );
	initObj.set_minimum_library_initialization_level( MODULE_INITIALIZATION_LEVEL_SERVERS );

	return initObj.init();
}
}
#endif /* GDEXTENSION */
