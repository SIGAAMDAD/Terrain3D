#!/usr/bin/env python3
"""RET-07 source-contract gate and optional active SPIR-V compile/runtime trace oracle.
Run without toolchain for deterministic source checks; use --glslang for full shader validation.
Runtime logs from ShaderTrace can be fed back with --trace after launching Godot.
"""
import argparse
import json
import re
from pathlib import Path


def check(root, trace=None, glslang=None):
    root = Path(root)
    rd = root / 'project/addons/terrain_3d/rd'
    src = root / 'src'
    shader = (rd / 'terrain_clipmap.glsl').read_text()
    catalog = (src / 'rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
    lib = (src / 'rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
    clip = (src / 'rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()
    compute = (src / 'rendering/compute/terrain_3d_compute.cpp').read_text()
    native = (src / 'rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h').read_text()
    from sys import path as syspath
    syspath.insert(0, str(src / 'diagnostics/tests/shaders'))
    from compile_fragment_variants import split_sections, variants, emit_fragment
    names = variants(split_sections(shader))
    assert len(names) == 81, 'Expected 81 UCMP shipping/editor/feedback versions'
    assert not any(n.startswith('compat_') for n in names)
    assert len([n for n in names if n.startswith('editor_')]) == 27
    assert shader.count('#define T3D_UCMP_COOKED 1') == 2
    assert '#define T3D_UCMP_EDITOR 1' in shader
    assert not re.search(r'\bT3D_TERRAIN_SHADER_PATH\b|\bT3D_TERRAIN_MATERIAL_BACKEND\b', shader)
    assert 'T3D_UCMP_EDITOR' in (rd/'terrain_clipmap_editor_brush_overlay.glslinc').read_text()
    assert 's_terrainPaths' not in catalog and 's_localPaths' not in catalog
    assert 't3dRdShaderCatalogIsActive' in catalog
    assert 't3dRdShaderCatalogActiveIdAt' in catalog
    assert 't3dRdShaderCatalogIsActive( id )' in lib
    assert 't3dRdShaderCatalogActiveIdAt( index )' in lib
    assert 'if ( !t3dRdShaderCatalogIsActive( id ) )' in lib
    assert 'm_hszShaders[' not in native and 'm_hszForwardShaders[' not in native
    assert 'm_bUnifiedCookedMaterialActive' not in clip
    assert 'm_hszRasterUniformSets[1]' in native
    assert 'm_hUnifiedRasterShader.isValid()' in native
    assert 'm_nTerrainMaterialBackend != T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED' in clip
    assert 'm_hszCompactSets[tier]' not in clip and 'state.m_hszPipelines[tier]' not in clip
    assert 'submit(' not in compute and 'sync(' not in compute
    assert 'processBrush(' not in compute and 'generateDerivedMaps(' not in compute
    assert 't3dSimdDerivedNormalsRg8' in (src/'data/editing/terrain_3d_map_edit_service.cpp').read_text()
    for obsolete in ('terrain_brush.glsl','terrain_derived.glsl'):
        assert not (root / 'project/addons/terrain_3d/compute' / obsolete).exists()
        assert obsolete not in catalog
    assert not (root/'project/addons/terrain_3d/shaders').exists() or not list((root/'project/addons/terrain_3d/shaders').rglob('*.*'))
    assert (root/'project/addons/terrain_3d/rd/terrain_world_raster.glsl').exists()
    assert not (root/'project/addons/terrain_3d/rd/terrain_far_height_vertex_v1.glslinc').exists()
    assert (root/'project/addons/terrain_3d/compute/terrain_brush_tiles.glsl').exists()
    for n in ('color_native','color_native_one_sun','color_native_compact',
              'feedback_color_native','feedback_color_native_one_sun','feedback_color_native_compact',
              'editor_color_native','editor_color_native_one_sun','editor_color_native_compact',
              'depth','depth_normal_roughness','depth_normal_roughness_voxel_gi'):
        assert n in names, 'Missing required pass '+n
    for stage in ('vertex','fragment'):
        assert '#VERSION_DEFINES' in split_sections(shader)[stage]
    # Every explicitly named UCMP variant in the authoritative catalog must exist in GLSL.
    for match in re.findall(r'"((?:editor_)?(?:feedback_)?(?:color|depth)[a-z0-9_]+)"', catalog):
        if match.startswith(('color_','depth','feedback_','editor_')):
            assert match in names, f'Catalog version missing from GLSL: {match}'
    if glslang:
        import subprocess, tempfile
        with tempfile.TemporaryDirectory() as folder:
            for name in names:
                source = emit_fragment(rd, 'terrain_clipmap.glsl', name)
                path = Path(folder)/'terrain.frag'
                path.write_text(source)
                cmd = [glslang, '-V', '--target-env','vulkan1.2','-S','frag',str(path),'-o',str(Path(folder)/'shader.spv')]
                result = subprocess.run(cmd,capture_output=True,text=True)
                if result.returncode:
                    raise RuntimeError(f'{name}: {result.stdout} {result.stderr}')
    if trace:
        valid = {(x, y) for x, y in [( 'res://addons/terrain_3d/rd/terrain_clipmap.glsl', n) for n in names]}
        events=0
        for line in Path(trace).read_text().splitlines():
            if '[Terrain3D][ShaderTrace]' not in line: continue
            match = re.search(r'source=(\S+) version=(\S+) id=', line)
            if not match: continue
            srcpath, version=match.groups()
            if srcpath.endswith('/terrain_clipmap.glsl'):
                if (srcpath,version) not in valid: raise AssertionError('Retired/unknown shader request: '+line)
            if any(s in srcpath for s in ('terrain_brush.glsl','terrain_derived.glsl','terrain_clipmap_near.glsl')):
                raise AssertionError('Retired shader request: '+line)
            events+=1
        assert events, 'No ShaderTrace events supplied'
        print('RET-07 trace events verified:',events)
    print('RET-07 source gate PASS:',len(names),'UCMP variants, CPU derived, catalog, BRP, WORLD')


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[4])
    p.add_argument('--glslang',default=None)
    p.add_argument('--trace',default=None)
    args=p.parse_args()
    check(args.root,args.trace,args.glslang)

if __name__=='__main__':main()
