#!/usr/bin/env python3
"""Compile both BR17 stages and compare descriptor contracts with glslang 16.6.0.

This is a source/SPIR-V gate, NOT a Godot/Vulkan runtime pass or image test.
"""
import argparse
from pathlib import Path
import sys
import subprocess
from compile_fragment_variants import INCLUDE, split_sections, variants, shader_root

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    ap.add_argument('--glslang', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args=ap.parse_args()
    rd=shader_root(args.root)
    sections=split_sections((rd/'terrain_clipmap_screen_space.glsl').read_text())
    args.out.mkdir(parents=True,exist_ok=True)
    assert set(variants(sections))=={'coverage','shade'}
    for variant in ('coverage','shade'):
        for stage in ('vertex','fragment'):
            body=INCLUDE.sub(lambda m: (rd/m.group(1)).read_text(), sections[stage])
            if INCLUDE.search(body) or body.count('#VERSION_DEFINES') != 1:
                raise ValueError('nested include or malformed stage root')
            body=body.replace('#VERSION_DEFINES',variants(sections)[variant])
            file=args.out/f'{variant}.{stage}.glsl';spv=args.out/f'{variant}.{stage}.spv'
            file.write_text(body)
            cmd=[str(args.glslang),'-V','--target-env','vulkan1.2','-S',
                 'vert' if stage=='vertex' else 'frag','--spirv-val',str(file),'-o',str(spv)]
            p=subprocess.run(cmd, capture_output=True, text=True, timeout=90)
            if p.returncode or not spv.exists() or 'SPIRV-Tools Validation Errors' in p.stdout + p.stderr:
                raise RuntimeError(f'{variant}:{stage}: {p.stdout}{p.stderr}')
    print('BR17: 4 stages GLSL + SPIR-V PASS')

if __name__=='__main__': main()
