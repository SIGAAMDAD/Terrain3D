#!/usr/bin/env python3
"""Regression gate for RDShaderFile feature flags and native/GLSL VT hint routing.

This checks preprocessor-active code in representative shader variants without
requiring a graphics device. GLSL/SPIR-V validation remains a separate gate.
"""
from pathlib import Path
import re
import shutil

from compile_fragment_variants import INCLUDE, emit_fragment, shader_root, split_sections, variants
from verify_fragment_material_variants import test_cpp

ROOT = Path(__file__).resolve().parents[4]
RD = shader_root(ROOT)
CPP = shutil.which('cpp')
assert CPP, 'GNU cpp required'

# Wave labels are history, not stable feature gates.
for path in list(RD.rglob('*.glsl')) + list(RD.rglob('*.glslinc')):
    assert not re.search(r'\bT3D_BR\d+_[A-Z0-9_]+\b', path.read_text()), path

# A single native gate controls the GPU buffer, resource plan, renderer, and
# shader import catalog. The build macro alone never rewrites GLSL versions.
cpp_files = tuple(ROOT / path for path in (
    'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp',
    'src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.cpp',
    'src/rendering/rd/vt/gpu/terrain_3d_vt_gpu.cpp',
    'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp',
))
for path in cpp_files:
    contents = path.read_text()
    assert 'T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS' in contents, path
    assert 'T3D_BR20_ENABLE_HINTS' not in contents, path
constants = (ROOT / 'src/Terrain3DConstants.h').read_text()
assert '#define T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS ( 0 )' in constants

# Godot replaces #VERSION_DEFINES from the named RDShaderFile version. A
# subgroup flag that is tested *before* this point silently stays disabled.
fragment_roots = ('terrain_clipmap_near.glsl', 'terrain_clipmap_middle.glsl',
                  'terrain_clipmap_far.glsl', 'terrain_instances.glsl',
                  'terrain_world_raster.glsl', 'terrain_clipmap_screen_space.glsl')
for name in fragment_roots:
    stage = split_sections((RD / name).read_text())['fragment']
    define = stage.index('#VERSION_DEFINES')
    subgroup = stage.index('#if defined(T3D_FORWARD_SUBGROUP_CLUSTER_LIGHTING)')
    assert define < subgroup, name

checks = 0

def expand(name, variant, extra=()):
    global checks
    source = emit_fragment(RD, name, variant)
    for macro in extra:
        source = source.replace('#version 450\n', '#version 450\n#define ' + macro + '\n', 1)
    output = test_cpp(CPP, source, ())
    checks += 1
    return output

for terrain in ('terrain_clipmap_near.glsl', 'terrain_clipmap_middle.glsl',
                'terrain_clipmap_far.glsl'):
    selected = variants(split_sections((RD / terrain).read_text()))
    assert '#define T3D_MATERIAL_ACTIVE_KEY_LOOP 1' in selected['active_key_native']
    assert '#define T3D_VT_FIXED_FILTER_TAPS 1' in selected['fixed_taps_native']
    for trial in ('vt_ancestor_hints_native', 'vt_ancestor_hints_prepared_bias_native',
                  'feedback_color_native_vt_hints',
                  'feedback_color_native_vt_hints_prepared_bias'):
        assert '#define T3D_VT_RESIDENT_ANCESTOR_HINTS 1' in selected[trial]

    plain = expand(terrain, 'color_native')
    hinted = expand(terrain, 'vt_ancestor_hints_native')
    unrolled = expand(terrain, 'fixed_taps_native')
    keyed = expand(terrain, 'active_key_native')
    disabled = expand(terrain, 'color_native', ('T3D_VT_RESIDENT_ANCESTOR_HINTS 0',))
    assert 'TerrainVTAncestorHints' not in plain and 'TerrainVTAncestorHints' not in disabled, terrain
    assert 'TerrainVTAncestorHints' in hinted, terrain
    assert plain != unrolled and plain != keyed, terrain

# The compact list variant must instantiate its optional set-4 ABI only when
# the corresponding semantic variant is selected, including explicit zero.
for shader, plain_name, compact_name in (
    ('terrain_clipmap_near.glsl', 'color_native', 'color_native_compact'),
    ('terrain_instances.glsl', 'color_native', 'color_native_compact'),
    ('terrain_world_raster.glsl', 'mesh_color_native', 'mesh_color_native_compact'),
):
    plain = expand(shader, plain_name)
    compact = expand(shader, compact_name)
    zero = expand(shader, plain_name, ('T3D_FORWARD_COMPACT_CLUSTER_LISTS 0',))
    assert 'T3DBr21CompactHeaders' not in plain and 'T3DBr21CompactHeaders' not in zero
    assert 'T3DBr21CompactHeaders' in compact, shader

# Stage-specific screen space flags must result in mutually exclusive vertex
# and fragment interfaces and retain their five RGBA32F render-target ABI.
screen = (RD / 'terrain_clipmap_screen_space.glsl').read_text()
sections = split_sections(screen)
selected = variants(sections)
assert '#define T3D_SCREEN_SPACE_COVERAGE_PASS 1' in selected['coverage']
assert '#define T3D_SCREEN_SPACE_SHADE_PASS 1' in selected['shade']
coverage = expand('terrain_clipmap_screen_space.glsl', 'coverage')
shade = expand('terrain_clipmap_screen_space.glsl', 'shade')
assert 'out vec4 t3d_screen_space_world_dxz_dy' in coverage
assert 'out vec4 t3d_screen_space_world_dxz_dy' not in shade
assert 't3d_screen_space_shade_fragment()' in shade
assert 't3d_br23_world_dxz_dy_input' in shade

for version in ('coverage', 'shade'):
    source = INCLUDE.sub(lambda m: (RD / m.group(1)).read_text(), sections['vertex'])
    assert not INCLUDE.search(source), 'unexpected nested includes'
    output = test_cpp(CPP, source.replace('#VERSION_DEFINES', selected[version]), ())
    checks += 1
    if version == 'shade':
        assert 'gl_VertexIndex << 1' in output
    else:
        assert 'gl_VertexIndex << 1' not in output

# Enabled subgroup path must differ from baseline, and zero must not enable it.
for shader in ('terrain_clipmap_near.glsl', 'terrain_instances.glsl'):
    baseline = expand(shader, 'color_native')
    enabled = expand(shader, 'color_native', ('T3D_FORWARD_SUBGROUP_CLUSTER_LIGHTING 1',))
    zero = expand(shader, 'color_native', ('T3D_FORWARD_SUBGROUP_CLUSTER_LIGHTING 0',))
    assert 'subgroupAllEqual' not in baseline and 'subgroupAllEqual' not in zero
    assert 'subgroupAllEqual' in enabled, shader

print(f'PASS RDShaderFile flag contracts: {len(fragment_roots)} fragment roots, {checks} expanded stages')
