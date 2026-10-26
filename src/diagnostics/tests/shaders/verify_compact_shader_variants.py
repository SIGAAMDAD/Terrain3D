#!/usr/bin/env python3
"""RET-07: validate active UCMP compact fragment versions and shared compute producer."""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[4])
    p.add_argument('--glslang',default='glslangValidator')
    p.add_argument('--compute-only',action='store_true')
    a=p.parse_args()
    if not __import__('shutil').which(a.glslang) and not Path(a.glslang).is_file():
        print('SKIPPED SPIR-V compact gate: glslangValidator unavailable; no GPU compilation claimed')
        return
    root=a.root.resolve()
    rd=root/'project/addons/terrain_3d/rd'
    sys.path.insert(0,str(root/'src/diagnostics/tests/shaders'))
    from compile_fragment_variants import split_sections,emit_fragment,variants
    from verify_wave19_shader_gate import check
    shader=rd/'terrain_clipmap.glsl'
    versions=[x for x in variants(split_sections(shader.read_text())) if x.endswith('_compact')]
    assert versions and not any(x.startswith('compat_') for x in versions)
    count=0
    with tempfile.TemporaryDirectory() as temp:
        tmp=Path(temp)
        if not a.compute_only:
            for version in versions:
                check(root,'terrain_clipmap.glsl',version,(), 'cpp',a.glslang,None,tmp)
                count+=1
            for name in ('terrain_world_raster','terrain_instances'):
                for version in variants(split_sections((rd/(name+'.glsl')).read_text())):
                    if not version.endswith('_compact'): continue
                    check(root,name+'.glsl',version,(), 'cpp',a.glslang,None,tmp)
                    count+=1
        compute=split_sections((rd/'terrain_cluster_compact.glsl').read_text())['compute']
        stage=tmp/'compact.comp';stage.write_text(compute.replace('#VERSION_DEFINES',''))
        cmd=[a.glslang,'-V','--target-env','vulkan1.2','-S','comp',str(stage),'-o',str(tmp/'compact.spv')]
        result=subprocess.run(cmd,capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    print('RET-07 compact PASS:',count,'raster versions plus one global compute')

if __name__=='__main__':main()
