# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Region ownership plus baked-lighting/shadow semantics for streamed nodes.
@tool
extends EditorInspectorPlugin

const REGION_NODES_NAME: StringName = &"RegionNodes"
const REGION_CONTAINER_META: StringName = &"terrain3d_workbench_region_container"
const NODE_META_BOUNDARY_POLICY: StringName = &"terrain3d_boundary_policy"
const NODE_META_LIGHTING_CLASS: StringName = &"terrain3d_lighting_class"
const NODE_META_BAKED_LIGHTING_MODE: StringName = &"terrain3d_baked_lighting_mode"
const NODE_META_CONTRIBUTE_BAKED_GI: StringName = &"terrain3d_contribute_baked_gi"
const NODE_META_RECEIVE_DYNAMIC_DIRECTIONAL: StringName = &"terrain3d_receive_dynamic_directional"
const NODE_META_DYNAMIC_SHADOW_ELIGIBLE: StringName = &"terrain3d_dynamic_shadow_eligible"
const NODE_META_SHADOW_PROXY_SCENE_PATH: StringName = &"terrain3d_shadow_proxy_scene_path"

const BOUNDARY_POLICY_ANCHOR: int = 0
const BOUNDARY_POLICY_BOUNDS: int = 1
const LIGHTING_TERRAIN: int = 0
const LIGHTING_FOLIAGE: int = 1
const LIGHTING_MEDIUM: int = 2
const LIGHTING_LARGE: int = 3
const LIGHTING_REGIONAL: int = 4
const BAKED_DISABLED: int = 0
const BAKED_LIGHTMAP: int = 1
const BAKED_PROBE: int = 2
const BAKED_MATERIAL_ONLY: int = 3


func _can_handle(object: Object) -> bool:
	return is_instance_valid(_find_region_node_root(object as Node))


func _parse_begin(object: Object) -> void:
	var region_node: Node3D = _find_region_node_root(object as Node)
	if not is_instance_valid(region_node):
		return

	add_custom_control(_make_boundary_row(region_node))
	add_custom_control(_make_lighting_class_row(region_node))
	add_custom_control(_make_baked_mode_row(region_node))
	add_custom_control(_make_bool_row(
		"Contribute to baked GI", NODE_META_CONTRIBUTE_BAKED_GI, true, region_node,
		"Includes this object as static bounce/occlusion geometry during regional lightmap baking, independently of whether it receives a lightmap."))
	add_custom_control(_make_bool_row(
		"Receive directional", NODE_META_RECEIVE_DYNAMIC_DIRECTIONAL, true, region_node,
		"Whether this receiver is eligible for the project's runtime sun/moon lighting path. The semantic value is serialized independently of its lightmap."))
	add_custom_control(_make_bool_row(
		"Dynamic shadow eligible", NODE_META_DYNAMIC_SHADOW_ELIGIBLE, true, region_node,
		"Whether this object may enter the directional shadow map when its semantic class is enabled by the quality preset."))
	add_custom_control(_make_shadow_proxy_row(region_node))


func _make_boundary_row(region_node: Node3D) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	var label := _label("Region boundary",
		"Single ownership stays with this root. Bounds Coverage keeps the owner region resident when the node extends into neighboring regions.")
	row.add_child(label)
	var options := OptionButton.new()
	options.add_item("Bounds Coverage", BOUNDARY_POLICY_BOUNDS)
	options.add_item("Anchor Only", BOUNDARY_POLICY_ANCHOR)
	options.tooltip_text = "Bounds Coverage avoids boundary popping without duplicating the node. Anchor Only streams strictly from the root region."
	var policy: int = int(region_node.get_meta(NODE_META_BOUNDARY_POLICY, BOUNDARY_POLICY_BOUNDS))
	_select_id(options, policy)
	options.item_selected.connect(_on_int_meta_selected.bind(weakref(region_node), options, NODE_META_BOUNDARY_POLICY))
	row.add_child(options)
	return row


func _make_lighting_class_row(region_node: Node3D) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.add_child(_label("Lighting class",
		"Semantic world role used by the selective directional-shadow policy. It is independent of LightmapGI participation."))
	var options := OptionButton.new()
	options.add_item("Terrain", LIGHTING_TERRAIN)
	options.add_item("Foliage Clutter", LIGHTING_FOLIAGE)
	options.add_item("Medium Obstacle", LIGHTING_MEDIUM)
	options.add_item("Large Occluder", LIGHTING_LARGE)
	options.add_item("Regional Occluder", LIGHTING_REGIONAL)
	options.tooltip_text = "Semantic lighting/shadow class for this streamed object. The class selects the quality-policy shadow treatment independently of baked-lighting participation."
	_select_id(options, int(region_node.get_meta(NODE_META_LIGHTING_CLASS, LIGHTING_MEDIUM)))
	options.item_selected.connect(_on_int_meta_selected.bind(weakref(region_node), options, NODE_META_LIGHTING_CLASS))
	row.add_child(options)
	return row


func _make_baked_mode_row(region_node: Node3D) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.add_child(_label("Baked lighting",
		"Controls GI participation for the streamed receiver. This does not determine its dynamic-shadow class."))
	var options := OptionButton.new()
	options.add_item("Disabled", BAKED_DISABLED)
	options.add_item("Lightmap", BAKED_LIGHTMAP)
	options.add_item("Probe / Dynamic", BAKED_PROBE)
	options.add_item("Material Only", BAKED_MATERIAL_ONLY)
	options.tooltip_text = "Baked-lighting participation for this streamed object: disabled, lightmap, probe/dynamic, or material-only contribution."
	_select_id(options, int(region_node.get_meta(NODE_META_BAKED_LIGHTING_MODE, BAKED_LIGHTMAP)))
	options.item_selected.connect(_on_int_meta_selected.bind(weakref(region_node), options, NODE_META_BAKED_LIGHTING_MODE))
	row.add_child(options)
	return row


func _make_bool_row(text: String, meta_key: StringName, default_value: bool,
		region_node: Node3D, tooltip: String) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.add_child(_label(text, tooltip))
	var checkbox := CheckBox.new()
	checkbox.button_pressed = bool(region_node.get_meta(meta_key, default_value))
	checkbox.tooltip_text = tooltip
	checkbox.toggled.connect(_on_bool_meta_toggled.bind(weakref(region_node), meta_key))
	row.add_child(checkbox)
	return row


func _make_shadow_proxy_row(region_node: Node3D) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.add_child(_label("Shadow proxy",
		"Optional low-detail PackedScene streamed with this object. Geometry in the proxy is forced to Shadows Only."))
	var picker := EditorResourcePicker.new()
	picker.base_type = "PackedScene"
	picker.tooltip_text = "Optional low-detail PackedScene used as a shadow-only proxy while this streamed object is resident."
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var path: String = String(region_node.get_meta(NODE_META_SHADOW_PROXY_SCENE_PATH, ""))
	if not path.is_empty() and ResourceLoader.exists(path, "PackedScene"):
		picker.edited_resource = load(path)
	picker.resource_changed.connect(_on_shadow_proxy_changed.bind(weakref(region_node)))
	row.add_child(picker)
	return row


func _label(text: String, tooltip: String) -> Label:
	var label := Label.new()
	label.text = text
	label.tooltip_text = tooltip
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return label


func _select_id(options: OptionButton, id: int) -> void:
	for index: int in range(options.item_count):
		if options.get_item_id(index) == id:
			options.select(index)
			return
	options.select(0)


func _on_int_meta_selected(index: int, node_ref: WeakRef, options: OptionButton, meta_key: StringName) -> void:
	var region_node := node_ref.get_ref() as Node3D
	if not is_instance_valid(region_node):
		return
	region_node.set_meta(meta_key, options.get_item_id(index))
	EditorInterface.mark_scene_as_unsaved()


func _on_bool_meta_toggled(enabled: bool, node_ref: WeakRef, meta_key: StringName) -> void:
	var region_node := node_ref.get_ref() as Node3D
	if not is_instance_valid(region_node):
		return
	region_node.set_meta(meta_key, enabled)
	EditorInterface.mark_scene_as_unsaved()


func _on_shadow_proxy_changed(resource: Resource, node_ref: WeakRef) -> void:
	var region_node := node_ref.get_ref() as Node3D
	if not is_instance_valid(region_node):
		return
	var path: String = resource.resource_path if is_instance_valid(resource) else ""
	region_node.set_meta(NODE_META_SHADOW_PROXY_SCENE_PATH, path)
	EditorInterface.mark_scene_as_unsaved()


func _find_region_node_root(node: Node) -> Node3D:
	var current: Node = node
	while is_instance_valid(current) and is_instance_valid(current.get_parent()):
		var parent: Node = current.get_parent()
		# Multi-region workbenches place streamed roots one level below a region
		# ownership container. Version-2 workbenches remain supported directly.
		if bool(parent.get_meta(REGION_CONTAINER_META, false)):
			return current as Node3D
		if parent.name == REGION_NODES_NAME and parent is Node3D and \
			not bool(current.get_meta(REGION_CONTAINER_META, false)):
			return current as Node3D
		current = parent
	return null
