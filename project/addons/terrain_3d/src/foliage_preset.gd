@tool
class_name Terrain3DFoliagePreset
extends Resource

const SCHEMA_VERSION: int = 2
@export var stable_uid: String = "":
	set(value):
		stable_uid = value
		emit_changed()
@export_range(0.0, 32.0, 0.01) var global_density_multiplier: float = 1.0:
	set(value):
		global_density_multiplier = value
		emit_changed()
@export var random_seed: int = 43721:
	set(value):
		random_seed = value
		emit_changed()
@export_range(1, 1000000, 1) var region_instance_limit: int = 50000:
	set(value):
		region_instance_limit = value
		emit_changed()
@export_range(1, 2000000, 1) var job_instance_limit: int = 250000:
	set(value):
		job_instance_limit = value
		emit_changed()
@export_range(8, 512, 8) var scratch_budget_mb: int = 64:
	set(value):
		scratch_budget_mb = value
		emit_changed()
@export var definitions: Array[Terrain3DFoliageDefinition] = []:
	set(value):
		definitions = value
		emit_changed()
# Legacy import only. Bake ownership now lives beside the terrain data, never in
# the portable preset. Keep this property loadable for existing preset resources.
@export_storage var manifest: Terrain3DFoliageManifest

func ensure_uid() -> void:
	if stable_uid.is_empty():
		stable_uid = Resource.generate_scene_unique_id()
