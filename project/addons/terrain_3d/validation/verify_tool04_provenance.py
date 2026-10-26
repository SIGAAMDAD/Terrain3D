#!/usr/bin/env python3
"""TOOL-04 source-only provenance/ownership verification (no Godot claim)."""
from pathlib import Path
import ast
import re
import sys

root = Path(__file__).resolve().parents[4]
addon = root / 'project/addons/terrain_3d'
reader = (addon / 'diagnostics/runtime_status_snapshot.gd').read_text()
window = (addon / 'src/runtime_profiler_window.gd').read_text()
publisher = (root / 'src/rendering/rd/core/terrain_3d_diagnostics_publisher.cpp').read_text()
runtime = (root / 'src/diagnostics/core/terrain_3d_runtime_diagnostics.cpp').read_text()
shader = (root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()

# All shipping source descriptors must match a real literal publisher field.
start = reader.index('const AUTHORING_FIELDS = [')
end = reader.index('# Contract requests;', start)
blocks = reader[start:end]
found = re.findall(r'\["([^"]+)", "(world|renderer)", "([^"]+)", "([^"]+)"\]', blocks)
assert len(found) >= 25, f'found too few native-bound fields: {len(found)}'
for name, scope, key, kind in found:
    assert key in publisher, (name, key)
    assert '"' + key + '"' in publisher, (name, key)
    assert kind in ('count', 'text', 'bytes', 'bool', 'error', 'phase')
assert 'diagnostics["shaders"] = render_diagnostics.get( "shader", Dictionary() );' in runtime
assert 'diagnostics["streaming_renderer"] = render_diagnostics;' in runtime
assert 'world_renderer["visibility_candidates"]' in publisher
for category in ('terrain', 'instance', 'world', 'ocean', 'compute'):
    assert 'spirv_ready_' in shader and 'physical_ready_' in shader and 'catalog_count_' in shader
assert 'get_runtime_diagnostics(_deep_introspection.button_pressed)' in window
assert '_update_status_products(p_snapshot)' in window
assert window.count('get_runtime_diagnostics(') <= 3
assert 'get_rd_renderer_diagnostics()' in window  # pre-existing RD section
for forbidden in ('set_runtime_', 'create_thread', 'sync()', 'recook(', 'bake(', 'FileAccess.open', 'DirAccess.open', 'get_data(', 'get_image('):
    assert forbidden not in reader, forbidden
assert 'far atlas' not in reader.lower()
assert '64 * 1024 * 1024' not in reader
assert '0 B' not in reader  # no hardcoded zero bytes
print('TOOL-04 native-key provenance and readonly safety PASS:', len(found), 'published fields')
