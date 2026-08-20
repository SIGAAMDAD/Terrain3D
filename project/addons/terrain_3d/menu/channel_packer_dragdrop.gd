# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Channel Packer Dragdropper for Terrain3D
@tool
class_name T3DChannelPackerDragDrop
extends Button

signal dropped(path: String)

func _can_drop_data(p_position: Vector2, p_data: Variant) -> bool:
	if typeof(p_data) == TYPE_DICTIONARY:
		if p_data.files.size() == 1:
			match p_data.files[0].get_extension():
				"png", "bmp", "exr", "hdr", "jpg", "jpeg", "tga", "svg", "webp", "ktx", "dds":
					return true
	return false

func _drop_data(p_position: Vector2, p_data: Variant) -> void:
	dropped.emit(p_data.files[0])
