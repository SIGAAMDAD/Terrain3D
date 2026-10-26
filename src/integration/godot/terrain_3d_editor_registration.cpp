// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Native ClassDB registration for the editor feature set.

#include "register_types.h"
#include "authoring/baking/terrain_3d_autoshader_baker.h"
#include "authoring/baking/terrain_3d_landform_baker.h"
#include "authoring/editor/terrain_3d_editor.h"
#include "authoring/erosion/terrain_3d_erosion_job.h"
#include "authoring/foliage/terrain_3d_foliage_generator.h"
#include "authoring/world/core/terrain_3d_world_controller.h"
#include "hlod/build/terrain_3d_hlod_builder.h"

#include <godot_cpp/core/class_db.hpp>

#if defined( T3D_EDITOR_NATIVE )
void t3dRegisterEditorTypes(
	void ) {
	ClassDB::register_class< Terrain3DAutoShaderBaker >();
	ClassDB::register_class< Terrain3DFoliageGenerator >();
	ClassDB::register_class< Terrain3DErosionJob >();
	ClassDB::register_class< Terrain3DEditor >();
	ClassDB::register_class< Terrain3DLandformBaker >();
	ClassDB::register_class< Terrain3DHLODBuilder >();
	ClassDB::register_class< Terrain3DWorldController >();
}
#endif // T3D_EDITOR_NATIVE
