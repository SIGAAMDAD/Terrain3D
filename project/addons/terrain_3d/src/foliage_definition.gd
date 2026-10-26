@tool
class_name Terrain3DFoliageDefinition
extends Resource

@export var stable_uid: String = "":
	set(value):
		stable_uid = value
		emit_changed()
@export var enabled: bool = true:
	set(value):
		enabled = value
		emit_changed()
@export var terrain_material: Terrain3DTextureAsset:
	set(value):
		terrain_material = value
		emit_changed()
@export var mesh_bank: Array[Terrain3DFoliageMeshRule] = []:
	set(value):
		mesh_bank = value
		emit_changed()

func ensure_uid() -> void:
	if stable_uid.is_empty():
		stable_uid = Resource.generate_scene_unique_id()
