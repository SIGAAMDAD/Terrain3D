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

void initializeTerrain3dModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
	ClassDB::register_class<Terrain3D>();
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
}

void uninitializeTerrain3dModule( ModuleInitializationLevel level ) {
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE ) {
		return;
	}
}

#ifdef GDEXTENSION
extern "C" {
// Initialization.
GDExtensionBool GDE_EXPORT terrain3dInit(
	GDExtensionInterfaceGetProcAddress getProcAddress,
	GDExtensionClassLibraryPtr library,
	GDExtensionInitialization *pInitialization
) {
	GDExtensionBinding::InitObject initObj( getProcAddress, library, pInitialization );

	initObj.register_initializer( initializeTerrain3dModule );
	initObj.register_terminator( uninitializeTerrain3dModule );
	initObj.set_minimum_library_initialization_level( MODULE_INITIALIZATION_LEVEL_SERVERS );

	return initObj.init();
}
}
#endif /* GDEXTENSION */
