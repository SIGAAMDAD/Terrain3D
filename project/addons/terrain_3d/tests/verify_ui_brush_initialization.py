#!/usr/bin/env python3
"""Static regression checks for editor initialization and optional brush masks."""
from pathlib import Path

ADDON = Path(__file__).resolve().parents[1]
ROOT = ADDON.parents[2]
UI = (ADDON / 'src/ui.gd').read_text(encoding='utf-8')
TOOL = (ADDON / 'src/tool_settings.gd').read_text(encoding='utf-8')
TOOLBAR = (ADDON / 'src/toolbar.gd').read_text(encoding='utf-8')
LANDFORM = (ADDON / 'src/landform_controller.gd').read_text(encoding='utf-8')
NATIVE = (ROOT / 'src/authoring/editor/terrain_3d_editor.cpp').read_text(encoding='utf-8')

for label, content in [('UI', UI), ('Settings', TOOL), ('Toolbar', TOOLBAR), ('Landform', LANDFORM)]:
    assert '\n@tool\n' in content[:200], f'{label}: editor-side script must run in editor'
assert 'is_instance_valid(terrain_menu)' in UI, 'visibility needs lifecycle guard'
assert 'tool_settings.settings.has(&"brush")' in UI, 'selection callback needs initialization guard'
assert 'if not _has_valid_brush(next_brush_data)' in UI
assert 'if not _has_valid_brush(brush_data)' in UI
assert 'settings["brush"] = brush_button_group' in TOOL
assert '_generate_brush_texture(default_brush_btn)' in TOOL
assert 'selected_brush_imgs = [img, tex]' in TOOL
assert 'rData.has( "brush" )' in NATIVE
assert 'rData["brush"]' not in NATIVE, 'native must not use unchecked Dictionary lookup'
assert 'if ( brush_values.size() != 2 )' in NATIVE
print('PASS: editor lifecycle, procedural brush initialization, UI and native guards')
