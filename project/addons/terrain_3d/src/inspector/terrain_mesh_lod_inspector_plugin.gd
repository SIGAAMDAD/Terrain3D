# Copyright (c) 2026 Terrain3D contributors.
# Offline conventional LOD cook and opt-in preview for Terrain3DMeshAsset.
@tool
extends EditorInspectorPlugin


func _can_handle(p_object: Object) -> bool:
	return p_object is Terrain3DMeshAsset


func _parse_begin(p_object: Object) -> void:
	var asset: Terrain3DMeshAsset = p_object as Terrain3DMeshAsset
	var panel := VBoxContainer.new()
	var heading := Label.new()
	heading.text = "Mesh LOD Cook (meshoptimizer 1.3)"
	panel.add_child(heading)
	var actions := HBoxContainer.new()
	panel.add_child(actions)
	var bake := Button.new()
	bake.text = "Bake Missing LODs"
	bake.tooltip_text = "Cook missing conventional mesh LODs from the last authored level, preserving authored levels and their materials."
	bake.disabled = asset.get_scene_file() == null
	actions.add_child(bake)
	var preview := CheckButton.new()
	preview.text = "Preview"
	actions.add_child(preview)
	var picker := OptionButton.new()
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.add_child(picker)
	var status := Label.new()
	status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	panel.add_child(status)
	var container := VBoxContainer.new()
	panel.add_child(container)
	var state: Dictionary = {}
	_refresh_lods(asset, picker, status)
	picker.item_selected.connect(_on_lod_selected.bind(asset, picker, status, state))
	preview.toggled.connect(_on_preview_toggled.bind(asset, picker, container, state))
	bake.pressed.connect(_on_bake_pressed.bind(asset, picker, status, state))
	add_custom_control(panel)


func _refresh_lods(asset: Terrain3DMeshAsset, picker: OptionButton, status: Label) -> void:
	var old_index := maxi(0, picker.selected)
	picker.clear()
	var stats: Array = asset.get_meshopt_lod_stats()
	for info: Dictionary in stats:
		var index: int = int(info.get("lod", 0))
		var suffix := "authored" if bool(info.get("authored", false)) else "generated"
		picker.add_item("LOD %d (%s)" % [index, suffix], index)
	if picker.item_count == 0:
		status.text = "No mesh LODs available. Select a source scene."
		return
	picker.select(mini(old_index, picker.item_count - 1))
	_update_stats(asset, picker, status)


func _update_stats(asset: Terrain3DMeshAsset, picker: OptionButton, status: Label) -> void:
	var index := picker.get_selected_id()
	var stats: Array = asset.get_meshopt_lod_stats()
	if index < 0 or index >= stats.size():
		return
	var info: Dictionary = stats[index]
	var error: float = float(info.get("geometric_error", -1.0))
	var error_label := "authored (unknown)" if error < 0.0 else "%.5f m (accumulated)" % error
	status.text = "Triangles: %d | Error: %s | End range: %.1f m" % [
		int(info.get("triangles", 0)), error_label, float(info.get("range", 0.0))]


func _on_lod_selected(_index: int, asset: Terrain3DMeshAsset,
	picker: OptionButton, status: Label, state: Dictionary) -> void:
	_update_stats(asset, picker, status)
	_update_preview(asset, picker, state)


func _on_bake_pressed(asset: Terrain3DMeshAsset,
	picker: OptionButton, status: Label, state: Dictionary) -> void:
	if not asset.bake_missing_lods():
		status.text = "No new LODs produced: check mesh format, triangle reduction and error ceiling."
		return
	_refresh_lods(asset, picker, status)
	_update_preview(asset, picker, state)
	asset.emit_changed()


func _on_preview_toggled(visible: bool, asset: Terrain3DMeshAsset,
	picker: OptionButton, holder: VBoxContainer, state: Dictionary) -> void:
	if not visible:
		for child in holder.get_children():
			child.queue_free()
		state.clear()
		return
	var frame := SubViewportContainer.new()
	frame.custom_minimum_size = Vector2(240, 180)
	frame.stretch = true
	holder.add_child(frame)
	var viewport := SubViewport.new()
	viewport.size = Vector2i(240, 180)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ONCE
	frame.add_child(viewport)
	var mesh_instance := MeshInstance3D.new()
	viewport.add_child(mesh_instance)
	var camera := Camera3D.new()
	camera.current = true
	camera.fov = 55.0
	viewport.add_child(camera)
	var sunlight := DirectionalLight3D.new()
	sunlight.rotation_degrees = Vector3(-45, -35, 0)
	viewport.add_child(sunlight)
	state["viewport"] = viewport
	state["instance"] = mesh_instance
	state["camera"] = camera
	_update_preview(asset, picker, state)


func _update_preview(asset: Terrain3DMeshAsset, picker: OptionButton, state: Dictionary) -> void:
	if not state.has("instance"):
		return
	var instance: MeshInstance3D = state["instance"] as MeshInstance3D
	var viewport: SubViewport = state["viewport"] as SubViewport
	var camera: Camera3D = state["camera"] as Camera3D
	if not is_instance_valid(instance) or not is_instance_valid(viewport):
		return
	instance.mesh = asset.get_mesh(picker.get_selected_id())
	if instance.mesh == null:
		return
	var bounds := instance.mesh.get_aabb()
	var radius := maxf(bounds.size.length(), 0.1)
	var center := bounds.get_center()
	camera.position = center + Vector3(1.0, 0.65, 1.0).normalized() * radius * 2.2
	camera.near = maxf(radius * 0.001, 0.01)
	camera.far = maxf(radius * 10.0, 100.0)
	camera.look_at(center, Vector3.UP)
	viewport.render_target_update_mode = SubViewport.UPDATE_ONCE
