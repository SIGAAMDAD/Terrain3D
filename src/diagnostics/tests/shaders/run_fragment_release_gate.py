#!/usr/bin/env python3
"""P08: integrated fragment optimization regression and Vulkan release gate.

An offline run verifies compiler, C++ contract and material-source test evidence.
A release run (--require-live) additionally requires complete Vulkan A/B captures,
measured GPU frame improvement and actual Godot/editor/validation logs. No live
sample can be replaced with synthetic data or a blank measurement.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import sys

from compare_fragment_captures import compare as compare_images, load as load_image
from compare_fragment_gpu_times import compare as compare_times, COHORT

ROOT = Path(__file__).resolve().parents[4]
TESTS = Path(__file__).resolve().parent
CS = TESTS.parent / 'composed_surface'
PYTHON_TESTS = [
    TESTS / 'test_fragment_shader_contracts.py',
    TESTS / 'test_fragment_release_gate.py',
    CS / 'test_cs11_static_v1_shader.py',
    CS / 'test_cs14_parity.py',
    CS / 'test_cs19_optin_policy.py',
    CS / 'test_cs20_editor_regression_gate.py',
    CS / 'test_cs21_cache_stages.py',
]
SMOKE_CHECKS = (
    'godot_cpp_build', 'vulkan_validation', 'runtime_scene',
    'editor_inspector_and_toolbar', 'editor_brush_undo_redo',
    'region_save_reload', 'hot_reload_and_shutdown',
)


def run_command(name, argv, cwd, out, env=None, timeout=600):
    out.mkdir(parents=True, exist_ok=True)
    path = out / (name + '.log')
    try:
        task = subprocess.run(argv, cwd=str(cwd), capture_output=True, text=True,
                              timeout=timeout, env=env)
        path.write_text((task.stdout or '') + (task.stderr or ''), encoding='utf-8')
        return {'accepted': task.returncode == 0, 'returncode': task.returncode,
                'log': str(path)}
    except (OSError, subprocess.TimeoutExpired) as error:
        path.write_text(str(error) + '\n', encoding='utf-8')
        return {'accepted': False, 'error': str(error), 'log': str(path)}


def expected_cohorts(root):
    matrix = json.loads((root / 'src/diagnostics/tests/composed_surface/capture_matrix.json').read_text())
    return {(case, resolution, state, path)
            for case in matrix['camera_cases']
            for resolution in matrix['resolution_modes']
            for state in matrix['repeated_modes']
            for path in matrix['shipping_paths']}


def compare_image_index(index_path, limits, expected):
    data = json.loads(index_path.read_text(encoding='utf-8'))
    if not isinstance(data, dict) or not isinstance(data.get('pairs'), list) or not data['pairs']:
        raise ValueError('capture index requires nonempty pairs')
    passed = True
    rows = []
    covered = set()
    for item in data['pairs']:
        cohort = tuple(item.get(key) for key in COHORT)
        if cohort in covered or cohort not in expected:
            raise ValueError(f'duplicate or unexpected capture cohort: {cohort}')
        covered.add(cohort)
        reference = (index_path.parent / item['reference']).resolve()
        candidate = (index_path.parent / item['candidate']).resolve()
        if reference == candidate:
            raise ValueError(f'{cohort}: reference and candidate must be separate captures')
        ident = load_image(reference)['identity']
        if ident['capture_case'] != cohort[0]:
            raise ValueError(f'{cohort}: capture case identity mismatch')
        state = {'cold_start': 'cold', 'warm_steady_state': 'warm'}.get(cohort[2], cohort[2])
        if ident['state'] not in (state, cohort[2]):
            raise ValueError(f'{cohort}: cold/warm identity mismatch')
        result = compare_images(reference, candidate, limits)
        rows.append({'cohort': dict(zip(COHORT, cohort)), 'accepted': result['accepted'],
                     'attachments': result['attachments']})
        passed &= result['accepted']
    missing = sorted(expected - covered)
    return {'accepted': passed and not missing, 'covered': len(covered),
            'required': len(expected), 'missing': missing, 'comparisons': rows}


def compare_timing_captures(baseline, patched, allowed_regression, improvement, expected):
    result = compare_times(baseline, patched, allowed_regression)
    values = [item for item in result['comparisons'] if item['metric'] == 'frame_gpu_usec']
    covered = {tuple(item['cohort'][key] for key in COHORT) for item in values}
    missing = sorted(expected - covered)
    if not values:
        raise ValueError('missing measured frame_gpu_usec samples')
    improved = any(item['p50']['delta_percent'] is not None and
                   item['p50']['delta_percent'] <= -improvement for item in values)
    result.update({'all_cohorts_covered': not missing, 'covered': len(covered),
                   'required': len(expected), 'missing': missing,
                   'frame_improvement_measured': improved})
    result['accepted'] &= not missing and improved
    return result


def check_smoke(path):
    data = json.loads(path.read_text(encoding='utf-8'))
    checks = data.get('checks')
    if not isinstance(checks, dict):
        raise ValueError('smoke evidence must declare a checks object')
    results = {}
    for key in SMOKE_CHECKS:
        entry = checks.get(key)
        if not isinstance(entry, dict) or entry.get('passed') is not True:
            results[key] = {'accepted': False, 'reason': 'missing or failed check'}
            continue
        log_name = entry.get('log')
        if not isinstance(log_name, str) or not log_name:
            results[key] = {'accepted': False, 'reason': 'missing evidence log'}
            continue
        log = (path.parent / log_name).resolve()
        passed = log.is_file() and log.stat().st_size > 0
        results[key] = {'accepted': passed, 'log': str(log)}
    return {'accepted': all(r['accepted'] for r in results.values()),
            'checks': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--glslang', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--quick', action='store_true', help='development smoke only; not a release proof')
    parser.add_argument('--require-live', action='store_true')
    parser.add_argument('--capture-index', type=Path)
    parser.add_argument('--capture-limits', type=Path)
    parser.add_argument('--gpu-baseline', type=Path)
    parser.add_argument('--gpu-patched', type=Path)
    parser.add_argument('--max-regression-percent', type=float)
    parser.add_argument('--min-frame-improvement-percent', type=float)
    parser.add_argument('--smoke-evidence', type=Path)
    args = parser.parse_args()
    if args.require_live and args.quick:
        parser.error('--require-live requires the full 365-variant compile')
    if args.jobs < 1 or args.jobs > 16:
        parser.error('jobs must be in [1, 16]')
    args.root, args.baseline = args.root.resolve(), args.baseline.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    report = {'schema': 1, 'scope': 'P06-P08 on P00-P05 source',
              'offline_only': not args.require_live, 'steps': {},
              'limitations': ['offline GLSL compilation does not establish pixel parity or GPU speedup']}
    env = os.environ.copy()
    env['PATH'] = str(args.glslang.resolve().parent) + os.pathsep + env.get('PATH', '')
    suite = args.root / 'src/diagnostics/tests/shaders/compile_fragment_variants.py'
    run = [sys.executable, str(suite), '--root', str(args.root), '--baseline',
           str(args.baseline), '--glslang', str(args.glslang),
           '--out', str(args.out / 'spirv'), '--jobs', str(args.jobs)]
    if not args.quick:
        run.append('--full')
    report['steps']['shader_matrix'] = run_command('shader_matrix', run, args.root, args.out, env, 1200)
    matrix_file = args.out / 'spirv/report.json'
    if report['steps']['shader_matrix']['accepted'] and matrix_file.is_file():
        matrix = json.loads(matrix_file.read_text(encoding='utf-8'))
        reference_bad = {k for k, v in matrix['baseline'].items() if not v['validated']}
        patched_bad = {k for k, v in matrix['patched'].items() if not v['validated']}
        report['steps']['shader_matrix'].update({
            'count': len(matrix['patched']), 'baseline_validation_failures': len(reference_bad),
            'patched_validation_failures': len(patched_bad),
            'new_validation_failures': sorted(patched_bad - reference_bad),
            'accepted': not matrix['failures'] and not (patched_bad - reference_bad)})
    for source in PYTHON_TESTS:
        report['steps'][source.stem] = run_command(source.stem,
            [sys.executable, str(source)], args.root, args.out, env)
    shader_cases = CS / 'test_cs19_shader_variants.py'
    report['steps']['cs19_compiled_variants'] = run_command('cs19_compiled_variants',
        [sys.executable, str(shader_cases), '--require-compiler'], args.root, args.out, env)
    native = [
        ('vt_upload_preflight', [args.root / 'src/diagnostics/tests/test_vt_upload_preflight.cpp']),
        ('cs11_static_v1', [args.root / 'src/diagnostics/tests/composed_surface/test_cs11_static_v1.cpp',
                            args.root / 'src/authoring/vt/build/terrain_3d_composed_surface_static_v1.cpp',
                            args.root / 'src/authoring/vt/build/terrain_3d_composed_surface_cooker.cpp']),
    ]
    for name, sources in native:
        binary = args.out / (name + '.native')
        compiled = run_command(name + '_build',
             [args.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
              '-I', str(args.root / 'src'), *map(str, sources), '-o', str(binary)],
             args.root, args.out, env)
        report['steps'][name + '_build'] = compiled
        if compiled['accepted']:
            report['steps'][name] = run_command(name + '_run', [str(binary)], args.root, args.out, env)
    if args.require_live:
        required = ('capture_index', 'capture_limits', 'gpu_baseline', 'gpu_patched',
                    'max_regression_percent', 'min_frame_improvement_percent', 'smoke_evidence')
        missing = [key for key in required if getattr(args, key) is None]
        if missing:
            report['steps']['live_acceptance'] = {'accepted': False,
                'reason': 'missing required Vulkan capture inputs: ' + ', '.join(missing)}
        elif not all(math.isfinite(float(getattr(args, n))) and float(getattr(args, n)) >= 0
                     for n in ('max_regression_percent', 'min_frame_improvement_percent')):
            report['steps']['live_acceptance'] = {'accepted': False, 'reason': 'invalid GPU thresholds'}
        elif args.min_frame_improvement_percent == 0:
            report['steps']['live_acceptance'] = {'accepted': False,
                'reason': 'set a strictly positive measured frame improvement threshold'}
        else:
            try:
                expected = expected_cohorts(args.root)
                limits = json.loads(args.capture_limits.read_text(encoding='utf-8'))
                images = compare_image_index(args.capture_index, limits, expected)
                times = compare_timing_captures(args.gpu_baseline, args.gpu_patched,
                    args.max_regression_percent, args.min_frame_improvement_percent, expected)
                smoke = check_smoke(args.smoke_evidence)
                report['steps']['live_acceptance'] = {
                    'accepted': images['accepted'] and times['accepted'] and smoke['accepted'],
                    'images': images, 'gpu_times': times, 'build_and_editor': smoke}
            except (KeyError, OSError, TypeError, ValueError, json.JSONDecodeError) as error:
                report['steps']['live_acceptance'] = {'accepted': False, 'reason': str(error)}
    accepted = all(step['accepted'] for step in report['steps'].values())
    report['accepted'] = accepted
    report['release_approved'] = accepted and args.require_live
    if not args.require_live:
        report['limitations'].append('release approval requires real Vulkan image, GPU time, and editor/build captures')
    output = args.out / 'release_report.json'
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    summary = {name: {'accepted': step['accepted']} for name, step in report['steps'].items()}
    print(json.dumps({'accepted': accepted, 'release_approved': report['release_approved'],
                      'report': str(output), 'steps': summary}, indent=2))
    return 0 if accepted else 1


if __name__ == '__main__':
    sys.exit(main())
