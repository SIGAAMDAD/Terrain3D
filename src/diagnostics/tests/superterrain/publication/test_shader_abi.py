#!/usr/bin/env python3
"""Static ST05 ABI wiring assertions. This does not compile GLSL or run Vulkan."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[5]
SHADERS = ROOT / 'project/addons/terrain_3d/rd'


def read(relative):
    result = (ROOT / relative).read_text(encoding='utf-8')
    assert result
    return result


for shader, set_no, bindings in (
    ('terrain_clipmap.glsl', 0, (46, 47, 48, 49, 50, 51)),
    ('terrain_world_raster.glsl', 1, (20, 21, 22, 23, 24, 25)),
):
    text = (SHADERS / shader).read_text(encoding='utf-8')
    assert text.count('#[vertex]') == 1 and text.count('#[fragment]') == 1
    vertex, fragment = text.split('#[fragment]', 1)
    macros = {
        'T3D_ST_VHM_SET': set_no,
        'T3D_ST_VHM_PAGE_BINDING': bindings[0],
        'T3D_ST_VHM_TAIL_BINDING': bindings[1],
        'T3D_ST_VHM_FINE_IMAGE_BINDING': bindings[2],
        'T3D_ST_VHM_ROOT_IMAGE_BINDING': bindings[3],
        'T3D_ST_CUTOUT_SET': set_no,
        'T3D_ST_CUTOUT_ENTRY_BINDING': bindings[4],
        'T3D_ST_CUTOUT_WORD_BINDING': bindings[5],
    }
    for macro, value in macros.items():
        branch = vertex if macro.startswith('T3D_ST_VHM') else fragment
        assert re.search(rf'^#define\s+{macro}\s+{value}$', branch, re.M), (shader, macro)
    assert vertex.index('#include "terrain_clipmap_height_vhm.glslinc"') < vertex.index('#include "terrain_clipmap_vertex_inc.glslinc"')
    assert fragment.index('#include "terrain_clipmap_cutout.glslinc"') < fragment.index('#include "terrain_clipmap_fragment_inc.glslinc"')

abi = read('src/Terrain3DTypes.h')
assert 'static_assert( sizeof( t3dRdTerrainRegionGpu_t ) == 64' in abi
region = read('project/addons/terrain_3d/rd/terrain_clipmap_contract.glslinc')
for field in ('composed_vt_allocation', 'vhm_root_index', 'mask_root_index',
              'publication_generation', 'region_state_flags'):
    assert field in region, field
for file in ('src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp',
             'src/rendering/rd/world/visibility/terrain_3d_world_raster.cpp'):
    code = read(file)
    for macro in ('ST_VHM_PAGES', 'ST_VHM_TAILS', 'ST_VHM_FINE', 'ST_VHM_ROOT',
                  'ST_MASK_ENTRIES', 'ST_MASK_WORDS'):
        assert re.search(r'T3D_RD_\w+_' + macro + r'_BINDING', code), (file, macro)

height = read('project/addons/terrain_3d/rd/terrain_clipmap_vertex_inc.glslinc')
scalar = height.split('float sample_height_texel_mode(', 1)[1].split('HeightSamples4 sample_height_texels4(', 1)[0]
vector = height.split('HeightSamples4 sample_height_texels4(', 1)[1].split('float sample_height_nearest_texel(', 1)[0]
assert 't3d_st_sample_height_meters(vec2(texel)' in scalar
assert all(f't3d_st_sample_height_meters(vec2(t{i})' in vector for i in range(4))
fragment = read('project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc')
assert 't3d_cutout_sample(' in fragment and 't3d_comp_surf_region_ready' in fragment
print('ST05: shader include order, descriptor indices, 64-byte region and all-distance VHM sampling: PASS')
