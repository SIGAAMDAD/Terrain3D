#!/usr/bin/env python3
"""P00-P05 Vulkan RDShaderFile compiler/validator and before/after SPIR-V counter gate.

Expands the shader root's direct includes exactly once, as Godot RDShaderFile
imports them. Neither this script nor a successful SPIR-V validation establishes
GPU visual parity or frame timing. A/B validation regressions fail the command.

Usage:
    python compile_fragment_wave.py --glslang path/to/glslangValidator --out output
    python compile_fragment_wave.py --glslang ... --baseline /path/to/previous/source --full
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
SHADERS = ('terrain_clipmap.glsl', 'terrain_instances.glsl', 'terrain_world_raster.glsl')
INCLUDE = re.compile(r'^\s*#include\s+"([^"\n]+)"\s*$', re.MULTILINE)
VARIANT = re.compile(r'^([a-zA-Z_0-9]+)\s*=\s*("(?:\\.|[^"\\])*")\s*;', re.MULTILINE)
SECTIONS = re.compile(r'^#\[(versions|vertex|fragment|compute)\]\s*$', re.MULTILINE)
# A compact authoritative count of instructions relevant to fragment pressure.
OPCODES = {61: 'OpLoad', 62: 'OpStore', 65: 'OpAccessChain',
           87: 'OpImageSampleImplicitLod', 88: 'OpImageSampleExplicitLod',
           95: 'OpImageFetch', 207: 'OpDPdx', 208: 'OpDPdy',
           209: 'OpFwidth', 252: 'OpKill'}
# All other opcodes remain visible in instruction_count; no static instruction
# count should be interpreted as a GPU cycle count.
QUICK = {
    'terrain_clipmap.glsl': ('color_native', 'feedback_color_native', 'depth',
                              'depth_normal_roughness', 'editor_color_native'),
    'terrain_instances.glsl': ('color_native', 'feedback_color_native', 'depth_normal_roughness'),
    'terrain_world_raster.glsl': ('mesh_color_native', 'feedback_mesh_color_native',
                                  'terrain_strip_color_native', 'terrain_strip_depth_normal_roughness'),
}

def shader_root(repo):
    rd = repo / 'project/addons/terrain_3d/rd'
    if not rd.is_dir():
        raise ValueError(f'not a Terrain3D source root: {repo}')
    return rd


def split_sections(text):
    chunks = SECTIONS.split(text)
    return dict(zip(chunks[1::2], chunks[2::2]))


def variants(sections):
    if 'versions' not in sections:
        return {'default': ''}
    parsed = {name: json.loads(macros) for name, macros in VARIANT.findall(sections['versions'])}
    if not parsed:
        raise ValueError('no RDShaderFile version declarations')
    return parsed


def emit_fragment(rd, filename, version):
    shader = rd / filename
    sections = split_sections(shader.read_text(encoding='utf-8'))
    choices = variants(sections)
    if version not in choices:
        raise ValueError(f'{filename} missing version {version}')
    body = sections['fragment']
    # Godot expands only directly authored includes from the shader stage root.
    # Missing or nested includes fail before GLSL compilation.
    def expand(m):
        path = (shader.parent / m.group(1)).resolve()
        if not path.is_relative_to(rd.resolve()) or not path.is_file():
            raise ValueError(f'invalid include {m.group(1)} in {filename}')
        return path.read_text(encoding='utf-8')
    body = INCLUDE.sub(expand, body)
    if INCLUDE.search(body):
        raise ValueError(f'nested include was not resolved by Godot: {filename}')
    if body.count('#VERSION_DEFINES') != 1:
        raise ValueError(f'unexpected #VERSION_DEFINES count: {filename}')
    body = body.replace('#VERSION_DEFINES', choices[version])
    if not body.lstrip().startswith('#version 450'):
        raise ValueError(f'fragment stage requires #version 450: {filename}')
    return body


def count_spirv(path):
    blob = path.read_bytes()
    if len(blob) % 4 or len(blob) < 20:
        raise ValueError(f'malformed SPIR-V: {path}')
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    if words[0] != 0x07230203:
        raise ValueError(f'wrong SPIR-V magic: {path}')
    counts = Counter()
    i = 5
    while i < len(words):
        count = words[i] >> 16
        opcode = words[i] & 0xffff
        if count == 0 or i + count > len(words):
            raise ValueError(f'bad SPIR-V instruction at word {i}')
        counts[OPCODES.get(opcode, 'Other')] += 1
        i += count
    return {'words': len(words), 'bytes': len(blob),
            'instruction_count': sum(counts.values()),
            'opcodes': {name: counts[name] for name in sorted(OPCODES.values())}}


def compile_variant(job):
    repo, shader, version, folder, args, label = job
    rd = shader_root(repo)
    name = shader.removesuffix('.glsl') + '__' + version
    key = shader + ':' + version
    folder.mkdir(parents=True, exist_ok=True)
    src = folder / (name + '.frag')
    spv = folder / (name + '.spv')
    try:
        src.write_text(emit_fragment(rd, shader, version), encoding='utf-8')
        cmd = [str(args.glslang), '-V', '--target-env', 'vulkan1.2',
               '-S', 'frag', '--spirv-val', str(src), '-o', str(spv)]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
        log = (result.stdout or '') + (result.stderr or '')
        compile_ok = result.returncode == 0 and spv.is_file()
        # Some glslang builds return status 0 even after SPIRV-Tools emits
        # validation errors, so the diagnostic is an independent failing gate.
        validation_ok = compile_ok and 'SPIRV-Tools Validation Errors' not in log
        data = {'shader': shader, 'variant': version, 'compiled': compile_ok,
                'validated': validation_ok,
                'diagnostics': [line.strip() for line in log.splitlines()
                                if 'error:' in line or 'ERROR:' in line][:8]}
        if compile_ok:
            data.update(count_spirv(spv))
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        data = {'shader': shader, 'variant': version, 'compiled': False,
                'validated': False, 'diagnostics': [str(error)]}
    return key, data


def compile_suite(repo, out, args, label):
    rd = shader_root(repo)
    jobs = []
    for shader in SHADERS:
        if args.shader and shader not in args.shader:
            continue
        sections = split_sections((rd / shader).read_text(encoding='utf-8'))
        names = list(variants(sections)) if args.full else list(QUICK[shader])
        if args.variant:
            names = [name for name in names if name in args.variant]
        for version in names:
            jobs.append((repo, shader, version, out / label, args, label))
    results = {}
    # Each job writes its own .frag/.spv pair. A small pool avoids hundreds
    # of serial multi-second compiler calls without exhausting system RAM.
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for key, data in executor.map(compile_variant, jobs):
            results[key] = data
            print(f'{label}: {key}: compile={data["compiled"]} validate={data["validated"]}',
                  flush=True)
    return results

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--baseline', type=Path, help='optional unchanged reference root')
    parser.add_argument('--glslang', type=Path, default=Path('glslangValidator'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--shader', action='append', choices=SHADERS)
    parser.add_argument('--variant', action='append')
    parser.add_argument('--full', action='store_true', help='all 365 shipping/editor shader versions')
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--jobs', type=int, default=2, help='maximum parallel glslang processes')
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 16:
        parser.error("jobs must be between 1 and 16")
    args.out.mkdir(parents=True, exist_ok=True)
    reference = compile_suite(args.baseline, args.out, args, 'baseline') if args.baseline else None
    current = compile_suite(args.root, args.out, args, 'patched')
    failures = []
    comparisons = {}
    for key, data in current.items():
        if not data['compiled']:
            failures.append(f'{key}: fragment failed to compile')
        if reference:
            before = reference[key]
            if before['compiled'] and not data['compiled']:
                failures.append(f'{key}: new compilation regression')
            if before['validated'] and not data['validated']:
                failures.append(f'{key}: new SPIR-V validation regression')
            if before['compiled'] and data['compiled']:
                comparisons[key] = {name: data['opcodes'][name] - before['opcodes'][name]
                                    for name in data['opcodes']}
                comparisons[key]['instruction_count'] = data['instruction_count'] - before['instruction_count']
    report = {'tool': 'glslangValidator --spirv-val (Vulkan 1.2)',
              'note': 'Compiled instruction counts and validation are not GPU timings or image parity.',
              'baseline': reference, 'patched': current, 'opcode_deltas': comparisons,
              'failures': failures}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'Checked {len(current)} fragments; compilation failures={len(failures)}; '
          f'validation failures={sum(not v["validated"] for v in current.values())}. '
          f'Report: {args.out / "report.json"}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
