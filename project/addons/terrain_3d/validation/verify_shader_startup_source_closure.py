#!/usr/bin/env python3
"""Ensure every active RD shader root is in the cold threaded source table.

The catalog's per-ID resolver and threaded source loader have independent tables.
A missing path can load 100% of the requested sources and then fail on the
last catalog ID with a misleading 'source unavailable' error.
"""

import argparse
import re
from pathlib import Path


PATH = re.compile(r'"(res://addons/terrain_3d/[^"\n]+\.glsl)"')


def check(root):
    root = Path(root)
    cpp = (root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text(encoding='utf-8')
    loader = (root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text(encoding='utf-8')

    table = cpp.split('s_otherGlobalPaths[] = {', 1)[1].split('};', 1)[0]
    fixed_sources = PATH.findall(table)
    assert len(fixed_sources) == len(set(fixed_sources)), 'duplicate shader source in startup table'

    optional_source_func = cpp.split('const char *t3dRdShaderCatalogGlobalSourcePath(', 1)[1].split('uint32_t t3dRdShaderCatalogSourceCount()', 1)[0]
    optional_sources = set(PATH.findall(optional_source_func))

    resolver = cpp.split('t3dRdShaderCatalogEntry_t t3dRdShaderCatalogGet(', 1)[1].split('bool t3dRdShaderCatalogIsActive(', 1)[0]
    resolved_paths = set(PATH.findall(resolver))
    registered = set(fixed_sources) | optional_sources
    missing = resolved_paths - registered
    assert not missing, 'catalog resolver source paths absent from startup table: ' + ', '.join(sorted(missing))

    brush = 'res://addons/terrain_3d/compute/terrain_brush_tiles.glsl'
    brush_case = resolver.split('case T3D_RD_SHADER_EDITOR_BRUSH_TILES:', 1)[1].split('case ', 1)[0]
    assert brush in PATH.findall(brush_case), 'BRP tile shader route changed'
    assert brush in fixed_sources, 'BRP tile shader was not scheduled for threaded loading'

    for path in registered:
        local = root / 'project' / path.removeprefix('res://')
        assert local.is_file(), 'registered shader source is missing on disk: ' + path

    assert 'Active RD shader source is missing from startup catalog:' in loader, 'missing cold-load closure preflight'
    assert 't3dRdShaderCatalogActiveIdAt( iActive )' in loader, 'preflight must enumerate active IDs'
    assert 't3dRdShaderCatalogSourcePath( iSource )' in loader, 'preflight must check threaded source table'
    print('PASS: RD startup source closure: {} routed roots, {} fixed sources, {} optional source'.format(
        len(resolved_paths), len(fixed_sources), len(optional_sources)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    args = parser.parse_args()
    check(args.root)


if __name__ == '__main__':
    main()
