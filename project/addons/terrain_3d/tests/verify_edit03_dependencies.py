#!/usr/bin/env python3
"""Static/source-contract and deterministic footprint checks for EDIT-03.

This is not a Godot GDScript/native compilation or an on-GPU bake test.
"""
from pathlib import Path
import math

root = Path(__file__).resolve().parents[1]
helper = (root / 'tools/authoring_dependencies.gd').read_text()
auto = (root / 'src/autoshader_panel.gd').read_text()
foliage = (root / 'tools/foliage_panel.gd').read_text()
hlod = (root / 'tools/hlod_baker.gd').read_text()
panel = (root / 'tools/hlod_panel.gd').read_text()
owner = (root.parents[2] / 'src/data/model/terrain_3d_data.cpp').read_text()


def check(condition, label):
    if not condition:
        raise AssertionError(label)
    print('PASS', label)


def footprint(x, z, sx, sz, width=128, spacing=1):
    if sx <= 0 or sz <= 0:
        return set()
    pad = spacing
    lo_x = math.floor((x - pad) / width)
    lo_z = math.floor((z - pad) / width)
    hi_x = math.floor((x + sx + pad - .0001) / width)
    hi_z = math.floor((z + sz + pad - .0001) / width)
    return {(a, b) for a in range(lo_x, hi_x + 1) for b in range(lo_z, hi_z + 1)}


def clusters(regions, max_level=6, origin=-32):
    return {(level, math.floor((x - origin) / (1 << level)),
             math.floor((y - origin) / (1 << level)))
            for x, y in regions for level in range(1, max_level + 1)}

check('static func products_for_sources' in helper, 'dependency source-product table present')
for tag in ['HEIGHT', 'CONTROL', 'COLOR', 'MATERIAL', 'RULES', 'TOPOLOGY']:
    check(('const '+tag+': int') in helper, f'{tag} source key present')
for tag in ['AUTOSHADER', 'FOLIAGE', 'HLOD', 'FAR', 'COLLISION']:
    check(('const '+tag+': int') in helper, f'{tag} derived product present')
check('UNKNOWN_MAP: int = HEIGHT | CONTROL | COLOR' in helper, 'unknown map sources have conservative fallback')
check('static func source_for_map_type(map_type: int)' in helper and
      all(f'Terrain3DRegion.TYPE_{tag}' in helper for tag in ('HEIGHT', 'CONTROL', 'COLOR')),
      'typed channel maps to native Terrain3DRegion enum constants')
check('static func regions_for_committed_edit' in helper and
      'committed.has(region + Vector2i(dx, dy))' in helper,
      'typed edit projection keeps only touched source regions and their halo')
check('ADD_SIGNAL( MethodInfo( "maps_edit_committed"' in owner and
      'PropertyInfo( Variant::INT, "map_type" )' in owner and
      'PropertyInfo( Variant::ARRAY, "changed_region_locations", PROPERTY_HINT_ARRAY_TYPE, "Vector2i" )' in owner and
      'PropertyInfo( Variant::AABB, "edited_area" )' in owner,
      'native owner publishes type, changed region array and AABB')
check('emit_signal( "maps_edit_committed", ( int ) ( eMapType ), changed_region_locations, mutation.m_editedArea )' in owner and
      'has_connections( "maps_edit_committed" )' in owner and
      'emit_signal( "maps_edited", mutation.m_editedArea )' in owner,
      'native emits typed signal after valid edit while preserving legacy listeners')

check('halo_samples: int = 1' in helper and 'area.end.x + padding' in helper,
      'halo included in both AABB endpoints')
check(footprint(20, 20, 2, 2) == {(0, 0)}, 'interior footprint exact')
check(footprint(126, 60, 4, 4) == {(0, 0), (1, 0)}, 'positive horizontal edge')
check(footprint(60, 126, 4, 4) == {(0, 0), (0, 1)}, 'positive vertical edge')
check(footprint(-1, -1, 2, 2) == {(-1, -1), (-1, 0), (0, -1), (0, 0)}, 'crosses 4 neighboring regions')
check(footprint(20, 20, 0, 1) == set(), 'no-op invalid area maps to empty set')
check((3, 4, 4) in clusters({(0, 0)}, 3), 'HLOD key stable for origin-offset quadtree')
check(len(clusters({(0, 0)}, 6)) == 6, 'one region dirties one cluster per HLOD level')
check(len(clusters({(0, 0), (1, 0)}, 6)) == 6, 'adjacent sectors collapse into same ancestors')
check('_schedule_auto_rebake()' in auto and
      'if _profile.mode == Terrain3DAutoShaderProfile.Mode.EDITOR_PREVIEW:' in auto,
      'autoshader preview remains explicit editor-only')
check('if _profile.mode == Terrain3DAutoShaderProfile.Mode.BAKED and _profile.auto_rebake_after_sculpt:' not in auto,
      'production autoshader not auto-baked by timer')
check('mode == "regenerate_dirty"' in foliage and '_is_region_stale(region)' in foliage and
      '_button("Regenerate Dirty"' in foliage and 'if regions.is_empty():' in foliage,
      'foliage dirty regeneration exposes selective explicit action and skips unchanged source keys')
check('_manifest.prepare_owner_changes(requests, regions, mode,' in foliage,
      'foliage selective recook uses existing transactional ledger')
check('if dirty_only and built == 0 and _same_published_manifest(existing, manifest):' in hlod,
      'HLOD no-op does not republish identical manifest')
check('previous.node_spawn_ids == planned.node_spawn_ids' in hlod and
      'previous.entries != planned.entries' in hlod, 'HLOD compares policy and all cluster entries')
check('if dirty_only and existing_entries.has(entry_key):' in hlod,
      'HLOD retains source signature branch reuse')
check('AuthoringDependencies.hlod_keys' in panel and
      'AuthoringDependencies.regions_for_committed_edit' in panel, 'HLOD panel consumes committed projection')
check('AuthoringDependencies.regions_for_committed_edit' in foliage and
      'AuthoringDependencies.regions_for_committed_edit' in auto,
      'authoring panels consume bounded typed committed regions')
for name, source, callback, product in (
    ('autoshader', auto, '_on_map_edit_committed', 'AUTOSHADER'),
    ('foliage', foliage, '_on_source_map_edit_committed', 'FOLIAGE'),
    ('HLOD', panel, '_on_map_edit_committed', 'HLOD'),
):
    check(f'maps_edit_committed.connect({callback})' in source and
          f'maps_edit_committed.disconnect({callback})' in source and
          f'func {callback}(map_type: int, changed_regions: Array[Vector2i], area: AABB)' in source and
          f'(products & AuthoringDependencies.{product}) == 0' in source and
          'AuthoringDependencies.source_for_map_type(map_type)' in source and
          'maps_edited.connect(' not in source,
          f'{name} uses typed source filtering, and avoids duplicate untyped callbacks')

def committed_footprint(changed, area):
    return {r for r in footprint(*area) if any(abs(r[0] - c[0]) <= 1 and
            abs(r[1] - c[1]) <= 1 for c in changed)}

check(committed_footprint({(0, 0)}, (20, 20, 3, 3)) == {(0, 0)},
      'typed edit interior leaves only committed region')
check(committed_footprint({(0, 0)}, (126, 20, 4, 3)) == {(0, 0), (1, 0)},
      'typed edit halo projects immediate neighbor at real boundary')
check(committed_footprint({(0, 0)}, (20, 20, 600, 3)) == {(0, 0), (1, 0)},
      'wide edited AABB cannot dirty uncommitted remote regions')
check(committed_footprint(set(), (20, 20, 600, 3)) == set(),
      'empty changed region vector cannot manufacture invalidation')

print('All EDIT-03 deterministic reference/static checks passed')
