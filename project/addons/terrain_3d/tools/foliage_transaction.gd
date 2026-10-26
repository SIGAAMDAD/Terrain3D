@tool
extends RefCounted

# One editor-only publication owner. All recovery/undo inputs are immutable files
# in a project/terrain/transaction namespace, never recycled serial filenames.
const MANIFEST = preload("res://addons/terrain_3d/src/foliage_manifest.gd")
const ROOT_NAME: String = ".terrain3d_foliage_transactions"
const SCHEMA: int = 1
const MAX_UNREFERENCED_ARCHIVES: int = 32
static var _live_directories: Dictionary = {}
var _native := Terrain3DFoliageEditor.new()
var record: Dictionary = {}
var error: String = ""
var failure_phase: String = "" # Test seam: PREPARED/REGIONS_APPLIED/LEDGER_APPLIED/COMMITTED.
var fail_restore: bool = false
var progress: Callable
var _owned_directory: String = ""

static func identity(terrain: Terrain3D) -> String:
	var path: String = ProjectSettings.globalize_path(terrain.data_directory).simplify_path().replace("\\", "/").trim_suffix("/")
	return path.to_lower() if OS.has_feature("windows") else path

static func journal_path(terrain: Terrain3D) -> String:
	return terrain.data_directory.path_join(ROOT_NAME).path_join("pending.json")

func execute(terrain: Terrain3D, batches: Array, before: Terrain3DFoliageManifest,
		after: Terrain3DFoliageManifest, sources: Dictionary) -> Dictionary:
	_native.set_terrain(terrain)
	var prepared: Dictionary = _native.prepare_owned_regions(batches)
	if not bool(prepared.get("ok", false)):
		return _failure(String(prepared.get("error", "Cannot prepare native foliage pages")))
	return execute_pages(terrain, prepared["before_pages"], prepared["pages"], before, after, sources)

func execute_pages(terrain: Terrain3D, before_pages: Dictionary, after_pages: Dictionary,
		before: Terrain3DFoliageManifest, after: Terrain3DFoliageManifest, sources: Dictionary,
		target_generations: Dictionary = {}) -> Dictionary:
	_native.set_terrain(terrain)
	if FileAccess.file_exists(journal_path(terrain)):
		return _failure("Pending foliage recovery must complete before another operation: " + journal_path(terrain))
	# Archive cleanup is performed by the asynchronous I/O worker.
	if before.terrain_identity != identity(terrain) or after.terrain_identity != identity(terrain):
		return _failure("Foliage transaction belongs to another terrain")
	var id: String = Crypto.new().generate_random_bytes(16).hex_encode()
	var directory: String = terrain.data_directory.path_join(ROOT_NAME).path_join(id)
	if DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory)) != OK:
		return _failure("Cannot create transaction directory: " + directory)
	record = {"schema": SCHEMA, "id": id, "identity": identity(terrain), "directory": directory,
		"publication_generation": after.publication_generation,
		"project": ProjectSettings.globalize_path("res://").sha256_text(), "sources": sources.duplicate(true),
		"members": [], "regions": [], "ledger_before": directory.path_join("ledger_before.tres"),
		"ledger_after": directory.path_join("ledger_after.tres")}
	_pin(directory)
	if ResourceSaver.save(before, record["ledger_before"]) != OK or ResourceSaver.save(after, record["ledger_after"]) != OK:
		return _failure("Cannot stage transaction owner ledgers: " + directory)
	record["ledger_before_hash"] = FileAccess.get_sha256(record["ledger_before"])
	record["ledger_after_hash"] = FileAccess.get_sha256(record["ledger_after"])
	var locations: Array = after_pages.keys()
	locations.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.x < b.x or (a.x == b.x and a.y < b.y))
	for index: int in range(locations.size()):
		var location: Vector2i = locations[index]
		if not before_pages.has(location) or not terrain.data.has_region(location):
			return _failure("Missing region snapshot: " + str(location))
		var region: Terrain3DRegion = terrain.data.get_region(location)
		var target: String = terrain.data_directory.path_join(Terrain3DUtil.location_to_filename(location))
		var staged: String = directory.path_join("after_%d.res" % index)
		var generation: int = int(target_generations.get(location, region.get_instance_generation() + 1))
		var stage_error: Error = Terrain3DFoliageEditor.stage_region_instances(terrain, location, terrain.data_directory,
			staged, after_pages[location], generation)
		if stage_error != OK:
			return _failure("Cannot stage instances for %s (%s). Save the authored terrain first." % [target, error_string(stage_error)])
		var before_path: String = directory.path_join("page_before_%d.bin" % index)
		var after_path: String = directory.path_join("page_after_%d.bin" % index)
		if not _write_bytes(before_path, before_pages[location]) or not _write_bytes(after_path, after_pages[location]):
			return _failure("Cannot stage instance snapshots: " + directory)
		record["regions"].append({"x": location.x, "y": location.y, "before": before_path, "after": after_path,
			"before_hash": FileAccess.get_sha256(before_path), "after_hash": FileAccess.get_sha256(after_path),
			"generation_before": region.get_instance_generation(), "generation_after": generation,
			"modified_before": region.modified, "dirty_domains_before": region.get_dirty_domains()})
		if not _add_member(target, staged, index):
			return _failure(error)
	if not _add_member(terrain.data_directory.path_join(MANIFEST.FILE_NAME), record["ledger_after"], locations.size()):
		return _failure(error)
	if not _validate(terrain):
		return _failure(error)
	# Durable descriptor and all snapshots precede every live/disk mutation.
	if not _write_journal(terrain) or not _phase("PREPARED"):
		return _failure("Cannot prepare recovery journal: " + journal_path(terrain))
	var applied: bool = _apply(terrain, "after")
	if applied and _phase("COMMITTED"):
		if not _clear_journal(terrain):
			return {"ok": true, "transaction": self, "manifest": after, "regions": locations.size(),
				"warning": "Committed; recovery journal cleanup needs retry: " + journal_path(terrain)}
		return {"ok": true, "transaction": self, "manifest": after, "regions": locations.size()}
	var publication_error: String = error
	if progress.is_valid():
		progress.call("RESTORING")
	if not _apply(terrain, "before"):
		return _failure("%s; rollback failed: %s. Recovery journal retained at %s" % [publication_error, error, journal_path(terrain)])
	if not _phase("ROLLED_BACK") or not _clear_journal(terrain):
		return _failure("Rollback completed; journal retained for retry: " + journal_path(terrain))
	return _failure("Foliage publication rolled back: " + publication_error)

func _add_member(target: String, after_path: String, index: int) -> bool:
	var before_path: String = String(record["directory"]).path_join("disk_before_%d.bin" % index)
	var existed: bool = FileAccess.file_exists(target)
	if existed and DirAccess.copy_absolute(ProjectSettings.globalize_path(target), ProjectSettings.globalize_path(before_path)) != OK:
		error = "Cannot back up publication target: " + target
		return false
	record["members"].append({"target": target, "before": before_path, "after": after_path,
		"existed": existed, "before_hash": FileAccess.get_sha256(before_path) if existed else "",
		"after_hash": FileAccess.get_sha256(after_path)})
	return true

func _validate(terrain: Terrain3D) -> bool:
	if int(record.get("schema", 0)) != SCHEMA or String(record.get("identity", "")) != identity(terrain):
		error = "Invalid foliage recovery identity/schema"
		return false
	if String(record.get("project", "")) != ProjectSettings.globalize_path("res://").sha256_text():
		error = "Foliage recovery belongs to another project"
		return false
	var directory: String = terrain.data_directory.path_join(ROOT_NAME).path_join(String(record.get("id", "")))
	if String(record.get("id", "")).length() != 32 or record.get("directory", "") != directory:
		error = "Invalid foliage transaction directory"
		return false
	for character: String in String(record["id"]):
		if not "0123456789abcdef".contains(character):
			error = "Invalid foliage transaction ID"
			return false
	if not record.get("regions", null) is Array or not record.get("members", null) is Array:
		error = "Invalid recovery member lists"
		return false
	var regions: Array = record["regions"]
	var members: Array = record["members"]
	if regions.is_empty() or members.size() != regions.size() + 1:
		error = "Incomplete foliage transaction member set"
		return false
	for member: Variant in members:
		if not member is Dictionary:
			error = "Invalid recovery file member"
			return false
		for key: String in ["target", "before", "after", "before_hash", "after_hash"]:
			if not member.get(key, null) is String:
				error = "Invalid recovery file field: " + key
				return false
		if not member.get("existed", null) is bool:
			error = "Invalid recovery presence flag"
			return false
	var seen: Dictionary = {}
	for index: int in range(regions.size()):
		if not regions[index] is Dictionary or not members[index] is Dictionary:
			error = "Invalid foliage recovery member"
			return false
		var item: Dictionary = regions[index]
		for key: String in ["x", "y", "before", "after", "before_hash", "after_hash", "generation_before", "generation_after", "dirty_domains_before", "modified_before"]:
			if not item.has(key):
				error = "Incomplete recovery region: " + key
				return false
		for key: String in ["x", "y", "generation_before", "generation_after", "dirty_domains_before"]:
			if not (item[key] is int or item[key] is float):
				error = "Invalid recovery region number: " + key
				return false
		for key: String in ["before", "after", "before_hash", "after_hash"]:
			if not item[key] is String:
				error = "Invalid recovery region path/hash: " + key
				return false
		var location := Vector2i(int(item["x"]), int(item["y"]))
		if seen.has(location) or not terrain.data.has_region(location):
			error = "Missing or repeated recovery region: " + str(location)
			return false
		seen[location] = true
		if members[index].get("target", "") != terrain.data_directory.path_join(Terrain3DUtil.location_to_filename(location)):
			error = "Invalid foliage recovery target"
			return false
		for side: String in ["before", "after"]:
			if not _verify_file(item.get(side, ""), item.get(side + "_hash", ""), directory):
				return false
			if not Terrain3DFoliageEditor.validate_instance_page(FileAccess.get_file_as_bytes(item[side])):
				error = "Invalid native recovery instance page: " + String(item[side])
				return false
			# Loading staged region resources also verifies native instance encoding.
			if side == "after":
				if not _verify_file(members[index]["after"], members[index]["after_hash"], directory):
					return false
				var snapshot: Terrain3DRegion = ResourceLoader.load(members[index]["after"], "Terrain3DRegion", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DRegion
				if snapshot == null or snapshot.get_instance_page() != FileAccess.get_file_as_bytes(item[side]):
					error = "Invalid staged instance resource: " + str(members[index]["after"])
					return false
	if members.back().get("target", "") != terrain.data_directory.path_join(MANIFEST.FILE_NAME):
		error = "Invalid foliage ledger target"
		return false
	for member: Dictionary in members:
		for key: String in ["target", "before", "after", "existed", "before_hash", "after_hash"]:
			if not member.has(key):
				error = "Incomplete recovery file member: " + key
				return false
		if not _verify_file(member.get("after", ""), member.get("after_hash", ""), directory):
			return false
		if bool(member.get("existed", false)) and not _verify_file(member.get("before", ""), member.get("before_hash", ""), directory):
			return false
	for side: String in ["before", "after"]:
		if not _verify_file(record.get("ledger_" + side, ""), record.get("ledger_" + side + "_hash", ""), directory):
			return false
		var ledger: Terrain3DFoliageManifest = ResourceLoader.load(record["ledger_" + side], "", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DFoliageManifest
		if ledger == null or ledger.terrain_identity != identity(terrain):
			error = "Invalid recovery owner ledger"
			return false
	return true

func _verify_file(path: String, digest: String, directory: String) -> bool:
	var absolute: String = ProjectSettings.globalize_path(path).simplify_path().replace("\\", "/")
	var prefix: String = ProjectSettings.globalize_path(directory).simplify_path().replace("\\", "/") + "/"
	if not absolute.begins_with(prefix) or digest.is_empty() or not FileAccess.file_exists(path) or FileAccess.get_sha256(path) != digest:
		error = "Missing, corrupt or invalid recovery snapshot: " + path
		return false
	return true

func _apply(terrain: Terrain3D, side: String) -> bool:
	if side == "before" and fail_restore:
		error = "Injected restoration failure"
		return false
	# Preflight the complete set before changing even one member.
	if not _validate(terrain):
		return false
	var pages: Array[PackedByteArray] = []
	for item: Dictionary in record["regions"]:
		pages.append(FileAccess.get_file_as_bytes(item[side]))
	var members: Array = record["members"]
	for index: int in range(members.size() - 1):
		if not _publish_member(members[index], side):
			return false
	if side == "after" and not _phase("REGIONS_APPLIED"):
		return false
	if not _publish_member(members.back(), side):
		return false
	if side == "after" and not _phase("LEDGER_APPLIED"):
		return false
	var native_pages: Dictionary = {}
	var generations: Dictionary = {}
	var persistence: Dictionary = {}
	for index: int in range(pages.size()):
		var item: Dictionary = record["regions"][index]
		var location := Vector2i(int(item["x"]), int(item["y"]))
		native_pages[location] = pages[index]
		generations[location] = int(item["generation_" + side])
		if side == "before":
			persistence[location] = {"dirty_domains": int(item["dirty_domains_before"]), "modified": bool(item["modified_before"])}
	_native.set_terrain(terrain)
	var restored: Dictionary = _native.restore_instance_pages(native_pages, generations, persistence)
	if not bool(restored.get("ok", false)):
		error = String(restored.get("error", "Native instance restoration failed"))
		return false
	return true

func _publish_member(member: Dictionary, side: String) -> bool:
	var target: String = member["target"]
	if side == "before" and not bool(member["existed"]):
		if FileAccess.file_exists(target) and DirAccess.remove_absolute(ProjectSettings.globalize_path(target)) != OK:
			error = "Cannot restore absent target: " + target
			return false
		return true
	var staging: String = target + "." + String(record["id"]) + ".publish"
	if DirAccess.copy_absolute(ProjectSettings.globalize_path(member[side]), ProjectSettings.globalize_path(staging)) != OK:
		error = "Cannot stage publication: " + target
		return false
	if FileAccess.get_sha256(staging) != member[side + "_hash"]:
		error = "Publication copy hash mismatch: " + staging
		return false
	if DirAccess.rename_absolute(ProjectSettings.globalize_path(staging), ProjectSettings.globalize_path(target)) != OK:
		error = "Cannot publish file: " + target
		return false
	return true

func _phase(phase: String) -> bool:
	if progress.is_valid():
		progress.call(phase)
	if failure_phase == phase:
		error = "Injected failure at " + phase
		return false
	return _write_bytes(String(record["directory"]).path_join(phase + ".phase"), String(record["id"]).to_utf8_buffer())

func _write_journal(terrain: Terrain3D) -> bool:
	var payload: String = JSON.stringify(record)
	var envelope: String = JSON.stringify({"payload": payload, "sha256": payload.sha256_text()})
	var temporary: String = journal_path(terrain) + "." + String(record["id"]) + ".new"
	if not _write_bytes(temporary, envelope.to_utf8_buffer()):
		return false
	return DirAccess.rename_absolute(ProjectSettings.globalize_path(temporary), ProjectSettings.globalize_path(journal_path(terrain))) == OK

func _clear_journal(terrain: Terrain3D) -> bool:
	return DirAccess.remove_absolute(ProjectSettings.globalize_path(journal_path(terrain))) == OK

func _write_bytes(path: String, bytes: PackedByteArray) -> bool:
	var file: FileAccess = FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		error = "Cannot write: " + path
		return false
	file.store_buffer(bytes)
	file.flush()
	var write_error: Error = file.get_error()
	var result: bool = write_error == OK and file.get_position() == bytes.size()
	file.close()
	if not result:
		error = "Write failed: %s (%s)" % [path, error_string(write_error)]
	return result

func recover(terrain: Terrain3D) -> Dictionary:
	var path: String = journal_path(terrain)
	if not FileAccess.file_exists(path):
		return {"ok": true, "recovered": false}
	var envelope: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not envelope is Dictionary or not envelope.get("payload", null) is String:
		return _failure("Invalid/truncated recovery journal retained: " + path)
	var payload: String = envelope["payload"]
	if payload.sha256_text() != envelope.get("sha256", ""):
		return _failure("Recovery journal checksum mismatch; retained: " + path)
	var parsed: Variant = JSON.parse_string(payload)
	if not parsed is Dictionary:
		return _failure("Invalid recovery descriptor retained: " + path)
	record = parsed
	if not _validate(terrain):
		return _failure(error + "; journal retained: " + path)
	_pin(record["directory"])
	var committed: bool = FileAccess.get_file_as_string(String(record["directory"]).path_join("COMMITTED.phase")) == record["id"] if FileAccess.file_exists(String(record["directory"]).path_join("COMMITTED.phase")) else false
	var side: String = "after" if committed else "before"
	if not _apply(terrain, side) or not _clear_journal(terrain):
		return _failure("Recovery failed: %s; journal retained: %s" % [error, path])
	var ledger: Terrain3DFoliageManifest = ResourceLoader.load(record["ledger_" + side], "", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DFoliageManifest
	return {"ok": true, "recovered": true, "manifest": ledger}

# Undo/redo creates another journaled publication using current saved raster
# sources and the immutable historical pages, preserving later terrain edits.
func restore_history(terrain: Terrain3D, side: String) -> void:
	if not is_instance_valid(terrain) or identity(terrain) != record.get("identity", ""):
		push_error("Foliage undo target is closed or its data directory changed")
		return
	if not _validate(terrain):
		push_error(error)
		return
	var desired: Dictionary = {}
	var current: Dictionary = {}
	var generations: Dictionary = {}
	for item: Dictionary in record["regions"]:
		var location := Vector2i(int(item["x"]), int(item["y"]))
		if not terrain.data.ensure_instance_region_resident(location):
			push_error("Cannot read foliage undo region: " + str(location))
			return
		desired[location] = FileAccess.get_file_as_bytes(item[side])
		generations[location] = int(item["generation_" + side])
		current[location] = terrain.data.get_region(location).get_instance_page()
	var target: Terrain3DFoliageManifest = ResourceLoader.load(record["ledger_" + side], "", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DFoliageManifest
	var live: Terrain3DFoliageManifest = ResourceLoader.load(terrain.data_directory.path_join(MANIFEST.FILE_NAME), "", ResourceLoader.CACHE_MODE_IGNORE) as Terrain3DFoliageManifest
	if live == null:
		live = MANIFEST.new()
		live.terrain_identity = identity(terrain)
	var operation = get_script().new()
	var result: Dictionary = operation.execute_pages(terrain, current, desired, live, target, {}, generations)
	if not bool(result.get("ok", false)):
		push_error("Foliage undo/redo failed: " + String(result.get("error", "Unknown error")))

func _failure(message: String) -> Dictionary:
	error = message
	return {"ok": false, "error": message}

func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE and has_method("_shutdown_worker"):
		call("_shutdown_worker")
	if what != NOTIFICATION_PREDELETE or _owned_directory.is_empty():
		return
	var directory: String = _owned_directory
	var remaining: int = int(_live_directories.get(directory, 1)) - 1
	if remaining > 0:
		_live_directories[directory] = remaining
		return
	_live_directories.erase(directory)
	# The undo manager explicitly retains this RefCounted owner. Its final release
	# is the artifact lifetime boundary. A pending journal always pins its files.
	var root: String = directory.get_base_dir()
	if FileAccess.file_exists(root.path_join("pending.json")):
		return
	# Artifact cleanup is deferred to bounded archive retention. Never issue a
	# multi-region file-delete burst from an undo-action destructor/editor tick.

func _pin(directory: String) -> void:
	if _owned_directory == directory:
		return
	_owned_directory = directory
	_live_directories[directory] = int(_live_directories.get(directory, 0)) + 1

func _prune_archives(terrain: Terrain3D) -> void:
	var root: String = terrain.data_directory.path_join(ROOT_NAME)
	var folder: DirAccess = DirAccess.open(root)
	if folder == null or FileAccess.file_exists(journal_path(terrain)):
		return
	var candidates: Array[Dictionary] = []
	for name: String in folder.get_directories():
		if name.length() != 32:
			continue
		var valid: bool = true
		for character: String in name:
			valid = valid and "0123456789abcdef".contains(character)
		var path: String = root.path_join(name)
		if valid and not _live_directories.has(path):
			candidates.append({"path": path, "time": FileAccess.get_modified_time(path.path_join("PREPARED.phase"))})
	candidates.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return int(a["time"]) < int(b["time"]))
	while candidates.size() > MAX_UNREFERENCED_ARCHIVES:
		var path: String = candidates.pop_front()["path"]
		var archive: DirAccess = DirAccess.open(path)
		if archive != null:
			for name: String in archive.get_files():
				archive.remove(name)
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
