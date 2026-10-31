#!/usr/bin/env python3
"""BR19 release gate: compile all RD fragments, experimental families and regressions.

Offline PASS does not activate an experiment. This gate records missing runtime/
Vulkan evidence explicitly; use --require-device-acceptance in a release pipeline.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
REGRESSIONS = (
    'test_fragment_wave_contracts.py', 'test_siggraph_integration_contracts.py',
    'test_fragment_pipeline_work.py', 'test_fragment_fallback_work.py',
    'test_fragment_cache_work.py', 'test_fragment_culling_work.py',
    'test_fragment_publication_work.py', 'test_fragment_samples.py',
    'test_branch_loop.py',
    'test_feedback_comparator.py',
)
REQUIRED_RUNTIME = (
    'full_native_link', 'runtime_smoke', 'editor_inspector_toolbar_smoke',
    'editor_paint_undo_reload', 'forward_plus_attachment_order',
    'cold_vt_eviction_slot_reuse', 'vt_feedback_parity',
    'shader_image_hdr_depth_dnr_alpha_parity', 'resource_plateau',
    'gpu_pass_frame_p50_p95_p99', 'intel_arc_140v_capture', 'nvidia_3060ti_capture',
)


def run_case(name, command, output):
    result = subprocess.run(command, capture_output=True, text=True, timeout=540)
    (output / (name + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
    return result.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, default=ROOT)
    ap.add_argument('--glslang', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--device-evidence', type=Path,
                    help='JSON mapping every REQUIRED_RUNTIME case to a recorded PASS and evidence file')
    ap.add_argument('--require-device-acceptance', action='store_true')
    ap.add_argument('--reuse-matrix', action='store_true', help='reuse only after hash-verifying all saved emitted shader sources')
    args = ap.parse_args()
    root = args.root.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    python = sys.executable
    tests = {}
    stages = {}
    report_path = args.out / 'fragment_matrix' / 'report.json'
    if args.reuse_matrix:
        from compile_fragment_wave import emit_fragment, shader_root
        if not report_path.is_file():
            raise RuntimeError('cached fragment matrix report missing')
        cache = json.loads(report_path.read_text())
        for key, entry in cache.get('patched', {}).items():
            name, version = key.split(':', 1)
            source_path = args.out / 'fragment_matrix' / 'patched' / (name.removesuffix('.glsl') + '__' + version + '.frag')
            spv_path = source_path.with_suffix('.spv')
            expected = emit_fragment(shader_root(root), name, version)
            if (not source_path.is_file() or not spv_path.is_file() or
                    source_path.read_text() != expected or not entry.get('validated')):
                raise RuntimeError(f'cached fragment matrix is stale or invalid: {key}')
        tests['fragment_matrix'] = bool(cache.get('patched')) and not cache.get('failures')
    else:
        tests['fragment_matrix'] = run_case('fragments',
            [python, str(HERE / 'compile_fragment_wave.py'), '--root', str(root),
             '--glslang', str(args.glslang), '--full', '--jobs', str(args.jobs),
             '--out', str(args.out / 'fragment_matrix')], args.out)
    matrix = json.loads(report_path.read_text()) if report_path.is_file() else {}
    stages['standard_variants'] = len(matrix.get('patched', {}))
    tests['br21_compact_compute'] = run_case('br21_compact_compute',
        [python, str(HERE / 'verify_compact_shaders.py'), '--root', str(root),
         '--glslang', str(args.glslang), '--compute-only'], args.out)
    for name in REGRESSIONS:
        tests[name] = run_case(name.removesuffix('.py'), [python, str(HERE / name)], args.out)
    runtime = {name: {'status': 'not_run', 'evidence': None} for name in REQUIRED_RUNTIME}
    if args.device_evidence and args.device_evidence.exists():
        record = json.loads(args.device_evidence.read_text())
        for name in REQUIRED_RUNTIME:
            entry = record.get(name)
            if isinstance(entry, dict) and entry.get('status') == 'pass' and entry.get('evidence'):
                artifact = Path(entry['evidence'])
                if artifact.is_file() and artifact.stat().st_size > 0:
                    runtime[name] = {'status': 'externally_reported_pass', 'evidence': str(artifact)}
    compiled = bool(tests['fragment_matrix']) and not matrix.get('failures')
    offline_pass = compiled and all(tests.values())
    hardware_proven = all(x['status'] == 'externally_reported_pass' for x in runtime.values())
    # External evidence remains a referenced assertion; this harness does not
    # certify renderer visual parity independently.
    report = {
        'schema': 1, 'stage': 'BR19 source/SPIR-V release gate',
        'generated_utc': datetime.now(timezone.utc).isoformat(),
        'glslang': str(args.glslang),
        'shader_count': stages['standard_variants'],
        'fragment_matrix_failures': matrix.get('failures', []),
        'tests': tests,
        'offline_pass': offline_pass,
        'runtime_requirements': runtime,
        'runtime_evidence_complete': hardware_proven,
        'production_accepted': False,
        'production_gate': 'BLOCKED until independently reviewed in-engine Vulkan parity, '
                           'full native build/link and measured GPU A/B acceptance; '
                           'cold experiments remain disabled',
        'note': 'SPIR-V instruction counts are not executed branch divergence or frame timings.',
        'baseline_hashes': {
            x: hashlib.sha256((root / x).read_bytes()).hexdigest()
            for x in ('project/addons/terrain_3d/rd/terrain_forward_lighting_inc.glslinc',
                      'src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.cpp')
        },
    }
    (args.out / 'wave24_release_gate.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'BR19 offline {"PASS" if offline_pass else "FAIL"}: '
          f'{stages["standard_variants"]} standard variants, '
          f'{sum(tests.values())}/{len(tests)} test suites; '
          f'production BLOCKED: {sum(v["status"] != "externally_reported_pass" for v in runtime.values())} '
          f'runtime evidence classes outstanding')
    if not offline_pass or args.require_device_acceptance and not hardware_proven:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
