#!/usr/bin/env python3
"""Wave18 fragment source gate and optional real glslang/SPIR-V compilation.

Run with --glslang /path/to/glslangValidator for compiler-backed validation.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys
from compile_fragment_wave import emit_fragment, shader_root
from verify_wave13_fragment_variants import test_cpp, test_glslang

ROOT=Path(__file__).resolve().parents[4]
CASES={
    'terrain_instances.glsl':('depth','depth_normal_roughness','color_native','feedback_color_native'),
    'terrain_world_raster.glsl':('mesh_depth','mesh_depth_normal_roughness','mesh_color_native','feedback_mesh_color_native'),
    'terrain_clipmap_near.glsl':('color_native','editor_color_native','feedback_color_native'),
    'terrain_clipmap_middle.glsl':('color_native','editor_color_native'),
    'terrain_clipmap_far.glsl':('color_native','editor_color_native'),
}
# Each switch is independent. Also test combined alpha and source reuse and
# combined authored-color shortcuts to detect macro-order interactions.
OPT={
    'terrain_instances.glsl': (
        ('T3D_VT_ALPHA_GATHER',),('T3D_VT_ALPHA_EDGE_REUSE',),
        ('T3D_VT_INSTANCE_ALBEDO_SAMPLE_REUSE',),
        ('T3D_VT_ALPHA_GATHER','T3D_VT_INSTANCE_ALBEDO_SAMPLE_REUSE')),
    'terrain_world_raster.glsl': (
        ('T3D_VT_ALPHA_GATHER',),('T3D_VT_ALPHA_EDGE_REUSE',)),
    'terrain_clipmap_near.glsl': (
        ('T3D_AUTHORED_BILINEAR_CORNER_REUSE',),
        ('T3D_MEGAVT_EARLY_COLOR_OVERRIDE',),
        ('T3D_AUTHORED_BILINEAR_CORNER_REUSE','T3D_MEGAVT_EARLY_COLOR_OVERRIDE')),
    'terrain_clipmap_middle.glsl': (
        ('T3D_AUTHORED_BILINEAR_CORNER_REUSE',),
        ('T3D_MEGAVT_EARLY_COLOR_OVERRIDE',)),
    'terrain_clipmap_far.glsl': (
        ('T3D_AUTHORED_BILINEAR_CORNER_REUSE',),
        ('T3D_MEGAVT_EARLY_COLOR_OVERRIDE',)),
}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=ROOT)
    parser.add_argument('--cpp',default=shutil.which('cpp'))
    parser.add_argument('--glslang',default=None)
    parser.add_argument('--fast',action='store_true')
    args=parser.parse_args()
    if not args.cpp:parser.error('C preprocessor missing')
    previous=Path(__file__).with_name('verify_wave17_fragment_variants.py')
    cmd=[sys.executable,str(previous),'--root',str(args.root),'--cpp',args.cpp]
    if args.fast:cmd.append('--fast')
    if args.glslang:cmd+=['--glslang',args.glslang]
    subprocess.run(cmd,check=True)
    rd=shader_root(args.root)
    count=0
    for shader,variants in CASES.items():
        choices=variants[:2] if args.fast else variants
        for version in choices:
            source=emit_fragment(rd,shader,version)
            for opt in OPT[shader]:
                defines=''.join(f'#define {x} 1\n' for x in opt)
                changed=source.replace('#version 450\n','#version 450\n'+defines,1)
                expanded=test_cpp(args.cpp,changed,())
                assert 'void main(' in expanded,(shader,version,opt)
                if 'T3D_VT_ALPHA_GATHER' in opt and 'depth' in version:
                    assert 'textureGather(' in expanded,(shader,version,opt,'gather path missing')
                if 'T3D_VT_ALPHA_EDGE_REUSE' in opt and 'depth' in version:
                    assert 'all(equal(p11, p10))' in expanded,(shader,version,opt,'edge reuse missing')
                if 'T3D_VT_INSTANCE_ALBEDO_SAMPLE_REUSE' in opt:
                    assert 't3d_vt_consumer_equal_sampling_address(' in expanded,(shader,version,opt,'alias test missing')
                if 'T3D_MEGAVT_EARLY_COLOR_OVERRIDE' in opt:
                    assert 'cooked_macro_valid' in expanded,(shader,version,opt,'MegaVT override missing')
                if 'T3D_AUTHORED_BILINEAR_CORNER_REUSE' in opt:
                    assert 't11.x == t00.x' in expanded,(shader,version,opt,'corner reuse missing')
                if args.glslang:
                    name=shader.removesuffix('.glsl')+'_'+version+'_'+('_'.join(opt))
                    test_glslang(args.glslang,changed,name)
                count+=1
    print(f'PASS Wave18: {count} additional shader source/preprocessor permutations')
    if args.glslang:
        print(f'PASS Wave18: {count} additional GLSL/SPIR-V variants compiled and validated')
    else:
        print('NOT RUN Wave18: native GLSL/SPIR-V compiler (provide --glslang)')

if __name__=='__main__': main()
