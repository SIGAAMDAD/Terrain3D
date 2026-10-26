#!/usr/bin/env python3
"""Check the opt-in Forward+ composed descriptor ABI and fail-closed ownership."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[4]
shader = (root / 'project/addons/terrain_3d/rd/terrain_clipmap_fragment_interface.glslinc').read_text()
world = (root / 'project/addons/terrain_3d/rd/terrain_world_raster.glsl').read_text()
gpu = (root / 'src/rendering/rd/vt/gpu/terrain_3d_composed_surface_gpu.cpp').read_text()
header = (root / 'src/rendering/rd/vt/gpu/terrain_3d_composed_surface_gpu.h').read_text()
table = (root / 'src/rendering/rd/vt/core/terrain_3d_composed_surface_gpu_table.h').read_text()
clip = (root / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()
owner = (root / 'src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h').read_text()

assert '#define T3D_CS_ACTIVE' not in shader
assert 'm_bMappingsCoherent' in header
assert 'm_bMappingsCoherent = false' in gpu
assert '!m_bMappingsCoherent || !m_pageTable.isValid()' in gpu
assert 'm_bComposedSurfaceReady &&' in owner
assert 'm_bComposedCutoverProofCurrent && m_ComposedGpu.areMappingsCoherent()' in owner
assert 'validateComposedSourceHot' in owner
assert 'rollbackStagedResident' in gpu and 'rollbackStagedResident' in table
assert 't3dComposedSurfaceGpuEntry_t' in table
assert 'sizeof( t3dComposedSurfaceGpuEntry_t ) == 48u' in table

standalone = [int(re.search(r'#define ' + label + r' (\d+)', shader).group(1)) for label in
              ('T3D_CS_PAGE_TABLE_BINDING', 'T3D_CS_ALBEDO_BINDING',
               'T3D_CS_NORMAL_BINDING', 'T3D_CS_SURFACE_BINDING')]
embedded = [int(re.search(r'#define ' + label + r' (\d+)', world).group(1)) for label in
            ('T3D_CS_PAGE_TABLE_BINDING', 'T3D_CS_ALBEDO_BINDING',
             'T3D_CS_NORMAL_BINDING', 'T3D_CS_SURFACE_BINDING')]
assert standalone == [39, 40, 41, 42]
assert embedded == [13, 14, 15, 16]
assert 'single( 38,' in clip  # binding 38 remains occupied by strip parts
assert len(standalone) == len(set(standalone)) == 4
assert len(embedded) == len(set(embedded)) == 4
print('CS17 opt-in composed descriptors and fail-closed publication topology PASS')
