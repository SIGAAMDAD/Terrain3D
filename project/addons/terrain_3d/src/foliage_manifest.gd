@tool
class_name Terrain3DFoliageManifest
extends Resource

const SCHEMA_VERSION: int = 1
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
	var attempts: int = 0
	while next_owner_id <= 2147483647 and is_used.is_valid() and bool(is_used.call(next_owner_id)):
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
