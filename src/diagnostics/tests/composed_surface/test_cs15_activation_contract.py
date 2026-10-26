#!/usr/bin/env python3
"""CS15: source-only guard that the new cold decoder cannot enable composed raster."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
decode = (root / 'src/authoring/vt/build/terrain_3d_composed_surface_v3_godot_decode.cpp').read_text()
source = (root / 'src/authoring/vt/build/terrain_3d_composed_surface_v3_reference_decode.cpp').read_text()
shader = (root / 'project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc').read_text()
assert 't3d_cs15_decode_reference_tile' in decode
assert 'Image::FORMAT_BPTC_RGBA' in decode
assert 'if (format != T3D_VT_PHYSICAL_FORMAT_BC7_UNORM) return false;' in decode
assert 'family != T3D_VT_PHYSICAL_FORMAT_BC5_UNORM' in source
assert 'terrain_composed_surface_sampling.glslinc' not in shader
assert '#include "terrain_composed_surface_sampling.glslinc"' in (root / 'project/addons/terrain_3d/rd/terrain_clipmap_near.glsl').read_text()
assert 'T3D_CS_ACTIVE' in (root / 'project/addons/terrain_3d/rd/terrain_clipmap_fragment_interface.glslinc').read_text()
print('CS15 editor decoder and opt-in shader topology PASS')
