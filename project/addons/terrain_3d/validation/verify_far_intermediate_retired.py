#!/usr/bin/env python3
"""ST06 gate: retired FAR staging and allocation have no active producers."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
COOK = (ROOT / 'src/authoring/data/terrain_3d_data_cooking.cpp').read_text()
CONTRACT = (ROOT / 'src/rendering/rd/vt/core/terrain_3d_vt_contract.h').read_text()
READER = (ROOT / 'src/data/formats/terrain_3d_vt_pack_reader.cpp').read_text()
GODOT = (ROOT / 'src/authoring/vt/io/terrain_3d_vt_godot_cooker.cpp').read_text()
WORKBENCH = (ROOT / 'project/addons/terrain_3d/tools/region_workbench.gd').read_text()

for path in (
    'src/rendering/surface/terrain_3d_far_regional_cooker.cpp',
    'src/rendering/surface/terrain_3d_far_regional_cooker.h',
    'project/addons/terrain_3d/rd/terrain_far_height_vertex_v1.glslinc',
    'src/rendering/rd/vt/io/terrain_3d_far_avt_region_file.h',
    'src/rendering/rd/vt/core/terrain_3d_far_avt_contract.h',
):
    assert not (ROOT / path).exists(), f'retired FAR file remains: {path}'

assert 'T3D_VT_CONSUMER_FAR' not in CONTRACT
assert 'T3D_VT_CONSUMER_COMPOSED' in CONTRACT
assert 'rAllocation.m_nConsumer > T3D_VT_CONSUMER_COMPOSED' in READER
assert 'region_height_cutout=terrain3d_surface.t3dst' in COOK
assert 'far_surface_vt=' not in COOK
assert 't3d_vt_godot_import_far_region_directory_v1' not in GODOT + COOK
assert '_archive_far_vt_regions' not in WORKBENCH
assert '_restore_far_vt_regions' not in WORKBENCH
print('PASS: FAR cooker, allocation, shader and intermediate retirement')
