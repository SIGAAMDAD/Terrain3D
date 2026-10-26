#!/usr/bin/env python3
"""Wave 16 RD fragment source/preprocessor gate, including ocean.

Passing --glslang runs real GLSL/SPIR-V validation. Without it this tool
checks filesystem include expansion, preprocessor paths and delimiter balance.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from compile_fragment_wave import emit_fragment, split_sections, variants, shader_root
from verify_wave13_fragment_variants import test_cpp, test_glslang

ROOT = Path(__file__).resolve().parents[4]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=ROOT)
    parser.add_argument('--cpp',default=shutil.which('cpp'))
    parser.add_argument('--glslang')
    parser.add_argument('--fast',action='store_true')
    args=parser.parse_args()
    if not args.cpp:
        parser.error('GNU cpp is required')
    previous=Path(__file__).with_name('verify_wave14_fragment_variants.py')
    cmd=[sys.executable,str(previous),'--root',str(args.root),'--cpp',args.cpp]
    if args.glslang:cmd+=['--glslang',args.glslang]
    if args.fast:cmd.append('--fast')
    subprocess.run(cmd,check=True)
    rd=shader_root(args.root)
    shader='terrain_ocean.glsl'
    ocean_versions=list(variants(split_sections((rd/shader).read_text())))
    for version in ocean_versions:
        stage=emit_fragment(rd,shader,version)
        test_cpp(args.cpp,stage,())
        if args.glslang:
            test_glslang(args.glslang,stage,'terrain_ocean_'+version)
    print(f'PASS Wave16: {len(ocean_versions)} ocean fragment versions source-expanded and CPP validated')
    if args.glslang:
        print(f'PASS Wave16: {len(ocean_versions)} ocean fragment versions compiled/validated for Vulkan 1.2')
    else:
        print('NOT RUN: ocean GLSL/SPIR-V compilation (provide --glslang)')


if __name__=='__main__':main()
