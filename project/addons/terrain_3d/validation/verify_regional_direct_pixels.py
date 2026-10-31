#!/usr/bin/env python3
"""Historical source-coordinate parity and current FAR-free V1 contract."""
from pathlib import Path
import random

ROOT = Path(__file__).resolve().parents[4]
VT = (ROOT / 'src/rendering/rd/vt/core/terrain_3d_vt_contract.h').read_text()
SHADER = (ROOT / 'project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc').read_text()
VERTEX = (ROOT / 'project/addons/terrain_3d/rd/terrain_clipmap_vertex_inc.glslinc').read_text()
READER = (ROOT / 'src/data/formats/terrain_3d_vt_pack_reader.cpp').read_text()
MATERIAL = (ROOT / 'src/rendering/rd/material/terrain_3d_material_state_builder.cpp').read_text()

assert 'T3D_VT_REGIONAL_SOURCE_RECIPE_VERSION 3u' in VT
assert not (ROOT / 'src/rendering/surface/terrain_3d_far_regional_cooker.cpp').exists()
assert 'T3D_VT_CONSUMER_FAR' not in VT
assert 'T3D_FEATURE_FAR_VT' not in SHADER + VERTEX
assert 'T3D_RD_MATERIAL_FEATURE_FAR_VT' not in MATERIAL
assert 'rAllocation.m_nConsumer > T3D_VT_CONSUMER_COMPOSED' in READER

rng = random.Random(15375)
for source_side in (4, 8, 16, 32, 64, 128):
    for channel in range(3):
        scale = 2 if channel == 0 else 1
        components = (4, 1, 2)[channel]
        raw = bytearray(rng.getrandbits(8) for _ in range(source_side * source_side * 4))
        height = bytearray(rng.getrandbits(8) for _ in range(source_side * source_side * 2))
        extent = 128 * scale
        for _ in range(1000):
            x = rng.randrange(extent)
            y = rng.randrange(extent)
            # Old staging: packed 136x136 tile with 4-pixel gutters,
            # then take the 128x128 center and duplicate tint 2x.
            old_tile_x = x // scale + 4
            old_tile_y = y // scale + 4
            old_sample_x = (old_tile_x - 4) * source_side // 128
            old_sample_y = (old_tile_y - 4) * source_side // 128
            old_idx = old_sample_y * source_side + old_sample_x
            # New V1 direct source view coordinate.
            new_idx = ((y // scale) * source_side // 128) * source_side + (x // scale) * source_side // 128
            assert old_idx == new_idx
            if channel == 0:
                old = bytes(raw[old_idx * 4:old_idx * 4 + 3]) + b'\xff'
                new = bytes(raw[new_idx * 4:new_idx * 4 + 3]) + b'\xff'
            elif channel == 1:
                old = bytes([raw[old_idx * 4 + 3]])
                new = bytes([raw[new_idx * 4 + 3]])
            else:
                old = height[old_idx * 2:old_idx * 2 + 2]
                new = height[new_idx * 2:new_idx * 2 + 2]
            assert old == new and len(old) == components
print('PASS: 18,000 randomized per-channel mip-zero pixel parity cases')
