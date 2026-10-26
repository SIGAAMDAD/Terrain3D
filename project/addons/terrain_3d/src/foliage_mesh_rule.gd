@tool
class_name Terrain3DFoliageMeshRule
extends Resource

const GENERATOR_VERSION: int = 1
@export var stable_uid: String = "":
	set(value):
		stable_uid = value
		emit_changed()
@export var enabled: bool = true
@export var mesh_asset: Terrain3DMeshAsset
@export var excluded_material_ids: PackedInt32Array = PackedInt32Array()
@export_range(0.0, 100000.0, 0.01, "or_greater") var density_per_square_meter: float = 0.2
@export_range(0.0, 100.0, 0.01) var relative_weight: float = 1.0
@export_range(0.01, 100.0, 0.01) var min_scale: float = 0.8
@export_range(0.01, 100.0, 0.01) var max_scale: float = 1.2
@export var random_yaw: bool = true
@export_range(0.0, 90.0, 0.1) var min_slope: float = 0.0
@export_range(0.0, 90.0, 0.1) var max_slope: float = 50.0
@export var min_elevation: float = -100000.0
@export var max_elevation: float = 100000.0
@export_range(0.0, 1.0, 0.01) var normal_alignment: float = 0.15
@export_range(0.0, 50.0, 0.05) var minimum_spacing: float = 0.0
@export_range(0.0, 1.0, 0.01) var clustering_strength: float = 0.0
@export_range(0.25, 500.0, 0.25) var clustering_scale: float = 12.0

func ensure_uid() -> void:
	if stable_uid.is_empty():
		stable_uid = Resource.generate_scene_unique_id()

func validate() -> String:
	ensure_uid()
	if not is_instance_valid(mesh_asset):
		return "Mesh rule has no Terrain3DMeshAsset"
	if min_scale > max_scale or min_slope > max_slope or min_elevation > max_elevation:
		return "Invalid scale, slope or elevation interval"
	if density_per_square_meter < 0.0 or relative_weight < 0.0:
		return "Density and weight must be non-negative"
	return ""
