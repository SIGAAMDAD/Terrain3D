# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Serializable configuration and bake-state record for the editor autoshader.
@tool
class_name T3DAutoShaderProfile
extends Resource


const AutoShaderRule = preload("res://addons/terrain_3d/src/autoshader_rule.gd")
const BAKE_VERSION: int = 2

enum Mode {
	# Serialized values 0-2 are retained for compatibility.
	LEGACY_RUNTIME,
	BAKED,
	DISABLED,
	EDITOR_PREVIEW,
}

enum MaskChannel {
	RED,
	GREEN,
	BLUE,
	ALPHA,
}

@export_category("Workflow")
@export_enum("Legacy Runtime", "Baked Production", "Manual / Disabled", "Live Editor Preview") var mode: int = Mode.BAKED:
	set(value):
		if mode == value:
			return
		mode = value
		_notify_changed()
@export var auto_rebake_after_sculpt: bool = false:
	set(value):
		if auto_rebake_after_sculpt == value:
			return
		auto_rebake_after_sculpt = value
		_notify_changed()
@export_range(0.05, 5.0, 0.05, "suffix:s") var auto_rebake_delay: float = 0.35:
	set(value):
		if auto_rebake_delay == value:
			return
		auto_rebake_delay = value
		_notify_changed()
@export var only_auto_pixels: bool = true:
	set(value):
		if only_auto_pixels == value:
			return
		only_auto_pixels = value
		_notify_changed()
@export var preserve_auto_mask: bool = true:
	set(value):
		if preserve_auto_mask == value:
			return
		preserve_auto_mask = value
		_notify_changed()

@export_category("Fallback Material")
@export_range(0, 31, 1) var default_base_texture: int = 0:
	set(value):
		if default_base_texture == value:
			return
		default_base_texture = value
		_notify_changed()
@export_range(0, 31, 1) var default_overlay_texture: int = 0:
	set(value):
		if default_overlay_texture == value:
			return
		default_overlay_texture = value
		_notify_changed()
@export_range(0.0, 1.0, 0.001) var default_blend: float = 0.0:
	set(value):
		if default_blend == value:
			return
		default_blend = value
		_notify_changed()

@export_category("Rules")
@export var rules: Array[T3DAutoShaderRule] = []:
	set(value):
		# Array edits from the Inspector can reuse the same container instance.
		# Always refresh nested-resource ownership when the property setter runs.
		rules = value
		sync_dependency_connections()
		_notify_changed()

@export_category("Derived Metrics")
@export_range(0.001, 10000.0, 0.001, "or_greater") var curvature_scale: float = 4.0:
	set(value):
		if curvature_scale == value:
			return
		curvature_scale = value
		_notify_changed()
@export_range(-100000.0, 100000.0, 0.1, "or_less", "or_greater", "suffix:m") var water_level: float = 0.0:
	set(value):
		if water_level == value:
			return
		water_level = value
		_notify_changed()

@export_category("Noise")
@export var noise_seed: int = 1337:
	set(value):
		if noise_seed == value:
			return
		noise_seed = value
		_notify_changed()
@export_range(0.000001, 10.0, 0.000001, "or_greater") var noise_scale: float = 0.01:
	set(value):
		if noise_scale == value:
			return
		noise_scale = value
		_notify_changed()
@export_range(1, 8, 1) var noise_octaves: int = 4:
	set(value):
		if noise_octaves == value:
			return
		noise_octaves = value
		_notify_changed()
@export_range(1.0, 8.0, 0.01) var noise_lacunarity: float = 2.0:
	set(value):
		if noise_lacunarity == value:
			return
		noise_lacunarity = value
		_notify_changed()
@export_range(0.0, 1.0, 0.001) var noise_gain: float = 0.5:
	set(value):
		if noise_gain == value:
			return
		noise_gain = value
		_notify_changed()

@export_category("Biome Mask")
@export var biome_mask: Texture2D:
	set(value):
		if biome_mask == value:
			return
		biome_mask = value
		sync_dependency_connections()
		_notify_changed()
@export var biome_mask_world_rect: Rect2 = Rect2(Vector2.ZERO, Vector2(1.0, 1.0)):
	set(value):
		if biome_mask_world_rect == value:
			return
		biome_mask_world_rect = value
		_notify_changed()
@export_enum("Red", "Green", "Blue", "Alpha") var biome_mask_channel: int = MaskChannel.RED:
	set(value):
		if biome_mask_channel == value:
			return
		biome_mask_channel = value
		_notify_changed()
@export var biome_mask_invert: bool = false:
	set(value):
		if biome_mask_invert == value:
			return
		biome_mask_invert = value
		_notify_changed()

@export_category("Flow Override")
@export var flow_mask: Texture2D:
	set(value):
		if flow_mask == value:
			return
		flow_mask = value
		sync_dependency_connections()
		_notify_changed()
@export var flow_mask_world_rect: Rect2 = Rect2(Vector2.ZERO, Vector2(1.0, 1.0)):
	set(value):
		if flow_mask_world_rect == value:
			return
		flow_mask_world_rect = value
		_notify_changed()
@export_enum("Red", "Green", "Blue", "Alpha") var flow_mask_channel: int = MaskChannel.RED:
	set(value):
		if flow_mask_channel == value:
			return
		flow_mask_channel = value
		_notify_changed()
@export var flow_mask_invert: bool = false:
	set(value):
		if flow_mask_invert == value:
			return
		flow_mask_invert = value
		_notify_changed()

@export_category("Deposition Override")
@export var deposition_mask: Texture2D:
	set(value):
		if deposition_mask == value:
			return
		deposition_mask = value
		sync_dependency_connections()
		_notify_changed()
@export var deposition_mask_world_rect: Rect2 = Rect2(Vector2.ZERO, Vector2(1.0, 1.0)):
	set(value):
		if deposition_mask_world_rect == value:
			return
		deposition_mask_world_rect = value
		_notify_changed()
@export_enum("Red", "Green", "Blue", "Alpha") var deposition_mask_channel: int = MaskChannel.RED:
	set(value):
		if deposition_mask_channel == value:
			return
		deposition_mask_channel = value
		_notify_changed()
@export var deposition_mask_invert: bool = false:
	set(value):
		if deposition_mask_invert == value:
			return
		deposition_mask_invert = value
		_notify_changed()

@export_category("Water Mask")
@export var water_mask: Texture2D:
	set(value):
		if water_mask == value:
			return
		water_mask = value
		sync_dependency_connections()
		_notify_changed()
@export var water_mask_world_rect: Rect2 = Rect2(Vector2.ZERO, Vector2(1.0, 1.0)):
	set(value):
		if water_mask_world_rect == value:
			return
		water_mask_world_rect = value
		_notify_changed()
@export_enum("Red", "Green", "Blue", "Alpha") var water_mask_channel: int = MaskChannel.RED:
	set(value):
		if water_mask_channel == value:
			return
		water_mask_channel = value
		_notify_changed()
@export var water_mask_invert: bool = false:
	set(value):
		if water_mask_invert == value:
			return
		water_mask_invert = value
		_notify_changed()

# Region location string -> height/profile signature. This is editor provenance,
# not runtime terrain data.
@export_storage var baked_region_signatures: Dictionary[String, int] = {}
@export_storage var baked_version: int = BAKE_VERSION

var _connected_rules: Array[T3DAutoShaderRule] = []
var _connected_dependencies: Array[Resource] = []
var _change_batch_depth: int = 0
var _change_pending: bool = false


func begin_change_batch() -> void:
	_change_batch_depth += 1


func end_change_batch() -> void:
	if _change_batch_depth <= 0:
		return
	_change_batch_depth -= 1
	if _change_batch_depth == 0 and _change_pending:
		_change_pending = false
		emit_changed()


func _notify_changed() -> void:
	if _change_batch_depth > 0:
		_change_pending = true
		return
	emit_changed()


func sync_rule_connections() -> void:
	for rule: T3DAutoShaderRule in _connected_rules:
		if is_instance_valid(rule) and rule.changed.is_connected(_on_rule_resource_changed):
			rule.changed.disconnect(_on_rule_resource_changed)
	_connected_rules.clear()
	for rule: T3DAutoShaderRule in rules:
		if not is_instance_valid(rule):
			continue
		rule.sync_subresource_connections()
		if not rule.changed.is_connected(_on_rule_resource_changed):
			rule.changed.connect(_on_rule_resource_changed)
		_connected_rules.append(rule)


func sync_dependency_connections() -> void:
	sync_rule_connections()
	for dependency: Resource in _connected_dependencies:
		if is_instance_valid(dependency) and dependency.changed.is_connected(_on_dependency_resource_changed):
			dependency.changed.disconnect(_on_dependency_resource_changed)
	_connected_dependencies.clear()
	var dependencies: Array[Resource] = []
	for texture: Texture2D in [biome_mask, flow_mask, deposition_mask, water_mask]:
		if is_instance_valid(texture):
			dependencies.append(texture)
	for dependency: Resource in dependencies:
		if not dependency.changed.is_connected(_on_dependency_resource_changed):
			dependency.changed.connect(_on_dependency_resource_changed)
		_connected_dependencies.append(dependency)


func add_rule(rule: T3DAutoShaderRule) -> void:
	if not is_instance_valid(rule):
		return
	rules.append(rule)
	sync_rule_connections()
	_notify_changed()


func remove_rule_at(index: int) -> void:
	if index < 0 or index >= rules.size():
		return
	rules.remove_at(index)
	sync_rule_connections()
	_notify_changed()


func _on_rule_resource_changed() -> void:
	_notify_changed()


func _on_dependency_resource_changed() -> void:
	_notify_changed()


func ensure_default_rules() -> void:
	if not rules.is_empty():
		return
	var steep := AutoShaderRule.new() as T3DAutoShaderRule
	steep.rule_name = "Steep exposed rock"
	steep.priority = 300
	steep.use_slope = true
	steep.slope_min = 42.0
	steep.slope_max = 90.0
	steep.base_texture = 0
	steep.overlay_texture = 1
	steep.blend = 1.0
	steep.slope_falloff = 6.0

	var channels := AutoShaderRule.new() as T3DAutoShaderRule
	channels.rule_name = "Depositional channels"
	channels.priority = 200
	channels.use_deposition = true
	channels.deposition_min = 0.25
	channels.deposition_max = 1.0
	channels.base_texture = 0
	channels.overlay_texture = 2
	channels.blend = 0.65
	channels.deposition_falloff = 0.1

	var ground := AutoShaderRule.new() as T3DAutoShaderRule
	ground.rule_name = "Ground fallback"
	ground.priority = 0
	ground.base_texture = default_base_texture
	ground.overlay_texture = default_overlay_texture
	ground.blend = default_blend
	rules.assign([steep, channels, ground])
	sync_dependency_connections()
	_notify_changed()


func get_profile_signature() -> int:
	var signature: int = hash([
		BAKE_VERSION,
		only_auto_pixels,
		preserve_auto_mask,
		default_base_texture,
		default_overlay_texture,
		default_blend,
		curvature_scale,
		water_level,
		noise_seed,
		noise_scale,
		noise_octaves,
		noise_lacunarity,
		noise_gain,
		_texture_signature(biome_mask),
		biome_mask_world_rect,
		int(biome_mask_channel),
		biome_mask_invert,
		_texture_signature(flow_mask),
		flow_mask_world_rect,
		int(flow_mask_channel),
		flow_mask_invert,
		_texture_signature(deposition_mask),
		deposition_mask_world_rect,
		int(deposition_mask_channel),
		deposition_mask_invert,
		_texture_signature(water_mask),
		water_mask_world_rect,
		int(water_mask_channel),
		water_mask_invert,
	])
	for rule: T3DAutoShaderRule in rules:
		if is_instance_valid(rule):
			signature = hash([signature, hash(rule.to_bake_dictionary())])
	return signature


func to_bake_dictionary() -> Dictionary[String, Variant]:
	var rule_data: Array[Variant] = []
	for rule: T3DAutoShaderRule in rules:
		if is_instance_valid(rule):
			rule_data.append(rule.to_bake_dictionary())
	return {
		"rules": rule_data,
		"only_auto_pixels": only_auto_pixels,
		"preserve_auto_mask": preserve_auto_mask,
		"default_base_texture": default_base_texture,
		"default_overlay_texture": default_overlay_texture,
		"default_blend": default_blend,
		"curvature_scale": curvature_scale,
		"water_level": water_level,
		"noise_seed": noise_seed,
		"noise_scale": noise_scale,
		"noise_octaves": noise_octaves,
		"noise_lacunarity": noise_lacunarity,
		"noise_gain": noise_gain,
		"biome_mask_image": _texture_image(biome_mask),
		"biome_mask_world_rect": biome_mask_world_rect,
		"biome_mask_channel": int(biome_mask_channel),
		"biome_mask_invert": biome_mask_invert,
		"flow_mask_image": _texture_image(flow_mask),
		"flow_mask_world_rect": flow_mask_world_rect,
		"flow_mask_channel": int(flow_mask_channel),
		"flow_mask_invert": flow_mask_invert,
		"deposition_mask_image": _texture_image(deposition_mask),
		"deposition_mask_world_rect": deposition_mask_world_rect,
		"deposition_mask_channel": int(deposition_mask_channel),
		"deposition_mask_invert": deposition_mask_invert,
		"water_mask_image": _texture_image(water_mask),
		"water_mask_world_rect": water_mask_world_rect,
		"water_mask_channel": int(water_mask_channel),
		"water_mask_invert": water_mask_invert,
		"profile_signature": get_profile_signature(),
	}


func get_mode_name() -> String:
	match mode:
		Mode.EDITOR_PREVIEW:
			return "Live Editor Preview"
		Mode.BAKED:
			return "Baked Production"
		Mode.LEGACY_RUNTIME:
			return "Legacy Runtime"
		Mode.DISABLED:
			return "Manual / Disabled"
	return "Unknown"

func set_bake_signatures(signatures: Dictionary[String, int]) -> void:
	for location: String in signatures:
		baked_region_signatures[location] = signatures[location]
	baked_version = BAKE_VERSION
	_notify_changed()


func clear_bake_signatures(locations: Array[Vector2i] = []) -> void:
	if locations.is_empty():
		baked_region_signatures.clear()
	else:
		for location: Vector2i in locations:
			baked_region_signatures.erase(T3DUtil.location_to_filename(location).trim_prefix("terrain3d").trim_suffix(".res"))
	_notify_changed()


func _texture_signature(texture: Texture2D) -> int:
	if not is_instance_valid(texture):
		return 0
	var image := texture.get_image()
	if not is_instance_valid(image) or image.is_empty():
		return hash([texture.resource_path, Vector2i.ZERO, -1])
	return hash([texture.resource_path, image.get_size(), image.get_format(), image.get_data()])


func _texture_image(texture: Texture2D) -> Image:
	if not is_instance_valid(texture):
		return null
	return texture.get_image()
