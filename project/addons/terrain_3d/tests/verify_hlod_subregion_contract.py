"""Source-level guard for opt-in 64m streamable HLOD leaves. Does not simulate Godot or GPU."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
read = lambda p: (root / p).read_text()
baker = read('project/addons/terrain_3d/tools/hlod_baker.gd')
panel = read('project/addons/terrain_3d/tools/hlod_panel.gd')
settings = read('src/hlod/format/terrain_3d_hlod_layer_settings.cpp')
codec = read('src/hlod/format/terrain_3d_hlod_runtime_codec.cpp')
streamer = read('src/hlod/runtime/terrain_3d_hlod_streamer.cpp')
schedule = read('project/addons/terrain_3d/tools/hlod_build_schedule.gd')
tests = read('project/addons/terrain_3d/tests/hlod_subregion_64m_test.gd')
checks = {
    'editor 64m source routing': '_subregion_key_for_source' in baker and 'subregion_slot' in baker,
    'instancer transform grouping': '_split_instancer_subregions' in baker and 'batches[slot].append(transform)' in baker,
    'merge preserves subgroup instances': 'for transform: Transform3D in direct_transforms' in baker,
    'region-level ownership': '_level_region_min(level, cluster_key)' in baker and '_level_region_max(level, cluster_key)' in baker,
    'signed level runtime representation': 'entry.m_nLevel = -1' in codec and 'writer.writeU8( ( uint8_t ) entry.m_nLevel )' in codec,
    'old hierarchy read compatibility': 'wire_version != 2u && wire_version != 3u' in codec,
    'packed spatial checks': 'const int scale = level < 0 ? 2 : 1' in codec,
    'offline scheduler level -1': 'level < -1' in schedule,
    'GPU publication independent per subcell': 'm_szRegionReadySubcells[subRegion] |= bit' in streamer and 'if ( !entry.m_bActive || !subcellGroupActive( entry )' not in streamer,
    'subcell lookup cold only': 'buildSubcellLookupCold( void )' in streamer,
    'new configuration invalidates cooking': 'm_bSubregionLeaves' in settings,
    'exposed editor control': '"64m streamable subcells"' in panel,
    'headless fixture included': 'manifest.compile_runtime_blob()' in tests and 'package.level = -1' in tests,
}
for label, passed in checks.items():
    assert passed, label
    print('PASS:', label)
