@tool
extends RefCounted

const IO = preload("res://addons/terrain_3d/tools/foliage_io_worker.gd")
var used_owners: Dictionary = {}
var frozen_sources: Dictionary = {}
var instance_revisions: Dictionary = {}
var _terrain: Terrain3D
var _targets: Array[Vector2i] = []
var _regions: Array = []
var _worker
var _index: int = 0
var _waiting: bool = false
var _cancelled: bool = false
var _stopping: bool = false
var _owners_needed: bool = true
var error: String = ""
var _stamp: int = 0

func begin(terrain: Terrain3D, targets: Array[Vector2i], owners_needed: bool) -> bool:
	_terrain = terrain
	_targets = targets.duplicate()
	_owners_needed = owners_needed
	frozen_sources = terrain.data.capture_foliage_sources(_targets, 4)
	_stamp = terrain.data.get_foliage_source_stamp()
	_regions = terrain.data.get_region_locations() if owners_needed else targets.duplicate()
	_worker = IO.new()
	return _worker.start()

func poll() -> Dictionary:
	if _cancelled:
		_stopping = true
	if _stopping:
		if not _worker.stopped():
			return {"running": true, "phase": "cancelling" if _cancelled else "preparing"}
		_worker.finish()
		return {"running": false, "ok": error.is_empty() and not _cancelled, "error": error, "cancelled": _cancelled}
	if _waiting:
		var result: Dictionary = _worker.poll()
		if result.is_empty():
			return {"running": true, "index": _index, "count": _regions.size()}
		_waiting = false
		if not bool(result.get("ok", false)):
			return _fail(String(result.get("error", "Cannot read foliage ownership")))
		used_owners.merge(result.get("owners", {}))
		_index += 1
	if not valid_sources():
		return _fail("Terrain sources or region topology changed during foliage preparation.")
	if _index >= _regions.size():
		_stopping = true
		_worker.stop()
		return {"running": true, "index": _index, "count": _regions.size()}
	var location: Vector2i = _regions[_index]
	var region: Terrain3DRegion = _terrain.data.get_region(location)
	if region == null:
		return _fail("Foliage source region was unloaded: " + str(location))
	instance_revisions[location] = [region.get_instance_id(), region.get_foliage_instance_revision()]
	if not _owners_needed or region.get_instance_count() == 0:
		_index += 1
	else:
		var page: PackedByteArray = region.get_instance_page()
		if page.is_empty() or page.size() > IO.MAX_FILE_BYTES:
			return _fail("Instance ownership page is unavailable or too large: " + str(location))
		_waiting = _worker.submit("owners", {"page": page})
	return {"running": true, "index": _index, "count": _regions.size()}

func valid_sources() -> bool:
	if not is_instance_valid(_terrain):
		return false
	var current_stamp: int = _terrain.data.get_foliage_source_stamp()
	var topology: int = int(_terrain.data.capture_foliage_sources([], 0)["topology"])
	if current_stamp == _stamp and topology == int(frozen_sources["topology"]):
		return true
	if _terrain.data.capture_foliage_sources(_targets, 4) != frozen_sources:
		return false
	_stamp = current_stamp
	return true

func valid_instances() -> bool:
	if not is_instance_valid(_terrain):
		return false
	for location: Vector2i in instance_revisions:
		var region: Terrain3DRegion = _terrain.data.get_region(location)
		var revision: Array = instance_revisions[location]
		if region == null or region.get_instance_id() != int(revision[0]) or region.get_foliage_instance_revision() != int(revision[1]):
			return false
	return true

func owner_is_used(owner: int) -> bool:
	return used_owners.has(owner)

func cancel() -> void:
	_cancelled = true
	_worker.stop()

func shutdown() -> void:
	if _worker != null:
		_worker.shutdown()

func _fail(message: String) -> Dictionary:
	error = message
	_stopping = true
	_worker.stop()
	return {"running": true, "phase": "stopping"}
