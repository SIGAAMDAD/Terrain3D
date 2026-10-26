@tool
class_name Terrain3DFoliagePreset
extends Resource

const SCHEMA_VERSION: int = 1
@export var stable_uid: String = ""
@export_range(0.0, 32.0, 0.01) var global_density_multiplier: float = 1.0
@export var random_seed: int = 43721
@export_range(1, 1000000, 1) var region_instance_limit: int = 50000
@export_range(1, 2000000, 1) var job_instance_limit: int = 250000
@export_range(8, 512, 8) var scratch_budget_mb: int = 64
@export var definitions: Array[Terrain3DFoliageDefinition] = []
@export var manifest: Terrain3DFoliageManifest

func ensure_uid() -> void:
	if stable_uid.is_empty():
		stable_uid = Resource.generate_scene_unique_id()
	if manifest == null:
		manifest = Terrain3DFoliageManifest.new()
