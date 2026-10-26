"""Source-contract validation for Terrain3D's Inspector integration.

Run with: python project/addons/terrain_3d/tests/verify_inspector_revision.py
This validates source relationships and does not substitute for Godot editor testing.
"""
from pathlib import Path
import json
import re

PROJECT = Path(__file__).resolve().parents[3]
ADDON = PROJECT / 'addons' / 'terrain_3d'
INSPECTOR = ADDON / 'src' / 'inspector'


def check():
    scripts = list(ADDON.rglob('*.gd'))
    missing = []
    references = re.compile(r'(?:preload|load)\(\s*"(res://[^\"]+)"\s*\)')
    for source in scripts:
        content = source.read_text(encoding='utf-8')
        assert not re.search(r'\b(?:var|const|for)\s+class_name\s*[:=]', content), source
        for uri in references.findall(content):
            path = PROJECT / uri.removeprefix('res://')
            if not path.is_file():
                missing.append((str(source.relative_to(PROJECT)), uri))
    assert not missing, f'Missing referenced resources: {missing}'

    plugin = (ADDON / 'src/editor_plugin.gd').read_text(encoding='utf-8')
    base = (ADDON / 'src/editor_plugin_base.gd').read_text(encoding='utf-8')
    menu = (ADDON / 'menu/terrain_menu.gd').read_text(encoding='utf-8')
    window = (INSPECTOR / 'terrain_settings_window.gd').read_text(encoding='utf-8')
    node_help = (INSPECTOR / 'terrain_inspector_property_help.gd').read_text(encoding='utf-8')
    resource_help = (INSPECTOR / 'terrain_resource_help_inspector_plugin.gd').read_text(encoding='utf-8')
    for name in ('property_help_inspector_plugin', 'resource_help_inspector_plugin'):
        assert f'add_inspector_plugin({name})' in plugin, name
        assert f'remove_inspector_plugin({name})' in plugin, name
    for method in ('open_terrain_settings', 'open_runtime_dataset_cook_confirmation'):
        assert f'func {method}()' in plugin, method
        assert f'@abstract func {method}()' in base, method
    assert 'MENU_TERRAIN_SETTINGS' in menu
    assert 'plugin.open_terrain_settings()' in menu
    assert 'func open_runtime_cook_confirmation()' in menu
    assert 'return p_object is Terrain3D' in node_help
    assert 'not p_object is Resource' in resource_help
    assert 'SettingMetadata.get_record("Terrain3D"' in window
    assert 'terrain_inspector_scope.json' not in window
    assert 'if is_node_ready() and visible:' in window
    assert 'if _layout_dirty:' in window
    assert 'native_properties.has(VIEW_MODES[index])' in window
    assert 'plugin.open_runtime_dataset_cook_confirmation()' in window

    records = json.loads((ADDON / 'setting_metadata.json').read_text(encoding='utf-8'))['properties']
    terrain_records = {key: row for key, row in records.items() if key.startswith('Terrain3D.')}
    assert terrain_records
    editable = [row for row in terrain_records.values()
                if row.get('state') in ('editable', 'generated_read_only')
                and row.get('visibility') != 'hidden']
    assert editable
    print('Inspector revision source checks passed: ',
          len(scripts), 'GDScript files,', len(terrain_records),
          'Terrain3D metadata records,', len(editable), 'visible settings')


if __name__ == '__main__':
    check()
