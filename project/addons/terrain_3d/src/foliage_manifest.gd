@tool
class_name Terrain3DFoliageManifest
extends Resource

const SCHEMA_VERSION: int = 2
const FILE_NAME: String = "terrain3d_foliage_manifest.tres"
@export var terrain_identity: String = ""
@export var publication_generation: int = 0
@export var next_owner_id: int = 1073741824
# Persistent ledger. Each entry: {key, owner, source, seed, revision, region}.
@export var entries: Array[Dictionary] = []

func owner_for(key: String) -> int:
	for record: Dictionary in entries:
		if String(record.get("key", "")) == key:
			return int(record.get("owner", 0))
	return 0

func assign_owner(key: String, region: Vector2i, is_used: Callable = Callable()) -> int:
	var existing: int = owner_for(key)
	if existing > 0:
		return existing
	next_owner_id = maxi(next_owner_id, 1073741824)
	var attempts: int = 0
	while next_owner_id <= 2147483647 and (_has_owner(next_owner_id) or
			(is_used.is_valid() and bool(is_used.call(next_owner_id)))):
		next_owner_id += 1
		attempts += 1
		if attempts >= 1024:
			push_error("Cannot validate a free foliage owner ID; ensure instance pages are readable")
			return 0
	if next_owner_id > 2147483647:
		push_error("Foliage owner ID exhausted")
		return 0
	var allocated: int = next_owner_id
	next_owner_id += 1
	entries.append({"key": key, "owner": allocated, "region": region, "source": "", "seed": 0, "revision": 0})
	emit_changed()
	return allocated

func _has_owner(owner: int) -> bool:
	for record: Dictionary in entries:
		if int(record.get("owner", 0)) == owner:
			return true
	return false

# Allocation happens on a detached copy. Callers publish this manifest only after
# region publication succeeds. Sorted requests make collision resolution stable.
func prepare_owner_changes(requests: Array[Dictionary], regions: Array[Vector2i],
		mode: String, is_used: Callable = Callable()) -> Dictionary:
	var staged: Terrain3DFoliageManifest = duplicate(true) as Terrain3DFoliageManifest
	var removals: Array[Dictionary] = []
	var requested: Dictionary[String, bool] = {}
	for request: Dictionary in requests:
		var key: String = String(request["key"])
		if requested.has(key):
			return {"ok": false, "error": "Duplicate foliage rule identity: " + key}
		requested[key] = true
	# Reserve old owner IDs before pruning so no retirement can reuse an ID in this job.
	for request: Dictionary in requests:
		var owner: int = staged.assign_owner(String(request["key"]), request["region"], is_used)
		if owner <= 0:
			return {"ok": false, "error": "Unable to reserve foliage owner ID"}
	if mode == "regenerate" or mode == "clear":
		for record: Dictionary in entries:
			if regions.has(record["region"]) and not requested.has(String(record["key"])):
				removals.append(record.duplicate(true))
		for index: int in range(staged.entries.size() - 1, -1, -1):
			var record: Dictionary = staged.entries[index]
			if regions.has(record["region"]) and not requested.has(String(record["key"])):
				staged.entries.remove_at(index)
	return {"ok": true, "manifest": staged, "removals": removals}

func mark_baked(key: String, signature: String, seed: int) -> void:
	for index: int in range(entries.size()):
		if String(entries[index].get("key", "")) != key:
			continue
		var record: Dictionary = entries[index].duplicate(true)
		record["source"] = signature
		record["seed"] = seed
		record["revision"] = int(record.get("revision", 0)) + 1
		entries[index] = record
		emit_changed()
		return

func region_records(region: Vector2i) -> Array[Dictionary]:
	var selected: Array[Dictionary] = []
	for record: Dictionary in entries:
		if record.get("region", Vector2i(2147483647, 2147483647)) == region:
			selected.append(record)
	return selected
