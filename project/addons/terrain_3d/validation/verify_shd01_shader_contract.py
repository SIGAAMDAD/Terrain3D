#!/usr/bin/env python3
"""SHD-01: source-grounded catalog, descriptor and shader preprocessor checks.

Usage: python project/addons/terrain_3d/validation/verify_shd01_shader_contract.py
Uses only Python stdlib and clang's C preprocessor; does not compile GLSL/SPIR-V.
"""
import argparse
import ast
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
SRC = ROOT / 'src/rendering/rd'
CPP_BINDINGS = SRC / 'shaders/terrain_3d_rd_material_vt_contract.h'
GLSL_BINDINGS = RD / 'terrain_material_vt_contract.glslinc'
CATALOG = SRC / 'shaders/terrain_3d_rd_shader_catalog.cpp'


def ensure(cond, message):
    if not cond:
        raise AssertionError(message)


def versions(path):
    s = path.read_text()
    ensure('#[versions]' in s, 'No RDShaderFile versions: ' + str(path))
    table = s.split('#[versions]', 1)[1].split('#[', 1)[0]
    entries = {}
    for match in re.finditer(r'(?m)^([a-zA-Z_][a-zA-Z_0-9]*)\s*=\s*("(?:\\.|[^"\\])*")\s*;?', table):
        entries[match.group(1)] = ast.literal_eval(match.group(2))
    return entries


def array_initializer(catalog, name):
    rx = r'\b' + re.escape(name) + r'\s*(?:\[[^;=]*?\])*\s*=\s*\{'
    match = re.search(rx, catalog)
    ensure(match is not None, 'Missing C++ version array ' + name)
    pos = match.end()
    depth = 1
    while depth > 0 and pos < len(catalog):
        if catalog[pos] == '{':
            depth += 1
        elif catalog[pos] == '}':
            depth -= 1
        pos += 1
    ensure(depth == 0, 'Unclosed C++ array ' + name)
    return re.findall(r'"([^"\n]*)"', catalog[match.end():pos - 1])


def enum_values(header):
    return {name: int(number) for name, number in
            re.findall(r'(?m)^\s*(T3D_RD_[A-Z_0-9]+)\s*=\s*(\d+)u\s*,', header)}


def binding_values(source):
    return {name: int(number) for name, number in
            re.findall(r'(?m)^#define\s+(T3D_(?:TERRAIN_VT|CS)_[A-Z_0-9]+)\s+(\d+)\s*$', source)}


def preprocess(shader, variant):
    text = shader.read_text()
    choices = versions(shader)
    ensure(variant in choices, f'No shader version {shader.name}: {variant}')
    ensure('#[fragment]' in text, 'Missing fragment stage: ' + str(shader))
    fragment = text.split('#[fragment]', 1)[1]
    # Clang preprocesses GLSL macros/includes, not GLSL-specific directives.
    fragment = re.sub(r'(?m)^#(?:version|extension)\b[^\n]*$', '', fragment)
    fragment = fragment.replace('#VERSION_DEFINES', choices[variant])
    result = subprocess.run(['clang', '-E', '-P', '-x', 'c', '-undef', '-nostdinc',
                             '-I', str(shader.parent), '-'], input=fragment, text=True,
                            capture_output=True, check=False)
    ensure(result.returncode == 0,
           f'Preprocessor error {shader.name} [{variant}]: {result.stderr[:1800]}')
    ensure('#include ' not in result.stdout, f'Unresolved includes: {shader.name} [{variant}]')
    return result.stdout


def verify_layout(source, expected_set, expected_bindings, context):
    for block, binding in expected_bindings.items():
        # The layout may contain std430 across multiple source lines.
        pat = (r'layout\(\s*set\s*=\s*' + str(expected_set) +
               r'\s*,\s*binding\s*=\s*' + str(binding) +
               r'\b[^)]*\)\s*[^;{}]*\b' + re.escape(block) + r'\b')
        ensure(re.search(pat, source, re.S) is not None,
               f'{context}: missing set {expected_set}, binding {binding}, {block}')


def normal_shader_tokens(source):
    # Moving identical forward declarations earlier in the shader is allowed.
    # Everything else in the evaluated GLSL code should remain byte-token equal.
    source = re.sub(r'bool\s+t3d_vt_resolve_page_core(?:_loaded)?\s*\([^;]*\);', '', source)
    return re.sub(r'\s+', '', source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, help='Optional source root for full GLSL token parity')
    args = parser.parse_args()
    cs = CATALOG.read_text()
    choices = {p.name: versions(p) for p in RD.glob('*.glsl') if '#[versions]' in p.read_text()}
    # All named version strings in the catalog must exist in an actual shader
    # version table. Validate source paths as well as the nine ordered arrays.
    all_versions = set()
    for file in (ROOT / 'project/addons/terrain_3d').rglob('*.glsl'):
        if '#[versions]' in file.read_text():
            all_versions.update(versions(file))
    catalog_literals = set(re.findall(r'"([A-Za-z_]\w*)"', cs))
    missing_versions = catalog_literals - all_versions
    ensure(not missing_versions, 'Catalog references absent RDShaderFile versions: ' +
           ', '.join(sorted(missing_versions)))
    for path in re.findall(r'"(res://[^"\n]+\.glsl)"', cs):
        ensure((ROOT / 'project' / path.removeprefix('res://')).is_file(),
               'Catalog references missing shader source ' + path)
    targets = ['terrain_clipmap.glsl']
    for name, group in [('s_forwardVersions', 11), ('s_oneSunVersions', 8)]:
        entries = array_initializer(cs, name)
        ensure(len(entries) == 2 * group, f'{name}: expected {2 * group}, got {len(entries)}')
        for path in range(2):
            for file in targets:
                for version in entries[path * group:(path + 1) * group]:
                    ensure(version in choices[file], f'{name}: {file} missing {version}')
    for name, shaders in [
        ('s_worldStripVersions', ['terrain_world_raster.glsl']),
        ('s_worldMeshVersions', ['terrain_world_raster.glsl']),
        ('s_worldMeshFeedbackVersions', ['terrain_world_raster.glsl']),
        ('s_instanceFeedbackVersions', ['terrain_instances.glsl']),
    ]:
        for entry in array_initializer(cs, name):
            for file in shaders:
                ensure(entry in choices[file], f'{name}: {file} missing {entry}')

    cpp = CPP_BINDINGS.read_text()
    glsl = GLSL_BINDINGS.read_text()
    ensure(glsl.count('#if defined(T3D_WORLD_RASTER_EMBEDDED)') == 1,
           'Material VT layout must have one WORLD/clipmap policy split')
    header, remainder = glsl.split('#if defined(T3D_WORLD_RASTER_EMBEDDED)', 1)
    world_part, remainder = remainder.split('#else', 1)
    terrain_part = remainder.split('#endif', 1)[0]
    terrain = binding_values(terrain_part)
    world = binding_values(world_part)
    constants = enum_values(cpp)
    fields = {'DESCRIPTOR_SET': 'DESCRIPTOR_SET',
              'ALLOCATION_BINDING': 'ALLOCATION_BINDING',
              'PAGE_TABLE_BINDING': 'PAGE_TABLE_BINDING',
              'TAIL_METADATA_BINDING': 'TAIL_METADATA_BINDING',
              'MIP_LAYOUT_BINDING': 'MIP_LAYOUT_BINDING',
              'HINT_BINDING': 'HINT_BINDING'}
    for name, fragment in fields.items():
        ensure(terrain['T3D_TERRAIN_VT_' + name] == constants['T3D_RD_TERRAIN_VT_' + fragment],
               'C++ / GLSL clipmap mismatch: ' + name)
        ensure(world['T3D_TERRAIN_VT_' + name] == constants['T3D_RD_WORLD_VT_' + fragment],
               'C++ / GLSL WORLD mismatch: ' + name)
    for prefix, table, family in [('T3D_RD_TERRAIN_VT_', terrain, 'T3D_TERRAIN_VT_'),
                                  ('T3D_RD_WORLD_VT_', world, 'T3D_TERRAIN_VT_')]:
        ensure(constants[prefix + 'PHYSICAL_BINDING_BASE'] == table[family + 'ALBEDO_BINDING'],
               prefix + 'physical base mismatch')
        ensure(constants[prefix + 'TAIL_TEXTURE_BINDING_BASE'] == table[family + 'ALBEDO_TAIL_BINDING'],
               prefix + 'tail base mismatch')
    for key, suffix in [('PAGE_TABLE_BINDING', 'PAGE_TABLE_BINDING'),
                        ('ALBEDO_BINDING', 'TEXTURE_BINDING_BASE')]:
        ensure(world['T3D_CS_' + key] == constants['T3D_RD_WORLD_VT_COMPOSED_' + suffix],
               'WORLD composed binding mismatch ' + key)

    iface = (RD / 'terrain_clipmap_fragment_interface.glslinc').read_text()
    for name in fields:
        ensure('#define T3D_TERRAIN_VT_' + name not in iface,
               'Legacy duplicate VT descriptor macro: ' + name)
    feedback = (RD / 'vt/terrain_vt_feedback_inc.glslinc').read_text()
    for name, binding in [('AllocationBuffer', 0), ('PageTableBuffer', 1),
                          ('GridBuffer', 2), ('CounterBuffer', 3), ('FeedbackParams', 4)]:
        ensure(re.search(r'layout\(set\s*=\s*3,\s*binding\s*=\s*' + str(binding) + r'\b', feedback),
               'Fragment-feedback layout lost ' + name)

    cases = {
        'terrain_clipmap.glsl': ['color_native', 'editor_color_native',
            'color_specular_motion', 'depth', 'depth_normal_roughness',
            'depth_normal_roughness_voxel_gi', 'color_native_one_sun',
            'feedback_color_native', 'feedback_color_native_one_sun',
            'feedback_color_native_vt_hints', 'color_native_compact',
            'color_native_vt_hints'],
        'terrain_world_raster.glsl': ['terrain_strip_color_native', 'terrain_strip_depth',
            'terrain_strip_depth_normal_roughness', 'terrain_strip_color_native_compact',
            'env_terrain_strip_color_native', 'env_terrain_strip_color_native_compact',
            'mesh_color_native', 'mesh_depth', 'mesh_depth_normal_roughness',
            'mesh_shadow', 'mesh_color_native_compact', 'feedback_mesh_color_native',
            'env_mesh_color_native', 'env_mesh_depth', 'env_feedback_mesh_color_native',
            'env_mesh_color_native_compact', 'env_mesh_shadow',
        ],
    }
    terrain_blocks = {
        'TerrainVTAllocationBuffer': 26, 'TerrainVTPageTableBuffer': 27,
        'TerrainVTTailMetadataBuffer': 32, 'TerrainVTMipLayoutBuffer': 37,
    }
    world_blocks = {
        'TerrainVTAllocationBuffer': 1, 'TerrainVTPageTableBuffer': 2,
        'TerrainVTTailMetadataBuffer': 7, 'TerrainVTMipLayoutBuffer': 12,
    }
    manifests = []
    for filename, variants in cases.items():
        shader = RD / filename
        for variant in variants:
            output = preprocess(shader, variant)
            if args.baseline:
                earlier = preprocess(args.baseline / 'project/addons/terrain_3d/rd' / filename, variant)
                ensure(normal_shader_tokens(output) == normal_shader_tokens(earlier),
                       filename + ':' + variant + ': output GLSL changed beyond prototype placement')
            verify_layout(output, 1 if filename == 'terrain_world_raster.glsl' else 0,
                          world_blocks if filename == 'terrain_world_raster.glsl' else terrain_blocks,
                          filename + ':' + variant)
            is_feedback = 'feedback_' in variant
            if is_feedback:
                verify_layout(output, 3, {'VtFragmentAllocationBuffer': 0,
                    'VtFragmentPageTableBuffer': 1, 'VtFragmentGridBuffer': 2,
                    'VtFragmentCounterBuffer': 3, 'VtFragmentFeedbackParams': 4},
                    filename + ':' + variant)
            if 'vt_hints' in variant or 'ancestor_hints' in variant:
                verify_layout(output, 0, {'TerrainVTAncestorHints': 43}, filename + ':' + variant)
            manifests.append(f'{filename}\t{variant}\tset={1 if filename == "terrain_world_raster.glsl" else 0}'
                             f'\tfeedback={int(is_feedback)}')
    print(f'PASS: Catalog name/source closure: {len(catalog_literals)} distinct version names')
    print('PASS: C++ production catalog version arrays checked against RDShaderFile versions')
    print('PASS: VT descriptor ABI: clipmap, WORLD, composed, hint and feedback bindings')
    print(f'PASS: Shader fragment preprocessing and descriptor reflection: {len(manifests)} cases')
    if args.baseline:
        print(f'PASS: Equivalent preprocessed GLSL tokens against baseline: {len(manifests)} cases')
    print('\n'.join(manifests))


if __name__ == '__main__':
    main()
