#!/usr/bin/env python3
"""fragment alpha optimization complete RD fragment source/preprocessor gate and opt-in matrix.

Pass --glslang PATH to run real Vulkan1.2 shader compilation+SPIR-V validation.
Without that binary this is only a C-preprocessor and balanced-source gate.
"""
from pathlib import Path
import argparse
import shutil
from compile_fragment_variants import SHADERS, emit_fragment, shader_root, split_sections, variants
from verify_fragment_material_variants import test_cpp, test_glslang

ROOT = Path(__file__).resolve().parents[4]
FEATURES = (
    ('T3D_CONTROL_TRANSFORM_REUSE',),
    ('T3D_VT_ALBEDO_FAMILY_BRANCH_HOIST',),
    ('T3D_COLOR_TWO_KEY_SPECIALIZATION', 'T3D_CONTROL_TRANSFORM_REUSE'),
    ('T3D_DNR_SOURCE_REUSE', 'T3D_CONTROL_TRANSFORM_REUSE'),
    ('T3D_VT_ALBEDO_FAMILY_BRANCH_HOIST', 'T3D_CONTROL_TRANSFORM_REUSE'),
    ('T3D_COLOR_TWO_KEY_SPECIALIZATION', 'T3D_DNR_SOURCE_REUSE',
     'T3D_CONTROL_TRANSFORM_REUSE', 'T3D_VT_ALBEDO_FAMILY_BRANCH_HOIST'),
)
FAMILIES = {'terrain_clipmap.glsl', 'terrain_clipmap.glsl',
            'terrain_clipmap.glsl', 'terrain_instances.glsl', 'terrain_world_raster.glsl'}
RELEVANT = ('color_native', 'editor_color_native', 'feedback_color_native',
            'depth', 'depth_normal_roughness', 'terrain_strip_color_native',
            'terrain_strip_depth_normal_roughness', 'world_mesh_color_native',
            'world_mesh_depth_normal_roughness')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=ROOT)
    ap.add_argument('--cpp', default=shutil.which('cpp'))
    ap.add_argument('--glslang')
    ap.add_argument('--fast', action='store_true', help='first 6 standard variants per shader')
    args = ap.parse_args()
    if not args.cpp: ap.error('cpp required for source gate')
    rd = shader_root(args.root)
    standard = optins = 0
    for shader in SHADERS:
        choices = list(variants(split_sections((rd / shader).read_text())))
        if args.fast:
            choices = choices[:6]
        for variant in choices:
            frag = emit_fragment(rd, shader, variant)
            try:
                test_cpp(args.cpp, frag, ())
                standard += 1
                if args.glslang:
                    test_glslang(args.glslang, frag, shader.removesuffix('.glsl') + '_' + variant)
                if shader in FAMILIES and variant in RELEVANT:
                    for feature_set in FEATURES:
                        defines = ''.join(f'#define {f} 1\n' for f in feature_set)
                        opt = frag.replace('#version 450\n', '#version 450\n' + defines, 1)
                        test_cpp(args.cpp, opt, ())
                        optins += 1
                        if args.glslang:
                            suffix = '_'.join(f.lower().removeprefix('t3d_') for f in feature_set)
                            test_glslang(args.glslang, opt, shader.removesuffix('.glsl')+'_'+variant+'_'+suffix)
            except Exception as e:
                raise RuntimeError(f'{shader}:{variant}: {e}') from e
    print(f'PASS fragment alpha optimization: {standard} standard RD fragments, {optins} experimental variants source-expanded and CPP validated')
    if args.glslang:
        print(f'PASS fragment alpha optimization: {standard + optins} compiled and validated with glslang/SPIR-V (Vulkan 1.2)')
    else:
        print('NOT RUN: glslang/SPIR-V validation (provide --glslang)')


if __name__ == '__main__':
    main()
