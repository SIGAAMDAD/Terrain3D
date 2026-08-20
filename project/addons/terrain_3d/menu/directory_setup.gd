# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Directory Setup for Terrain3D
extends Node

const DIRECTORY_SETUP: String = "res://addons/terrain_3d/menu/directory_setup.tscn"
const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")

var plugin: T3DEditorPluginBase
var dialog: ConfirmationDialog
var select_dir_btn: Button
var selected_dir_le: LineEdit
var validation_label: Label
var editor_file_dialog: EditorFileDialog


func _init() -> void:
	editor_file_dialog = EditorFileDialog.new()
	editor_file_dialog.file_mode = EditorFileDialog.FILE_MODE_OPEN_DIR
	editor_file_dialog.access = EditorFileDialog.ACCESS_RESOURCES
	editor_file_dialog.ok_button_text = "Choose Directory"
	editor_file_dialog.title = "Choose Terrain3D Data Directory"
	editor_file_dialog.dir_selected.connect(_on_dir_selected)
	editor_file_dialog.transient = false
	editor_file_dialog.exclusive = false
	editor_file_dialog.popup_window = true
	add_child(editor_file_dialog)


func directory_setup_popup() -> void:
	if is_instance_valid(dialog):
		dialog.queue_free()

	dialog = load(DIRECTORY_SETUP).instantiate() as ConfirmationDialog
	dialog.hide()
	dialog.dialog_hide_on_ok = false
	select_dir_btn = dialog.get_node("Margin/VBox/DirHBox/SelectDir") as Button
	selected_dir_le = dialog.get_node("Margin/VBox/DirHBox/LineEdit") as LineEdit
	validation_label = dialog.get_node("%ValidationLabel") as Label
	selected_dir_le.tooltip_text = "Terrain3D data directory for the selected terrain. Region resources and compatible cooked runtime data are loaded from this folder; changing it switches the terrain repository."
	select_dir_btn.tooltip_text = "Browse for the Terrain3D data directory assigned to the selected terrain."

	if is_instance_valid(plugin.terrain) and not plugin.terrain.data_directory.is_empty():
		selected_dir_le.text = plugin.terrain.data_directory

	EditorUI.set_button_editor_icon(select_dir_btn, "Folder")
	select_dir_btn.pressed.connect(_on_select_file_pressed)
	selected_dir_le.text_changed.connect(func(_text: String) -> void: _set_validation(""))
	dialog.canceled.connect(_on_close_requested)
	dialog.get_ok_button().pressed.connect(_on_ok_pressed)

	var preferred := Vector2i(int(EditorUI.scaled(680.0)), int(EditorUI.scaled(350.0)))
	var minimum := Vector2i(int(EditorUI.scaled(520.0)), int(EditorUI.scaled(300.0)))
	dialog.min_size = minimum
	EditorInterface.popup_dialog_centered(dialog, EditorUI.popup_size_for_screen(dialog, preferred, minimum))


func _on_close_requested() -> void:
	if is_instance_valid(dialog):
		dialog.queue_free()
	dialog = null


func _on_select_file_pressed() -> void:
	EditorUI.configure_file_dialog(editor_file_dialog)
	editor_file_dialog.popup_centered()


func _on_dir_selected(p_path: String) -> void:
	selected_dir_le.text = p_path
	selected_dir_le.caret_column = selected_dir_le.text.length()
	_set_validation("")


func _on_ok_pressed() -> void:
	if not is_instance_valid(plugin.terrain):
		_set_validation("Select a Terrain3D node before choosing its data directory.")
		return

	var path: String = selected_dir_le.text.strip_edges()
	if path.is_empty():
		_set_validation("Choose a data directory.")
		selected_dir_le.grab_focus()
		return
	if not DirAccess.dir_exists_absolute(path):
		_set_validation("Directory does not exist: %s" % path)
		selected_dir_le.grab_focus()
		return

	print("Setting terrain directory: ", path)
	plugin.terrain.data_directory = path
	_on_close_requested()


func _set_validation(p_message: String) -> void:
	if is_instance_valid(validation_label):
		validation_label.text = p_message
