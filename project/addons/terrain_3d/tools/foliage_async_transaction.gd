@tool
extends "res://addons/terrain_3d/tools/foliage_transaction.gd"

const IO = preload("res://addons/terrain_3d/tools/foliage_io_worker.gd")
var _worker
var _terrain: Terrain3D
var _before_ledger: Terrain3DFoliageManifest
var _after_ledger: Terrain3DFoliageManifest
var _guard: Callable
var _by_region: Dictionary = {}
var _locations: Array = []
var _before_pages: Dictionary = {}
var _after_pages: Dictionary = {}
var _expected: Dictionary = {}
var _operations: Array[Dictionary] = []
var _operation: Dictionary = {}
var _index: int = 0
var _stage: String = ""
var _waiting: bool = false
var _cancelled: bool = false
var _committed: bool = false
var _publication_attempted: bool = false
var _running: bool = false
var _memory_bytes: int = 0
var _memory_limit: int = 67108864
var _final_result: Dictionary = {}
var metrics: Dictionary = {"io_bytes": 0, "worker_usec": 0, "last_tick_usec": 0,
	"max_tick_usec": 0, "publication_usec": 0, "source_copy_bytes": 0}

func begin_async(terrain: Terrain3D, batches: Array, before: Terrain3DFoliageManifest,
		after: Terrain3DFoliageManifest, sources: Dictionary, guard: Callable, scratch_mb: int = 64) -> Dictionary:
	if FileAccess.file_exists(journal_path(terrain)):
		return _failure("Pending foliage recovery must complete first: " + journal_path(terrain))
	_terrain = terrain
	_before_ledger = before.duplicate(true) as Terrain3DFoliageManifest
	_after_ledger = after.duplicate(true) as Terrain3DFoliageManifest
	_guard = guard
	_memory_limit = clampi(scratch_mb, 8, 512) * 1048576
	var id: String = Crypto.new().generate_random_bytes(16).hex_encode()
	var directory: String = terrain.data_directory.path_join(ROOT_NAME).path_join(id)
	if DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory)) != OK:
		return _failure("Cannot create foliage transaction directory: " + directory)
	record = {"schema": SCHEMA, "id": id, "identity": identity(terrain), "directory": directory,
		"publication_generation": after.publication_generation,
		"project": ProjectSettings.globalize_path("res://").sha256_text(), "sources": sources.duplicate(true),
		"members": [], "regions": [], "ledger_before": directory.path_join("ledger_before.tres"),
		"ledger_after": directory.path_join("ledger_after.tres")}
	_pin(directory)
	for batch: Dictionary in batches:
		var region: Vector2i = batch["region"]
		var local: Array = _by_region.get(region, [])
		local.append(batch)
		_by_region[region] = local
	_locations = _by_region.keys()
	_locations.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.x < b.x or (a.x == b.x and a.y < b.y))
	_worker = IO.new()
	if not _worker.start():
		return _failure("Cannot start foliage staging worker")
	_queue("prune", {"root": terrain.data_directory.path_join(ROOT_NAME), "live": _live_directories.keys(),
		"limit": MAX_UNREFERENCED_ARCHIVES})
	_stage = "prepare"
	_running = true
	return {"ok": true, "running": true}

func poll_async(budgets: Dictionary = {}) -> Dictionary:
	var begin: int = Time.get_ticks_usec()
	if not _running:
		return _final_result
	if _cancelled or not is_instance_valid(_terrain):
		cancel_async()
	if _stage == "stopping":
		if not _worker.stopped():
			return {"running": true, "phase": "cancelling" if _cancelled else "finishing"}
		_worker.finish()
		if is_instance_valid(_terrain):
			_terrain.instancer.cancel_foliage_restore(String(record["id"]))
		if not _committed and not _publication_attempted and is_instance_valid(_terrain) and _owns_pending_journal():
			if not _phase("ROLLED_BACK") or not _clear_journal(_terrain):
				_final_result = _failure("Cancelled/failed before publication; recovery journal retained: " + journal_path(_terrain))
		_running = false
		# No live worker or native preparation can still read these buffers.
		_before_pages.clear()
		_after_pages.clear()
		_operations.clear()
		_operation = {}
		_guard = Callable()
		if _final_result.is_empty():
			_final_result = {"ok": false, "cancelled": true, "error": "Foliage cancelled before publication."}
		_final_result["running"] = false
		_final_result["metrics"] = metrics.duplicate()
		var completed: Dictionary = _final_result.duplicate()
		if bool(completed.get("ok", false)):
			completed["transaction"] = self
		return completed
	if _waiting:
		var result: Dictionary = _worker.poll()
		if result.is_empty():
			return {"running": true, "phase": _stage, "regions_prepared": _index, "region_count": _locations.size()}
		_waiting = false
		metrics["io_bytes"] += int(result.get("io_bytes", 0))
		metrics["worker_usec"] += int(result.get("worker_usec", 0))
		if not bool(result.get("ok", false)):
			_fail_async(String(result.get("error", "Foliage staging failed")))
			return {"running": true, "phase": "stopping"}
		if _stage == "journal":
			_stage = "publish"
		else:
			if not bool(result.get("done", true)):
				_operation["payload"]["offset"] = int(result["offset"])
				_operations.push_front(_operation)
			elif result.has("digest"):
				if _operation.has("expect") and String(result["digest"]) != String(_operation["expect"]):
					_fail_async("Snapshot verification failed: " + String(_operation["payload"].get("source", "")))
					return {"running": true}
				if _operation.has("scope"):
					_store_hash(_operation, String(result["digest"]))
	if Time.get_ticks_usec() - begin >= int(budgets.get("usec", 2000)):
		return {"running": true, "phase": _stage}
	match _stage:
		"prepare":
			_prepare_one()
		"io":
			if not _operations.is_empty():
				_operation = _operations.pop_front()
				_operation["payload"]["budget"] = int(budgets.get("io_bytes", IO.CHUNK_BYTES))
				_waiting = _worker.submit(String(_operation["kind"]), _operation["payload"])
			elif _index < _locations.size():
				_stage_region(_index)
				_index += 1
			else:
				_begin_native()
		"native":
			var decoded: Dictionary = _terrain.instancer.step_foliage_restore(String(record["id"]),
				int(budgets.get("pages", 1)), int(budgets.get("decode_bytes", 1048576)))
			if not bool(decoded.get("ok", false)):
				_fail_async(String(decoded.get("error", "Native foliage preparation failed")))
			elif bool(decoded.get("ready", false)):
				_queue_verification()
		"verify":
			if not _operations.is_empty():
				_operation = _operations.pop_front()
				_operation["payload"]["budget"] = int(budgets.get("io_bytes", IO.CHUNK_BYTES))
				_waiting = _worker.submit("hash", _operation["payload"])
			else:
				_stage = "journal"
				_waiting = _worker.submit("journal", {"record": record.duplicate(true), "path": journal_path(_terrain),
					"temporary": journal_path(_terrain) + "." + String(record["id"]) + ".new"})
		"publish":
			_publish_atomic()
	metrics["last_tick_usec"] = Time.get_ticks_usec() - begin
	metrics["max_tick_usec"] = maxi(int(metrics["max_tick_usec"]), int(metrics["last_tick_usec"]))
	return {"running": true, "phase": _stage, "regions_prepared": _index, "region_count": _locations.size(), "metrics": metrics}

func _prepare_one() -> void:
	if not _guard_valid():
		_fail_async("Terrain, rules or instance ownership changed during staging; restart generation.")
		return
	if _index >= _locations.size():
		_by_region.clear()
		_index = 0
		_queue("save_resource", {"resource": _before_ledger, "destination": record["ledger_before"]})
		_queue("hash", {"source": record["ledger_before"]}, "root", -1, "ledger_before_hash")
		_queue("save_resource", {"resource": _after_ledger, "destination": record["ledger_after"]})
		_queue("hash", {"source": record["ledger_after"]}, "root", -1, "ledger_after_hash")
		_stage = "io"
		return
	var location: Vector2i = _locations[_index]
	var prepared: Dictionary = _terrain.instancer.prepare_foliage_owned_regions(_by_region[location])
	if not bool(prepared.get("ok", false)):
		_fail_async(String(prepared.get("error", "Cannot prepare foliage region")))
		return
	var before: PackedByteArray = prepared["before_pages"][location]
	var after: PackedByteArray = prepared["pages"][location]
	_memory_bytes += before.size() + after.size()
	if before.size() > IO.MAX_FILE_BYTES or after.size() > IO.MAX_FILE_BYTES or _memory_bytes > _memory_limit:
		_fail_async("Foliage commit snapshots exceed the staging budget; reduce the job scope.")
		return
	_before_pages[location] = before
	_after_pages[location] = after
	var region: Terrain3DRegion = _terrain.data.get_region(location)
	_expected[location] = region.get_foliage_instance_revision()
	var directory: String = record["directory"]
	record["regions"].append({"x": location.x, "y": location.y,
		"before": directory.path_join("page_before_%d.bin" % _index), "after": directory.path_join("page_after_%d.bin" % _index),
		"before_hash": "", "after_hash": "", "generation_before": region.get_instance_generation(),
		"generation_after": region.get_instance_generation() + 1, "modified_before": region.modified,
		"dirty_domains_before": region.get_dirty_domains()})
	var target: String = _terrain.data_directory.path_join(Terrain3DUtil.location_to_filename(location))
	record["members"].append({"target": target, "before": directory.path_join("disk_before_%d.bin" % _index),
		"after": directory.path_join("after_%d.res" % _index), "existed": true, "before_hash": "", "after_hash": ""})
	_index += 1

func _stage_region(index: int) -> void:
	var member: Dictionary = record["members"][index]
	var item: Dictionary = record["regions"][index]
	var location := Vector2i(int(item["x"]), int(item["y"]))
	_queue("stage_region", {"source": member["target"], "destination": member["after"],
		"page": _after_pages[location], "generation": item["generation_after"]})
	_queue("write", {"bytes": _before_pages[location], "destination": item["before"]})
	_queue("hash", {"source": item["before"]}, "region", index, "before_hash")
	_queue("write", {"bytes": _after_pages[location], "destination": item["after"]})
	_queue("hash", {"source": item["after"]}, "region", index, "after_hash")
	_queue("copy", {"source": member["target"], "destination": member["before"]})
	_queue("hash", {"source": member["before"]}, "member", index, "before_hash")
	_queue("hash", {"source": member["after"]}, "member", index, "after_hash")
	_queue_publish_copies(index)

func _queue_publish_copies(index: int) -> void:
	var member: Dictionary = record["members"][index]
	var directory: String = record["directory"]
	member["publish_after"] = directory.path_join("publish_after_%d.res" % index)
	member["publish_before"] = directory.path_join("publish_before_%d.res" % index)
	_queue("copy", {"source": member["after"], "destination": member["publish_after"]})
	if bool(member["existed"]):
		_queue("copy", {"source": member["before"], "destination": member["publish_before"]})

func _begin_native() -> void:
	var index: int = _locations.size()
	if record["members"].size() == index:
		var target: String = _terrain.data_directory.path_join(MANIFEST.FILE_NAME)
		record["members"].append({"target": target, "before": String(record["directory"]).path_join("disk_before_ledger.bin"),
			"after": record["ledger_after"], "existed": FileAccess.file_exists(target),
			"before_hash": "", "after_hash": record["ledger_after_hash"]})
		if FileAccess.file_exists(target):
			_queue("copy", {"source": target, "destination": record["members"][index]["before"]})
			_queue("hash", {"source": record["members"][index]["before"]}, "member", index, "before_hash")
		_queue_publish_copies(index)
		return
	var before_generations: Dictionary = {}
	var after_generations: Dictionary = {}
	var persistence: Dictionary = {}
	for item: Dictionary in record["regions"]:
		var location := Vector2i(int(item["x"]), int(item["y"]))
		before_generations[location] = item["generation_before"]
		after_generations[location] = item["generation_after"]
		persistence[location] = {"dirty_domains": item["dirty_domains_before"], "modified": item["modified_before"]}
	var result: Dictionary = _terrain.instancer.begin_foliage_restore(String(record["id"]), _after_pages,
		_before_pages, after_generations, before_generations, persistence, _expected)
	if not bool(result.get("ok", false)):
		_fail_async(String(result.get("error", "Cannot prepare atomic foliage publication")))
		return
	_stage = "native"

func _queue_verification() -> void:
	for index: int in range(record["members"].size()):
		var member: Dictionary = record["members"][index]
		_verify(member["after"], member["after_hash"])
		_verify(member["publish_after"], member["after_hash"])
		if bool(member["existed"]):
			_verify(member["before"], member["before_hash"])
			_verify(member["publish_before"], member["before_hash"])
	for item: Dictionary in record["regions"]:
		_verify(item["before"], item["before_hash"])
		_verify(item["after"], item["after_hash"])
	_verify(record["ledger_before"], record["ledger_before_hash"])
	_verify(record["ledger_after"], record["ledger_after_hash"])
	_stage = "verify"

func _verify(path: String, digest: String) -> void:
	_operations.append({"kind": "hash", "payload": {"source": path}, "expect": digest})

func _queue(kind: String, payload: Dictionary, scope: String = "", index: int = -1, field: String = "") -> void:
	var operation: Dictionary = {"kind": kind, "payload": payload}
	if not scope.is_empty():
		operation["scope"] = scope
		operation["index"] = index
		operation["field"] = field
	_operations.append(operation)

func _store_hash(operation: Dictionary, digest: String) -> void:
	match String(operation["scope"]):
		"root": record[operation["field"]] = digest
		"region": record["regions"][int(operation["index"])][operation["field"]] = digest
		"member": record["members"][int(operation["index"])][operation["field"]] = digest

func _guard_valid() -> bool:
	return (is_instance_valid(_terrain) and identity(_terrain) == record.get("identity", "") and
		(not _guard.is_valid() or bool(_guard.call())))

func _publish_atomic() -> void:
	if not _guard_valid():
		_fail_async("Foliage sources changed before publication; no generated instances were applied.")
		return
	var begin: int = Time.get_ticks_usec()
	_publication_attempted = true
	var published: Array[int] = []
	var native_applied: bool = false
	var success: bool = _phase("PREPARED")
	for index: int in range(record["members"].size()):
		if not success:
			break
		if index == record["members"].size() - 1:
			success = _phase("REGIONS_APPLIED")
			if not success:
				break
		var member: Dictionary = record["members"][index]
		if DirAccess.rename_absolute(ProjectSettings.globalize_path(member["publish_after"]), ProjectSettings.globalize_path(member["target"])) != OK:
			error = "Cannot publish foliage file: " + String(member["target"])
			success = false
		else:
			published.append(index)
	if success:
		success = _phase("LEDGER_APPLIED")
	if success:
		var native: Dictionary = _terrain.instancer.publish_foliage_restore(String(record["id"]), true)
		success = bool(native.get("ok", false))
		native_applied = success
		if not success:
			error = String(native.get("error", "Native publication failed"))
	if success:
		success = _phase("COMMITTED")
	if success:
		_committed = true
		_final_result = {"ok": true, "manifest": _after_ledger, "regions": _locations.size()}
		if not _clear_journal(_terrain):
			_final_result["warning"] = "Foliage committed; recovery journal cleanup needs retry."
	else:
		var publication_error: String = error
		var restored: bool = not fail_restore
		if progress.is_valid():
			progress.call("RESTORING")
		for index: int in published:
			var member: Dictionary = record["members"][index]
			var result: Error = OK
			if bool(member["existed"]):
				result = DirAccess.rename_absolute(ProjectSettings.globalize_path(member["publish_before"]), ProjectSettings.globalize_path(member["target"]))
			else:
				result = DirAccess.remove_absolute(ProjectSettings.globalize_path(member["target"]))
			if result != OK:
				restored = false
				error = "Rollback could not restore " + String(member["target"])
		if restored and native_applied:
			restored = bool(_terrain.instancer.publish_foliage_restore(String(record["id"]), false).get("ok", false))
		var cleaned: bool = false
		if restored and _phase("ROLLED_BACK"):
			cleaned = _clear_journal(_terrain)
		var detail: String = "restored previous foliage" if restored else "rollback incomplete"
		if not cleaned:
			detail += "; recovery journal retained at " + journal_path(_terrain)
		_final_result = _failure("%s; %s" % [publication_error, detail])
	metrics["publication_usec"] = Time.get_ticks_usec() - begin
	_stage = "stopping"
	_worker.stop()

func _owns_pending_journal() -> bool:
	if not is_instance_valid(_terrain) or not FileAccess.file_exists(journal_path(_terrain)):
		return false
	var envelope: Variant = JSON.parse_string(FileAccess.get_file_as_string(journal_path(_terrain)))
	if not envelope is Dictionary:
		return false
	var payload: Variant = JSON.parse_string(String(envelope.get("payload", "")))
	return payload is Dictionary and payload.get("id", "") == record.get("id", "")

func _fail_async(message: String) -> void:
	_final_result = _failure(message)
	_stage = "stopping"
	_worker.stop()

func cancel_async() -> void:
	if _committed:
		return
	_cancelled = true
	_stage = "stopping"
	if _worker != null:
		_worker.stop()

func shutdown_async() -> void:
	cancel_async()
	if _worker != null:
		_worker.shutdown()
	if is_instance_valid(_terrain) and not record.is_empty():
		_terrain.instancer.cancel_foliage_restore(String(record["id"]))

func _shutdown_worker() -> void:
	if _worker != null:
		_worker.shutdown()
