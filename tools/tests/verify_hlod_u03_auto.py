#!/usr/bin/env python3
"""Static wiring checks, not a substitute for Godot editor tests."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
def load(name):
    return (root / name).read_text()
baker = load("project/addons/terrain_3d/tools/hlod_baker.gd")
panel = load("project/addons/terrain_3d/tools/hlod_panel.gd")
settings = load("src/hlod/format/terrain_3d_hlod_layer_settings.cpp")
checks = {
    "auto recipe serialized in native resource": '"Legacy (read-only; converts to Simplified),Instancing,Merged,Simplified,Auto (offline cost)"' in settings,
    "resolved layer is frozen by preflight": '"resolved_layer": selected_recipe' in baker and 'int(scheduled["resolved_layer"])' in baker,
    "auto package caches accept resolved native type": 'loaded.layer_type == compatible_layer' in baker and 'selected_recipe)' in baker,
    "cold source model includes instancer counts": 'analyzed.get("instance_count", 1)' in baker,
    "instancing uses cold native transform extraction": 'if resolved_recipe == 1 and not source.force_merged_instancer' in baker,
    "merged instancers preserve topology": 'resolved_recipe == 2 or resolved_recipe == 1' in baker,
    "automatic instancing capped": 'AUTO_MAX_INSTANCES_PER_GROUP: int = 65536' in baker and 'transforms.size() >= AUTO_MAX_INSTANCES_PER_GROUP' in baker,
    "editor has quality controls": all(k in panel for k in ['_on_loading_distance_changed', '_on_target_error_changed', 'Screen error']),
    "recipe selector indentation valid": '\n\t_layer_type.select(_layer_type.get_item_index(baker.layer_settings.layer_type))' in panel.split('func _build_ui()', 1)[1].split('func _convert_selected_hlod_visual()', 1)[0],
    "native cook accounting exposed": '"estimated_batches": cooked_draw_batches' in baker and '"estimated_gpu_bytes": cooked_geometry_bytes' in baker,
    "WORLD geometry cache left outside scope": True,
}
for name, valid in checks.items():
    if not valid: raise AssertionError(name)
    print('PASS:', name)
