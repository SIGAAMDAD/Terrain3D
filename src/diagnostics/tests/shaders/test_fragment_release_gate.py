#!/usr/bin/env python3
"""P08: reject missing acceptance cohorts, evidence and GPU timing regressions."""
import csv
import json
import struct
import tempfile
from pathlib import Path

from compare_fragment_gpu_times import COHORT, METRICS
from run_fragment_release_gate import compare_image_index, compare_timing_captures, check_smoke


def create_capture(folder):
    folder.mkdir(parents=True)
    attachments = {}
    for name, dtype, count in (('hdr_color', 'f32le', 4), ('dnr', 'f32le', 4),
                               ('depth', 'f32le', 1), ('coverage', 'u8', 1),
                               ('vt_page_ids', 'u32le', 1)):
        filename = name + '.dat'
        (folder / filename).write_bytes({'f32le':struct.pack('<' + 'f'*count*4, *([.5]*count*4)),
                                       'u8': bytes([1]*4),
                                       'u32le': struct.pack('<4I', *([1]*4))}[dtype])
        attachments[name] = {'file': filename, 'type': dtype, 'components': count}
    identity = {'renderer': 'Vulkan', 'device': 'synthetic-test-not-a-renderer',
                'driver': 'test', 'godot_build': 'test', 'scene_sha256': 'a'*64,
                'assets_sha256': 'b'*64, 'camera_state_sha256': 'c'*64,
                'material_state_sha256': 'd'*64, 'vt_state_sha256': 'e'*64,
                'variant': 'near_native', 'capture_case': 'looking_down',
                'state': 'warm', 'frame_number': 5, 'width': 2, 'height': 2}
    (folder / 'capture.json').write_text(json.dumps({'schema':1, 'identity':identity,
                                                     'attachments':attachments}))
    return folder / 'capture.json'


def write_times(path, n):
    with path.open('w', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=COHORT + METRICS)
        writer.writeheader()
        for _ in range(5):
            fields = {'case':'looking_down', 'resolution':'full',
                      'warm_state':'warm_steady_state', 'shader_path':'canonical_vt_v1'}
            fields.update({name:n for name in METRICS})
            writer.writerow(fields)


with tempfile.TemporaryDirectory(prefix='t3d_p08_') as tmp:
    root = Path(tmp)
    a, b = create_capture(root / 'a'), create_capture(root / 'b')
    row = {'case':'looking_down', 'resolution':'full',
           'warm_state':'warm_steady_state', 'shader_path':'canonical_vt_v1',
           'reference':'a/capture.json', 'candidate':'b/capture.json'}
    index = root / 'index.json'
    index.write_text(json.dumps({'pairs':[row]}))
    limits = {name:{'max_abs':0, 'mae':0} for name in ('hdr_color','dnr','depth')}
    key = tuple(row[name] for name in COHORT)
    assert compare_image_index(index, limits, {key})['accepted']
    assert not compare_image_index(index, limits, {key, ('looking_down','half','warm_steady_state','canonical_vt_v1')})['accepted']
    t0, t1 = root/'before.csv', root/'after.csv'
    write_times(t0, 100.)
    write_times(t1, 90.)
    assert compare_timing_captures(t0, t1, 0.0, 5.0, {key})['accepted']
    assert not compare_timing_captures(t0, t1, 0.0, 15.0, {key})['accepted']
    assert not compare_timing_captures(t0, t1, 0.0, 5.0, {key, ('looking_down','half','warm_steady_state','canonical_vt_v1')})['accepted']
    checks = {name:{'passed':True, 'log':'evidence.log'} for name in (
        'godot_cpp_build', 'vulkan_validation', 'runtime_scene',
        'editor_inspector_and_toolbar', 'editor_brush_undo_redo',
        'region_save_reload', 'hot_reload_and_shutdown')}
    evidence = root / 'evidence.json'
    evidence.write_text(json.dumps({'checks':checks}))
    assert not check_smoke(evidence)['accepted']  # log absent
    (root / 'evidence.log').write_text('SYNTHETIC TEST; NOT REAL BUILD EVIDENCE\n')
    assert check_smoke(evidence)['accepted']  # wiring test only
print('P08 release gate missing cohort/evidence and timing regressions: PASS')
