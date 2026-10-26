# Use the matching custom Godot 4.7.2 editor executable:
# godot --headless --editor --path project -s res://addons/terrain_3d/tests/dual_dll/authoring_attachment_smoke.gd
extends SceneTree


func _initialize() -> void:
    if not ClassDB.class_exists("Terrain3DEditor"):
        push_error("P03 attachment smoke requires the editor native library")
        quit(1)
        return

    var terrain: Terrain3D = Terrain3D.new()
    var authoring: Terrain3DEditor = Terrain3DEditor.new()
    terrain.set_editor(authoring)
    authoring.set_terrain(terrain)
    if terrain.get_editor() != authoring or authoring.get_terrain() != terrain:
        push_error("P03 editor attachment failed")
        authoring.set_terrain(null)
        terrain.set_editor(null)
        authoring.free()
        terrain.free()
        quit(1)
        return

    # Detach editor before terrain deletion, as the editor plugin does.
    authoring.set_terrain(null)
    if terrain.get_editor() != null or authoring.get_terrain() != null:
        push_error("P03 editor detach failed")
        authoring.free()
        terrain.free()
        quit(1)
        return
    terrain.set_editor(null)  # Idempotent after the editor has detached itself.
    authoring.free()
    terrain.free()
    print("[Terrain3D][P03] PASS: editor attachment and detachment")
    quit(0)
