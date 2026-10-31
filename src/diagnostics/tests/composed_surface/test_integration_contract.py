#!/usr/bin/env python3
"""material classification cold-classification, opt-in cutover and bounded-cache contracts."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
read = lambda s: (root / s).read_text()
edit = read('src/data/editing/terrain_3d_map_edit_service.cpp')
data = read('src/data/model/terrain_3d_data.cpp')
runtime = read('src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.cpp')
clipmap = read('src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp')
shader = read('src/rendering/material/terrain_3d_shader_specializer.h')
assert 't3d_material_tiles_invalidate' in edit
assert 'rebuildControlTileClassesCold( location, pRegion )' in edit
assert 'rebuildControlTileClassesCold( regionLocation, pRegion.ptr() )' in edit
assert 'refreshControlTileClassesCold( regionLoc, rRegion.ptr() )' in data
assert 'invalidateControlTileClassesCold( regionLoc )' in data
assert 'bCompleteDrawCoverage' in shader and 'bStaticControls' in shader
assert 'pWholeDrawTileCoverage' in shader
assert 'm_bComposedCutoverProofCurrent = false;' in runtime
assert 'm_bComposedCutoverProofCurrent = t3d_comp_surf_cutover_proof_current(' in runtime
assert 'm_bComposedSurfaceReady && m_bComposedCutoverProofCurrent' in runtime
assert 'forward_pipeline_cache_high_water' in clipmap
assert 'forward_pipeline_cache_evictions' in clipmap
assert 'forward_pipeline_cache_capacity' in clipmap
assert not any('prepareComposedSurfaceCold(' in read(str(p.relative_to(root))) for p in
               (root/'src').rglob('*.cpp') if p.name != 'terrain_3d_shared_vt_runtime.cpp')
print('BR22 classification, fail-closed composed cutover and bounded diagnostics PASS')
