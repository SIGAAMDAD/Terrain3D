"""Offline source-integration assertions; not a Godot streaming or GPU test."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
def read(path): return (root / path).read_text()
checks = {
    'baker writes source ownership': 'manifest.instancer_subcell_masks = mesh_masks' in read('project/addons/terrain_3d/tools/hlod_baker.gd'),
    'baker protects cross-boundary meshes': 'mesh_masks[offset] = mesh_masks[offset] & ~(1 << slot)' in read('project/addons/terrain_3d/tools/hlod_baker.gd'),
    'V2 V3 V4 compatible reader': 'wire_version != 2u && wire_version != 3u && wire_version != 4u' in read('src/hlod/format/terrain_3d_hlod_runtime_codec.cpp'),
    'cold codec checks leaf ownership': 'leafMask[T3D_REGION_SLOTS]' in read('src/hlod/format/terrain_3d_hlod_runtime_codec.cpp'),
    'instance publication bounds guard': 't3dHlodSubcellOwnerForBounds(' in read('src/instances/storage/terrain_3d_instance_publication_builder.cpp'),
    'node published by owned subcell': 'isHlodNodeVisualSuppressed(' in read('src/streaming/runtime/terrain_3d_node_streamer.cpp'),
    'mask changes republish detail': 'pOldSubcells[i] != m_szRegionReadySubcells[i]' in read('src/hlod/runtime/terrain_3d_hlod_streamer.cpp'),
    'leaf never claims whole region': 'Level -1 represents one quadrant, never the entire region.' in read('src/hlod/runtime/terrain_3d_hlod_streamer.cpp'),
    'no atomic group publication gate': '!subcellGroupActive( entry )' not in read('src/hlod/runtime/terrain_3d_hlod_streamer.cpp'),
}
for label, passed in checks.items():
    assert passed, label
    print('PASS', label)
