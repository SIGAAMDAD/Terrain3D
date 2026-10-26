# Copyright (c) 2023-2026 Terrain3D contributors.
@tool
extends RefCounted
class_name Terrain3DHLODBuildSchedule

# Offline HLOD build ordering/validation. This is a planning contract, not a
# separate runtime hierarchy or a parallel worker implementation.
const PLAN_FORMAT: int = 1


static func job_id(level: int, cluster: Vector2i) -> String:
	return "%d:%d:%d" % [level, cluster.x, cluster.y]


static func sorted_clusters(plans: Dictionary) -> Array[Vector2i]:
	var keys: Array[Vector2i] = []
	for key: Vector2i in plans:
		keys.append(key)
	keys.sort_custom(func(a: Vector2i, b: Vector2i) -> bool:
		return a.x < b.x if a.x != b.x else a.y < b.y)
	return keys


# A child-first schedule is the only legal publication order. A parent may
# reference several child HLODs; every one must have been planned earlier.
# Missing/duplicate children, impossible parent cells and reversed workload
# order are rejected before the first output package is written.
static func validate_jobs(jobs: Array[Dictionary]) -> Dictionary:
	if jobs.is_empty():
		return {"ok": false, "error": "No HLOD jobs were scheduled"}
	var seen: Dictionary[String, Dictionary] = {}
	var expected_children: Dictionary[String, PackedStringArray] = {}
	var previous_level: int = -2
	for job: Dictionary in jobs:
		var level: int = int(job.get("level", 0))
		var cluster: Vector2i = job.get("cluster", Vector2i(-1, -1))
		var id: String = String(job.get("id", ""))
		if level < -1 or level > 6 or level < previous_level or cluster.x < 0 or cluster.y < 0:
			return {"ok": false, "error": "Invalid or unordered HLOD job %s" % id}
		if id != job_id(level, cluster) or seen.has(id):
			return {"ok": false, "error": "Duplicate or inconsistent HLOD job %s" % id}
		var dependencies: PackedStringArray = job.get("dependencies", PackedStringArray())
		var expected: PackedStringArray = expected_children.get(id, PackedStringArray())
		if expected.size() != dependencies.size():
			return {"ok": false, "error": "HLOD parent %s has an incomplete child list" % id}
		var unique_dependencies: Dictionary[String, bool] = {}
		for child_id: String in dependencies:
			if not seen.has(child_id) or unique_dependencies.has(child_id):
				return {"ok": false, "error": "Missing, late or duplicate HLOD child %s for %s" % [child_id, id]}
			var child: Dictionary = seen[child_id]
			var child_cluster: Vector2i = child["cluster"]
			if int(child["level"]) != level - 1 or \
					Vector2i(child_cluster.x >> 1, child_cluster.y >> 1) != cluster:
				return {"ok": false, "error": "HLOD child %s has incompatible parent %s" % [child_id, id]}
			unique_dependencies[child_id] = true
		seen[id] = job
		if level < 6:
			var parent_id: String = job_id(level + 1, Vector2i(cluster.x >> 1, cluster.y >> 1))
			var parent_children: PackedStringArray = expected_children.get(parent_id, PackedStringArray())
			parent_children.append(id)
			expected_children[parent_id] = parent_children
		previous_level = level
	# Every non-root child feeds one parent in each enabled hierarchy level.
	for parent_id: String in expected_children:
		var parent_level: int = int(parent_id.get_slice(":", 0))
		if parent_level <= previous_level and not seen.has(parent_id):
			return {"ok": false, "error": "Missing required HLOD parent %s" % parent_id}
	return {"ok": true, "count": jobs.size(), "format": PLAN_FORMAT}
