// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Native ClassDB registration for the runtime feature set.

#include "register_types.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "assets/import/terrain_3d_world_import_profile.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "assets/resources/terrain_3d_packed_pbr_texture.h"
#include "assets/resources/terrain_3d_rendering_material.h"
#include "assets/resources/terrain_3d_texture_asset.h"
#include "assets/resources/terrain_3d_world_material_resource.h"
#include "authoring/erosion/terrain_3d_erosion_settings.h"
#include "core/utility/terrain_3d_util.h"
#include "data/model/terrain_3d_data.h"
#include "data/model/terrain_3d_region.h"
#include "diagnostics/core/terrain_3d_diagnostics.h"
#include "hlod/format/terrain_3d_hlod_manifest.h"
#include "hlod/format/terrain_3d_hlod_package.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "instances/runtime/terrain_3d_world_instance_3d.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"
#include "physics/runtime/terrain_3d_collision.h"
#include "rendering/material/terrain_3d_material.h"
#include "rendering/rd/compositor/terrain_3d_rd_compositor_effect.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"
#include "rendering/surface/terrain_3d_ocean_settings.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "terrain_3d.h"

#include <godot_cpp/core/class_db.hpp>

void t3dRegisterRuntimeTypes(
	void ) {
	ClassDB::register_class< Terrain3D >();
	ClassDB::register_class< Terrain3DWorldMaterial >();
	ClassDB::register_class< Terrain3DRenderingMaterial >();
	ClassDB::register_class< Terrain3DWorldImportProfile >();
	ClassDB::register_class< Terrain3DWorldInstance3D >();
	ClassDB::register_class< Terrain3DRdCompositorEffect >();
	ClassDB::register_internal_class< Terrain3DRdTextureManager >();
	Terrain3DRdTextureManager::initializeSingleton();
	ClassDB::register_internal_class< Terrain3DRdForwardPlusBridgeAdapter >();
	Terrain3DRdForwardPlusBridgeAdapter::initializeSingleton();
	ClassDB::register_class< Terrain3DMemoryStats >();
	ClassDB::register_class< Terrain3DShaderTierStats >();
	ClassDB::register_class< Terrain3DCacheStats >();
	ClassDB::register_class< Terrain3DFrameStats >();
	ClassDB::register_class< Terrain3DAssets >();
	ClassDB::register_class< Terrain3DErosionSettings >();
	ClassDB::register_class< Terrain3DData >();
	ClassDB::register_class< Terrain3DCollision >();
	ClassDB::register_class< Terrain3DInstancer >();
	ClassDB::register_class< Terrain3DLightmapStreamer >();
	ClassDB::register_class< Terrain3DHLODPackage >();
	ClassDB::register_class< Terrain3DHLODManifest >();
	ClassDB::register_class< Terrain3DHLODStreamer >();
	ClassDB::register_class< Terrain3DNodeStreamer >();
	ClassDB::register_class< Terrain3DMaterial >();
	ClassDB::register_class< Terrain3DOceanSettings >();
	ClassDB::register_class< Terrain3DMeshAsset >();
	ClassDB::register_class< Terrain3DRegion >();
	ClassDB::register_class< Terrain3DRegionLighting >();
	ClassDB::register_class< Terrain3DPackedPBRTexture >();
	ClassDB::register_class< Terrain3DTextureAsset >();
	ClassDB::register_class< Terrain3DUtil >();
}
