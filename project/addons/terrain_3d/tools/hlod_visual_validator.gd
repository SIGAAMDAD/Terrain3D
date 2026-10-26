# Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
@tool
extends RefCounted

# Canonical HLOD render-only scene validator shared by baking and conversion.
static func validate(node: Node) -> String:
	if node.get_script() != null:
		return "scripted nodes are behavioral"
	if node is CollisionObject3D or node is CollisionShape3D or node is CollisionPolygon3D:
		return "physics/collision nodes must remain detailed"
	if node is AnimationPlayer or node is AnimationTree or node is Skeleton3D:
		return "animated/skeletal content is unsupported"
	if node is AudioStreamPlayer3D or node is GPUParticles3D or node is CPUParticles3D:
		return "audio/particle content is not render-only"
	if node is NavigationRegion3D or node is NavigationLink3D or node is NavigationObstacle3D:
		return "navigation content must remain detailed"
	if node is Light3D:
		return "lights are not HLOD proxy geometry"
	if node is MultiMeshInstance3D:
		return "MultiMesh content must use Terrain3D instancer HLOD"
	if node is MeshInstance3D:
		var mesh_node := node as MeshInstance3D
		if not is_instance_valid(mesh_node.mesh):
			return "MeshInstance3D has no mesh"
		if mesh_node.mesh.get_blend_shape_count() > 0:
			return "blend shapes are unsupported"
		if is_instance_valid(mesh_node.material_overlay):
			return "material overlays are unsupported"
	for child: Node in node.get_children():
		var reason: String = validate(child)
		if not reason.is_empty():
			return reason
	return ""
