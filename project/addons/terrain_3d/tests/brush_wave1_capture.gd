@tool
extends RefCounted
## BRP-00 editor reference capture. Run with identical inputs against the original
## Baseline2 and BRP-1 builds; compare map-data hex and presence bit-for-bit.
## The user supplies an initialized Terrain3DData and Terrain3DEditor.

static func capture_maps(data: Object, locations: Array[Vector2i]) -> Dictionary:
	var regions := {}
	for location in locations:
		var region = data.get_region(location)
		var channels := {}
		for channel in [0, 1, 2]:
			var image: Image = region.get_map(channel) if region != null else null
			var key := str(channel)
			if image == null:
				channels[key] = {"map_absent": true}
			else:
				var native_bytes := image.get_data()
				channels[key] = {
					"map_absent": false,
					"format": image.get_format(),
					"width": image.get_width(),
					"height": image.get_height(),
					"mipmaps": image.has_mipmaps(),
					"data_hex": native_bytes.hex_encode(),
					"data_sha256": native_bytes.sha256_text()
				}
		regions[str(location)] = channels
	return regions

static func capture_stroke(data: Object, editor: Object, locations: Array[Vector2i],
		tool: int, operation: int, brush_data: Dictionary, positions: Array[Vector3],
		cancel: bool = false) -> Dictionary:
	var output := {"before": capture_maps(data, locations)}
	if positions.is_empty():
		output["after"] = output["before"]
		return output
	editor.set_tool(tool)
	editor.set_operation(operation)
	editor.set_brush_data(brush_data)
	editor.start_operation(positions[0])
	for position in positions:
		editor.operate(position, 0.0)
	if cancel:
		editor.cancel_operation()
	else:
		editor.stop_operation()
	output["after"] = capture_maps(data, locations)
	output["performance"] = editor.get_brush_performance_stats() if editor.has_method("get_brush_performance_stats") else {}
	return output

static func write_capture(path: String, capture: Dictionary) -> Error:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_string(JSON.stringify(capture, "\t"))
	file.close()
	return OK
