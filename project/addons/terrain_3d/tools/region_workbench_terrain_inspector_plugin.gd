@tool
extends EditorInspectorPlugin

const WORKBENCH_META: StringName = &"terrain3d_region_workbench"


func _can_handle(object: Object) -> bool:
	return object is Terrain3D and _in_workbench()


func _parse_begin(_object: Object) -> void:
	var notice := Label.new()
	notice.text = "Region Workbench terrain configuration is locked. Global Terrain3D resources and rendering/profile settings belong to the source terrain; edit terrain maps with the Terrain3D tools and streamed objects under RegionNodes."
	notice.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	notice.add_theme_color_override("font_color", Color(0.95, 0.78, 0.32))
	add_custom_control(notice)


func _parse_property(_object: Object, _type: Variant.Type, _name: String, _hint_type: PropertyHint,
		_hint_string: String, _usage_flags: int, _wide: bool) -> bool:
	return true


func _in_workbench() -> bool:
	var root: Node = EditorInterface.get_edited_scene_root()
	return is_instance_valid(root) and bool(root.get_meta(WORKBENCH_META, false))
