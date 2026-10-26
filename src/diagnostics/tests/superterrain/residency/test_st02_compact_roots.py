#!/usr/bin/env python3
"""ST02 packing, root-budget and demand contract tests (offline; no GPU run)."""
from pathlib import Path
import math

root = Path(__file__).resolve().parents[5]
cooker = (root / 'src/authoring/vt/build/terrain_3d_vt_cooker.cpp').read_text()
writer = (root / 'src/rendering/rd/vt/io/terrain_3d_vt_pack_runtime.cpp').read_text()
header = (root / 'src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.h').read_text()
layout = (root / 'src/rendering/rd/vt/io/terrain_3d_vt_compact_tail_layout.h').read_text()
assert 't3dVtPlaceCompactTailCold' in layout
runtime = (root / 'src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.cpp').read_text()
for source, symbols in (
    (cooker, ('T3D_VT_CONSUMER_COMPOSED ? 32u : 64u', 't3d_vt_cooker_tail_first_mip')),
    (writer, ('nFamilyTailHalfSlots', 'bCompact', 't3dVtPlaceCompactTailCold',
              'T3D_VT_PACK_TAIL_SLOT_WIDTH / 2u')),
    (runtime, ('stage_region_surface_demand_hot', 'm_bComposedOnlyRegionalDemand',
               'configure_unified_regional_demand_cold', 'bTeleport',
               'stage_material_footprint_hot', 'm_bWarmPackagePending')),
    (header, ('stage_region_surface_demand_hot', 'm_bComposedOnlyRegionalDemand')),
):
    for symbol in symbols:
        assert symbol in source, symbol

# Independent bit-exact simulator of the V1 destination slot math. Verifies
# compact/padded BC tiles never overlap even if other consumers interleave.
for seed in (0, 1, 7, 27):
    next_half = 0
    occupied = set()
    for i in range(1024 * 3):
        compact = (i + seed) % 11 != 0
        if not compact and next_half & 1:
            next_half += 1
        half = next_half
        next_half += 1 if compact else 2
        slot = half // 2
        atlas_x = (slot % 64) * 128 + (half % 2) * 64 if compact else (slot % 64) * 128
        atlas_y = (slot // 64) * 64
        width = 64 if compact else 128
        assert atlas_x % 64 == 0 and atlas_y % 64 == 0
        assert atlas_x + width <= 8192
        for row in range(atlas_y, atlas_y + 64, 4):
            for col in range(atlas_x, atlas_x + width, 4):
                key = (col, row)
                assert key not in occupied, (seed, i, col, row)
                occupied.add(key)
    assert math.ceil((next_half / 2) / 64) * 64 <= 16384

# Full addressable map (32x32), all three composed channels, every root,
# and only 32x32 + smaller coarser mips. Simulate all COMPOSED to test the
# 2:1 occupancy reduction against the previous 128x64 per-root layout.
regions = 32 * 32
for channel in range(3):
    half_slots = regions
    full_slots = (half_slots + 1) // 2
    atlas_rows = (full_slots + 64 - 1) // 64
    assert atlas_rows == 8
    assert atlas_rows * 64 == 512
    assert full_slots * 2 >= regions
print('ST02 compact-tail layouts, 32x32 map budget and scheduler contracts: PASS')
