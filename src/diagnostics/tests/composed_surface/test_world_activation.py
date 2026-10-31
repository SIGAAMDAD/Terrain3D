#!/usr/bin/env python3
"""AVT-01: no alternate composed-GPU/world selection path is active."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
owner = (root / 'src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h').read_text()
forward = (root / 'src/rendering/rd/forward/terrain_3d_forward_pass_preparer.cpp').read_text()
coordinator = (root / 'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text()
catalog = (root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
assert 'm_ComposedGpu' not in owner
assert 'm_ComposedCache' not in owner
assert 'acquireComposedSurfaceBindingsHot(' not in forward
assert '&rServices.m_virtualTexture.get_gpu(), NULL, pEnvironment )' in forward
assert 'serviceComposedGpuCompletionsHot(' not in coordinator
assert 'findComposedRegionAllocationCold(' in owner
# SHD-05 retired composed raster trial variants before this migration.
assert '"cs_terrain_strip_' not in catalog
print('AVT-01 retired independent composed GPU raster owner: PASS')
