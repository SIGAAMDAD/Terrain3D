# Test against the matching custom Godot 4.7.2 editor/debug/release host.
# Run: godot --headless --path project -s res://addons/terrain_3d/tests/dual_dll/native_flavor_smoke.gd -- --expected=editor
extends SceneTree

const EDITOR_ONLY: Array[String] = [
    "Terrain3DEditor",
    "Terrain3DErosionJob",
    "Terrain3DAutoShaderBaker",
    "Terrain3DLandformBaker",
    "Terrain3DFoliageGenerator",
    "Terrain3DHLODBuilder",
    "Terrain3DWorldController",
]
const RETAINED_SHARED: Array[String] = [
    "Terrain3DWorldImportProfile",
    "Terrain3DErosionSettings",
]


func _initialize() -> void:
    var expected: String = ""
    for arg: String in OS.get_cmdline_user_args():
        if arg.begins_with("--expected="):
            expected = arg.trim_prefix("--expected=")
    if not expected in ["editor", "template_debug", "template_release"]:
        push_error("Pass -- --expected=editor|template_debug|template_release")
        quit(1)
        return

    if not ClassDB.class_exists("Terrain3D") or not ClassDB.class_exists("Terrain3DUtil"):
        push_error("Terrain3D native extension was not loaded")
        quit(1)
        return

    var flavor: String = Terrain3DUtil.get_native_build_flavor()
    if flavor != expected:
        push_error("Terrain3D native flavor mismatch: expected %s, got %s" % [expected, flavor])
        quit(1)
        return

    for cls: String in RETAINED_SHARED:
        if not ClassDB.class_exists(cls):
            push_error("Missing shared serialized class: " + cls)
            quit(1)
            return
    for cls: String in EDITOR_ONLY:
        if ClassDB.class_exists(cls) != (expected == "editor"):
            push_error("Incorrect registration for " + cls + " in " + expected)
            quit(1)
            return
    if not ClassDB.class_has_method("Terrain3D", "get_native_build_flavor"):
        push_error("Missing Terrain3D native flavor method")
        quit(1)
        return
    print("[Terrain3D][P02] PASS: ", expected, " native registry")
    quit(0)
