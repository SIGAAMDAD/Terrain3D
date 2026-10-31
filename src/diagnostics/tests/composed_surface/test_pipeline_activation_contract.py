#!/usr/bin/env python3
"""AVT-01: V1 composed-page semantic/ABI allocation contract."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
enums = (root / 'src/Terrain3DEnums.h').read_text()
contract = (root / 'src/rendering/rd/vt/core/terrain_3d_vt_contract.h').read_text()
reader = (root / 'src/data/formats/terrain_3d_vt_pack_reader.cpp').read_text()
cooker = (root / 'src/authoring/vt/build/terrain_3d_vt_cooker.cpp').read_text()
profiles = (root / 'src/rendering/rd/vt/runtime/terrain_3d_vt_fixed_profile.cpp').read_text()
assert 'T3D_VT_CONSUMER_COMPOSED = 2u' in enums
assert 'T3D_VT_COMPOSED_REQUIRED_CHANNEL_MASK' in contract
assert 'T3D_VT_LOGICAL_CHANNEL_NORMAL ) { rSlot = 1u' in contract
assert 'T3D_VT_LOGICAL_CHANNEL_PBR ) { rSlot = 2u' in contract
assert 't3d_vt_is_composed_region_stable_key(' in contract
assert 't3d_vt_consumer_allowed_mask(' in reader
assert 't3d_vt_consumer_required_mask(' in reader
assert 't3d_vt_consumer_required_mask(' in cooker
assert 't3d_vt_consumer_required_mask(' in profiles
print('AVT-01 V1 BC1/BC5/BC7 semantic allocation contract: PASS')
