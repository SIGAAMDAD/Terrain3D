#!/usr/bin/env python3
"""Freeze the actual Godot RDShaderFile variants and pass coverage."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]
SHADERS = (
    'project/addons/terrain_3d/rd/terrain_clipmap.glsl',
    'project/addons/terrain_3d/rd/terrain_world_raster.glsl',
)
VERSION = re.compile(r'^([A-Za-z][A-Za-z0-9_]*)\s*=\s*"([^"\r\n]*)"\s*;?\s*$')
DEFINE = re.compile(r'#define\s+([A-Za-z_][A-Za-z_0-9]*)\b')
INCLUDE = re.compile(r'#include\s+"([^"\r\n]+)"')


def scan():
    rows = []
    inputs = []
    for rel in SHADERS:
        data = (ROOT / rel).read_bytes()
        source = data.decode('utf-8')
        inputs.append({'path': rel, 'sha256': hashlib.sha256(data).hexdigest(),
                       'includes': sorted(set(INCLUDE.findall(source)))})
        in_versions = False
        for number, line in enumerate(source.splitlines(), 1):
            if line.startswith('#['):
                in_versions = line.strip() == '#[versions]'
                continue
            if not in_versions:
                continue
            match = VERSION.match(line.strip())
            if not match:
                continue
            name, definition = match.groups()
            tags = sorted(set(DEFINE.findall(definition.replace('\\n', '\n'))))
            defines = set(tags)
            if 'T3D_WORLD_SHADOW' in defines or '_shadow' in name:
                kind = 'shadow'
            elif 'T3D_FORWARD_NORMAL_ROUGHNESS' in defines:
                kind = 'depth_normal_roughness'
            elif 'T3D_FORWARD_DEPTH' in defines:
                kind = 'depth'
            else:
                kind = 'color'
            geometry = ('mesh' if 'T3D_WORLD_RASTER_MESH' in defines else
                        'terrain_strip' if 'T3D_WORLD_RASTER_TERRAIN_STRIP' in defines else
                        'clipmap' if 'clipmap' in rel else 'unknown')
            rows.append({'root': rel, 'line': number, 'variant': name, 'pass': kind,
                         'geometry': geometry, 'editor': 'T3D_UCMP_EDITOR' in defines,
                         'vt_feedback': 'T3D_VT_FRAGMENT_FEEDBACK' in defines, 'defines': tags})
    if not rows:
        raise AssertionError('No shader modes found: RDShaderFile parsing changed')
    present = {row['pass'] for row in rows}
    if not {'color', 'depth', 'depth_normal_roughness', 'shadow'} <= present:
        raise AssertionError('Incomplete shader pass inventory: ' + str(present))
    if not any(row['editor'] for row in rows) or not any(row['vt_feedback'] for row in rows):
        raise AssertionError('No editor or feedback variant found')
    return {'schema': 'ST00_SHADER_PASS_MATRIX_V1', 'inputs': inputs,
            'variant_count': len(rows), 'variants': rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    content = scan()
    output = Path(__file__).with_name('shader_pass_matrix_v1.json')
    payload = json.dumps(content, indent=2, sort_keys=True) + '\n'
    if args.write:
        output.write_text(payload, encoding='utf-8')
    elif not output.is_file() or output.read_text(encoding='utf-8') != payload:
        raise SystemExit('Shader pass matrix drift, explicitly re-freeze before Wave 2')
    print(f'ST00 shader pass matrix OK: {content["variant_count"]} variants across {len(SHADERS)} roots')


if __name__ == '__main__':
    main()
