from pathlib import Path

root = Path(__file__).resolve().parents[1]
instance = (root / 'src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp').read_text()
ocean = (root / 'src/rendering/rd/ocean/terrain_3d_rd_ocean_renderer.cpp').read_text()
constants = (root / 'src/Terrain3DConstants.h').read_text()
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text()

checks = [
    ('instance transparent color framebuffer uses three logical blend slots',
     'if ( transparent_pass ) {' in instance and 'blend_count = 3u;' in instance),
    ('instance transparent blending is restricted to color slot zero',
     'const bool blend_color = transparent_pass && i == 0u;' in instance),
    ('ocean transparent path supplies all three Forward+ color slots',
     'for ( uint32_t i = 0u; i < 3u; ++i )' in ocean and 'const bool blend_color = i == 0u;' in ocean),
    ('native/editor build IDs agree',
     any(build in constants and build in plugin for build in (
         'forward-color-blend-attachment-contract-fix-2026-09-09',
         'instancer-category-policy-p3-2026-09-09',
         'instancer-terrain-follow-p4-2026-09-09',
         'instancer-publication-p5-2026-09-09',
         'instancer-hlod-handoff-p6-2026-09-09',
         'instancer-occlusion-p7-2026-09-09',
         'mesh-derived-instance-physics-occlusion-p13-2-2026-09-10',
     ))),
]

failed = [name for name, ok in checks if not ok]
if failed:
    for name in failed:
        print('FAIL:', name)
    raise SystemExit(1)

print('Forward+ color blend attachment contract verification PASS')
