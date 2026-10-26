# Editor-only dependency projections for committed Terrain3D source edits.
# No map ownership, cooked-file writes, or preview publication lives here.
@tool
extends RefCounted

const HEIGHT: int = 1 << 0
const CONTROL: int = 1 << 1
const COLOR: int = 1 << 2
const MATERIAL: int = 1 << 3
const RULES: int = 1 << 4
const TOPOLOGY: int = 1 << 5
const UNKNOWN_MAP: int = HEIGHT | CONTROL | COLOR

const AUTOSHADER: int = 1 << 0
const FOLIAGE: int = 1 << 1
const HLOD: int = 1 << 2
const FAR: int = 1 << 3
const COLLISION: int = 1 << 4

# One map can feed several downstream products. HLOD rebuilds only if its
# actual instancer/node source signatures change; this mask marks candidates.
static func products_for_sources(sources: int) -> int:
	var products: int = 0
	if sources & (HEIGHT | CONTROL | MATERIAL | RULES | TOPOLOGY):
		products |= AUTOSHADER
	if sources & (HEIGHT | CONTROL | MATERIAL | RULES | TOPOLOGY):
		products |= FOLIAGE
	if sources & (HEIGHT | MATERIAL | TOPOLOGY):
		products |= HLOD
	if sources & (HEIGHT | CONTROL | COLOR | MATERIAL | RULES | TOPOLOGY):
		products |= FAR
	if sources & (HEIGHT | CONTROL | TOPOLOGY):
		products |= COLLISION
	return products


# Native committed map types are the existing Terrain3DRegion constants.
# Unknown channels conservatively invalidate all map-dependent products.
static func source_for_map_type(map_type: int) -> int:
	match map_type:
		Terrain3DRegion.TYPE_HEIGHT:
			return HEIGHT
		Terrain3DRegion.TYPE_CONTROL:
			return CONTROL
		Terrain3DRegion.TYPE_COLOR:
			return COLOR
	return UNKNOWN_MAP


# The one-vertex halo covers slope derivatives, edge snapping and FAR gutters.
static func regions_for_area(terrain: Terrain3D, area: AABB, halo_samples: int = 1) -> Array[Vector2i]:
	var result: Array[Vector2i] = []
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.data):
		return result
	if area.size.x <= 0.0 or area.size.z <= 0.0:
		return result
	var region_width: float = float(terrain.region_size) * terrain.vertex_spacing
	if region_width <= 0.0:
		return result
	var padding: float = maxf(float(halo_samples) * terrain.vertex_spacing, 0.001)
	var left: int = floori((area.position.x - padding) / region_width)
	var top: int = floori((area.position.z - padding) / region_width)
	var right: int = floori((area.end.x + padding - 0.0001) / region_width)
	var bottom: int = floori((area.end.z + padding - 0.0001) / region_width)
	var half: int = int(Terrain3DData.REGION_MAP_SIZE) / 2
	left = maxi(left, -half)
	right = mini(right, half - 1)
	top = maxi(top, -half)
	bottom = mini(bottom, half - 1)
	for y: int in range(top, bottom + 1):
		for x: int in range(left, right + 1):
			var region := Vector2i(x, y)
			if terrain.data.has_region(region):
				result.append(region)
	return result


# Keep only actually committed source regions plus immediate sampling neighbors.
# The AABB determines which edge neighbors are within the one-vertex halo.
static func regions_for_committed_edit(terrain: Terrain3D, changed_regions: Array[Vector2i], area: AABB) -> Array[Vector2i]:
	var committed: Dictionary[Vector2i, bool] = {}
	for region: Vector2i in changed_regions:
		committed[region] = true
	var result: Array[Vector2i] = []
	if committed.is_empty():
		return result
	for region: Vector2i in regions_for_area(terrain, area):
		var affected: bool = false
		for dy: int in range(-1, 2):
			for dx: int in range(-1, 2):
				if committed.has(region + Vector2i(dx, dy)):
					affected = true
					break
			if affected:
				break
		if affected:
			result.append(region)
	return result


# HLOD clusters use the baker's fixed region-map origin and quadtree spans.
# Keys are consumed for invalidation/status; the HLOD builder still verifies
# its own per-source signatures before electing to recook any package.
static func hlod_keys(regions: Array[Vector2i], max_level: int) -> Dictionary[String, bool]:
	var result: Dictionary[String, bool] = {}
	var origin: int = -int(Terrain3DData.REGION_MAP_SIZE) / 2
	for region: Vector2i in regions:
		for level: int in range(1, clampi(max_level, 1, 6) + 1):
			var span: int = 1 << level
			var x: int = floori(float(region.x - origin) / float(span))
			var y: int = floori(float(region.y - origin) / float(span))
			result["%d:%d:%d" % [level, x, y]] = true
	return result


static func reconcile_regions(terrain: Terrain3D, dirty: Dictionary) -> void:
	if not is_instance_valid(terrain) or not is_instance_valid(terrain.data):
		dirty.clear()
		return
	for region: Vector2i in dirty.keys():
		if not terrain.data.has_region(region):
			dirty.erase(region)
