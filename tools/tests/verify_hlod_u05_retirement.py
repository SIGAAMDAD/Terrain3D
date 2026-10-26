#!/usr/bin/env python3
"""Structural U05 retirement gate; native source is compiled separately."""
from pathlib import Path
r=Path(__file__).resolve().parents[2]
def read(p): return (r/p).read_text()
builder=read('src/hlod/build/terrain_3d_hlod_builder.cpp')
header=read('src/hlod/build/terrain_3d_hlod_builder.h')
baker=read('project/addons/terrain_3d/tools/hlod_baker.gd')
settings=read('src/hlod/format/terrain_3d_hlod_layer_settings.cpp')
panel=read('project/addons/terrain_3d/tools/hlod_panel.gd')
tests=read('src/diagnostics/tests/unit_testing.cpp')
checks={
 'no SurfaceTool native generate_lod fallback':'generate_lod(' not in builder,
 'legacy topology floor retired':'topology_floor' not in builder,
 'legacy native binding stays available':'"simplify_mesh"' in builder and 'simplifyMeshWithError( rMesh, fRatio, 0.0f )' in builder,
 'chunk simplification takes explicit error ceiling': 'fMaxWorldError' in header and 'simplifyMeshWithError( exact, fPreRatio, fMaxWorldError )' in builder,
 'dense instancer enforces triangle budget': 'proxy_triangles > T3D_HLOD_PRE_PROXY_TRIANGLE_BUDGET' in builder,
 'source pre-cook gets budget from editor': 'pre_error_budget' in baker and 'source.mesh_id, resolved_recipe == 2 or resolved_recipe == 1, pre_error_budget)' in baker,
 'new packages are not legacy v1': 'package.format_version = 1 if' not in baker and 'package.recipe_version = 2' in baker,
 'old runtime compatibility retained': 'T3D_HLOD_LAYER_LEGACY_SIMPLIFIED' in read('src/hlod/format/terrain_3d_hlod_package.h'),
 'new recipe default and old serialized selection migrate': 'm_nLayerType = T3D_HLOD_LAYER_SIMPLIFIED' in read('src/hlod/format/terrain_3d_hlod_layer_settings.h') and 'value == T3D_HLOD_LAYER_LEGACY_SIMPLIFIED' in settings,
 'cook version invalidates older legacy output': 'terrain3d-hlod-recipe-v5' in baker and 't3dHlodRecipeHashWord( nHash, 5 )' in settings,
 'UI no longer advertises legacy cooking': 'add_item("Legacy Simplified' not in panel and '_layer_type.get_item_index(' in panel,
 'per-layer disk and error bake report': 'per_layer_stats[recipe_id]' in baker and 'package_disk_bytes' in baker and 'max_geometric_error' in panel,
 'native migration regression assertions': 'Serialized legacy authoring selection migrates' in tests,
}
for k,v in checks.items():
    assert v,k
    print('PASS:',k)
