# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Canonical metadata keys shared by Terrain3D region-workbench tooling.
@tool
extends RefCounted

const WORKBENCH_META: StringName = &"terrain3d_region_workbench"
const MANIFEST_META: StringName = &"terrain3d_workbench_manifest"
const REGION_NODES_NAME: StringName = &"RegionNodes"
const REGION_NODES_LOADED_META: StringName = &"terrain3d_region_nodes_loaded"
const REGION_CONTAINER_META: StringName = &"terrain3d_workbench_region_container"
const REGION_LOCATION_META: StringName = &"terrain3d_workbench_region_location"
const NODE_META_ID: StringName = &"terrain3d_stream_id"
const NODE_META_FLAGS: StringName = &"terrain3d_stream_flags"
const NODE_META_STATE: StringName = &"terrain3d_stream_state"
const NODE_META_SOURCE_PATH: StringName = &"terrain3d_stream_source_path"
const NODE_META_LOAD_ERROR: StringName = &"terrain3d_stream_load_error"
const NODE_META_BOUNDARY_POLICY: StringName = &"terrain3d_boundary_policy"
const NODE_META_COVERAGE_MIN: StringName = &"terrain3d_coverage_min"
const NODE_META_COVERAGE_MAX: StringName = &"terrain3d_coverage_max"
const NODE_META_LIGHTING_CLASS: StringName = &"terrain3d_lighting_class"
const NODE_META_BAKED_LIGHTING_MODE: StringName = &"terrain3d_baked_lighting_mode"
const NODE_META_CONTRIBUTE_BAKED_GI: StringName = &"terrain3d_contribute_baked_gi"
const NODE_META_RECEIVE_DYNAMIC_DIRECTIONAL: StringName = &"terrain3d_receive_dynamic_directional"
const NODE_META_DYNAMIC_SHADOW_ELIGIBLE: StringName = &"terrain3d_dynamic_shadow_eligible"
const NODE_META_SHADOW_PROXY_SCENE_PATH: StringName = &"terrain3d_shadow_proxy_scene_path"

const BOUNDARY_POLICY_ANCHOR: int = 0
const BOUNDARY_POLICY_BOUNDS: int = 1

static func append_checkout_state(
		result: Dictionary[String, Variant],
		last_archive_directory: String,
		last_region_node_directory: String,
		streamed_node_count: int,
		committed_unix: int) -> void:
	if not last_archive_directory.is_empty():
		result["last_archive_directory"] = last_archive_directory
	if not last_region_node_directory.is_empty():
		result["last_region_node_directory"] = last_region_node_directory
	if streamed_node_count != 0:
		result["streamed_node_count"] = streamed_node_count
	if committed_unix != 0:
		result["committed_unix"] = committed_unix

