# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
# Channel Packer for Terrain3D
extends RefCounted

const WINDOW_SCENE: String = "res://addons/terrain_3d/menu/channel_packer.tscn"
const TEMPLATE_PATH: String = "res://addons/terrain_3d/menu/channel_packer_import_template.txt"
const DRAG_DROP_SCRIPT: String = "res://addons/terrain_3d/menu/channel_packer_dragdrop.gd"
const EditorUI = preload("res://addons/terrain_3d/src/editor_ui_util.gd")
enum {
	INFO,
	WARN,
	ERROR,
}

enum {
	IMAGE_ALBEDO,
	IMAGE_HEIGHT,
	IMAGE_NORMAL,
	IMAGE_ROUGHNESS,
	IMAGE_AO
}

enum OutputFormat {
	PNG_GODOT_IMPORT,
	DDS_GPU_NATIVE,
}

var plugin: Terrain3DEditorPluginBase
var window: Window
var save_file_dialog: EditorFileDialog
var open_file_dialog: EditorFileDialog
var invert_green_checkbox: CheckBox
var invert_smooth_checkbox: CheckBox
var invert_height_checkbox: CheckBox
var normalize_height_checkbox: CheckBox
var lumin_height_button: Button
var generate_mipmaps_checkbox: CheckBox
var high_quality_checkbox: CheckBox
var output_format_option: OptionButton
var output_memory_label: Label
var align_normals_checkbox: CheckBox
var resize_toggle_checkbox: CheckBox
var resize_option_box: SpinBox
var height_channel: Array[Button]
var height_channel_selected: int = 0
var roughness_channel: Array[Button]
var roughness_channel_selected: int = 0
var occlusion_channel: Array[Button]
var occlusion_channel_selected: int = 0
var last_opened_directory: String
var last_saved_directory: String
var packing_albedo: bool = false
var queue_pack_normal_roughness: bool = false
var images: Array[Image] = [null, null, null, null, null]
var status_label: Label
var no_op: Callable = func() -> void: pass
var last_file_selected_fn: Callable = no_op
var normal_vector: Vector3


func pack_textures_popup() -> void:
	if is_instance_valid(window):
		window.show()
		window.grab_focus()
		EditorUI.configure_window(
			window,
			Vector2i(int(EditorUI.scaled(1100.0)), int(EditorUI.scaled(760.0))),
			Vector2i(int(EditorUI.scaled(720.0)), int(EditorUI.scaled(560.0)))
		)
		return

	# Set background color to match theme
	window = (load(WINDOW_SCENE) as PackedScene).instantiate() as Window
	var base_color: Color = plugin.editor_settings.get_setting("interface/theme/base_color")
	var panel_style := StyleBoxFlat.new()
	panel_style.bg_color = base_color
	window.get_node("PanelContainer").set("theme_override_styles/panel", panel_style)

	# Connect button signals
	lumin_height_button = window.get_node("%LuminanceAsHeightButton") as Button

	invert_height_checkbox = window.get_node("%ConvertDepthToHeight") as CheckBox
	normalize_height_checkbox = window.get_node("%NormalizeHeight") as CheckBox

	invert_green_checkbox = window.get_node("%InvertGreenChannelCheckBox") as CheckBox
	align_normals_checkbox = window.get_node("%AlignNormalsCheckBox") as CheckBox

	invert_smooth_checkbox = window.get_node("%InvertSmoothCheckBox") as CheckBox

	resize_toggle_checkbox = window.get_node("%ResizeToggle") as CheckBox
	resize_option_box = window.get_node("%ResizeOptionBox") as SpinBox
	resize_toggle_checkbox.toggled.connect(func(val: bool) -> void: resize_option_box.set_visible(val))
	generate_mipmaps_checkbox = window.get_node("%GenerateMipmapsCheckBox") as CheckBox
	high_quality_checkbox = window.get_node("%HighQualityCheckBox") as CheckBox
	output_format_option = window.get_node("%OutputFormatOption") as OptionButton
	output_memory_label = window.get_node("%OutputMemoryLabel") as Label
	output_format_option.clear()
	output_format_option.add_item("PNG + Godot VRAM import", OutputFormat.PNG_GODOT_IMPORT)
	output_format_option.add_item("DDS GPU-native (BC1 albedo / BC7 surface / R8 displacement)", OutputFormat.DDS_GPU_NATIVE)
	output_format_option.select(OutputFormat.PNG_GODOT_IMPORT)
	output_format_option.item_selected.connect(_on_output_format_selected)
	generate_mipmaps_checkbox.toggled.connect(func(_enabled: bool) -> void: _update_output_estimate())
	resize_toggle_checkbox.toggled.connect(func(_enabled: bool) -> void: _update_output_estimate())
	resize_option_box.value_changed.connect(func(_value: float) -> void: _update_output_estimate())

	(window.get_node("%PackButton") as Button).pressed.connect(_on_pack_button_pressed)
	status_label = window.get_node("%StatusLabel") as Label
	(window.get_node("%CloseButton") as Button).pressed.connect(_on_close_requested)
	window.close_requested.connect(_on_close_requested)
	window.window_input.connect(func(event: InputEvent) -> void:
		if event is InputEventKey:
			if event.pressed and event.keycode == KEY_ESCAPE:
				_on_close_requested()
		)

	height_channel = [
		window.get_node("%HeightChannelR") as Button,
		window.get_node("%HeightChannelG") as Button,
		window.get_node("%HeightChannelB") as Button,
		window.get_node("%HeightChannelA") as Button
	]
	roughness_channel = [
		window.get_node("%RoughnessChannelR") as Button,
		window.get_node("%RoughnessChannelG") as Button,
		window.get_node("%RoughnessChannelB") as Button,
		window.get_node("%RoughnessChannelA") as Button
	]
	occlusion_channel = [
		window.get_node("%AOChannelR") as Button,
		window.get_node("%AOChannelG") as Button,
		window.get_node("%AOChannelB") as Button,
		window.get_node("%AOChannelA") as Button
	]

	height_channel[0].pressed.connect(func() -> void: height_channel_selected = 0)
	height_channel[1].pressed.connect(func() -> void: height_channel_selected = 1)
	height_channel[2].pressed.connect(func() -> void: height_channel_selected = 2)
	height_channel[3].pressed.connect(func() -> void: height_channel_selected = 3)

	roughness_channel[0].pressed.connect(func() -> void: roughness_channel_selected = 0)
	roughness_channel[1].pressed.connect(func() -> void: roughness_channel_selected = 1)
	roughness_channel[2].pressed.connect(func() -> void: roughness_channel_selected = 2)
	roughness_channel[3].pressed.connect(func() -> void: roughness_channel_selected = 3)

	occlusion_channel[0].pressed.connect(func() -> void: occlusion_channel_selected = 0)
	occlusion_channel[1].pressed.connect(func() -> void: occlusion_channel_selected = 1)
	occlusion_channel[2].pressed.connect(func() -> void: occlusion_channel_selected = 2)
	occlusion_channel[3].pressed.connect(func() -> void: occlusion_channel_selected = 3)

	plugin.add_child(window)
	EditorUI.configure_window(
		window,
		Vector2i(int(EditorUI.scaled(1100.0)), int(EditorUI.scaled(760.0))),
		Vector2i(int(EditorUI.scaled(720.0)), int(EditorUI.scaled(560.0)))
	)
	_apply_accessible_control_sizes(window)
	_apply_ui_documentation()
	_init_file_dialogs()

	# the dialog disables the parent window "on top" so, restore it after 1 frame to alow the dialog to clear.
	var set_on_top_fn: Callable = func(_file: String = "") -> void:
		await plugin.get_tree().process_frame
		window.always_on_top = true
	save_file_dialog.file_selected.connect(set_on_top_fn)
	save_file_dialog.canceled.connect(set_on_top_fn)
	open_file_dialog.file_selected.connect(set_on_top_fn)
	open_file_dialog.canceled.connect(set_on_top_fn)

	_init_texture_picker(window.get_node("%AlbedoVBox"), IMAGE_ALBEDO)
	_init_texture_picker(window.get_node("%HeightVBox"), IMAGE_HEIGHT)
	_init_texture_picker(window.get_node("%NormalVBox"), IMAGE_NORMAL)
	_init_texture_picker(window.get_node("%RoughnessVBox"), IMAGE_ROUGHNESS)
	_init_texture_picker(window.get_node("%AOVBox"), IMAGE_AO)
	_update_output_estimate()


func _apply_ui_documentation() -> void:
	var help: Dictionary[String, String] = {
		"AlbHtLabel": "Albedo + displacement input group. These channels become the terrain base-color and height/displacement assets.",
		"NrmRghLabel": "Normal + roughness + AO input group. These channels become Terrain3D's packed surface/PBR asset.",
		"PackingLabel": "Output controls shared by PNG authoring output and GPU-native DDS output.",
		"AlbedoLabel": "Base-color source. RGB is preserved as albedo; DDS output encodes it as BC1 sRGB.",
		"HeightLabel": "Displacement source. Select the channel containing height/depth data before packing.",
		"NormalLabel": "Tangent-space normal source. X/Y are packed into the optimized surface texture.",
		"RoughnessLabel": "Roughness or smoothness source. Select its channel and enable inversion when the source stores smoothness.",
		"AOLabel": "Optional ambient-occlusion source. Missing AO defaults to fully unoccluded.",
		"AlbedoSize": "Loaded albedo source dimensions in pixels.",
		"HeightSize": "Loaded displacement source dimensions in pixels.",
		"NormalSize": "Loaded normal source dimensions in pixels.",
		"RoughnessSize": "Loaded roughness/smoothness source dimensions in pixels.",
		"AOSize": "Loaded ambient-occlusion source dimensions in pixels.",
		"LuminanceAsHeightButton": "Generate a height source from albedo luminance for quick authoring or preview use.",
		"HeightChannelLabel": "Choose which source channel contains displacement.",
		"HeightChannelR": "Use the red source channel as displacement.",
		"HeightChannelG": "Use the green source channel as displacement.",
		"HeightChannelB": "Use the blue source channel as displacement.",
		"HeightChannelA": "Use the alpha source channel as displacement.",
		"ConvertDepthToHeight": "Invert the selected displacement channel after optional normalization so depth maps become height maps.",
		"NormalizeHeight": "Remap the selected displacement channel's observed minimum and maximum to 0..1 before packing.",
		"InvertGreenChannelCheckBox": "Invert tangent-space normal Y/green for sources authored with the opposite normal-map convention.",
		"AlignNormalsCheckBox": "Rotate the average normal toward +Z so Terrain3D detiling and UV rotation use an orthogonal tangent-space normal map.",
		"RoughnessChannelLabel": "Choose which source channel contains roughness or smoothness.",
		"RoughnessChannelR": "Use the red source channel as roughness/smoothness.",
		"RoughnessChannelG": "Use the green source channel as roughness/smoothness.",
		"RoughnessChannelB": "Use the blue source channel as roughness/smoothness.",
		"RoughnessChannelA": "Use the alpha source channel as roughness/smoothness.",
		"InvertSmoothCheckBox": "Invert the selected scalar channel so smoothness/gloss input becomes roughness. Applies identically to PNG and DDS output.",
		"AOChannelLabel": "Choose which source channel contains ambient occlusion.",
		"AOChannelR": "Use the red source channel as ambient occlusion.",
		"AOChannelG": "Use the green source channel as ambient occlusion.",
		"AOChannelB": "Use the blue source channel as ambient occlusion.",
		"AOChannelA": "Use the alpha source channel as ambient occlusion.",
		"ResizeToggle": "Resample all sources in each packed output to a common square resolution. Source images are duplicated first and are never modified in place.",
		"ResizeOptionBox": "Target square resolution used when Resize Packed Image is enabled.",
		"GenerateMipmapsCheckBox": "Generate the full mip chain before saving. Recommended for runtime terrain sampling and explicit distant texture LODs.",
		"HighQualityCheckBox": "For PNG output, request the high-quality Godot VRAM import profile in the generated .import file.",
		"OutputFormatLabel": "Select authoring PNG output or native compressed DDS output.",
		"OutputFormatOption": "PNG writes Terrain3D-compatible packed authoring textures. DDS writes BC1 albedo, BC7/BC3 surface, and R8 displacement that can remain compressed at runtime.",
		"OutputMemoryLabel": "Estimated GPU payload for the selected DDS sources, resolution, and mipmap setting.",
		"Instructions": "Pack Albedo+Height, Normal+Roughness(+AO), or both sets. When both are ready the save dialogs run sequentially.",
		"PackButton": "Validate the selected sources and pack all complete texture sets using the selected output format.",
		"StatusLabel": "Displays channel-packer validation, compression fallback, save, and reload-verification results.",
		"CloseButton": "Close the Terrain3D channel packer.",
	}
	for node_name: String in help:
		var control: Control = window.find_child(node_name, true, false) as Control
		if control:
			control.tooltip_text = help[node_name]


func _apply_accessible_control_sizes(p_root: Node) -> void:
	for child: Node in p_root.find_children("*", "", true, false):
		if child is LineEdit or child is SpinBox:
			EditorUI.configure_compact_control(child as Control)
		elif child is Button and not (child is TextureButton):
			EditorUI.configure_action_button(child as Button)


func _on_close_requested() -> void:
	last_file_selected_fn = no_op
	images = [null, null, null, null, null]
	window.queue_free()
	window = null


func _init_file_dialogs() -> void:
	save_file_dialog = EditorFileDialog.new()
	save_file_dialog.set_filters(PackedStringArray(["*.png"]))
	save_file_dialog.set_file_mode(EditorFileDialog.FILE_MODE_SAVE_FILE)
	save_file_dialog.access = EditorFileDialog.ACCESS_FILESYSTEM
	save_file_dialog.file_selected.connect(_on_save_file_selected)
	save_file_dialog.ok_button_text = "Save"
	EditorUI.configure_file_dialog(save_file_dialog)
	#save_file_dialog.transient = false
	#save_file_dialog.exclusive = false
	#save_file_dialog.popup_window = true

	open_file_dialog = EditorFileDialog.new()
	open_file_dialog.set_filters(PackedStringArray(
		["*.png", "*.bmp", "*.exr", "*.hdr", "*.jpg", "*.jpeg", "*.tga", "*.svg", "*.webp", "*.ktx", "*.dds"]))
	open_file_dialog.set_file_mode(EditorFileDialog.FILE_MODE_OPEN_FILE)
	open_file_dialog.access = EditorFileDialog.ACCESS_FILESYSTEM
	open_file_dialog.ok_button_text = "Open"
	EditorUI.configure_file_dialog(open_file_dialog)
	#open_file_dialog.transient = false
	#open_file_dialog.exclusive = false
	#open_file_dialog.popup_window = true

	window.add_child(save_file_dialog)
	window.add_child(open_file_dialog)


func _init_texture_picker(p_parent: Node, p_image_index: int) -> void:
	var source_names: PackedStringArray = ["albedo", "height/displacement", "normal", "roughness/smoothness", "ambient-occlusion"]
	var source_name: String = source_names[p_image_index] if p_image_index >= 0 and p_image_index < source_names.size() else "source"
	var line_edit: LineEdit = p_parent.find_child("LineEdit") as LineEdit
	var file_pick_button: Button = p_parent.find_child("PickButton") as Button
	var clear_button: Button = p_parent.find_child("ClearButton") as Button
	var texture_rect: TextureRect = p_parent.find_child("TextureRect") as TextureRect
	var texture_button_base: Button = p_parent.find_child("TextureButton") as Button
	line_edit.placeholder_text = "Choose or drop an image"
	line_edit.tooltip_text = "Filesystem path for the %s source. Files can also be dropped onto the preview." % source_name
	file_pick_button.tooltip_text = "Browse for the %s source image." % source_name
	clear_button.tooltip_text = "Clear the loaded %s source without modifying the file on disk." % source_name
	texture_button_base.tooltip_text = "Preview for the %s source. Click to browse, or drop one image file here." % source_name
	texture_button_base.set_script(load(DRAG_DROP_SCRIPT) as GDScript)
	var texture_button: Terrain3DChannelPackerDragDrop = texture_button_base as Terrain3DChannelPackerDragDrop
	if texture_button == null:
		push_error("Unable to initialize Terrain3D channel-packer drag/drop button")
		return

	var set_channel_fn: Callable = func(used_channels: int) -> void:
		var channel_count: int = 4
		# enum Image.UsedChannels
		match used_channels:
			Image.USED_CHANNELS_L, Image.USED_CHANNELS_R: channel_count = 1
			Image.USED_CHANNELS_LA, Image.USED_CHANNELS_RG: channel_count = 2
			Image.USED_CHANNELS_RGB: channel_count = 3
			Image.USED_CHANNELS_RGBA: channel_count = 4
		if p_image_index == IMAGE_HEIGHT:
			for i: int in 4:
				height_channel[i].visible = i < channel_count
			height_channel[0].button_pressed = true
			height_channel[0].pressed.emit()
		elif p_image_index == IMAGE_ROUGHNESS:
			for i: int in 4:
				roughness_channel[i].visible = i < channel_count
			roughness_channel[0].button_pressed = true
			roughness_channel[0].pressed.emit()
		elif p_image_index == IMAGE_AO:
			for i: int in 4:
				occlusion_channel[i].visible = i < channel_count
			occlusion_channel[0].button_pressed = true
			occlusion_channel[0].pressed.emit()

	var load_image_fn: Callable = func(path: String) -> void:
		var image: Image = Image.new()
		var error: int = OK
		# DDS files selected through ACCESS_FILESYSTEM may live outside res://, so
		# decode the file bytes directly instead of routing them through ResourceLoader.
		if path.get_extension().to_lower() == "dds":
			var bytes: PackedByteArray = FileAccess.get_file_as_bytes(path)
			error = image.load_dds_from_buffer(bytes) if not bytes.is_empty() else FAILED
			if error == OK and not image.is_empty():
				image.clear_mipmaps()
				if image.is_compressed():
					error = image.decompress()
			else:
				error = FAILED
		else:
			error = image.load(path)
		if error != OK:
			_show_message(ERROR, "Failed to load texture '" + path + "'")
			texture_rect.texture = null
			images[p_image_index] = null
		else:
			_show_message(INFO, "Loaded texture '" + path + "'")
			texture_rect.texture = ImageTexture.create_from_image(image)
			images[p_image_index] = image
			_set_wh_labels(p_image_index, image.get_width(), image.get_height())
			if p_image_index == IMAGE_NORMAL:
				_set_normal_vector(image)
			if p_image_index in [ IMAGE_HEIGHT, IMAGE_ROUGHNESS, IMAGE_AO ]:
				set_channel_fn.call(image.detect_used_channels())
			_update_output_estimate()

	var os_drop_fn: Callable = func(files: PackedStringArray) -> void:
		# OS drag drop holds mouse focus until released,
		# Get mouse pos and check directly if inside texture_rect
		var rect: Rect2 = texture_button.get_global_rect()
		var mouse_position: Vector2 = texture_button.get_global_mouse_position()
		if rect.has_point(mouse_position):
			if files.size() != 1:
				_show_message(ERROR, "Cannot load multiple files")
			else:
				line_edit.text = files[0]
				load_image_fn.call(files[0])

	var godot_drop_fn: Callable = func(path: String) -> void:
		path = ProjectSettings.globalize_path(path)
		line_edit.text = path
		load_image_fn.call(path)

	var open_fn: Callable = func() -> void:
		open_file_dialog.current_path = last_opened_directory
		if last_file_selected_fn != no_op:
			open_file_dialog.file_selected.disconnect(last_file_selected_fn)
		last_file_selected_fn = func(path: String) -> void:
			line_edit.text = path
			load_image_fn.call(path)
		open_file_dialog.file_selected.connect(last_file_selected_fn)
		open_file_dialog.popup_centered_ratio()

	var line_edit_submit_fn: Callable = func(path: String) -> void:
		line_edit.text = path
		load_image_fn.call(path)

	var clear_fn: Callable = func() -> void:
		line_edit.text = ""
		texture_rect.texture = null
		images[p_image_index] = null
		_set_wh_labels(p_image_index, -1, -1)
		_update_output_estimate()

	line_edit.text_submitted.connect(line_edit_submit_fn)
	file_pick_button.pressed.connect(open_fn)
	texture_button.pressed.connect(open_fn)
	clear_button.pressed.connect(clear_fn)
	texture_button.dropped.connect(godot_drop_fn)
	window.files_dropped.connect(os_drop_fn)

	if p_image_index == IMAGE_HEIGHT:
		var lumin_fn: Callable = func() -> void:
			if !images[IMAGE_ALBEDO]:
				_show_message(ERROR, "Albedo Image Required for Operation")
			else:
				line_edit.text = "Generated Height"
				var height_texture: Image = Terrain3DUtil.luminance_to_height(images[IMAGE_ALBEDO])
				if height_texture.is_empty():
					_show_message(ERROR, "Height Texture Generation error")
				# blur the image by resizing down and back..
				var w: int = height_texture.get_width()
				var h: int = height_texture.get_height()
				height_texture.resize(w / 4, h / 4)
				height_texture.resize(w, h, Image.INTERPOLATE_CUBIC)
				# "Load" the height texture
				images[IMAGE_HEIGHT] = height_texture
				texture_rect.texture = ImageTexture.create_from_image(images[IMAGE_HEIGHT])
				_set_wh_labels(IMAGE_HEIGHT, height_texture.get_width(), height_texture.get_height())
				set_channel_fn.call(Image.USED_CHANNELS_R)
				_show_message(INFO, "Height Texture generated sucsessfully")
		lumin_height_button.pressed.connect(lumin_fn)
	EditorUI.set_button_editor_icon(file_pick_button, "Folder")
	EditorUI.set_button_editor_icon(clear_button, "Remove")


func _set_wh_labels(p_image_index: int, width: int, height: int) -> void:
	match p_image_index:
		0:
			(window.get_node("%AlbedoSize") as Label).text = "(%d, %d)" % [ width, height ]
		1:
			(window.get_node("%HeightSize") as Label).text = "(%d, %d)" % [ width, height ]
		2:
			(window.get_node("%NormalSize") as Label).text = "(%d, %d)" % [ width, height ]
		3:
			(window.get_node("%RoughnessSize") as Label).text = "(%d, %d)" % [ width, height ]
		4:
			(window.get_node("%AOSize") as Label).text = "(%d, %d)" % [ width, height ]


func _show_message(p_level: int, p_text: String) -> void:
	status_label.text = p_text
	match p_level:
		INFO:
			print("Terrain3DChannelPacker: " + p_text)
			status_label.add_theme_color_override("font_color", Color(0, 0.82, 0.14))
		WARN:
			push_warning("Terrain3DChannelPacker: " + p_text)
			status_label.add_theme_color_override("font_color", Color(0.9, 0.9, 0))
		ERROR,_:
			push_error("Terrain3DChannelPacker: " + p_text)
			status_label.add_theme_color_override("font_color", Color(0.9, 0, 0))


func _create_import_file(png_path: String) -> void:
	var dst_import_path: String = png_path + ".import"
	var file: FileAccess = FileAccess.open(TEMPLATE_PATH, FileAccess.READ)
	var template_content: String = file.get_as_text()
	file.close()
	template_content = template_content.replace(
		"$SOURCE_FILE", png_path).replace(
		"$HIGH_QUALITY", str(high_quality_checkbox.button_pressed)).replace(
		"$GENERATE_MIPMAPS", str(generate_mipmaps_checkbox.button_pressed)
	)
	var import_content: String = template_content
	file = FileAccess.open(dst_import_path, FileAccess.WRITE)
	file.store_string(import_content)
	file.close()


func _is_dds_output() -> bool:
	return is_instance_valid(output_format_option) and output_format_option.get_selected_id() == OutputFormat.DDS_GPU_NATIVE


func _on_output_format_selected(_p_index: int) -> void:
	high_quality_checkbox.visible = not _is_dds_output()
	_update_output_estimate()


func _configure_save_dialog(p_surface: bool) -> void:
	if _is_dds_output():
		save_file_dialog.set_filters(PackedStringArray(["*.dds"]))
		if p_surface:
			save_file_dialog.current_path = last_saved_directory + "terrain_surface.dds"
			save_file_dialog.title = "Save Terrain Packed Surface DDS"
		else:
			save_file_dialog.current_path = last_saved_directory + "terrain_albedo.dds"
			save_file_dialog.title = "Save Terrain Albedo DDS (also writes companion displacement DDS)"
	else:
		save_file_dialog.set_filters(PackedStringArray(["*.png"]))
		if p_surface:
			save_file_dialog.current_path = last_saved_directory + "packed_normal_roughness.png"
			save_file_dialog.title = "Save Packed Normal/Roughness Texture"
		else:
			save_file_dialog.current_path = last_saved_directory + "packed_albedo_height.png"
			save_file_dialog.title = "Save Packed Albedo/Height Texture"


func _on_pack_button_pressed() -> void:
	packing_albedo = images[IMAGE_ALBEDO] != null and images[IMAGE_HEIGHT] != null
	var packing_normal_roughness: bool = images[IMAGE_NORMAL] != null and images[IMAGE_ROUGHNESS] != null

	if not packing_albedo and not packing_normal_roughness:
		_show_message(WARN, "Please select an albedo and height texture or a normal and roughness texture")
		return
	if packing_albedo:
		_configure_save_dialog(false)
		save_file_dialog.popup_centered_ratio()
		if packing_normal_roughness:
			queue_pack_normal_roughness = true
		return
	if packing_normal_roughness:
		_configure_save_dialog(true)
		save_file_dialog.popup_centered_ratio()


func _on_save_file_selected(p_dst_path: String) -> void:
	last_saved_directory = p_dst_path.get_base_dir() + "/"
	var error: int
	if _is_dds_output():
		if packing_albedo:
			error = _pack_albedo_displacement_dds(images[IMAGE_ALBEDO], images[IMAGE_HEIGHT], p_dst_path)
		else:
			error = _pack_surface_dds(images[IMAGE_NORMAL], images[IMAGE_ROUGHNESS], images[IMAGE_AO], p_dst_path)
	else:
		if packing_albedo:
			error = _pack_textures(images[IMAGE_ALBEDO], images[IMAGE_HEIGHT], null, p_dst_path, false,
				invert_height_checkbox.button_pressed, false, normalize_height_checkbox.button_pressed, height_channel_selected)
		else:
			error = _pack_textures(images[IMAGE_NORMAL], images[IMAGE_ROUGHNESS], images[IMAGE_AO], p_dst_path,
				invert_green_checkbox.button_pressed, invert_smooth_checkbox.button_pressed,
				align_normals_checkbox.button_pressed, false, roughness_channel_selected, occlusion_channel_selected)

	if error == OK:
		# Only ask the editor filesystem to discover files that were actually saved
		# inside the project. Waiting for resources_reimported can deadlock the packer
		# for filesystem DDS exports because no import event is required to occur.
		var localized_path: String = ProjectSettings.localize_path(p_dst_path)
		if localized_path.begins_with("res://"):
			EditorInterface.get_resource_filesystem().scan()
	else:
		queue_pack_normal_roughness = false

	if error == OK and queue_pack_normal_roughness:
		queue_pack_normal_roughness = false
		packing_albedo = false
		_configure_save_dialog(true)
		save_file_dialog.popup_centered_ratio.call_deferred()
		save_file_dialog.grab_focus.call_deferred()


func _target_size_for(p_image: Image) -> Vector2i:
	if resize_toggle_checkbox.button_pressed:
		var side: int = maxi(128, int(resize_option_box.value))
		return Vector2i(side, side)
	return p_image.get_size() if p_image else Vector2i.ZERO


func _prepare_authoring_image(p_image: Image, p_size: Vector2i, p_format: Image.Format) -> Image:
	if not p_image:
		return null
	var result: Image = p_image.duplicate()
	if result.is_compressed() and result.decompress() != OK:
		return null
	if result.has_mipmaps():
		result.clear_mipmaps()
	if result.get_size() != p_size:
		result.resize(p_size.x, p_size.y, Image.INTERPOLATE_LANCZOS)
	if result.get_format() != p_format:
		result.convert(p_format)
	return result


func _save_and_validate_dds(p_image: Image, p_path: String, p_expected_formats: Array[int]) -> Error:
	var error: Error = p_image.save_dds(p_path)
	if error != OK:
		_show_message(ERROR, "DDS write failed for '" + p_path + "' (error %d)." % error)
		return error
	var bytes: PackedByteArray = FileAccess.get_file_as_bytes(p_path)
	var reloaded: Image = Image.new()
	error = reloaded.load_dds_from_buffer(bytes)
	if error != OK or reloaded.is_empty():
		_show_message(ERROR, "DDS reload validation failed for '" + p_path + "'.")
		return FAILED
	if reloaded.get_size() != p_image.get_size() or reloaded.get_format() not in p_expected_formats:
		_show_message(ERROR, "DDS reload changed size or compression format for '" + p_path + "'.")
		return FAILED
	if generate_mipmaps_checkbox.button_pressed != reloaded.has_mipmaps():
		_show_message(ERROR, "DDS reload changed the requested mipmap layout for '" + p_path + "'.")
		return FAILED
	return OK


func _pack_albedo_displacement_dds(p_albedo: Image, p_height: Image, p_dst_path: String) -> Error:
	if not p_albedo or not p_height:
		return FAILED
	var target_size: Vector2i = _target_size_for(p_albedo)
	if not resize_toggle_checkbox.button_pressed and p_height.get_size() != target_size:
		_show_message(ERROR, "Albedo and displacement textures must be the same size, or enable resize.")
		return FAILED
	var albedo: Image = _prepare_authoring_image(p_albedo, target_size, Image.FORMAT_RGB8)
	var height: Image = _prepare_authoring_image(p_height, target_size, Image.FORMAT_RGBA8)
	if not albedo or not height:
		_show_message(ERROR, "DDS source decompression or conversion failed.")
		return FAILED
	if generate_mipmaps_checkbox.button_pressed and albedo.generate_mipmaps() != OK:
		_show_message(ERROR, "Albedo mipmap generation failed.")
		return FAILED
	var compress_error: Error = albedo.compress(Image.COMPRESS_S3TC, Image.COMPRESS_SOURCE_SRGB)
	if compress_error != OK or albedo.get_format() != Image.FORMAT_DXT1:
		_show_message(ERROR, "BC1/S3TC albedo compression is unavailable in this Godot build.")
		return FAILED
	# Build both members of the DDS pair before publishing either one. A failed
	# height conversion must not leave a valid-looking orphan albedo asset behind.
	# Reuse the same native alpha-channel transformation used by PNG output so
	# Depth->Height and Normalize Height have identical semantics in DDS mode.
	var transformed_height: Image = Terrain3DUtil.pack_image(height, height, null, false,
		invert_height_checkbox.button_pressed, normalize_height_checkbox.button_pressed, height_channel_selected, 0)
	if not transformed_height:
		_show_message(ERROR, "Height channel conversion failed.")
		return FAILED
	var displacement: Image = Terrain3DUtil.pack_displacement_r8(transformed_height, 3,
		generate_mipmaps_checkbox.button_pressed)
	if not displacement or displacement.get_format() != Image.FORMAT_R8:
		_show_message(ERROR, "R8 displacement packing failed.")
		return FAILED
	var displacement_path: String = p_dst_path.get_basename() + "_displacement.dds"
	if _save_and_validate_dds(albedo, p_dst_path, [Image.FORMAT_DXT1]) != OK:
		return FAILED
	if _save_and_validate_dds(displacement, displacement_path, [Image.FORMAT_R8]) != OK:
		DirAccess.remove_absolute(p_dst_path)
		DirAccess.remove_absolute(displacement_path)
		_show_message(ERROR, "DDS pair publication failed; removed the incomplete albedo/displacement outputs.")
		return FAILED
	_show_message(INFO, "Saved BC1 sRGB albedo and R8 displacement DDS. Reload validation passed.")
	return OK


func _pack_surface_dds(p_normal: Image, p_roughness: Image, p_ao: Image, p_dst_path: String) -> Error:
	if not p_normal or not p_roughness:
		return FAILED
	var target_size: Vector2i = _target_size_for(p_normal)
	if not resize_toggle_checkbox.button_pressed:
		if p_roughness.get_size() != target_size or (p_ao and p_ao.get_size() != target_size):
			_show_message(ERROR, "Normal, roughness, and AO textures must be the same size, or enable resize.")
			return FAILED
	var normal: Image = _prepare_authoring_image(p_normal, target_size, Image.FORMAT_RGBA8)
	var roughness: Image = _prepare_authoring_image(p_roughness, target_size, Image.FORMAT_RGBA8)
	var ao: Image = _prepare_authoring_image(p_ao, target_size, Image.FORMAT_RGBA8) if p_ao else null
	if not normal or not roughness or (p_ao and not ao):
		_show_message(ERROR, "Packed-surface source decompression or conversion failed.")
		return FAILED
	if align_normals_checkbox.button_pressed and normal_vector.dot(Vector3(0.0, 0.0, 1.0)) < 0.999:
		_align_normals(normal)
	var packed_roughness_source: Image = roughness
	var packed_roughness_channel: int = roughness_channel_selected
	if invert_smooth_checkbox.button_pressed:
		packed_roughness_source = Terrain3DUtil.pack_image(roughness, roughness, null, false, true, false,
			roughness_channel_selected, 0)
		packed_roughness_channel = 3
		if not packed_roughness_source:
			_show_message(ERROR, "Smoothness to roughness conversion failed.")
			return FAILED
	var surface: Image = Terrain3DUtil.pack_surface_rgba8(normal, packed_roughness_source, ao,
		invert_green_checkbox.button_pressed, packed_roughness_channel, occlusion_channel_selected,
		generate_mipmaps_checkbox.button_pressed)
	if not surface or surface.get_format() != Image.FORMAT_RGBA8:
		_show_message(ERROR, "Terrain3D RGBA8 surface packing failed.")
		return FAILED

	var error: Error = surface.compress(Image.COMPRESS_BPTC, Image.COMPRESS_SOURCE_GENERIC)
	var expected_formats: Array[int] = [Image.FORMAT_BPTC_RGBA]
	var format_name: String = "BC7"
	if error != OK or surface.get_format() != Image.FORMAT_BPTC_RGBA:
		# BC3 is a supported four-channel fallback. It is reported explicitly because
		# its normal/roughness quality is below BC7 even though memory is identical.
		surface = Terrain3DUtil.pack_surface_rgba8(normal, packed_roughness_source, ao,
			invert_green_checkbox.button_pressed, packed_roughness_channel, occlusion_channel_selected,
			generate_mipmaps_checkbox.button_pressed)
		error = surface.compress_from_channels(Image.COMPRESS_S3TC, Image.USED_CHANNELS_RGBA)
		expected_formats = [Image.FORMAT_DXT5]
		format_name = "BC3"
		if error != OK or surface.get_format() != Image.FORMAT_DXT5:
			_show_message(ERROR, "Neither BC7/BPTC nor BC3/S3TC surface compression is available in this Godot build.")
			return FAILED
		_show_message(WARN, "BC7 is unavailable; using BC3 for the packed surface.")
	if _save_and_validate_dds(surface, p_dst_path, expected_formats) != OK:
		return FAILED
	_show_message(INFO, "Saved %s packed surface DDS (normal XY, roughness, AO). Reload validation passed." % format_name)
	return OK


func _dds_level_bytes(p_size: Vector2i, p_block_bytes: int) -> int:
	var total: int = 0
	var width: int = maxi(p_size.x, 1)
	var height: int = maxi(p_size.y, 1)
	while true:
		total += maxi(1, (width + 3) / 4) * maxi(1, (height + 3) / 4) * p_block_bytes
		if not generate_mipmaps_checkbox.button_pressed or (width == 1 and height == 1):
			break
		width = maxi(width / 2, 1)
		height = maxi(height / 2, 1)
	return total


func _r8_level_bytes(p_size: Vector2i) -> int:
	var total: int = 0
	var width: int = maxi(p_size.x, 1)
	var height: int = maxi(p_size.y, 1)
	while true:
		total += width * height
		if not generate_mipmaps_checkbox.button_pressed or (width == 1 and height == 1):
			break
		width = maxi(width / 2, 1)
		height = maxi(height / 2, 1)
	return total


func _format_bytes(p_bytes: int) -> String:
	if p_bytes >= 1024 * 1024:
		return "%.2f MiB" % (float(p_bytes) / float(1024 * 1024))
	return "%.1f KiB" % (float(p_bytes) / 1024.0)


func _update_output_estimate() -> void:
	if not is_instance_valid(output_memory_label):
		return
	if not _is_dds_output():
		output_memory_label.text = "PNG output; Godot import compression determines final VRAM usage."
		return
	var total: int = 0
	var parts: PackedStringArray = []
	if images[IMAGE_ALBEDO] and images[IMAGE_HEIGHT]:
		var albedo_size: Vector2i = _target_size_for(images[IMAGE_ALBEDO])
		var albedo_bytes: int = _dds_level_bytes(albedo_size, 8)
		var height_bytes: int = _r8_level_bytes(albedo_size)
		total += albedo_bytes + height_bytes
		parts.append("BC1 albedo %s" % _format_bytes(albedo_bytes))
		parts.append("R8 displacement %s" % _format_bytes(height_bytes))
	if images[IMAGE_NORMAL] and images[IMAGE_ROUGHNESS]:
		var surface_size: Vector2i = _target_size_for(images[IMAGE_NORMAL])
		var surface_bytes: int = _dds_level_bytes(surface_size, 16)
		total += surface_bytes
		parts.append("BC7 surface %s" % _format_bytes(surface_bytes))
	if parts.is_empty():
		output_memory_label.text = "DDS profile: BC1 albedo / BC7 surface / R8 displacement. Add source images for an estimate."
	else:
		output_memory_label.text = "DDS profile: %s. Estimated GPU payload: %s." % [", ".join(parts), _format_bytes(total)]


func _alignment_basis(normal: Vector3) -> Basis:
	var up: Vector3 = Vector3(0, 0, 1)
	var v: Vector3 = normal.cross(up)
	var c: float = normal.dot(up)
	var k: float = 1.0 / (1.0 + c)

	var vxy: float = v.x * v.y * k
	var vxz: float = v.x * v.z * k
	var vyz: float = v.y * v.z * k

	return Basis(Vector3(v.x * v.x * k + c, vxy - v.z, vxz + v.y),
		Vector3(vxy + v.z, v.y * v.y * k + c, vyz - v.x),
		Vector3(vxz - v.y, vyz + v.x, v.z * v.z * k + c)
	)


func _set_normal_vector(source: Image, quiet: bool = false) -> void:
	# Calculate texture normal sum direction
	var normal: Image = source
	var sum: Color = Color(0.0, 0.0, 0.0, 0.0)
	for x: int in normal.get_width():
		for y: int in normal.get_height():
			sum += normal.get_pixel(x, y)
	var div: float = normal.get_height() * normal.get_width()
	sum /= Color(div, div, div)
	sum *= 2.0
	sum -= Color(1.0, 1.0, 1.0)
	normal_vector = Vector3(sum.r, sum.g, sum.b).normalized()
	if normal_vector.dot(Vector3(0.0, 0.0, 1.0)) < 0.999 && !quiet:
		_show_message(WARN, "Normal Texture Not Orthoganol to UV plane.\nFor Compatability with Detiling and Rotation, Select Orthoganolize Normals")


func _align_normals(source: Image, iteration: int = 0) -> void:
	# generate matrix to re-align the normalmap
	var mat3: Basis = _alignment_basis(normal_vector)
	# re-align the normal map pixels
	for x: int in source.get_width():
		for y: int in source.get_height():
			var old_pixel: Color = source.get_pixel(x, y)
			var vector_pixel: Vector3 = Vector3(old_pixel.r, old_pixel.g, old_pixel.b)
			vector_pixel *= 2.0
			vector_pixel -= Vector3.ONE
			vector_pixel = vector_pixel.normalized()
			vector_pixel = vector_pixel * mat3
			vector_pixel += Vector3.ONE
			vector_pixel *= 0.5
			var new_pixel: Color = Color(vector_pixel.x, vector_pixel.y, vector_pixel.z, old_pixel.a)
			source.set_pixel(x, y, new_pixel)
	_set_normal_vector(source, true)
	if normal_vector.dot(Vector3(0.0, 0.0, 1.0)) < 0.999 && iteration < 3:
		iteration += 1
		_align_normals(source, iteration)


func _pack_textures(p_rgb_image: Image, p_a_image: Image, p_ao_image: Image, p_dst_path: String, p_invert_green: bool,
	p_invert_smooth: bool, p_align_normals: bool, p_normalize_height: bool, p_alpha_channel: int, p_occlusion_channel: int = 0) -> Error:
	if p_rgb_image and p_a_image:
		var rgb_image: Image = p_rgb_image.duplicate()
		var alpha_image: Image = p_a_image.duplicate()
		var ao_image: Image = p_ao_image.duplicate() if p_ao_image else null
		if rgb_image.get_size() != alpha_image.get_size() and !resize_toggle_checkbox.button_pressed:
			_show_message(ERROR, "Textures must be the same size.\nEnable resize to override image dimensions")
			return FAILED
		if p_ao_image:
			if rgb_image.get_size() != ao_image.get_size() and !resize_toggle_checkbox.button_pressed:
				_show_message(ERROR, "Textures must be the same size.\nEnable resize to override image dimensions")
				return FAILED

		if resize_toggle_checkbox.button_pressed:
			var size: int = maxi(128, int(resize_option_box.value))
			rgb_image.resize(size, size, Image.INTERPOLATE_CUBIC)
			alpha_image.resize(size, size, Image.INTERPOLATE_CUBIC)
			if ao_image:
				ao_image.resize(size, size, Image.INTERPOLATE_CUBIC)

		if p_align_normals and normal_vector.dot(Vector3(0.0, 0.0, 1.0)) < 0.999:
			_align_normals(rgb_image)
		elif p_align_normals:
			_show_message(INFO, "Alignment OK, skipping Normal Orthogonalization")

		var output_image: Image = Terrain3DUtil.pack_image(rgb_image, alpha_image, ao_image,
			p_invert_green, p_invert_smooth, p_normalize_height, p_alpha_channel, p_occlusion_channel)

		if not output_image:
			_show_message(ERROR, "Failed to pack textures")
			return FAILED
		if output_image.detect_alpha() != Image.ALPHA_BLEND:
			_show_message(WARN, "Warning, Alpha channel empty")

		var save_error: Error = output_image.save_png(p_dst_path)
		if save_error != OK:
			_show_message(ERROR, "PNG write failed for '%s' (error %d)." % [p_dst_path, save_error])
			return save_error
		_create_import_file(p_dst_path)
		_show_message(INFO, "Packed to " + p_dst_path + ".")
		return OK
	else:
		_show_message(ERROR, "Failed to load one or more textures")
		return FAILED
