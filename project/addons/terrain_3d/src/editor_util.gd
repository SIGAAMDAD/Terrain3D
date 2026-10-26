# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Small deterministic editor helpers shared by Terrain3D tooling.
@tool
extends RefCounted


static func region_location_less(a: Vector2i, b: Vector2i) -> bool:
	return a.y < b.y or (a.y == b.y and a.x < b.x)


static func find_terrain(node: Node) -> Terrain3D:
	if not is_instance_valid(node):
		return null
	if node is Terrain3D:
		return node as Terrain3D
	for child: Node in node.get_children():
		var found: Terrain3D = find_terrain(child)
		if is_instance_valid(found):
			return found
	return null


static func select_terrain(preferred: Terrain3D, fallback: Terrain3D) -> Terrain3D:
	if is_instance_valid(preferred):
		return preferred
	return fallback if is_instance_valid(fallback) else null


static func format_integer(value: int) -> String:
	var text: String = str(value)
	var result: String = ""
	while text.length() > 3:
		result = "," + text.right(3) + result
		text = text.left(text.length() - 3)
	return text + result

static func vector2i_from_value(value: Variant, fallback: Vector2i = Vector2i.ZERO) -> Vector2i:
	if value is Vector2i:
		return value
	if value is Array:
		var values: Array[Variant] = value
		if values.size() == 2:
			return Vector2i(int(values[0]), int(values[1]))
	return fallback


static func vector2i_to_array(value: Vector2i) -> Array[int]:
	return [value.x, value.y]

