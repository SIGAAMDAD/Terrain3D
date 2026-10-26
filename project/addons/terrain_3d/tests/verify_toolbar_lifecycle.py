#!/usr/bin/env python3
"""Source-level guards for the Terrain3D 3D-editor tool sidebar lifecycle.

These checks intentionally do not claim to execute Godot editor callbacks.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
ADDON = ROOT / 'addons' / 'terrain_3d'
UI = (ADDON / 'src/ui.gd').read_text(encoding='utf-8')
EDITOR = (ADDON / 'src/editor_plugin.gd').read_text(encoding='utf-8')
TOOLBAR = (ADDON / 'src/toolbar.gd').read_text(encoding='utf-8')


def function(source: str, name: str) -> str:
    marker = f'func {name}('
    assert marker in source, f'missing {name}'
    start = source.index(marker)
    next_func = source.find('\nfunc ', start + len(marker))
    return source[start:next_func if next_func >= 0 else len(source)]


def test_sidebar_registered() -> None:
    setup = function(UI, '_enter_tree')
    teardown = function(UI, '_exit_tree')
    identifier = 'EditorPlugin.CONTAINER_SPATIAL_EDITOR_SIDE_LEFT, toolbar'
    assert 'plugin.add_control_to_container(' + identifier + ')' in setup
    assert 'plugin.remove_control_from_container(' + identifier + ')' in teardown
    assert 'toolbar.hide()' in setup
    assert 'toolbar.set_visible(p_visible)' in function(UI, 'set_visible')
    assert TOOLBAR.count('add_tool_button(') >= 10, 'tool buttons not populated'


def test_deferred_first_selection() -> None:
    tool_change = function(UI, '_on_tool_changed')
    assert 'not tool_settings.settings.has(&"brush")' in tool_change
    assert '_tool_settings_initialized and _selected_tool == p_tool' in tool_change
    assert '_tool_settings_initialized = true' in tool_change
    dock = function(UI, 'set_asset_dock')
    assert '_on_tool_changed(_selected_tool, _selected_operation)' in dock


def test_missing_brush_retains_tool_input() -> None:
    changed = function(UI, '_on_setting_changed')
    invalid = changed.index('if not _has_valid_brush(next_brush_data):')
    body = changed[invalid:changed.index('var previous_touchscreen_invert_active', invalid)]
    assert 'set_active_operation()' in body
    assert 'hide_decal()' in body
    missing_asset = changed.index('if not is_instance_valid(asset_dock):')
    assert 'set_active_operation()' in changed[missing_asset:invalid]
    assert 'plugin.editor.set_tool(active_tool)' in function(UI, 'set_active_operation')


def test_selection_visibility() -> None:
    bind = function(EDITOR, '_bind_terrain')
    same = bind.index('if p_terrain == terrain:')
    assert 'ui.set_visible(true)' in bind[same:bind.index('return', same)]
    visible = function(EDITOR, '_make_visible')
    assert 'p_visible and (is_selected() or is_terrain_valid())' in visible
    assert 'ui.set_visible(false)' in visible
    connect = function(EDITOR, '_connect_editor_signals')
    disconnect = function(EDITOR, '_disconnect_editor_signals')
    sync = function(EDITOR, '_sync_tools_after_selection')
    assert 'selection.selection_changed.connect(_on_editor_selection_changed)' in connect
    assert 'selection.selection_changed.disconnect(_on_editor_selection_changed)' in disconnect
    assert '_sync_tools_after_selection.call_deferred()' in function(EDITOR, '_on_editor_selection_changed')
    assert 'var selected_terrain: Terrain3D = node as Terrain3D' in sync
    assert '_bind_terrain(selected_terrain)' in sync
    assert 'ui.set_visible(false)' in sync


def test_preload_targets() -> None:
    for file in [UI, EDITOR, TOOLBAR]:
        for resource in re.findall(r'preload\("res://([^"\n]+)"\)', file):
            assert (ROOT / resource).is_file(), f'missing {resource}'
    assert 'var class_name:' not in (ADDON / 'src/runtime_export_preflight.gd').read_text()


if __name__ == '__main__':
    for test in [test_sidebar_registered, test_deferred_first_selection, test_missing_brush_retains_tool_input, test_selection_visibility, test_preload_targets]:
        test()
        print('PASS', test.__name__)
