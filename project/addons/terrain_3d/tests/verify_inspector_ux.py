"""Source-only Inspector UX checks. Run against the current addon source tree.

Does not replace opening the Godot 4.7.2 editor and testing live UI behavior.
"""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[4]
ADDON = ROOT / 'project/addons/terrain_3d'
NODE = ROOT / 'src/integration/godot/terrain_3d_bindings.cpp'
MATERIAL = ROOT / 'src/rendering/material/terrain_3d_material.cpp'
BIND_PATTERN = re.compile(r'PropertyInfo\s*\(\s*Variant::\w+\s*,\s*"([a-z_][a-z_0-9]*)"')


def native_keys(source):
    text = source.read_text()
    if '\n\tADD_SIGNAL(' in text:
        text = text.split('\n\tADD_SIGNAL(', 1)[0]
    return BIND_PATTERN.findall(text)


def verify():
    node_keys = native_keys(NODE)
    material_keys = native_keys(MATERIAL)
    assert len(node_keys) == 245, f'Expected 245 Terrain3D bindings, got {len(node_keys)}'
    assert len(material_keys) == 104, f'Expected 104 material bindings, got {len(material_keys)}'
    assert len(set(node_keys)) == len(node_keys)
    assert len(set(material_keys)) == len(material_keys)
    metadata = json.loads((ADDON / 'setting_metadata.json').read_text())['properties']
    for class_name, keys in [('Terrain3D', node_keys), ('Terrain3DMaterial', material_keys)]:
        for key in keys:
            record = metadata.get(class_name + '.' + key)
            assert record, f'Missing documentation for {class_name}.{key}'
            assert len(record.get('tooltip', '').strip()) >= 12, key
            assert 'Purpose: Configures' not in record['tooltip'], key
            assert '.t3dpage' not in record['tooltip'], key
    native = NODE.read_text()
    assert '"editor_runtime_streaming_preview", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE' in native
    for key in ['save_16_bit', 'show_grid', 'gpu_region_guard_regions', 'gpu_region_prefetch_rows']:
        assert f'"{key}"' in native, f'Lost compatibility key {key}'
    addon_plugin = (ADDON / 'src/editor_plugin.gd').read_text()
    assert 'add_inspector_plugin(property_help_inspector_plugin)' in addon_plugin
    assert 'remove_inspector_plugin(property_help_inspector_plugin)' in addon_plugin
    help_plugin = (ADDON / 'src/inspector/terrain_inspector_property_help.gd').read_text()
    assert '_parse_property(' not in help_plugin
    assert '.get_property_list()' in help_plugin
    assert 'add_custom_control(foldable)' in help_plugin
    factory = (ADDON / 'src/setting_control_factory.gd').read_text()
    assert '(p_control as SpinBox).editable = p_enabled' in factory
    assert '(p_control as BaseButton).disabled = not p_enabled' in factory
    shader = (ADDON / 'src/shader_performance_panel.gd').read_text()
    assert 'control.set("disabled", not p_enabled)' not in shader
    assert 'SettingControlFactory.set_control_enabled(control, p_enabled)' in shader
    profiler = (ADDON / 'src/runtime_profiler_window.gd').read_text()
    for auto_flag in ('gpu_region_cache_budget_auto', 'cpu_region_cache_budget_auto',
                      'instance_region_cache_budget_auto', 'lightmap_residency_budget_auto',
                      'hlod_memory_budget_auto'):
        assert auto_flag in profiler
    assert 'SettingControlFactory.set_control_enabled(_control_bindings[property_name], enabled)' in profiler
    print('Inspector UX validation passed:',len(node_keys),'node keys,',len(material_keys),'material keys,',len(metadata),'help records')


if __name__ == '__main__':
    verify()
