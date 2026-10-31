#!/usr/bin/env python3
"""CS20: bounded composed journal tracks edits; no brush-time baking."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[4]
coordinator=(root/'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text()
fragment=(root/'project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc').read_text()
source=coordinator.split('void Terrain3DRenderCoordinator::onTerrainMapsEdited(',1)[1].split('bool Terrain3DRenderCoordinator::claimTerrainMegaVTIncrementalCook(',1)[0]
assert re.search(r'#if\s+defined\(\s*T3D_EDITOR_NATIVE\s*\)', source)
assert 't3d_comp_surf_editor_dirty_bounds(' in source
assert 'persistTerrainMegaVTEditorStateHot();' in source
math=(root/'project/addons/terrain_3d/rd/terrain_composed_material_math.glslinc').read_text()
assert 'float pair_weight_inv = 1.0 / max(base_weight + overlay_weight, 0.00001);' in math
assert 'return overlay_weight * pair_weight_inv;' in math
assert 'float height_blend = t3d_comp_surf_height_blend(blend, samples.base.height, samples.overlay.height, sharpness);' in fragment
print('CS20 editor journal and original shipping blend PASS')
