extends SceneTree
## Example: --headless --path project -s res://addons/terrain_3d/validation/streaming_profile_capture.gd -- --scene res://world.tscn --frames 900 --output user://streaming_profile.json
## Times CPU work and GPU submission overhead; does not report GPU device timestamp durations.

func _initialize() -> void:
    call_deferred("_run")


func _argument(args: PackedStringArray, flag: String, fallback: String) -> String:
    var index: int = args.find(flag)
    return args[index + 1] if index >= 0 and index + 1 < args.size() else fallback


func _find_terrain(node: Node) -> Node:
    if node.is_class("Terrain3D"):
        return node
    for child: Node in node.get_children():
        var result: Node = _find_terrain(child)
        if result != null:
            return result
    return null


func _find_camera(node: Node) -> Camera3D:
    if node is Camera3D:
        return node as Camera3D
    for child: Node in node.get_children():
        var found: Camera3D = _find_camera(child)
        if found != null:
            return found
    return null


func _run() -> void:
    var args: PackedStringArray = OS.get_cmdline_user_args()
    var scene_path: String = _argument(args, "--scene", "")
    if scene_path.is_empty():
        push_error("Missing --scene res://path/to/world.tscn")
        quit(2)
        return
    var source: PackedScene = load(scene_path) as PackedScene
    if source == null:
        push_error("Failed to load scene " + scene_path)
        quit(2)
        return
    var scene: Node = source.instantiate()
    root.add_child(scene)
    var terrain: Node = _find_terrain(scene)
    if terrain == null or not terrain.has_method("set_streaming_profiler_enabled"):
        push_error("Terrain3D streaming profiler is unavailable in this native build")
        quit(2)
        return
    var camera: Camera3D = _find_camera(scene)
    var origin: Vector3 = camera.global_position if camera != null else Vector3.ZERO
    var warmup: int = maxi(0, int(_argument(args, "--warmup-frames", "120")))
    for i: int in range(warmup):
        await process_frame
    var frames: int = maxi(60, int(_argument(args, "--frames", "900")))
    var every: int = clampi(int(_argument(args, "--sample-every", "8")), 1, 1024)
    var output: String = _argument(args, "--output", "user://streaming_profile.json")
    var sample_interval: int = maxi(1, int(_argument(args, "--snapshot-frames", "30")))
    terrain.call("reset_streaming_profiler")
    terrain.call("set_streaming_profiler_enabled", true, every)
    var snapshots: Array[Dictionary] = []
    var last_usec: int = Time.get_ticks_usec()
    var frame_intervals_usec: Array[float] = []
    for frame: int in range(frames):
        # Deterministic region transitions plus one mid-capture teleport.
        # Set --stationary to capture the warm-residency control workload.
        if camera != null and not args.has("--stationary") and frame % 30 == 0:
            var step: int = int(frame / 30)
            var position: Vector3 = origin + Vector3(float(step % 8) * 128.0, 0.0, float(int(step / 8) % 3) * 128.0)
            if frame >= int(frames / 2):
                position += Vector3(512.0, 0.0, -512.0)
            camera.global_position = position
        await process_frame
        var now_usec: int = Time.get_ticks_usec()
        frame_intervals_usec.append(float(now_usec - last_usec))
        last_usec = now_usec
        if (frame + 1) % sample_interval == 0:
            snapshots.append({"frame": frame + 1, "profile": terrain.call("get_streaming_profiler_stats")})
    terrain.call("set_streaming_profiler_enabled", false, every)
    frame_intervals_usec.sort()
    var n: int = frame_intervals_usec.size()
    var report: Dictionary = {
        "scene": scene_path,
        "frames": frames,
        "warmup_frames": warmup,
        "camera_route": "stationary" if args.has("--stationary") else "region_stepping_and_teleport",
        "sample_every": every,
        "profile": terrain.call("get_streaming_profiler_stats"),
        "snapshots": snapshots,
        "frame_interval_usec_p50": frame_intervals_usec[clampi(ceili(float(n) * 0.50) - 1, 0, n - 1)],
        "frame_interval_usec_p95": frame_intervals_usec[clampi(ceili(float(n) * 0.95) - 1, 0, n - 1)],
        "frame_interval_usec_p99": frame_intervals_usec[clampi(ceili(float(n) * 0.99) - 1, 0, n - 1)],
        "note": "Per-stage times measure CPU wall time and GPU submission, not GPU execution. Headless frame intervals are not GPU frame time."
    }
    var file: FileAccess = FileAccess.open(output, FileAccess.WRITE)
    if file == null:
        push_error("Cannot write " + output)
        quit(2)
        return
    file.store_string(JSON.stringify(report, "\t"))
    file.close()
    print("Streaming profiler report: ", output)
    quit(0)
