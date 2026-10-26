@tool
class_name Terrain3DFoliageMeshRule
extends Resource

const GENERATOR_VERSION: int = 1
@export var stable_uid: String = "":
	set(value):
		stable_uid = value
		emit_changed()
@export var enabled: bool = true:
	set(value):
		enabled = value
		emit_changed()
@export var mesh_asset: Terrain3DMeshAsset:
	set(value):
		mesh_asset = value
		emit_changed()
@export var excluded_material_ids: PackedInt32Array = PackedInt32Array():
	set(value):
		excluded_material_ids = value
		emit_changed()
@export_range(0.0, 100000.0, 0.01, "or_greater") var density_per_square_meter: float = 0.2:
	set(value):
		density_per_square_meter = value
		emit_changed()
@export_range(0.0, 100.0, 0.01) var relative_weight: float = 1.0:
	set(value):
		relative_weight = value
		emit_changed()
@export_range(0.01, 100.0, 0.01) var min_scale: float = 0.8:
	set(value):
		min_scale = value
		emit_changed()
@export_range(0.01, 100.0, 0.01) var max_scale: float = 1.2:
	set(value):
		max_scale = value
		emit_changed()
@export var random_yaw: bool = true:
	set(value):
		random_yaw = value
		emit_changed()
@export_range(0.0, 90.0, 0.1) var min_slope: float = 0.0:
	set(value):
		min_slope = value
		emit_changed()
@export_range(0.0, 90.0, 0.1) var max_slope: float = 50.0:
	set(value):
		max_slope = value
		emit_changed()
@export var min_elevation: float = -100000.0:
	set(value):
		min_elevation = value
		emit_changed()
@export var max_elevation: float = 100000.0:
	set(value):
		max_elevation = value
		emit_changed()
@export_range(0.0, 1.0, 0.01) var normal_alignment: float = 0.15:
	set(value):
		normal_alignment = value
		emit_changed()
@export_range(0.0, 50.0, 0.05) var minimum_spacing: float = 0.0:
	set(value):
		minimum_spacing = value
		emit_changed()
@export_range(0.0, 1.0, 0.01) var clustering_strength: float = 0.0:
	set(value):
		clustering_strength = value
		emit_changed()
@export_range(0.25, 500.0, 0.25) var clustering_scale: float = 12.0:
	set(value):
		clustering_scale = value
		emit_changed()

func ensure_uid() -> void:
	if stable_uid.is_empty():
		stable_uid = Resource.generate_scene_unique_id()

func validate() -> String:
	if not is_finite(minimum_spacing) or minimum_spacing < 0.0 or minimum_spacing > 50.0:
		return "Spacing must be finite and between 0 and 50 metres."
	if not is_instance_valid(mesh_asset):
		return "Mesh rule has no Terrain3DMeshAsset"
	if min_scale > max_scale or min_slope > max_slope or min_elevation > max_elevation:
		return "Invalid scale, slope or elevation interval"
	if density_per_square_meter < 0.0 or relative_weight < 0.0:
		return "Density and weight must be non-negative"
	return ""

func validate_spacing(effective_density: float) -> String:
	if effective_density <= 0.0:
		return ""
	var cell: float = clampf(1.0 / sqrt(effective_density), 0.05, 128.0)
	if minimum_spacing > 0.0 and ceili(minimum_spacing / cell) + 1 > 32:
		return "Spacing is too large at this density; reduce spacing or density."
	return ""
