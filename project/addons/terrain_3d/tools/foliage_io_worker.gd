@tool
extends RefCounted

# A single persistent worker and a one-item mailbox provide backpressure. Payloads
# contain immutable bytes/paths or detached resources, never a Terrain3D/Node/RD.
const CHUNK_BYTES: int = 262144
const MAX_FILE_BYTES: int = 33554432
var _thread: Thread
var _wake := Semaphore.new()
var _mutex := Mutex.new()
var _task: Dictionary = {}
var _result: Dictionary = {}
var _busy: bool = false
var _stopping: bool = false
var _hashes: Dictionary = {}

func start() -> bool:
	_thread = Thread.new()
	return _thread.start(_loop) == OK

func submit(kind: String, payload: Dictionary) -> bool:
	_mutex.lock()
	if _busy or _stopping:
		_mutex.unlock()
		return false
	_busy = true
	_task = {"kind": kind, "payload": payload}
	_mutex.unlock()
	_wake.post()
	return true

func poll() -> Dictionary:
	_mutex.lock()
	var result: Dictionary = _result
	_result = {}
	if not result.is_empty():
		_busy = false
	_mutex.unlock()
	return result

func stop() -> void:
	_mutex.lock()
	_stopping = true
	_mutex.unlock()
	_wake.post()

func stopped() -> bool:
	return _thread == null or not _thread.is_alive()

func finish() -> void:
	# Poll stopped() first during normal editing/cancellation. Shutdown may wait
	# for the current bounded resource operation before freeing its buffers.
	if _thread != null and _thread.is_started():
		_thread.wait_to_finish()
	_thread = null
	_hashes.clear()

func shutdown() -> void:
	stop()
	finish()

func _loop() -> void:
	while true:
		_wake.wait()
		_mutex.lock()
		var stopping: bool = _stopping
		var task: Dictionary = _task
		_task = {}
		_mutex.unlock()
		if stopping:
			return
		if task.is_empty():
			continue
		var begin: int = Time.get_ticks_usec()
		var result: Dictionary = _perform(String(task["kind"]), task["payload"])
		result["worker_usec"] = Time.get_ticks_usec() - begin
		_mutex.lock()
		_result = result
		_mutex.unlock()

func _perform(kind: String, payload: Dictionary) -> Dictionary:
	match kind:
		"owners":
			var region := Terrain3DRegion.new()
			region.set_instance_page(payload["page"])
			return region.get_foliage_owner_ids()
		"stage_region":
			var source: FileAccess = FileAccess.open(payload["source"], FileAccess.READ)
			if source == null or source.get_length() > MAX_FILE_BYTES:
				return _failed("Saved region is missing or exceeds the staging limit: " + String(payload["source"]))
			source.close()
			var region: Terrain3DRegion = ResourceLoader.load(payload["source"], "Terrain3DRegion", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as Terrain3DRegion
			if region == null or region.get_height_map() == null:
				return _failed("Save the authored terrain before generating foliage: " + String(payload["source"]))
			region.set_instance_page(payload["page"])
			region.set_instance_generation(int(payload["generation"]))
			if region.get_instance_page() != payload["page"]:
				return _failed("Invalid staged instance page")
			var saved: Error = ResourceSaver.save(region, payload["destination"], ResourceSaver.FLAG_COMPRESS)
			return {"ok": true} if saved == OK else _failed("Cannot compress/stage region: " + String(payload["destination"]))
		"save_resource":
			var saved: Error = ResourceSaver.save(payload["resource"], payload["destination"])
			return {"ok": true} if saved == OK else _failed("Cannot stage owner ledger: " + String(payload["destination"]))
		"copy", "write", "hash":
			return _chunk(kind, payload)
		"prune":
			return _prune(payload)
		"journal":
			var text: String = JSON.stringify(payload["record"])
			var envelope: String = JSON.stringify({"payload": text, "sha256": text.sha256_text()})
			var file: FileAccess = FileAccess.open(payload["temporary"], FileAccess.WRITE)
			if file == null:
				return _failed("Cannot stage foliage recovery journal")
			file.store_string(envelope)
			file.flush()
			var error: Error = file.get_error()
			file.close()
			if error != OK or DirAccess.rename_absolute(ProjectSettings.globalize_path(payload["temporary"]), ProjectSettings.globalize_path(payload["path"])) != OK:
				return _failed("Cannot publish foliage recovery journal")
			return {"ok": true}
	return _failed("Unknown foliage I/O task: " + kind)

func _chunk(kind: String, payload: Dictionary) -> Dictionary:
	var offset: int = int(payload.get("offset", 0))
	var budget: int = clampi(int(payload.get("budget", CHUNK_BYTES)), 4096, 1048576)
	var data: PackedByteArray
	var length: int = 0
	if kind == "write":
		var bytes: PackedByteArray = payload["bytes"]
		length = bytes.size()
		if offset < 0 or offset > length or length > MAX_FILE_BYTES:
			return _failed("Invalid foliage staging size/offset")
		data = bytes.slice(offset, mini(offset + budget, length))
	else:
		var source: FileAccess = FileAccess.open(payload["source"], FileAccess.READ)
		if source == null:
			return _failed("Cannot read staged file: " + String(payload["source"]))
		length = source.get_length()
		if length > MAX_FILE_BYTES or offset < 0 or offset > length:
			source.close()
			return _failed("Region snapshot exceeds the staging limit: " + String(payload["source"]))
		source.seek(offset)
		data = source.get_buffer(mini(budget, length - offset))
		source.close()
		if data.size() != mini(budget, length - offset):
			return _failed("Short read during foliage staging")
	if length > MAX_FILE_BYTES or offset > length:
		return _failed("Invalid foliage staging size/offset")
	var result: Dictionary = {"ok": true, "offset": offset + data.size(), "done": offset + data.size() == length,
		"io_bytes": data.size() * (2 if kind == "copy" else 1)}
	if kind == "hash":
		var path: String = payload["source"]
		if offset == 0:
			var context := HashingContext.new()
			context.start(HashingContext.HASH_SHA256)
			_hashes[path] = context
		var hasher: HashingContext = _hashes[path]
		hasher.update(data)
		if result["done"]:
			result["digest"] = hasher.finish().hex_encode()
			_hashes.erase(path)
	else:
		var destination: FileAccess = FileAccess.open(payload["destination"], FileAccess.WRITE if offset == 0 else FileAccess.READ_WRITE)
		if destination == null:
			return _failed("Cannot write staged file: " + String(payload["destination"]))
		destination.seek(offset)
		destination.store_buffer(data)
		destination.flush()
		var error: Error = destination.get_error()
		destination.close()
		if error != OK:
			return _failed("Write failed: " + String(payload["destination"]))
	return result

func _failed(message: String) -> Dictionary:
	return {"ok": false, "error": message}

func _prune(payload: Dictionary) -> Dictionary:
	var root: String = payload["root"]
	var folder: DirAccess = DirAccess.open(root)
	if folder == null or FileAccess.file_exists(root.path_join("pending.json")):
		return {"ok": true}
	var candidates: Array[Dictionary] = []
	for name: String in folder.get_directories():
		var valid: bool = name.length() == 32
		for character: String in name:
			valid = valid and "0123456789abcdef".contains(character)
		var path: String = root.path_join(name)
		if valid and not payload["live"].has(path):
			candidates.append({"path": path, "time": FileAccess.get_modified_time(path.path_join("PREPARED.phase"))})
	candidates.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return int(a["time"]) < int(b["time"]))
	while candidates.size() > int(payload["limit"]):
		_mutex.lock()
		var stopping: bool = _stopping
		_mutex.unlock()
		if stopping:
			break
		var path: String = candidates.pop_front()["path"]
		var archive: DirAccess = DirAccess.open(path)
		if archive != null:
			for name: String in archive.get_files():
				archive.remove(name)
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	return {"ok": true}
