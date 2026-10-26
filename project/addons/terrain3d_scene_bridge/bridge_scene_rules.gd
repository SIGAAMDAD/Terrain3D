@tool
extends RefCounted

const MAX_LOD_COUNT: int = 10
const SUPPORTED_EXTENSIONS: PackedStringArray = ["glb", "gltf", "tscn", "scn"]
const META_INSTANCE_CATEGORY: StringName = &"terrain3d_instance_category"
const INSTANCE_CATEGORY_CLUTTER: int = 0
const INSTANCE_CATEGORY_GROUND_COVER: int = 1
const INSTANCE_CATEGORY_OCCLUDER: int = 2


func is_supported_scene_path(scene_path: String) -> bool:
	if scene_path.is_empty():
		return false
	return SUPPORTED_EXTENSIONS.has(scene_path.get_extension().to_lower())


func type_name_from_path(scene_path: String) -> String:
	return safe_node_name(scene_path.get_file().get_basename())


func mesh_asset_key(scene_path: String, mesh_path: NodePath, instance_category: int = INSTANCE_CATEGORY_GROUND_COVER) -> String:
	return asset_key_with_category("%s::mesh::%s" % [scene_path, String(mesh_path)], instance_category)


func compound_asset_key(scene_path: String, instance_category: int = INSTANCE_CATEGORY_GROUND_COVER) -> String:
	return asset_key_with_category("%s::compound" % scene_path, instance_category)


func asset_key_with_category(base_key: String, instance_category: int) -> String:
	var marker: String = "::category::"
	var marker_index: int = base_key.rfind(marker)
	var stable_base: String = base_key.substr(0, marker_index) if marker_index >= 0 else base_key
	return "%s%s%d" % [stable_base, marker, clampi(instance_category, INSTANCE_CATEGORY_CLUTTER, INSTANCE_CATEGORY_OCCLUDER)]


func resolve_instance_category(source_node: Node, scene_path: String, stop_node: Node = null) -> int:
	var cursor: Node = source_node
	while cursor != null:
		if cursor.has_meta(META_INSTANCE_CATEGORY):
			var explicit_category: int = _category_from_value(cursor.get_meta(META_INSTANCE_CATEGORY))
			if explicit_category >= 0:
				return explicit_category
		if cursor == stop_node:
			break
		cursor = cursor.get_parent()

	cursor = source_node.get_parent() if source_node != null else null
	while cursor != null:
		var semantic_category: int = _category_from_semantic_name(String(cursor.name))
		if semantic_category >= 0:
			return semantic_category
		if cursor == stop_node:
			break
		cursor = cursor.get_parent()

	var path_category: int = _category_from_source_path(scene_path)
	if path_category >= 0:
		return path_category
	return INSTANCE_CATEGORY_GROUND_COVER


func _category_from_value(value: Variant) -> int:
	var value_type: int = typeof(value)
	if value_type == TYPE_INT:
		var numeric: int = int(value)
		return numeric if numeric >= INSTANCE_CATEGORY_CLUTTER and numeric <= INSTANCE_CATEGORY_OCCLUDER else -1
	if value_type == TYPE_STRING or value_type == TYPE_STRING_NAME:
		return _category_from_semantic_name(String(value))
	return -1


func _category_from_semantic_name(value: String) -> int:
	var token: String = _normalized_category_token(value)
	if token == "clutter" or token == "foliage" or token == "foliages":
		return INSTANCE_CATEGORY_CLUTTER
	if token == "groundcover" or token == "groundcovers":
		return INSTANCE_CATEGORY_GROUND_COVER
	if token == "occluder" or token == "occluders" or token.ends_with("occluders"):
		return INSTANCE_CATEGORY_OCCLUDER
	return -1


func _category_from_source_path(scene_path: String) -> int:
	var found: Dictionary = {}
	var normalized_path: String = scene_path.replace("\\", "/")
	for component: String in normalized_path.split("/", false):
		var token: String = _normalized_category_token(component.get_basename())
		var category: int = -1
		if token.contains("groundcover"):
			category = INSTANCE_CATEGORY_GROUND_COVER
		elif token.contains("occluder"):
			category = INSTANCE_CATEGORY_OCCLUDER
		elif token.contains("clutter") or token.contains("foliage"):
			category = INSTANCE_CATEGORY_CLUTTER
		if category >= 0:
			found[category] = true
	if found.size() == 1:
		return int(found.keys()[0])
	return -1


func _normalized_category_token(value: String) -> String:
	var token: String = value.strip_edges().to_lower()
	for separator: String in [" ", "_", "-", "."]:
		token = token.replace(separator, "")
	return token


func is_explicit_lod_mesh_name(mesh_name: String) -> bool:
	var upper: String = mesh_name.to_upper()
	for lod_index: int in range(MAX_LOD_COUNT):
		if upper.ends_with("LOD%d" % lod_index):
			return true
	return false


func has_explicit_lod_meshes(mesh_nodes: Array[MeshInstance3D]) -> bool:
	for mesh_node: MeshInstance3D in mesh_nodes:
		if is_explicit_lod_mesh_name(String(mesh_node.name)):
			return true
	return false


func type_name_from_mesh(scene_path: String, mesh_path: NodePath, mesh_name: String) -> String:
	var scene_name: String = type_name_from_path(scene_path)
	var path_text: String = String(mesh_path)
	if path_text.is_empty() or path_text == ".":
		return scene_name
	var suffix: String = mesh_name if not mesh_name.is_empty() else path_text
	return safe_node_name("%s__%s" % [scene_name, suffix])


func parse_region_name(node_name: String) -> Dictionary:
	var parts: PackedStringArray = node_name.split("_", false)
	if parts.size() != 3 or parts[0] != "Region":
		return {"ok": false}
	if not parts[1].is_valid_int() or not parts[2].is_valid_int():
		return {"ok": false}
	return {
		"ok": true,
		"location": Vector2i(parts[1].to_int(), parts[2].to_int()),
	}


func safe_node_name(value: String) -> String:
	var result: String = value.strip_edges()
	for invalid_character: String in [".", "/", "\\", ":", "*", "?", "\"", "<", ">", "|", "@", "%"]:
		result = result.replace(invalid_character, "_")
	if result.is_empty():
		result = "MeshType"
	return result


func validate_mesh_instance_for_terrain(mesh_instance: MeshInstance3D, source_label: String) -> String:
	if mesh_instance == null:
		return "Mesh source is null: %s" % source_label
	if mesh_instance.mesh == null:
		return "MeshInstance3D has no mesh and cannot be sent to Terrain3D: %s" % source_label
	return ""


func build_single_mesh_packed_scene(mesh_instance: MeshInstance3D, type_name: String) -> PackedScene:
	var result: Dictionary = build_single_mesh_packed_scene_result(mesh_instance, type_name)
	return result.get("scene", null) as PackedScene


func build_single_mesh_packed_scene_result(mesh_instance: MeshInstance3D, type_name: String) -> Dictionary:
	var result: Dictionary = {
		"scene": null,
		"shader_surface_count": 0,
		"fallback_surface_count": 0,
		"alpha_coerced_surface_count": 0,
	}
	if mesh_instance == null or mesh_instance.mesh == null:
		return result
	var duplicate_node: Node = mesh_instance.duplicate()
	if not duplicate_node is MeshInstance3D:
		if duplicate_node != null:
			duplicate_node.free()
		return result
	var clone: MeshInstance3D = duplicate_node as MeshInstance3D
	_strip_children(clone)
	clone.name = safe_node_name(type_name)
	clone.transform = Transform3D.IDENTITY
	clone.owner = null

	# Terrain3D's current mesh importer canonicalizes every imported surface into a
	# Terrain3DRenderingMaterial. Its native import path accepts BaseMaterial3D
	# only and rejects blended-alpha BaseMaterial3D modes. Canonicalize the active
	# MeshInstance3D materials here so set_scene_file() never receives a material
	# that the instancing renderer cannot represent.
	var mesh_copy: Mesh = clone.mesh.duplicate(true) as Mesh
	if mesh_copy == null:
		clone.free()
		return result
	for surface_index: int in range(mesh_copy.get_surface_count()):
		var source_material: Material = clone.get_active_material(surface_index)
		var material_result: Dictionary = _make_terrain3d_compatible_material(source_material)
		mesh_copy.surface_set_material(surface_index, material_result.get("material", null) as Material)
		if bool(material_result.get("shader", false)):
			result["shader_surface_count"] = int(result["shader_surface_count"]) + 1
		if bool(material_result.get("fallback", false)):
			result["fallback_surface_count"] = int(result["fallback_surface_count"]) + 1
		if bool(material_result.get("alpha_coerced", false)):
			result["alpha_coerced_surface_count"] = int(result["alpha_coerced_surface_count"]) + 1
	clone.mesh = mesh_copy
	clone.set_material_override(null)
	for override_index: int in range(clone.get_surface_override_material_count()):
		clone.set_surface_override_material(override_index, null)

	var packed_root: Node3D = Node3D.new()
	packed_root.name = safe_node_name(type_name)
	clone.name = "%s_LOD0" % safe_node_name(type_name)
	packed_root.add_child(clone)
	clone.owner = packed_root

	var source_occluder: OccluderInstance3D = _find_matching_occluder(mesh_instance)
	if source_occluder != null:
		var duplicate_occluder_node: Node = source_occluder.duplicate()
		var occluder_clone: OccluderInstance3D = duplicate_occluder_node as OccluderInstance3D
		if occluder_clone != null:
			_strip_children(occluder_clone)
			occluder_clone.name = "%s_Occluder" % safe_node_name(type_name)
			occluder_clone.transform = mesh_instance.global_transform.affine_inverse() * source_occluder.global_transform
			packed_root.add_child(occluder_clone)
			occluder_clone.owner = packed_root
		elif duplicate_occluder_node != null:
			duplicate_occluder_node.free()

	var packed_scene: PackedScene = PackedScene.new()
	var pack_error: Error = packed_scene.pack(packed_root)
	packed_root.free()
	if pack_error != OK:
		return result
	result["scene"] = packed_scene
	return result


func build_compound_packed_scene_result(scene_root: Node3D, type_name: String) -> Dictionary:
	var result: Dictionary = {
		"scene": null,
		"shader_surface_count": 0,
		"fallback_surface_count": 0,
		"alpha_coerced_surface_count": 0,
	}
	if scene_root == null:
		return result

	var mesh_nodes: Array[MeshInstance3D] = []
	_collect_mesh_instances(scene_root, mesh_nodes)
	if mesh_nodes.is_empty():
		return result

	# A multi-mesh GLB is one logical instanced object. Build one transient scene
	# containing all component meshes in scene-root local space. Terrain3D merges
	# this scene into one LOD0 mesh, so a building cannot be split across unrelated
	# mesh IDs and later alias another asset slot.
	var packed_root: Node3D = Node3D.new()
	packed_root.name = safe_node_name(type_name)
	var root_inverse: Transform3D = scene_root.global_transform.affine_inverse()
	var component_index: int = 0
	for source_mesh: MeshInstance3D in mesh_nodes:
		if source_mesh.mesh == null:
			continue
		var mesh_copy: Mesh = source_mesh.mesh.duplicate(true) as Mesh
		if mesh_copy == null:
			packed_root.free()
			return result
		for surface_index: int in range(mesh_copy.get_surface_count()):
			var source_material: Material = source_mesh.get_active_material(surface_index)
			var material_result: Dictionary = _make_terrain3d_compatible_material(source_material)
			mesh_copy.surface_set_material(surface_index, material_result.get("material", null) as Material)
			if bool(material_result.get("shader", false)):
				result["shader_surface_count"] = int(result["shader_surface_count"]) + 1
			if bool(material_result.get("fallback", false)):
				result["fallback_surface_count"] = int(result["fallback_surface_count"]) + 1
			if bool(material_result.get("alpha_coerced", false)):
				result["alpha_coerced_surface_count"] = int(result["alpha_coerced_surface_count"]) + 1

		var component: MeshInstance3D = MeshInstance3D.new()
		component.name = "Component_%03d_%s" % [component_index, safe_node_name(String(source_mesh.name))]
		component.mesh = mesh_copy
		component.transform = root_inverse * source_mesh.global_transform
		packed_root.add_child(component)
		component.owner = packed_root
		component_index += 1

	var occluder_nodes: Array[OccluderInstance3D] = []
	_collect_occluder_instances(scene_root, occluder_nodes)
	var occluder_index: int = 0
	for source_occluder: OccluderInstance3D in occluder_nodes:
		if not _is_valid_occluder_instance(source_occluder):
			continue
		var duplicate_occluder_node: Node = source_occluder.duplicate()
		var occluder_component: OccluderInstance3D = duplicate_occluder_node as OccluderInstance3D
		if occluder_component == null:
			if duplicate_occluder_node != null:
				duplicate_occluder_node.free()
			continue
		_strip_children(occluder_component)
		occluder_component.name = "Occluder_%03d_%s" % [occluder_index, safe_node_name(String(source_occluder.name))]
		occluder_component.transform = root_inverse * source_occluder.global_transform
		packed_root.add_child(occluder_component)
		occluder_component.owner = packed_root
		occluder_index += 1

	if component_index == 0:
		packed_root.free()
		return result
	var packed_scene: PackedScene = PackedScene.new()
	var pack_error: Error = packed_scene.pack(packed_root)
	packed_root.free()
	if pack_error != OK:
		return result
	result["scene"] = packed_scene
	return result


func _collect_mesh_instances(current: Node, output: Array[MeshInstance3D]) -> void:
	if current is MeshInstance3D:
		output.append(current as MeshInstance3D)
	for child: Node in current.get_children():
		_collect_mesh_instances(child, output)


func _collect_occluder_instances(current: Node, output: Array[OccluderInstance3D]) -> void:
	if current is OccluderInstance3D:
		output.append(current as OccluderInstance3D)
	for child: Node in current.get_children():
		_collect_occluder_instances(child, output)


func _is_valid_occluder_instance(occluder_instance: OccluderInstance3D) -> bool:
	if occluder_instance == null or occluder_instance.occluder == null:
		return false
	var vertices: PackedVector3Array = occluder_instance.occluder.get_vertices()
	var indices: PackedInt32Array = occluder_instance.occluder.get_indices()
	return vertices.size() >= 3 and indices.size() >= 3 and indices.size() % 3 == 0


func _find_matching_occluder(mesh_instance: MeshInstance3D) -> OccluderInstance3D:
	if mesh_instance == null or mesh_instance.get_parent() == null:
		return null
	var mesh_name: String = String(mesh_instance.name).to_lower()
	var only_occluder: OccluderInstance3D = null
	var valid_count: int = 0
	for sibling: Node in mesh_instance.get_parent().get_children():
		if not sibling is OccluderInstance3D:
			continue
		var candidate: OccluderInstance3D = sibling as OccluderInstance3D
		if not _is_valid_occluder_instance(candidate):
			continue
		only_occluder = candidate
		valid_count += 1
		var occluder_name: String = String(candidate.name).to_lower()
		if occluder_name.begins_with(mesh_name) or occluder_name.contains(mesh_name):
			return candidate
	return only_occluder if valid_count == 1 else null


func _make_terrain3d_compatible_material(material: Material) -> Dictionary:
	if material == null:
		return {"material": null, "shader": false, "fallback": false, "alpha_coerced": false}

	if material is BaseMaterial3D:
		var source_base: BaseMaterial3D = material as BaseMaterial3D
		var base_copy: BaseMaterial3D = source_base.duplicate(true) as BaseMaterial3D
		if base_copy == null:
			base_copy = StandardMaterial3D.new()
		var transparency: int = int(base_copy.get_transparency())
		var alpha_coerced: bool = transparency != BaseMaterial3D.TRANSPARENCY_DISABLED and transparency != BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		if alpha_coerced:
			base_copy.set_transparency(BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR)
		return {"material": base_copy, "shader": false, "fallback": false, "alpha_coerced": alpha_coerced}

	if material is ShaderMaterial:
		return _convert_shader_material_to_base(material as ShaderMaterial)

	# Terrain3D's instancer has no representation for arbitrary Material classes.
	# Use a neutral BaseMaterial3D so the mesh import remains valid and report the
	# fallback once in the bridge completion summary rather than emitting one error
	# per surface from Terrain3DMeshAsset.
	return {
		"material": StandardMaterial3D.new(),
		"shader": false,
		"fallback": true,
		"alpha_coerced": false,
	}


func _convert_shader_material_to_base(shader_material: ShaderMaterial) -> Dictionary:
	var proxy: StandardMaterial3D = StandardMaterial3D.new()
	var shader: Shader = shader_material.shader
	if shader == null:
		return {"material": proxy, "shader": true, "fallback": true, "alpha_coerced": false}

	var parameters: Dictionary = {}
	for uniform_variant in shader.get_shader_uniform_list(false):
		if not uniform_variant is Dictionary:
			continue
		var uniform_info: Dictionary = uniform_variant as Dictionary
		var uniform_name_text: String = String(uniform_info.get("name", ""))
		if uniform_name_text.is_empty():
			continue
		parameters[uniform_name_text.to_lower()] = shader_material.get_shader_parameter(StringName(uniform_name_text))

	var recognized: bool = false
	var albedo_texture: Texture2D = _find_shader_texture(parameters,
		["albedo_texture", "albedo_tex", "albedo_map", "albedo", "texture_albedo", "tex_albedo", "base_color_texture", "base_color_tex", "base_color_map", "diffuse_texture", "diffuse_tex", "diffuse_map", "main_texture", "main_tex"],
		["albedo", "basecolor", "base_color", "diffuse"]
	)
	if albedo_texture != null:
		proxy.set_texture(BaseMaterial3D.TEXTURE_ALBEDO, albedo_texture)
		recognized = true

	var use_normal_map_value = _find_shader_parameter(parameters, ["use_normal_map", "normal_enabled", "enable_normal_map"])
	var use_normal_map: bool = not (typeof(use_normal_map_value) == TYPE_BOOL and not bool(use_normal_map_value))
	var normal_texture: Texture2D = _find_shader_texture(parameters,
		["normal_texture", "normal_tex", "texture_normal", "tex_normal", "normal_map", "normalmap", "normal"],
		["normal", "nrm"]
	)
	if normal_texture != null and use_normal_map:
		proxy.set_texture(BaseMaterial3D.TEXTURE_NORMAL, normal_texture)
		recognized = true

	var use_orm_map_value = _find_shader_parameter(parameters, ["use_orm_map", "orm_enabled", "enable_orm_map"])
	var use_orm_map: bool = not (typeof(use_orm_map_value) == TYPE_BOOL and not bool(use_orm_map_value))
	var orm_texture: Texture2D = _find_shader_texture(parameters,
		["orm_texture", "orm_tex", "texture_orm", "tex_orm", "orm_map", "orm", "surface_texture", "surface_tex", "surface_map", "pbr_texture", "pbr_map"],
		["orm", "occlusionroughnessmetallic", "occlusion_roughness_metallic", "arm", "rma", "pbr"]
	)
	if orm_texture != null and use_orm_map:
		proxy.set_texture(BaseMaterial3D.TEXTURE_ORM, orm_texture)
		var ao_strength_value = _find_shader_parameter(parameters, ["ao_strength", "ambient_occlusion_strength", "occlusion_strength"])
		var ao_strength: float = 1.0
		if typeof(ao_strength_value) == TYPE_FLOAT or typeof(ao_strength_value) == TYPE_INT:
			ao_strength = clampf(float(ao_strength_value), 0.0, 1.0)
		proxy.set_ao_light_affect(ao_strength)
		recognized = true
	else:
		var roughness_texture: Texture2D = _find_shader_texture(parameters,
			["roughness_texture", "roughness_tex", "roughness_map"],
			["roughness"]
		)
		if roughness_texture != null:
			proxy.set_texture(BaseMaterial3D.TEXTURE_ROUGHNESS, roughness_texture)
			proxy.set_roughness_texture_channel(BaseMaterial3D.TEXTURE_CHANNEL_RED)
			recognized = true

	var height_texture: Texture2D = _find_shader_texture(parameters,
		["height_texture", "height_tex", "height_map", "displacement_texture", "displacement_tex", "displacement_map"],
		["height", "displacement", "disp"]
	)
	if height_texture != null:
		proxy.set_texture(BaseMaterial3D.TEXTURE_HEIGHTMAP, height_texture)
		recognized = true

	var albedo_color = _find_shader_parameter(parameters, ["albedo_color", "albedo_tint", "base_color", "base_colour", "tint_color", "tint", "color"])
	if typeof(albedo_color) == TYPE_COLOR:
		proxy.set_albedo(albedo_color)
		recognized = true
	var roughness_value = _find_shader_parameter(parameters, ["roughness", "roughness_value", "default_roughness"])
	if typeof(roughness_value) == TYPE_FLOAT or typeof(roughness_value) == TYPE_INT:
		proxy.set_roughness(clampf(float(roughness_value), 0.0, 1.0))
		recognized = true
	var metallic_value = _find_shader_parameter(parameters, ["metallic", "metalness", "metallic_value", "default_metallic"])
	if typeof(metallic_value) == TYPE_FLOAT or typeof(metallic_value) == TYPE_INT:
		proxy.set_metallic(clampf(float(metallic_value), 0.0, 1.0))
		recognized = true
	var normal_scale_value = _find_shader_parameter(parameters, ["normal_scale", "normal_strength"])
	if typeof(normal_scale_value) == TYPE_FLOAT or typeof(normal_scale_value) == TYPE_INT:
		proxy.set_normal_scale(maxf(float(normal_scale_value), 0.0))
		recognized = true

	var shader_code: String = shader.get_code().to_lower()
	# Preserve the authored semantics used by the project's shared rock/ground-cover
	# shader. Terrain3D still canonicalizes to Terrain3DRenderingMaterial, but these
	# metadata fields tell the native material compiler that ORM is absolute and
	# that albedo saturation is part of the authored material rather than a generic
	# StandardMaterial3D multiplier profile.
	var direct_surface_profile: bool = (
		parameters.has("albedo_map")
		and parameters.has("normal_map")
		and parameters.has("orm_map")
		and parameters.has("saturation")
		and shader_code.contains("nomad_sun_shape")
	)
	if direct_surface_profile:
		proxy.set_meta("terrain3d_direct_orm", orm_texture != null and use_orm_map)
		var saturation_value = _find_shader_parameter(parameters, ["saturation"])
		if typeof(saturation_value) == TYPE_FLOAT or typeof(saturation_value) == TYPE_INT:
			proxy.set_meta("terrain3d_albedo_saturation", clampf(float(saturation_value), 0.0, 1.5))
		proxy.set_meta("terrain3d_stylized_sun", true)
		var dielectric_value = _find_shader_parameter(parameters, ["dielectric_specular"])
		if typeof(dielectric_value) == TYPE_VECTOR3:
			var dielectric_vector: Vector3 = dielectric_value
			proxy.set_meta("terrain3d_dielectric_specular", clampf((dielectric_vector.x + dielectric_vector.y + dielectric_vector.z) / 3.0, 0.0, 1.0))
		elif typeof(dielectric_value) == TYPE_COLOR:
			var dielectric_color: Color = dielectric_value
			proxy.set_meta("terrain3d_dielectric_specular", clampf((dielectric_color.r + dielectric_color.g + dielectric_color.b) / 3.0, 0.0, 1.0))
		proxy.set_meta("terrain3d_shader_profile", "nomad_surface_v1")
	if shader_code.contains("cull_disabled"):
		proxy.set_cull_mode(BaseMaterial3D.CULL_DISABLED)
	if shader_code.contains("unshaded"):
		proxy.set_shading_mode(BaseMaterial3D.SHADING_MODE_UNSHADED)
	var alpha_threshold = _find_shader_parameter(parameters, ["alpha_scissor_threshold", "alpha_cutoff", "cutoff", "alpha_threshold"])
	if shader_code.contains("alpha_scissor_threshold") or typeof(alpha_threshold) == TYPE_FLOAT or typeof(alpha_threshold) == TYPE_INT:
		proxy.set_transparency(BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR)
		if typeof(alpha_threshold) == TYPE_FLOAT or typeof(alpha_threshold) == TYPE_INT:
			proxy.set_alpha_scissor_threshold(clampf(float(alpha_threshold), 0.0, 1.0))

	return {
		"material": proxy,
		"shader": true,
		"fallback": not recognized,
		"alpha_coerced": false,
	}


func _find_shader_parameter(parameters: Dictionary, aliases: Array) -> Variant:
	for alias_variant in aliases:
		var alias: String = String(alias_variant)
		if parameters.has(alias):
			return parameters[alias]
	return null


func _find_shader_texture(parameters: Dictionary, aliases: Array, keywords: Array) -> Texture2D:
	var direct = _find_shader_parameter(parameters, aliases)
	if direct is Texture2D:
		return direct as Texture2D
	for parameter_name_variant in parameters.keys():
		var parameter_name: String = String(parameter_name_variant)
		var value = parameters[parameter_name_variant]
		if not value is Texture2D:
			continue
		var texture: Texture2D = value as Texture2D
		var searchable: String = parameter_name.to_lower()
		if not texture.resource_path.is_empty():
			searchable += " " + texture.resource_path.get_file().to_lower()
		for keyword_variant in keywords:
			var keyword: String = String(keyword_variant)
			if searchable.contains(keyword):
				return texture
	return null


func instantiate_source_mesh(scene_path: String, mesh_path: NodePath) -> MeshInstance3D:
	var packed_scene: PackedScene = ResourceLoader.load(scene_path) as PackedScene
	if packed_scene == null or not packed_scene.can_instantiate():
		return null
	var source_root: Node = packed_scene.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	if source_root == null:
		return null
	var path_text: String = String(mesh_path)
	var source_mesh: Node = source_root if path_text.is_empty() or path_text == "." else source_root.get_node_or_null(mesh_path)
	if not source_mesh is MeshInstance3D:
		source_root.free()
		return null
	var duplicate_node: Node = source_mesh.duplicate()
	if not duplicate_node is MeshInstance3D:
		if duplicate_node != null:
			duplicate_node.free()
		source_root.free()
		return null
	var clone: MeshInstance3D = duplicate_node as MeshInstance3D
	_strip_children(clone)
	clone.transform = Transform3D.IDENTITY
	clone.owner = null
	source_root.free()
	return clone


func validate_packed_scene_for_terrain(packed_scene: PackedScene, scene_path: String) -> String:
	# Legacy v1.0-v1.2 whole-scene bindings still use this path on restore/rebake.
	var root: Node = packed_scene.instantiate()
	if root == null:
		return "Cannot instantiate mesh source: %s" % scene_path

	var lod_nodes: Array[Node] = root.find_children("*LOD?", "MeshInstance3D")
	var mesh_nodes: Array[Node] = root.find_children("*", "MeshInstance3D")
	if root is MeshInstance3D:
		mesh_nodes.append(root)
	if lod_nodes.size() > MAX_LOD_COUNT:
		root.free()
		return "%s contains %d LOD-named meshes; Terrain3D accepts at most %d." % [scene_path, lod_nodes.size(), MAX_LOD_COUNT]
	if not lod_nodes.is_empty():
		if mesh_nodes.size() != lod_nodes.size():
			root.free()
			return (
				"%s mixes %d *LOD# meshes with %d additional MeshInstance3D nodes. Terrain3D would ignore the additional meshes."
				% [scene_path, lod_nodes.size(), mesh_nodes.size() - lod_nodes.size()]
			)
		root.free()
		return ""

	if mesh_nodes.is_empty():
		root.free()
		return "%s contains no MeshInstance3D and cannot become a Terrain3D mesh asset." % scene_path
	if mesh_nodes.size() > 1:
		root.free()
		return (
			"%s contains %d MeshInstance3D nodes. Legacy whole-scene bindings cannot interpret these as one Terrain3D mesh asset."
			% [scene_path, mesh_nodes.size()]
		)
	root.free()
	return ""


func _strip_children(node: Node) -> void:
	var children: Array[Node] = []
	for child: Node in node.get_children():
		children.append(child)
	for child: Node in children:
		node.remove_child(child)
		child.free()
