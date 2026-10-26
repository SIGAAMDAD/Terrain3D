from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]

def read(rel):
    return (root / rel).read_text(encoding='utf-8')

baker = read('project/addons/terrain_3d/tools/hlod_baker.gd')
panel = read('project/addons/terrain_3d/tools/hlod_panel.gd')
plugin = read('project/addons/terrain_3d/src/editor_plugin.gd')
builder = read('src/hlod/build/terrain_3d_hlod_builder.cpp')
builder_h = read('src/hlod/build/terrain_3d_hlod_builder.h')
manifest = read('src/hlod/format/terrain_3d_hlod_manifest.cpp')
manifest_h = read('src/hlod/format/terrain_3d_hlod_manifest.h')

checks = {
    'plugin initializes HLOD panel hidden': 'hlod_panel.visible = false' in plugin,
    'HLOD panel hides itself in _ready': 'close_requested.connect(hide)\n\thide()' in panel,
    'builder version bumped for new bake contract': 'const BUILDER_VERSION: int = 7' in baker,
    'instancer analysis API declared': 'analyzeInstancerSource' in builder_h,
    'instancer analysis API bound': 'analyze_instancer_source' in builder,
    'analysis path is deferred': '_native_builder.analyze_instancer_source' in baker and 'source.deferred_instancer = true' in baker,
    'dirty bake materializes only after reuse decision': '_materialize_deferred_instancer_sources' in baker and baker.index('_materialize_deferred_instancer_sources') > baker.index('if dirty_only and existing_entries.has(entry_key)'),
    'native instancer source is chunked': 's_nChunkTriangleBudget' in builder and 'result["chunks"] = chunks' in builder,
    'dense instancer preproxy budget exists': 's_nPreProxyTriangleBudget' in builder,
    'manifest path centralized': baker.count('_resolve_manifest_path(terrain)') >= 3,
    'packed runtime blob validation exposed': 'validateRuntimeBlob' in manifest_h and 'validate_runtime_blob' in manifest,
    'validation checks active build signature': 'expected_build_signature' in baker and 'manifest.build_signature != expected_build_signature' in baker,
    'validation checks runtime blob': 'manifest.validate_runtime_blob()' in baker,
    'pathless resources recursively content-signed': '_resource_content_signature_recursive' in baker and '_variant_content_signature' in baker,
    'coplanar simplification gets topology error floor': 'topology_floor' in builder,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(('PASS' if ok else 'FAIL') + ': ' + name)
if failed:
    print(f'HLOD system patch verification FAIL ({len(failed)} checks)')
    sys.exit(1)
print('HLOD system patch verification PASS')
