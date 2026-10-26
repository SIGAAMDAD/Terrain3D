# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Asset Dock for Terrain3D
@tool
class_name Terrain3DAssetDock
extends PanelContainer

signal confirmation_closed
signal confirmation_confirmed
signal confirmation_canceled

const ES_DOCK_TILE_SIZE: String = "terrain3d/dock/tile_size"
const ES_DOCK_PINNED: String = "terrain3d/dock/always_on_top"
const ES_DOCK_TAB: String = "terrain3d/dock/tab"

var texture_list: ListContainer
var mesh_list: ListContainer
var current_list: ListContainer
var _updating_list: bool

var pinned_btn: Button
var size_slider: HSlider
var box: BoxContainer
var buttons: BoxContainer
var textures_btn: Button
var meshes_btn: Button
var asset_container: ScrollContainer
var confirm_dialog: ConfirmationDialog
var _confirmed: bool = false
var search_box: TextEdit
var search_button: Button

var _dock: EditorDock
var _initialized: bool = false
var plugin: Terrain3DEditorPluginBase
var window: Window
var _connected_assets: Terrain3DAssets


func _notification(what: int) -> void:
	if what == NOTIFICATION_ENTER_TREE:
		await get_tree().process_frame
		update_layout()


func initialize(p_plugin: Terrain3DEditorPluginBase) -> void:
	if p_plugin:
		plugin = p_plugin

	_dock = EditorDock.new()
	_dock.name = "Terrain3DAssetsDock"
	_dock.title = "Terrain3D"
	_dock.layout_key = "terrain3d_assets"
	_dock.dock_icon = preload("../icons/terrain3d.svg")
	_dock.default_slot = EditorDock.DOCK_SLOT_BOTTOM
	_dock.closable = false
	_dock.set_global(false)
	_dock.available_layouts = EditorDock.DOCK_LAYOUT_ALL
	_dock.add_child(self)
	plugin.add_dock(_dock)
	_dock.make_visible()

	pinned_btn = $Box/Buttons/Pinned
	size_slider = $Box/Buttons/SizeSlider
	size_slider.owner = null
	size_slider.tooltip_text = "Asset tile preview size in the Terrain3D dock. This changes editor presentation only and does not affect terrain runtime texture or mesh quality."
	box = $Box
	buttons = $Box/Buttons
	textures_btn = $Box/Buttons/TexturesBtn
	meshes_btn = $Box/Buttons/MeshesBtn
	asset_container = $Box/ScrollContainer
	search_box = $Box/Buttons/SearchBox
	search_box.tooltip_text = "Filter the Terrain3D asset dock by texture or mesh asset name. This changes editor presentation only."
	search_box.owner = null
	# Scale left column width to editor scale
	var editor_scale: float = EditorInterface.get_editor_scale()
	search_box.custom_minimum_size = Vector2(100. * editor_scale, 30. * editor_scale)
	search_button = $Box/Buttons/SearchBox/SearchButton

	texture_list = ListContainer.new()
	texture_list.name = "TextureList"
	texture_list.plugin = plugin
	texture_list.type = Terrain3DAssets.TYPE_TEXTURE
	asset_container.add_child(texture_list, true)
	mesh_list = ListContainer.new()
	mesh_list.name = "MeshList"
	mesh_list.plugin = plugin
	mesh_list.type = Terrain3DAssets.TYPE_MESH
	mesh_list.visible = false
	asset_container.add_child(mesh_list, true)
	current_list = texture_list

	load_editor_settings()

	# Connect signals
	resized.connect(update_layout)
	textures_btn.pressed.connect(_on_textures_pressed)
	meshes_btn.pressed.connect(_on_meshes_pressed)
	pinned_btn.toggled.connect(_on_pin_changed)
	pinned_btn.owner = null
	size_slider.value_changed.connect(_on_slider_changed)

	meshes_btn.add_theme_font_size_override("font_size", int(16. * EditorInterface.get_editor_scale()))
	textures_btn.add_theme_font_size_override("font_size", int(16. * EditorInterface.get_editor_scale()))

	search_box.text_changed.connect(_on_search_text_changed)
	search_button.pressed.connect(_on_search_button_pressed)

	confirm_dialog = ConfirmationDialog.new()
	add_child(confirm_dialog, true)
	confirm_dialog.hide()
	confirm_dialog.confirmed.connect(func() -> void: _confirmed = true; \
		confirmation_closed.emit(); \
		confirmation_confirmed.emit() )
	confirm_dialog.canceled.connect(func() -> void: _confirmed = false; \
		confirmation_closed.emit(); \
		confirmation_canceled.emit() )

	# Setup styles
	set("theme_override_styles/panel", get_theme_stylebox("panel", "Panel"))
	# Avoid saving icon resources in tscn when editing w/ a tool script
	if EditorInterface.get_edited_scene_root() != self:
		pinned_btn.icon = get_theme_icon("Pin", "EditorIcons")
		pinned_btn.text = ""
		search_button.icon = get_theme_icon("Search", "EditorIcons")

	_initialized = true
	update_dock()
	update_layout()


func _gui_input(p_event: InputEvent) -> void:
	if p_event is InputEventMouseButton:
		if search_box.has_focus():
			if plugin.debug:
				print("Terrain3DAssetDock: _on_box_gui_input: search_box releasing focus")
			search_box.release_focus()


## Dock placement


func remove_dock(_p_force: bool = false) -> void:
	_initialized = false
	_set_connected_assets(null)
	if is_instance_valid(plugin) and is_instance_valid(plugin.godot_editor_window):
		if plugin.godot_editor_window.mouse_entered.is_connected(_on_godot_window_entered):
			plugin.godot_editor_window.mouse_entered.disconnect(_on_godot_window_entered)
	window = null
	if is_instance_valid(_dock):
		if is_instance_valid(plugin):
			plugin.remove_dock(_dock)
		_dock.queue_free()
		_dock = null


func update_dock() -> void:
	if not _initialized or not is_instance_valid(_dock):
		return
	update_assets()
	_dock.make_visible()


func update_layout() -> void:
	if not _initialized or not is_instance_valid(plugin):
		return
	if plugin.debug > 1:
		print("Terrain3DAssetDock: update_layout")

	## Detect whether the dock is floating without depending on private editor hierarchy.
	var editor_window: Window = plugin.godot_editor_window if is_instance_valid(plugin) else null
	var current_window := get_window()
	var floating_window: Window = null
	if is_instance_valid(editor_window) and is_instance_valid(current_window) and current_window != editor_window:
		floating_window = current_window
	if window != floating_window:
		if is_instance_valid(editor_window) and editor_window.mouse_entered.is_connected(_on_godot_window_entered):
			editor_window.mouse_entered.disconnect(_on_godot_window_entered)
		window = floating_window
		if is_instance_valid(window):
			_on_pin_changed(pinned_btn.button_pressed)
			if is_instance_valid(editor_window) and not editor_window.mouse_entered.is_connected(_on_godot_window_entered):
				editor_window.mouse_entered.connect(_on_godot_window_entered)

	## Vertical layout: buttons on top
	if size.x < 700:
		box.vertical = true
		buttons.vertical = false
		search_box.reparent(box)
		box.move_child(search_box, 1)
		size_slider.reparent(box)
		box.move_child(size_slider, 2)
		pinned_btn.reparent(buttons)
	else:
	# Wide layout: buttons on left
		box.vertical = false
		buttons.vertical = true
		search_box.reparent(buttons)
		buttons.move_child(search_box, 0)
		size_slider.reparent(buttons)
		buttons.move_child(size_slider, 3)
		pinned_btn.reparent(box)

	pinned_btn.visible = is_instance_valid(window)
	save_editor_settings()


# When a floating asset dock has focus and the mouse returns, grab focus so cursor decal works
func _on_godot_window_entered() -> void:
	if is_instance_valid(plugin) and plugin.debug > 1:
		print("Terrain3DAssetDock: _on_godot_window_entered")
	if is_instance_valid(plugin) and is_instance_valid(plugin.godot_editor_window) and \
			is_instance_valid(window) and window.has_focus():
		plugin.godot_editor_window.grab_focus()


func set_selected_by_asset_id(p_id: int, p_type: int) -> void:
	search_box.text = ""
	_on_search_text_changed()
	if p_type == Terrain3DAssets.TYPE_MESH:
		_on_meshes_pressed()
		mesh_list.set_selected_asset_id(p_id)
	else:
		_on_textures_pressed()
		texture_list.set_selected_asset_id(p_id)


func _on_search_text_changed() -> void:
	if plugin.debug:
		print("Terrain3DAssetDock: _on_search_text_changed: ", search_box.text)
	search_box.text = search_box.text.strip_escapes()
	var len: int = search_box.text.length()
	if len > 0:
		search_box.set_caret_column(len)
		search_button.icon = get_theme_icon("Close", "EditorIcons")
	else:
		search_button.icon = get_theme_icon("Search", "EditorIcons")

	mesh_list.search_text = search_box.text
	texture_list.search_text = search_box.text
	current_list.update_asset_list()


func _on_search_button_pressed() -> void:
	if plugin.debug:
		print("Terrain3DAssetDock: _on_search_button_pressed")
	if search_box.text.length() > 0:
		search_box.text = ""
		_on_search_text_changed()
	else:
		if plugin.debug:
			print("Terrain3DAssetDock: _on_search_button_pressed: Search box grabbing focus")
		search_box.grab_focus()


## Dock Button handlers


func _on_pin_changed(p_toggled: bool) -> void:
	if is_instance_valid(window):
		window.always_on_top = p_toggled
	save_editor_settings()


func _on_slider_changed(value: float) -> void:
	# Set both lists so they match
	if texture_list:
		texture_list.set_entry_width(value)
	if mesh_list:
		mesh_list.set_entry_width(value)
	save_editor_settings()
	# Hack to trigger ScrollContainer::_reposition_children() to update size of scroll bar handle
	asset_container.layout_direction = Control.LAYOUT_DIRECTION_LTR
	asset_container.layout_direction = Control.LAYOUT_DIRECTION_INHERITED


func _on_textures_pressed() -> void:
	if plugin.debug:
		print("Terrain3DAssetDock: _on_textures_pressed")
	if _updating_list or current_list == texture_list:
		return
	_updating_list = true
	current_list = texture_list
	texture_list.visible = true
	mesh_list.visible = false
	textures_btn.set_pressed_no_signal(true)
	meshes_btn.set_pressed_no_signal(false)
	texture_list.update_asset_list()
	if plugin.is_terrain_valid():
		EditorInterface.edit_node(plugin.terrain)
	save_editor_settings()
	_updating_list = false


func _on_meshes_pressed() -> void:
	if plugin.debug:
		print("Terrain3DAssetDock: _on_meshes_pressed")
	if _updating_list or current_list == mesh_list:
		return
	_updating_list = true
	current_list = mesh_list
	mesh_list.visible = true
	texture_list.visible = false
	meshes_btn.set_pressed_no_signal(true)
	textures_btn.set_pressed_no_signal(false)
	mesh_list.update_asset_list()
	if plugin.is_terrain_valid():
		EditorInterface.edit_node(plugin.terrain)
	save_editor_settings()
	_updating_list = false


func _on_tool_changed(p_tool: int, p_operation: int) -> void:
	if plugin.debug:
		print("Terrain3DAssetDock: _on_tool_changed: ", p_tool, ", ", p_operation)
	remove_all_highlights()
	if p_tool == Terrain3DEditor.INSTANCER:
		_on_meshes_pressed()
	elif p_tool in [ Terrain3DEditor.TEXTURE, Terrain3DEditor.COLOR, Terrain3DEditor.ROUGHNESS ]:
		_on_textures_pressed()


## Update Dock Contents


func update_assets() -> void:
	if not _initialized or not is_instance_valid(plugin):
		return
	if plugin.debug:
		print("Terrain3DAssetDock: update_assets: ", plugin.terrain.assets if plugin.terrain else "")

	var target_assets: Terrain3DAssets = null
	if plugin.is_terrain_valid():
		target_assets = plugin.terrain.assets
	var source_changed: bool = _connected_assets != target_assets
	_set_connected_assets(target_assets)
	if source_changed:
		# Both lists are queried directly by tool type even when their tab is hidden.
		# Refresh both on terrain/asset-source changes so a manually selected dock tab
		# cannot leave the other tool's asset IDs pointing at the previous terrain.
		texture_list.update_asset_list()
		mesh_list.update_asset_list()
	else:
		current_list.update_asset_list()


func _set_connected_assets(p_assets: Terrain3DAssets) -> void:
	if _connected_assets == p_assets:
		return
	if is_instance_valid(_connected_assets):
		if _connected_assets.textures_changed.is_connected(texture_list.update_asset_list):
			_connected_assets.textures_changed.disconnect(texture_list.update_asset_list)
		if _connected_assets.meshes_changed.is_connected(mesh_list.update_asset_list):
			_connected_assets.meshes_changed.disconnect(mesh_list.update_asset_list)
	_connected_assets = p_assets
	if not is_instance_valid(_connected_assets):
		return
	if not _connected_assets.textures_changed.is_connected(texture_list.update_asset_list):
		_connected_assets.textures_changed.connect(texture_list.update_asset_list)
	if not _connected_assets.meshes_changed.is_connected(mesh_list.update_asset_list):
		_connected_assets.meshes_changed.connect(mesh_list.update_asset_list)


func remove_all_highlights() -> void:
	if not plugin.terrain:
		return
	for i: int in texture_list.entries.size():
		var resource := texture_list.entries[i].resource as Terrain3DTextureAsset
		if resource and resource.is_highlighted():
			resource.set_highlighted(false)
	for i: int in mesh_list.entries.size():
		var resource := mesh_list.entries[i].resource as Terrain3DMeshAsset
		if resource and resource.is_highlighted():
			resource.set_highlighted(false)


## Manage Editor Settings


func load_editor_settings() -> void:
	# Remove old editor settings
	const ES_DOCK: String = "terrain3d/dock/"
	for setting: String in [ "slot", "floating", "window_position", "window_size" ]:
		plugin.erase_setting(ES_DOCK + setting)
	pinned_btn.button_pressed = plugin.get_setting(ES_DOCK_PINNED, true)
	size_slider.value = plugin.get_setting(ES_DOCK_TILE_SIZE, 90)
	_on_slider_changed(size_slider.value)
	if plugin.get_setting(ES_DOCK_TAB, 0) == 1:
		_on_meshes_pressed()


func save_editor_settings() -> void:
	if not _initialized:
		return
	plugin.set_setting(ES_DOCK_TILE_SIZE, size_slider.value)
	plugin.set_setting(ES_DOCK_PINNED, pinned_btn.button_pressed)
	plugin.set_setting(ES_DOCK_TAB, 0 if current_list == texture_list else 1)


##############################################################
## class ListContainer
##############################################################


class ListContainer extends Container:
	var plugin: Terrain3DEditorPluginBase
	var type: int = int(Terrain3DAssets.TYPE_TEXTURE)
	var entries: Array[ListEntry]
	var selected_id: int = 0
	var height: float = 0.
	var width: float = 90.
	var focus_style: StyleBox
	var _clear_request_pending: bool = false
	var search_text: String = ""


	func _ready() -> void:
		set_v_size_flags(SIZE_EXPAND_FILL)
		set_h_size_flags(SIZE_EXPAND_FILL)
		add_theme_color_override("font_color", Color.WHITE)
		add_theme_color_override("font_shadow_color", Color.BLACK)
		add_theme_constant_override("shadow_offset_x", 1)
		add_theme_constant_override("shadow_offset_y", 1)


	func clear() -> void:
		for e: ListEntry in entries:
			e.set_edited_resource(null)
			e.get_parent().remove_child(e)
			e.queue_free()
		entries.clear()


	func update_asset_list() -> void:
		if plugin.debug:
			print("Terrain3DListContainer ", name, ": update_asset_list")
		var previous_asset_id: int = get_selected_asset_id()
		clear()

		# Grab terrain
		var t: Terrain3D = plugin.get_terrain()
		if not (t and t.assets):
			return

		if type == Terrain3DAssets.TYPE_TEXTURE:
			var texture_count: int = t.assets.get_texture_count()
			for i: int in texture_count:
				var texture: Terrain3DTextureAsset = t.assets.get_texture_asset(i)
				add_item(texture, i)
			if texture_count < Terrain3DAssets.MAX_TEXTURE_ASSETS:
				add_item(null, texture_count)
		else:
			_request_mesh_thumbnails()
			var mesh_count: int = t.assets.get_mesh_count()
			for i: int in mesh_count:
				var mesh: Terrain3DMeshAsset = t.assets.get_mesh_asset(i)
				add_item(mesh, i)
			if mesh_count < Terrain3DAssets.MAX_MESH_ASSETS:
				add_item(null, mesh_count)
		select_asset_or_first(previous_asset_id)


	func add_item(p_resource: Resource = null, p_asset_id: int = -1) -> void:
		var entry: ListEntry = ListEntry.new()
		entry.focus_style = focus_style
		entry.asset_id = p_asset_id
		entry.set_edited_resource(p_resource)
		if not entry.get_resource_name().containsn(search_text) and not search_text == "":
			entry.free()
			return

		var res_id: int = p_asset_id
		if res_id < 0:
			var texture_asset := p_resource as Terrain3DTextureAsset
			var mesh_asset := p_resource as Terrain3DMeshAsset
			if is_instance_valid(texture_asset):
				res_id = texture_asset.id
			elif is_instance_valid(mesh_asset):
				res_id = mesh_asset.id
		entry.hovered.connect(_on_resource_hovered.bind(res_id))
		entry.clicked.connect(clicked_id.bind(entries.size()))
		entry.inspected.connect(_on_resource_inspected)
		entry.changed.connect(_on_resource_changed.bind(res_id))
		entry.clear_requested.connect(_on_clear_requested.bind(res_id))
		entry.resource_replaced.connect(_on_entry_resource_replaced)
		entry.type = type
		add_child(entry, true)
		entries.push_back(entry)

		_connect_asset_id_signal(p_resource)


	func _connect_asset_id_signal(p_resource: Resource) -> void:
		var texture_asset := p_resource as Terrain3DTextureAsset
		var mesh_asset := p_resource as Terrain3DMeshAsset
		if is_instance_valid(texture_asset) and not texture_asset.id_changed.is_connected(set_selected_after_swap):
			texture_asset.id_changed.connect(set_selected_after_swap)
		elif is_instance_valid(mesh_asset) and not mesh_asset.id_changed.is_connected(set_selected_after_swap):
			mesh_asset.id_changed.connect(set_selected_after_swap)


	func _disconnect_asset_id_signal(p_resource: Resource) -> void:
		var texture_asset := p_resource as Terrain3DTextureAsset
		var mesh_asset := p_resource as Terrain3DMeshAsset
		if is_instance_valid(texture_asset) and texture_asset.id_changed.is_connected(set_selected_after_swap):
			texture_asset.id_changed.disconnect(set_selected_after_swap)
		elif is_instance_valid(mesh_asset) and mesh_asset.id_changed.is_connected(set_selected_after_swap):
			mesh_asset.id_changed.disconnect(set_selected_after_swap)


	func _on_entry_resource_replaced(p_previous: Resource, p_current: Resource) -> void:
		_disconnect_asset_id_signal(p_previous)
		_connect_asset_id_signal(p_current)


	func _on_resource_hovered(p_id: int) -> void:
		if type == Terrain3DAssets.TYPE_MESH:
			_request_mesh_thumbnails(p_id)


	func _request_mesh_thumbnails(p_id: int = -1) -> void:
		var terrain: Terrain3D = plugin.get_terrain() if is_instance_valid(plugin) else null
		if not is_instance_valid(terrain) or not is_instance_valid(terrain.assets):
			return
		# Preview targets/readback are independent from terrain-region residency.
		# C++ owns preview-slot backpressure and typed transient deferral.
		terrain.assets.create_mesh_thumbnails(p_id, Vector2i(256, 256), false)



	func set_selected_after_swap(p_type: int, p_old_id: int, p_new_id: int) -> void:
		EditorInterface.mark_scene_as_unsaved()
		set_selected_asset_id(p_new_id)


	func clicked_id(p_id: int) -> void:
		# Select Tool if clicking an asset
		plugin.select_terrain()
		if type == Terrain3DAssets.TYPE_TEXTURE and \
				not plugin.editor.get_tool() in [ Terrain3DEditor.TEXTURE, Terrain3DEditor.COLOR, Terrain3DEditor.ROUGHNESS ]:
			plugin.change_tool("PaintTexture")
		elif type == Terrain3DAssets.TYPE_MESH and plugin.editor.get_tool() != Terrain3DEditor.INSTANCER:
			plugin.change_tool("InstanceMeshes")
		set_selected_id(p_id)


	func set_selected_id(p_id: int) -> void:
		if entries.is_empty():
			selected_id = -1
			plugin.notify_ui_setting_changed()
			return
		var max_id: int = entries.size() - 1
		if plugin.debug:
			print("Terrain3DListContainer ", name, ": set_selected_id: ", selected_id, " to ", clampi(p_id, 0, max_id))
		selected_id = clampi(p_id, 0, max_id)
		for i: int in entries.size():
			var entry: ListEntry = entries[i]
			entry.set_selected(i == selected_id)
		plugin.notify_ui_setting_changed()


	func set_selected_asset_id(p_asset_id: int) -> void:
		for i: int in entries.size():
			var entry: ListEntry = entries[i]
			if entry.asset_id == p_asset_id and is_instance_valid(entry.resource):
				set_selected_id(i)
				return
		select_asset_or_first(-1)


	func select_asset_or_first(p_preferred_asset_id: int) -> void:
		if entries.is_empty():
			selected_id = -1
			plugin.notify_ui_setting_changed()
			return
		if p_preferred_asset_id >= 0:
			for i: int in entries.size():
				var preferred: ListEntry = entries[i]
				if preferred.asset_id == p_preferred_asset_id and is_instance_valid(preferred.resource):
					set_selected_id(i)
					return
		for i: int in entries.size():
			if is_instance_valid(entries[i].resource):
				set_selected_id(i)
				return
		set_selected_id(0)


	func get_selected_asset_id() -> int:
		if selected_id < 0 or selected_id >= entries.size():
			return -1
		var id: int = selected_id
		if plugin.debug:
			print("Terrain3DListContainer ", name, ": get_selected_asset_id: selected_id: ", selected_id, ", entries: ", entries.size())
		var res := entries[id].resource as Resource
		if not res:
			return -1
		if type == Terrain3DAssets.TYPE_MESH:
			return (res as Terrain3DMeshAsset).id
		else:
			return (res as Terrain3DTextureAsset).id


	func _on_resource_inspected(p_resource: Resource) -> void:
		await get_tree().process_frame
		EditorInterface.edit_resource(p_resource)


	func _on_resource_changed(p_resource: Resource, p_id: int) -> void:
		# Resource-change signals are non-destructive. A null resource can occur while
		# controls are being rebuilt or invalidated and must never be interpreted as
		# user intent to delete an asset.
		if not is_instance_valid(p_resource):
			return

		if not plugin.is_terrain_valid():
			plugin.select_terrain()
			await get_tree().process_frame

		if plugin.is_terrain_valid():
			if type == Terrain3DAssets.TYPE_TEXTURE:
				plugin.terrain.assets.set_texture_asset(p_id, p_resource)
			else:
				plugin.terrain.assets.set_mesh_asset(p_id, p_resource)
			set_selected_asset_id(p_id)


	func _on_clear_requested(p_resource: Resource, p_id: int) -> void:
		if _clear_request_pending or not is_instance_valid(p_resource):
			return
		_clear_request_pending = true
		var asset_dock: Terrain3DAssetDock = get_parent().get_parent().get_parent() as Terrain3DAssetDock
		asset_dock._confirmed = false
		if type == Terrain3DAssets.TYPE_TEXTURE:
			asset_dock.confirm_dialog.dialog_text = "Are you sure you want to clear this texture?"
		else:
			asset_dock.confirm_dialog.dialog_text = "Are you sure you want to clear this mesh and delete all instances?"
		asset_dock.confirm_dialog.popup_centered()
		await asset_dock.confirmation_closed
		if not asset_dock._confirmed:
			_clear_request_pending = false
			return

		if not plugin.is_terrain_valid():
			plugin.select_terrain()
			await get_tree().process_frame

		if plugin.is_terrain_valid():
			if type == Terrain3DAssets.TYPE_TEXTURE:
				plugin.terrain.assets.set_texture_asset(p_id, null)
			else:
				plugin.terrain.assets.set_mesh_asset(p_id, null)
			EditorInterface.inspect_object(null)
		_clear_request_pending = false


	func set_entry_width(value: float) -> void:
		var min_width: float = 90.0 * maxf(1.0, EditorInterface.get_editor_scale())
		width = clampf(value, min_width, 256.0 * EditorInterface.get_editor_scale())
		redraw()


	func get_entry_width() -> float:
		return width


	func redraw() -> void:
		height = 0
		var id: int = 0
		var separation: float = 2.
		var columns: int = 3
		columns = clampi(int(size.x / width), 1, 100)
		var tile_size: Vector2 = Vector2(width, width) - Vector2(separation, separation)
		var count_font_size := int(clampf(tile_size.x/11., 11., 16.) * EditorInterface.get_editor_scale())
		var name_font_size := int(clampf(tile_size.x/12., 12., 16.) * EditorInterface.get_editor_scale())
		for child: Node in get_children():
			var entry := child as ListEntry
			if is_instance_valid(entry):
				entry.size = tile_size
				entry.position = Vector2(id % columns, id / columns) * width + \
					Vector2(separation / columns, separation / columns)
				height = maxf(height, entry.position.y + width)
				id += 1
				if type == Terrain3DAssets.TYPE_MESH:
					entry.count_label.add_theme_font_size_override("font_size", count_font_size)
				entry.name_label.add_theme_font_size_override("font_size", name_font_size)


	# Needed to enable ScrollContainer scroll bar
	func _get_minimum_size() -> Vector2:
		return Vector2(0, height)


	func _notification(p_what: int) -> void:
		if p_what == NOTIFICATION_SORT_CHILDREN:
			redraw()


##############################################################
## class ListEntry
##############################################################


class ListEntry extends MarginContainer:
	signal hovered()
	signal clicked()
	signal changed(resource: Resource)
	signal clear_requested(resource: Resource)
	signal resource_replaced(previous: Resource, current: Resource)
	signal inspected(resource: Resource)

	var resource: Resource
	var asset_id: int = -1
	var type: int = int(Terrain3DAssets.TYPE_TEXTURE)
	var _thumbnail: Texture2D
	var drop_data: bool = false
	var is_hovered: bool = false
	var is_selected: bool = false
	var is_highlighted: bool = false

	var name_label: Label
	var count_label: Label
	var button_row: FlowContainer
	var button_enabled: TextureButton
	var button_highlight: TextureButton
	var button_edit: TextureButton
	var spacer: Control
	var button_clear: TextureButton

	@onready var focus_style: StyleBox = get_theme_stylebox("focus", "Button").duplicate()
	@onready var background: StyleBox = get_theme_stylebox("pressed", "Button")
	@onready var clear_icon: Texture2D = get_theme_icon("Close", "EditorIcons")
	@onready var edit_icon: Texture2D = get_theme_icon("Edit", "EditorIcons")
	@onready var enabled_icon: Texture2D = get_theme_icon("GuiVisibilityVisible", "EditorIcons")
	@onready var disabled_icon: Texture2D = get_theme_icon("GuiVisibilityHidden", "EditorIcons")
	@onready var highlight_icon: Texture2D = get_theme_icon("PreviewSun", "EditorIcons")
	@onready var add_icon: Texture2D = get_theme_icon("Add", "EditorIcons")


	func _ready() -> void:
		name = "ListEntry"
		custom_minimum_size = Vector2i(86., 86.)
		mouse_filter = Control.MOUSE_FILTER_PASS
		add_theme_constant_override("margin_top", 5)
		add_theme_constant_override("margin_left", 5)
		add_theme_constant_override("margin_right", 5)

		if resource is Terrain3DTextureAsset:
			is_highlighted = (resource as Terrain3DTextureAsset).is_highlighted()
		elif resource is Terrain3DMeshAsset:
			is_highlighted = (resource as Terrain3DMeshAsset).is_highlighted()

		setup_buttons()
		setup_label()
		setup_count_label()
		var flat_focus_style := focus_style as StyleBoxFlat
		if is_instance_valid(flat_focus_style):
			flat_focus_style.set_border_width_all(2)
			flat_focus_style.set_border_color(Color(1, 1, 1, .67))


	func setup_buttons() -> void:
		destroy_buttons()

		button_row = FlowContainer.new()
		button_enabled = TextureButton.new()
		button_highlight = TextureButton.new()
		button_edit = TextureButton.new()
		spacer = Control.new()
		button_clear = TextureButton.new()

		var icon_size: Vector2 = Vector2(12, 12)

		button_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		button_row.alignment = FlowContainer.ALIGNMENT_CENTER
		button_row.mouse_filter = Control.MOUSE_FILTER_PASS
		add_child(button_row, true)

		if type == Terrain3DAssets.TYPE_MESH:
			button_enabled.set_texture_normal(enabled_icon)
			button_enabled.set_texture_pressed(disabled_icon)
			button_enabled.set_custom_minimum_size(icon_size)
			button_enabled.set_h_size_flags(Control.SIZE_SHRINK_END)
			button_enabled.set_visible(resource != null)
			button_enabled.tooltip_text = "Enable Instances"
			button_enabled.toggle_mode = true
			button_enabled.mouse_filter = Control.MOUSE_FILTER_PASS
			button_enabled.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
			button_enabled.pressed.connect(_on_enable)
			button_row.add_child(button_enabled, true)

		button_highlight.set_texture_normal(highlight_icon)
		button_highlight.set_custom_minimum_size(icon_size)
		button_highlight.set_h_size_flags(Control.SIZE_SHRINK_END)
		button_highlight.set_visible(resource != null)
		button_highlight.tooltip_text = "Highlight " + ( "Instances" if type == Terrain3DAssets.TYPE_MESH else "Texture" )
		button_highlight.toggle_mode = true
		button_highlight.mouse_filter = Control.MOUSE_FILTER_PASS
		button_highlight.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
		button_highlight.set_pressed_no_signal(is_highlighted)
		button_highlight.pressed.connect(_on_highlight)
		button_row.add_child(button_highlight, true)

		button_edit.set_texture_normal(edit_icon)
		button_edit.set_custom_minimum_size(icon_size)
		button_edit.set_h_size_flags(Control.SIZE_SHRINK_END)
		button_edit.set_visible(resource != null)
		button_edit.tooltip_text = "Edit Asset"
		button_edit.mouse_filter = Control.MOUSE_FILTER_PASS
		button_edit.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
		button_edit.pressed.connect(_on_edit)
		button_row.add_child(button_edit, true)

		spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		spacer.mouse_filter = Control.MOUSE_FILTER_PASS
		button_row.add_child(spacer, true)

		button_clear.set_texture_normal(clear_icon)
		button_clear.set_custom_minimum_size(icon_size)
		button_clear.set_h_size_flags(Control.SIZE_SHRINK_END)
		button_clear.set_visible(resource != null)
		button_clear.tooltip_text = "Clear Asset"
		button_clear.mouse_filter = Control.MOUSE_FILTER_PASS
		button_clear.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
		button_clear.pressed.connect(_on_clear)
		button_row.add_child(button_clear, true)


	func destroy_buttons() -> void:
		# Mesh entries parent button_enabled under button_row, while texture entries
		# leave it unparented. Free the unparented control explicitly, then free the
		# row once; its parented controls are destroyed with it.
		if is_instance_valid(button_enabled) and button_enabled.get_parent() == null:
			button_enabled.free()
		if is_instance_valid(button_row):
			button_row.free()
		button_row = null
		button_enabled = null
		button_highlight = null
		button_edit = null
		spacer = null
		button_clear = null


	func get_resource_name() -> StringName:
		if resource:
			if resource is Terrain3DMeshAsset:
				return (resource as Terrain3DMeshAsset).get_asset_name()
			elif resource is Terrain3DTextureAsset:
				return (resource as Terrain3DTextureAsset).get_asset_name()
		return ""


	func setup_label() -> void:
		name_label = Label.new()
		name_label.name = "MeshLabel"
		name_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		name_label.size_flags_vertical = Control.SIZE_EXPAND_FILL
		name_label.add_theme_font_size_override("font_size", int(14. * EditorInterface.get_editor_scale()))
		name_label.add_theme_color_override("font_color", Color.WHITE)
		name_label.add_theme_color_override("font_shadow_color", Color.BLACK)
		name_label.add_theme_constant_override("shadow_offset_x", 1)
		name_label.add_theme_constant_override("shadow_offset_y", 1)
		name_label.visible = false
		name_label.autowrap_mode = TextServer.AUTOWRAP_OFF
		name_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		add_child(name_label, true)


	func setup_count_label() -> void:
		count_label = Label.new()
		count_label.name = "CountLabel"
		count_label.text = ""
		count_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		count_label.vertical_alignment = VERTICAL_ALIGNMENT_BOTTOM
		count_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		count_label.size_flags_vertical = Control.SIZE_EXPAND_FILL
		count_label.add_theme_font_size_override("font_size", int(14. * EditorInterface.get_editor_scale()))
		count_label.add_theme_color_override("font_color", Color.WHITE)
		count_label.add_theme_color_override("font_shadow_color", Color.BLACK)
		count_label.add_theme_constant_override("shadow_offset_x", 1)
		count_label.add_theme_constant_override("shadow_offset_y", 1)
		add_child(count_label, true)
		var mesh_resource: Terrain3DMeshAsset = resource as Terrain3DMeshAsset
		if not mesh_resource:
			return
		if not mesh_resource.instance_count_changed.is_connected(update_count_label):
			mesh_resource.instance_count_changed.connect(update_count_label)
		update_count_label()


	func update_count_label() -> void:
		var mesh_resource: Terrain3DMeshAsset = resource as Terrain3DMeshAsset
		if not type == Terrain3DAssets.TYPE_MESH or \
				( is_instance_valid(mesh_resource) and not mesh_resource.is_enabled() ):
			count_label.text = ""
			return
		if not mesh_resource:
			count_label.text = str(0)
		else:
			count_label.text = _format_number(mesh_resource.get_instance_count())


	func _notification(p_what: int) -> void:
		match p_what:
			NOTIFICATION_PREDELETE:
				_disconnect_resource_signals(resource)
				destroy_buttons()
			NOTIFICATION_DRAW:
				# Hide spacer if icons are crowding small textures
				spacer.visible = size.x > 94. or type == Terrain3DAssets.TYPE_TEXTURE
				var rect: Rect2 = Rect2(Vector2.ZERO, get_size())
				if !resource:
					draw_style_box(background, rect)
					draw_texture(add_icon, (get_size() / 2) - (add_icon.get_size() / 2))
				else:
					var texture_asset := resource as Terrain3DTextureAsset
					var mesh_asset := resource as Terrain3DMeshAsset
					_thumbnail = texture_asset.get_thumbnail() if is_instance_valid(texture_asset) else \
						mesh_asset.get_thumbnail() if is_instance_valid(mesh_asset) else null
					if _thumbnail:
						draw_texture_rect(_thumbnail, rect, false)
						texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS
					else:
						draw_rect(rect, Color(.15, .15, .15, 1.))
					if is_instance_valid(texture_asset):
						self_modulate = texture_asset.get_highlight_color() if is_highlighted else texture_asset.get_albedo_color()
					elif is_instance_valid(mesh_asset):
						button_enabled.set_pressed_no_signal(!mesh_asset.is_enabled())
						self_modulate = mesh_asset.get_highlight_color()
					button_highlight.self_modulate = Color("FC7F7F") if is_highlighted else Color.WHITE
				if drop_data:
					draw_style_box(focus_style, rect)
				if is_hovered:
					draw_rect(rect, Color(1, 1, 1, 0.2))
				if is_selected:
					draw_style_box(focus_style, rect)
			NOTIFICATION_MOUSE_ENTER:
				if not resource:
					name_label.visible = false
				else:
					name_label.visible = true
				is_hovered = true
				name_label.text = get_resource_name()
				tooltip_text = get_resource_name()
				emit_signal("hovered")
				queue_redraw()
			NOTIFICATION_MOUSE_EXIT:
				name_label.visible = false
				is_hovered = false
				drop_data = false
				queue_redraw()


	func _gui_input(p_event: InputEvent) -> void:
		var mouse_button := p_event as InputEventMouseButton
		if is_instance_valid(mouse_button) and mouse_button.is_pressed():
			match mouse_button.get_button_index():
				MOUSE_BUTTON_LEFT:
					# If `Add new` is clicked
					if !resource:
						if type == Terrain3DAssets.TYPE_TEXTURE:
							set_edited_resource(Terrain3DTextureAsset.new(), false)
						else:
							set_edited_resource(Terrain3DMeshAsset.new(), false)
						_on_edit()
					else:
						emit_signal("clicked")
				MOUSE_BUTTON_RIGHT:
					if resource:
						_on_edit()


	func _can_drop_data(_p_at_position: Vector2, p_data: Variant) -> bool:
		drop_data = false
		if p_data is Dictionary:
			var drag_data: Dictionary = p_data
			var files: Variant = drag_data.get("files")
			if files is PackedStringArray and (files as PackedStringArray).size() == 1:
				queue_redraw()
				drop_data = true
		return drop_data


	func _drop_data(_p_at_position: Vector2, p_data: Variant) -> void:
		if not _can_drop_data(Vector2.ZERO, p_data):
			return
		var drag_data: Dictionary = p_data
		var files: PackedStringArray = drag_data["files"]
		var res: Resource = load(files[0])
		if res is Texture2D and type == Terrain3DAssets.TYPE_TEXTURE:
			var ta := Terrain3DTextureAsset.new()
			var current_texture := resource as Terrain3DTextureAsset
			if is_instance_valid(current_texture):
				ta.id = current_texture.id
			ta.set_albedo_texture(res)
			set_edited_resource(ta, false)
		elif res is Terrain3DTextureAsset and type == Terrain3DAssets.TYPE_TEXTURE:
			var texture_res := res as Terrain3DTextureAsset
			var current_texture := resource as Terrain3DTextureAsset
			if is_instance_valid(current_texture):
				texture_res.id = current_texture.id
			set_edited_resource(texture_res, false)
		elif res is PackedScene and type == Terrain3DAssets.TYPE_MESH:
			var current_mesh := resource as Terrain3DMeshAsset
			if not is_instance_valid(current_mesh):
				current_mesh = Terrain3DMeshAsset.new()
			set_edited_resource(current_mesh, false)
			current_mesh.set_scene_file(res)
		elif res is Terrain3DMeshAsset and type == Terrain3DAssets.TYPE_MESH:
			var mesh_res := res as Terrain3DMeshAsset
			var current_mesh := resource as Terrain3DMeshAsset
			if is_instance_valid(current_mesh):
				mesh_res.id = current_mesh.id
			set_edited_resource(mesh_res, false)
		emit_signal("clicked")
		emit_signal("inspected", resource)

	func _disconnect_resource_signals(p_resource: Resource) -> void:
		if not is_instance_valid(p_resource):
			return
		var texture_asset := p_resource as Terrain3DTextureAsset
		var mesh_asset := p_resource as Terrain3DMeshAsset
		if is_instance_valid(texture_asset):
			if texture_asset.setting_changed.is_connected(_on_resource_changed):
				texture_asset.setting_changed.disconnect(_on_resource_changed)
			if texture_asset.file_changed.is_connected(_on_resource_changed):
				texture_asset.file_changed.disconnect(_on_resource_changed)
		elif is_instance_valid(mesh_asset):
			if mesh_asset.setting_changed.is_connected(_on_resource_changed):
				mesh_asset.setting_changed.disconnect(_on_resource_changed)
			if mesh_asset.instancer_setting_changed.is_connected(_on_resource_changed):
				mesh_asset.instancer_setting_changed.disconnect(_on_resource_changed)
			if mesh_asset.instance_count_changed.is_connected(update_count_label):
				mesh_asset.instance_count_changed.disconnect(update_count_label)


	func _connect_resource_signals(p_resource: Resource) -> void:
		if not is_instance_valid(p_resource):
			return
		var texture_asset := p_resource as Terrain3DTextureAsset
		var mesh_asset := p_resource as Terrain3DMeshAsset
		if is_instance_valid(texture_asset):
			if not texture_asset.setting_changed.is_connected(_on_resource_changed):
				texture_asset.setting_changed.connect(_on_resource_changed)
			if not texture_asset.file_changed.is_connected(_on_resource_changed):
				texture_asset.file_changed.connect(_on_resource_changed)
		elif is_instance_valid(mesh_asset):
			if not mesh_asset.setting_changed.is_connected(_on_resource_changed):
				mesh_asset.setting_changed.connect(_on_resource_changed)
			if not mesh_asset.instancer_setting_changed.is_connected(_on_resource_changed):
				mesh_asset.instancer_setting_changed.connect(_on_resource_changed)
			if is_instance_valid(count_label) and not mesh_asset.instance_count_changed.is_connected(update_count_label):
				mesh_asset.instance_count_changed.connect(update_count_label)


	func set_edited_resource(p_res: Resource, p_no_signal: bool = true) -> void:
		if resource != p_res:
			var previous: Resource = resource
			_disconnect_resource_signals(previous)
			resource = p_res
			_connect_resource_signals(resource)
			resource_replaced.emit(previous, resource)

		var has_resource: bool = is_instance_valid(resource)
		if button_enabled:
			button_enabled.set_visible(has_resource and type == Terrain3DAssets.TYPE_MESH)
		if button_highlight:
			button_highlight.set_visible(has_resource)
		if button_edit:
			button_edit.set_visible(has_resource)
		if button_clear:
			button_clear.set_visible(has_resource)
		if is_instance_valid(count_label):
			update_count_label()

		queue_redraw()
		if not p_no_signal:
			emit_signal("changed", resource)


	func _on_resource_changed(_value: int = 0) -> void:
		queue_redraw()
		emit_signal("changed", resource)


	func set_selected(value: bool) -> void:
		if not is_inside_tree():
			#push_error("not in tree")
			return
		is_selected = value
		if is_selected:
			# Handle scrolling to show the selected item.
			await get_tree().process_frame
			if is_inside_tree():
				var item_parent := get_parent() as Control
				var scroll := item_parent.get_parent() as ScrollContainer if item_parent else null
				if is_instance_valid(scroll) and item_parent.size.y > 0.0:
					scroll.get_v_scroll_bar().ratio = position.y / item_parent.size.y
		queue_redraw()


	func _on_clear() -> void:
		if resource:
			clear_requested.emit(resource)


	func _on_edit() -> void:
		emit_signal("clicked")
		emit_signal("inspected", resource)


	func _on_enable() -> void:
		var mesh_asset := resource as Terrain3DMeshAsset
		if is_instance_valid(mesh_asset):
			mesh_asset.set_enabled(!mesh_asset.is_enabled())


	func _on_highlight() -> void:
		is_highlighted = !is_highlighted
		var texture_asset := resource as Terrain3DTextureAsset
		var mesh_asset := resource as Terrain3DMeshAsset
		if is_instance_valid(texture_asset):
			texture_asset.set_highlighted(is_highlighted)
		elif is_instance_valid(mesh_asset):
			mesh_asset.set_highlighted(is_highlighted)


	func _format_number(num: int) -> String:
		var is_negative: bool = num < 0
		var str_num: String = str(absi(num))
		var result: String = ""
		var length: int = str_num.length()
		for i: int in length:
			result = str_num[length - 1 - i] + result
			if i < length - 1 and (i + 1) % 3 == 0:
				result = "," + result
		return "-" + result if is_negative else result
