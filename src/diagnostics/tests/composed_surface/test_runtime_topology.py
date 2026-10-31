#!/usr/bin/env python3
"""AVT-01: single GPU residency and page-table owner contract."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
header = (root / 'src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h').read_text()
owner = (root / 'src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.cpp').read_text()
coordinator = (root / 'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text()
v3 = (root / 'src/rendering/rd/vt/runtime/terrain_3d_virtual_texture.cpp').read_text()
assert 'Terrain3DVirtualTexture' in header
assert 'Terrain3DComposedSurfaceGpu m_ComposedGpu' not in header
assert 'Terrain3DComposedSurfaceCache m_ComposedCache' not in header
assert 'prepareComposedSurfaceCold(' not in owner
assert 'serviceComposedGpuCompletionsHot(' not in coordinator
assert 'findComposedRegionAllocationCold(' in owner
assert 't3d_vt_composed_region_stable_key(' in owner
assert 'm_szComposedRegionAllocations[region]' in v3
assert 'stage_material_footprint_hot( id,' in v3
assert 'beginVtResourceReplacementCold(' in owner
assert 'invalidateBoundDescriptorsCold( rContext );' in owner
print('AVT-01 single V1 allocation/page-table owner contract: PASS')
