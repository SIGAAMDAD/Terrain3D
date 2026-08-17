# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends Node
class_name Terrain3DStreamingConverter

const MODE_META: StringName = &"terrain3d_stream_mode"
const MODE_STATIC: StringName = &"static"
const MODE_DYNAMIC: StringName = &"dynamic"
const MODE_IGNORE: StringName = &"ignore"


class SourceRecord extends RefCounted:
	var node: Node3D
	var parent: Node
	var index: int

	func _init(p_node: Node3D, p_parent: Node, p_index: int) -> void:
		node = p_node
		parent = p_parent
		index = p_index


class StaticGroup extends RefCounted:
	var transforms: Array[Transform3D] = []
	var colors: PackedColorArray = PackedColorArray()


class DynamicDescriptor extends RefCounted:
	var scene_path: String = ""
	var scene: PackedScene
	var transform: Transform3D = Transform3D.IDENTITY
	var state: Dictionary[StringName, Variant] = {}
	var flags: int = 0


class ConversionRecord extends RefCounted:
	var terrain: Terrain3D
	var roots: Array[SourceRecord] = []
	var static_groups: Dictionary[int, StaticGroup] = {}
	var planned_assets: Dictionary[int, Terrain3DMeshAsset] = {}
	var dynamic_nodes: Array[DynamicDescriptor] = []
	var unsupported: PackedStringArray = PackedStringArray()
	var spawn_ids: Array[int] = []
	var owner_id: int = 0


var plugin: Terrain3DEditorPluginBase
var _next_token: int = 1
var _records: Dictionary[int, ConversionRecord] = {}


func convert_selected() -> void:
	var terrain: Terrain3D = plugin.terrain if is_instance_valid(plugin) else null
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.instancer):
		push_error("Select a Terrain3D node before converting streamed objects")
		return
	var selected: Array[Node] = EditorInterface.get_selection().get_selected_nodes()
	var roots: Array[Node3D] = []
	for candidate: Node in selected:
		if not candidate is Node3D or candidate == terrain:
			continue
		var nested: bool = false
		for other: Node in selected:
			if other != candidate and other.is_ancestor_of(candidate):
				nested = true
				break
		if not nested:
			roots.push_back(candidate as Node3D)
	if roots.is_empty():
		push_error("Select one or more Node3D roots to convert")
		return

	var record := ConversionRecord.new()
	record.terrain = terrain
	record.owner_id = terrain.allocate_stream_owner_id()
	for root: Node3D in roots:
		var parent: Node = root.get_parent()
		if not parent:
			continue
		record.roots.push_back(SourceRecord.new(root, parent, root.get_index()))
		_scan_node(
			root,
			terrain,
			record.static_groups,
			record.planned_assets,
			record.dynamic_nodes,
			record.unsupported
		)
	if not record.unsupported.is_empty():
		push_error("Terrain3D streaming conversion stopped because these nodes need an explicit " +
			"terrain3d_stream_mode of dynamic or ignore:\n" + "\n".join(record.unsupported))
		return
	if record.static_groups.is_empty() and record.dynamic_nodes.is_empty():
		push_error("The selection contains no convertible meshes or dynamic Node3D scenes")
		return

	var token: int = _next_token
	_next_token += 1
	_records[token] = record
	var undo: EditorUndoRedoManager = plugin.get_undo_redo()
	undo.create_action("Convert Node Tree To Terrain3D Streaming")
	undo.add_do_method(self, &"_apply_conversion", token)
	undo.add_undo_method(self, &"_revert_conversion", token)
	for source: SourceRecord in record.roots:
		undo.add_do_reference(source.node)
	undo.commit_action()


func _scan_node(
	node: Node,
	terrain: Terrain3D,
	static_groups: Dictionary[int, StaticGroup],
	planned_assets: Dictionary[int, Terrain3DMeshAsset],
	dynamic_nodes: Array[DynamicDescriptor],
	unsupported: PackedStringArray
) -> bool:
	if node.has_meta(MODE_META):
		var explicit_mode := StringName(str(node.get_meta(MODE_META)).to_lower())
		if explicit_mode == MODE_IGNORE:
			return true
		if explicit_mode == MODE_DYNAMIC and node is Node3D:
			if _append_dynamic(node as Node3D, dynamic_nodes):
				return true
			unsupported.push_back(str(node.get_path()) + " [dynamic scene packing failed]")
			return false
		if explicit_mode == MODE_STATIC and node is MeshInstance3D:
			if not _append_static_mesh(node as MeshInstance3D, terrain, static_groups, planned_assets):
				unsupported.push_back(str(node.get_path()) + " [static mesh conversion failed]")
				return false
			return _scan_children(node, terrain, static_groups, planned_assets, dynamic_nodes, unsupported)
		if explicit_mode == MODE_STATIC and node is MultiMeshInstance3D:
			if not _append_static_multimesh(node as MultiMeshInstance3D, terrain, static_groups, planned_assets):
				unsupported.push_back(str(node.get_path()) + " [static MultiMesh conversion failed]")
				return false
			return _scan_children(node, terrain, static_groups, planned_assets, dynamic_nodes, unsupported)
		unsupported.push_back(str(node.get_path()) + " [invalid explicit mode for " + node.get_class() + "]")
		return false
	if node.get_script() != null:
		if node is Node3D:
			if _append_dynamic(node as Node3D, dynamic_nodes):
				return true
			unsupported.push_back(str(node.get_path()) + " [dynamic scene packing failed]")
			return false
		unsupported.push_back(str(node.get_path()) + " [scripted non-Node3D]")
		return false
	if node is MeshInstance3D:
		var mesh_node := node as MeshInstance3D
		if mesh_node.mesh:
			if not _append_static_mesh(mesh_node, terrain, static_groups, planned_assets):
				unsupported.push_back(str(node.get_path()) + " [static mesh conversion failed]")
				return false
			return _scan_children(node, terrain, static_groups, planned_assets, dynamic_nodes, unsupported)
	if node is MultiMeshInstance3D:
		var multimesh_node := node as MultiMeshInstance3D
		if multimesh_node.multimesh and multimesh_node.multimesh.mesh:
			if not _append_static_multimesh(multimesh_node, terrain, static_groups, planned_assets):
				unsupported.push_back(str(node.get_path()) + " [static MultiMesh conversion failed]")
				return false
			return _scan_children(node, terrain, static_groups, planned_assets, dynamic_nodes, unsupported)
	if node.get_class() != "Node" and node.get_class() != "Node3D":
		unsupported.push_back(str(node.get_path()) + " [" + node.get_class() + "]")
		return false
	return _scan_children(node, terrain, static_groups, planned_assets, dynamic_nodes, unsupported)


func _scan_children(
	node: Node,
	terrain: Terrain3D,
	static_groups: Dictionary[int, StaticGroup],
	planned_assets: Dictionary[int, Terrain3DMeshAsset],
	dynamic_nodes: Array[DynamicDescriptor],
	unsupported: PackedStringArray
) -> bool:
	var complete: bool = true
	for child: Node in node.get_children():
		if not _scan_node(child, terrain, static_groups, planned_assets, dynamic_nodes, unsupported):
			complete = false
	return complete


func _append_static_mesh(
	mesh_node: MeshInstance3D,
	terrain: Terrain3D,
	static_groups: Dictionary[int, StaticGroup],
	planned_assets: Dictionary[int, Terrain3DMeshAsset]
) -> bool:
	var overrides: Array[Material] = []
	for surface: int in mesh_node.mesh.get_surface_count():
		overrides.push_back(mesh_node.get_surface_override_material(surface))
	var mesh_id: int = _find_or_create_mesh_asset(
		terrain,
		mesh_node.mesh,
		mesh_node.name,
		mesh_node.cast_shadow,
		mesh_node.material_override,
		overrides,
		planned_assets
	)
	if mesh_id < 0:
		push_error("Terrain3D mesh asset capacity is exhausted; maximum is %d" % Terrain3DAssets.TERRAIN3D_MAX_MESH_ASSETS)
		return false
	_append_static_record(static_groups, mesh_id, mesh_node.global_transform, Color.WHITE)
	return true


func _append_static_multimesh(
	multimesh_node: MultiMeshInstance3D,
	terrain: Terrain3D,
	static_groups: Dictionary[int, StaticGroup],
	planned_assets: Dictionary[int, Terrain3DMeshAsset]
) -> bool:
	var multimesh: MultiMesh = multimesh_node.multimesh
	if not multimesh or not multimesh.mesh:
		return false
	if multimesh.transform_format != MultiMesh.TRANSFORM_3D:
		push_error("Terrain3D streaming conversion only accepts 3D MultiMesh transforms: %s" % multimesh_node.get_path())
		return false
	var mesh_id: int = _find_or_create_mesh_asset(
		terrain,
		multimesh.mesh,
		multimesh_node.name,
		multimesh_node.cast_shadow,
		multimesh_node.material_override,
		[],
		planned_assets
	)
	if mesh_id < 0:
		push_error("Terrain3D mesh asset capacity is exhausted; maximum is %d" % Terrain3DAssets.TERRAIN3D_MAX_MESH_ASSETS)
		return false
	var instance_count: int = multimesh.instance_count
	if multimesh.visible_instance_count >= 0:
		instance_count = mini(instance_count, multimesh.visible_instance_count)
	for instance_index: int in instance_count:
		var transform: Transform3D = multimesh_node.global_transform * multimesh.get_instance_transform(instance_index)
		var color: Color = multimesh.get_instance_color(instance_index) if multimesh.use_colors else Color.WHITE
		_append_static_record(static_groups, mesh_id, transform, color)
	return true


func _append_static_record(
	static_groups: Dictionary[int, StaticGroup],
	mesh_id: int,
	transform: Transform3D,
	color: Color
) -> void:
	if not static_groups.has(mesh_id):
		static_groups[mesh_id] = StaticGroup.new()
	var group: StaticGroup = static_groups[mesh_id]
	group.transforms.push_back(transform)
	group.colors.push_back(color)


func _append_dynamic(node: Node3D, dynamic_nodes: Array[DynamicDescriptor]) -> bool:
	var descriptor := DynamicDescriptor.new()
	descriptor.transform = node.global_transform
	if not node.scene_file_path.is_empty():
		descriptor.scene_path = node.scene_file_path
		dynamic_nodes.push_back(descriptor)
		return true
	var scene: PackedScene = _scene_for_node(node)
	if not scene:
		push_error("Could not pack dynamic streamed node: %s" % node.get_path())
		return false
	descriptor.scene = scene
	dynamic_nodes.push_back(descriptor)
	return true


func _mesh_key(
	mesh: Mesh,
	cast_shadow: int,
	material_override: Material,
	surface_overrides: Array[Material]
) -> String:
	var key: String = mesh.resource_path if not mesh.resource_path.is_empty() else str(mesh.get_rid())
	key += "|shadow=%d" % cast_shadow
	key += "|override=" + _resource_key(material_override)
	for surface: int in mesh.get_surface_count():
		var material: Material = surface_overrides[surface] if surface < surface_overrides.size() else null
		if not material:
			material = mesh.surface_get_material(surface)
		key += "|surface=" + _resource_key(material)
	return key


func _resource_key(resource: Resource) -> String:
	if not resource:
		return "null"
	return resource.resource_path if not resource.resource_path.is_empty() else str(resource.get_rid())


func _find_or_create_mesh_asset(
	terrain: Terrain3D,
	mesh: Mesh,
	source_name: String,
	cast_shadow: int,
	material_override: Material,
	surface_overrides: Array[Material],
	planned_assets: Dictionary[int, Terrain3DMeshAsset]
) -> int:
	var key: String = _mesh_key(mesh, cast_shadow, material_override, surface_overrides)
	for mesh_id: int in Terrain3DAssets.TERRAIN3D_MAX_MESH_ASSETS:
		var existing: Terrain3DMeshAsset = terrain.assets.get_mesh_asset(mesh_id) as Terrain3DMeshAsset
		if existing and existing.has_meta(&"terrain3d_source_key") and existing.get_meta(&"terrain3d_source_key") == key:
			return mesh_id
	for planned_id: int in planned_assets:
		var planned: Terrain3DMeshAsset = planned_assets[planned_id]
		if planned and planned.has_meta(&"terrain3d_source_key") and planned.get_meta(&"terrain3d_source_key") == key:
			return planned_id
	var free_id: int = -1
	for mesh_id: int in Terrain3DAssets.TERRAIN3D_MAX_MESH_ASSETS:
		if terrain.assets.get_mesh_asset(mesh_id) == null and not planned_assets.has(mesh_id):
			free_id = mesh_id
			break
	if free_id < 0:
		return -1

	var scene_root := Node3D.new()
	scene_root.name = "Terrain3DMeshAssetSource"
	var mesh_copy := MeshInstance3D.new()
	mesh_copy.name = source_name
	mesh_copy.mesh = mesh
	mesh_copy.cast_shadow = cast_shadow
	mesh_copy.material_override = material_override
	for surface: int in mesh.get_surface_count():
		if surface < surface_overrides.size() and surface_overrides[surface]:
			mesh_copy.set_surface_override_material(surface, surface_overrides[surface])
	scene_root.add_child(mesh_copy)
	mesh_copy.owner = scene_root
	var packed := PackedScene.new()
	if packed.pack(scene_root) != OK:
		scene_root.free()
		return -1
	scene_root.free()
	var asset := Terrain3DMeshAsset.new()
	asset.resource_name = source_name
	asset.scene_file = packed
	asset.set_meta(&"terrain3d_source_key", key)
	planned_assets[free_id] = asset
	return free_id


func _scene_for_node(source: Node3D) -> PackedScene:
	if not source.scene_file_path.is_empty():
		var loaded := load(source.scene_file_path) as PackedScene
		if loaded:
			return loaded
	var copy := source.duplicate(
		Node.DUPLICATE_SIGNALS | Node.DUPLICATE_GROUPS | Node.DUPLICATE_SCRIPTS | Node.DUPLICATE_USE_INSTANTIATION
	) as Node3D
	if not copy:
		return null
	copy.transform = Transform3D.IDENTITY
	_assign_owner_recursive(copy, copy)
	var packed := PackedScene.new()
	if packed.pack(copy) != OK:
		copy.free()
		return null
	copy.free()
	return packed


func _assign_owner_recursive(node: Node, scene_owner: Node) -> void:
	for child: Node in node.get_children():
		child.owner = scene_owner
		_assign_owner_recursive(child, scene_owner)


func _apply_conversion(token: int) -> void:
	if not _records.has(token):
		return
	var record: ConversionRecord = _records[token]
	var terrain: Terrain3D = record.terrain
	if not is_instance_valid(terrain):
		return
	for mesh_id: int in record.planned_assets:
		var planned: Terrain3DMeshAsset = record.planned_assets[mesh_id]
		var current: Terrain3DMeshAsset = terrain.assets.get_mesh_asset(mesh_id) as Terrain3DMeshAsset
		if current != null and current != planned:
			push_error("Terrain3D mesh asset slot %d changed before conversion redo" % mesh_id)
			return
	for mesh_id: int in record.planned_assets:
		terrain.assets.set_mesh_asset(mesh_id, record.planned_assets[mesh_id])
	for mesh_id: int in record.static_groups:
		var group: StaticGroup = record.static_groups[mesh_id]
		terrain.instancer.add_owned_transforms(
			mesh_id,
			group.transforms,
			record.owner_id,
			group.colors,
			true,
			false
		)
	var spawn_ids: Array[int] = []
	for descriptor: DynamicDescriptor in record.dynamic_nodes:
		var spawn_id: int = 0
		if not descriptor.scene_path.is_empty():
			spawn_id = terrain.add_streamed_scene_path(
				descriptor.scene_path,
				descriptor.transform,
				descriptor.state,
				descriptor.flags
			)
		else:
			spawn_id = terrain.add_streamed_scene(
				descriptor.scene,
				descriptor.transform,
				descriptor.state,
				descriptor.flags
			)
		if spawn_id > 0:
			spawn_ids.push_back(spawn_id)
	record.spawn_ids = spawn_ids
	for source: SourceRecord in record.roots:
		if is_instance_valid(source.node) and is_instance_valid(source.parent) and source.node.get_parent() == source.parent:
			source.parent.remove_child(source.node)


func _revert_conversion(token: int) -> void:
	if not _records.has(token):
		return
	var record: ConversionRecord = _records[token]
	var terrain: Terrain3D = record.terrain
	if is_instance_valid(terrain):
		terrain.instancer.remove_owned_instances(record.owner_id, true, false)
		for spawn_id: int in record.spawn_ids:
			terrain.remove_streamed_scene(spawn_id)
		for mesh_id: int in record.planned_assets:
			var planned: Terrain3DMeshAsset = record.planned_assets[mesh_id]
			if terrain.assets.get_mesh_asset(mesh_id) == planned and planned.get_instance_count() == 0:
				terrain.assets.set_mesh_asset(mesh_id, null)
	for source: SourceRecord in record.roots:
		if is_instance_valid(source.node) and is_instance_valid(source.parent) and source.node.get_parent() == null:
			source.parent.add_child(source.node)
			source.parent.move_child(source.node, mini(source.index, source.parent.get_child_count() - 1))
