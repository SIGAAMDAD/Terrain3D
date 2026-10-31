#!/usr/bin/env python3
"""Wave 17 GLSL source/preprocessor matrix with optional real SPIR-V validation.

--glslang PATH performs actual shader compilation + Vulkan 1.2 SPIR-V checks.
Without it this only validates C preprocessor expansion and bracket balance.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys
from compile_fragment_wave import emit_fragment, shader_root
from verify_fragment_pipeline_variants import test_cpp, test_glslang

ROOT = Path(__file__).resolve().parents[4]
EXPERIMENTS = {
    'terrain_instances.glsl': (
        'color_native','color_specular','feedback_color_native','depth_normal_roughness','color_native_diag_constant'),
    'terrain_world_raster.glsl': (
        'mesh_color_native','mesh_color_specular','feedback_mesh_color_native',
        'mesh_depth_normal_roughness','terrain_strip_color_native'),
}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,default=ROOT)
    p.add_argument('--cpp',default=shutil.which('cpp'))
    p.add_argument('--glslang')
    p.add_argument('--fast',action='store_true')
    args=p.parse_args()
    if not args.cpp: p.error('C preprocessor required')
    previous=Path(__file__).with_name('verify_fragment_variant_culling.py')
    cmd=[sys.executable,str(previous),'--root',str(args.root),'--cpp',args.cpp]
    if args.glslang: cmd+=['--glslang',args.glslang]
    if args.fast: cmd.append('--fast')
    subprocess.run(cmd,check=True)
    count=0
    rd=shader_root(args.root)
    for shader,versions in EXPERIMENTS.items():
        for version in versions:
            source=emit_fragment(rd,shader,version)
            opt=source.replace('#version 450\n','#version 450\n#define T3D_UNSHADED_SURFACE_FAST_PATH 1\n',1)
            expanded=test_cpp(args.cpp,opt,())
            # Instance feedback variants retain original request ordering;
            # world-mesh feedback emits all requests before this fast path.
            fast_assignment=('evaluate_surface = (' if shader == 'terrain_world_raster.glsl'
                             else 'evaluate_surface = !(')
            if (shader=='terrain_world_raster.glsl' and (version.startswith('mesh_color') or version.startswith('feedback_mesh_color'))
                or shader=='terrain_instances.glsl' and version.startswith('color')
                    and '_diag_' not in version):
                assert fast_assignment in expanded,(shader,version,'missing fast assignment')
            if 'depth' in version or shader=='terrain_instances.glsl' and 'feedback' in version:
                assert fast_assignment not in expanded,(shader,version,'feedback/depth fast path leaked')
            if args.glslang:
                test_glslang(args.glslang,opt,shader.removesuffix('.glsl')+'_'+version+'_unshaded')
            count+=1
    print(f'PASS Wave17: {count} additional opt-in shader variants source-expanded and CPP checked')
    if args.glslang:
        print(f'PASS Wave17: {count} additional opt-in variants GLSL compiled and SPIR-V validated')
    else:
        print('NOT RUN: GLSL/SPIR-V compilation (provide --glslang)')

if __name__=='__main__': main()
