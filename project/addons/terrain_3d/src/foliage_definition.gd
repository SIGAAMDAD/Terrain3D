@tool
class_name Terrain3DFoliageDefinition
extends Resource

@export var stable_uid: String = ""
@export var enabled: bool = true
@export var terrain_material: Terrain3DTextureAsset
@export var mesh_bank: Array[Terrain3DFoliageMeshRule] = []

func ensure_uid() -> void:
	if stable_uid.is_empty():
		stable_uid = Resource.generate_scene_unique_id()
