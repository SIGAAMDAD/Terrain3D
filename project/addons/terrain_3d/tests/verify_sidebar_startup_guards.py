#!/usr/bin/env python3
"""Static startup-path guard. Runtime behavior requires Godot editor execution."""
from pathlib import Path

addon = Path(__file__).resolve().parents[1]
plugin = (addon/'src/editor_plugin.gd').read_text(encoding='utf-8')
probe = (addon/'diagnostics/probe_tool_sidebar.gd').read_text(encoding='utf-8')

for eager in (
    'preload("res://addons/terrain_3d/src/inspector/terrain_inspector_property_help.gd")',
    'preload("res://addons/terrain_3d/src/inspector/terrain_resource_help_inspector_plugin.gd")',
):
    assert eager not in plugin, f'optional help is an eager parser dependency: {eager}'
assert 'preload("res://addons/terrain_3d/src/runtime_export_preflight.gd")' not in plugin
assert 'preflight.call("validate_project")' in plugin
assert plugin.index('ui = Terrain3DUI.new()') < plugin.index('_load_optional_inspector(TERRAIN_PROPERTY_HELP_PATH)')
assert 'script.can_instantiate()' in plugin
assert 'if not _validate_native_editor_build():' in plugin, 'must not bypass native ABI gate'
assert 'if is_terrain_valid(selected_terrain):' in plugin
assert 'else:\n\t\t\tui.set_visible(true)' in plugin
assert 'var active_root: Node = EditorInterface.get_edited_scene_root()' in plugin
assert '_sync_tools_after_selection.call_deferred()' in plugin
for marker in ('extends EditorScript', 'native Terrain3DEditor:', 'editor plugin node found:',
               'toolbar node found:', 'native editor initialized:', 'sidebar visible in tree:'):
    assert marker in probe, f'missing probe output: {marker}'
for bad in ('const Terrain3D', 'var terrain: Terrain3D', 'Terrain3D.new('):
    assert bad not in probe, 'probe must run even when native extension is absent'
print('PASS: optional inspector isolation, pre-data selection, ABI guard, startup probe')
