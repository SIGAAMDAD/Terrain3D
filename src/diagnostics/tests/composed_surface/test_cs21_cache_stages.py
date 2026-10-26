#!/usr/bin/env python3
"""CS21: stage capture parity, BC delta attribution, fail-closed coverage."""
import json
from pathlib import Path
import struct
import tempfile

from compare_cs21_cache_stages import compare_stages
from export_cs21_reference import COUNT, LAYOUT, export


def captured(folder, stage, changes=None):
    folder.mkdir()
    metadata = {'width': 2, 'height': 2, 'region_key': '0xabc', 'mip': 0,
                'source_fingerprint': '0x555', 'composition_revision': 19,
                'capture_stage': stage, 'channels': {}}
    values = {'albedo': [.25, .5, .75] * 4, 'normal': [0., 1., 0.] * 4,
              'orm': [.4, .6, .1, .8] * 4}
    if changes:
        for channel, element, new_value in changes:
            values[channel][element] = new_value
    for name, components in LAYOUT.items():
        filename = f'{name}.f32le'
        metadata['channels'][name] = {'file': filename, 'components': components}
        (folder / filename).write_bytes(struct.pack(f'<{4*components}f', *values[name]))
    (folder / 'capture.json').write_text(json.dumps(metadata), encoding='utf-8')
    return folder / 'capture.json'


limits = {step: {name: {'max_abs': 0.0, 'mae': 0.0,
                         **({'max_degrees': 0.0, 'mean_degrees': 0.0} if name == 'normal' else {})}
                 for name in LAYOUT} for step in ('source_to_baker', 'baker_to_decoded_bc')}
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    ref = captured(root / 'raster', 'raster_source')
    mid = captured(root / 'baker', 'uncompressed_baker')
    bc = captured(root / 'bc', 'decoded_bc')
    assert compare_stages(ref, mid, bc, limits)['accepted']
    # BC introduces a measurable error in one channel; numerical zero parity fails.
    bc = captured((root / 'bc2'), 'decoded_bc', [('orm', 0, .405)])
    report = compare_stages(ref, mid, bc, limits)
    assert not report['accepted'] and report['source_to_baker']['accepted']
    assert report['baker_to_decoded_bc']['channels']['orm']['max_abs'] > 0.004
    limits['baker_to_decoded_bc']['orm']['max_abs'] = .006
    limits['baker_to_decoded_bc']['orm']['mae'] = .001
    assert compare_stages(ref, mid, bc, limits)['accepted']
    # Pre-BC source disagreement is never hidden by BC tolerances.
    wrong = captured(root / 'wrong', 'uncompressed_baker', [('albedo', 0, .4)])
    assert not compare_stages(ref, wrong, bc, limits)['accepted']
    # Source provenance and three independent stage identity must be present.
    try:
        compare_stages(ref, ref, bc, limits)
        assert False, 'duplicate stage should fail'
    except ValueError:
        pass
    manifest = json.loads(bc.read_text())
    manifest['source_fingerprint'] = 'other'
    bc.write_text(json.dumps(manifest))
    try:
        compare_stages(ref, mid, bc, limits)
        assert False, 'different material source should fail'
    except ValueError as error:
        assert 'identity' in str(error)

    # RGBA float capture conversion is byte exact, including normal XYZ and ORM.
    raw = root / 'linear_input'
    raw.mkdir()
    input_paths = {}
    for name in LAYOUT:
        values = [0.25, 0.5, 0.75, 1.0] * COUNT
        path = raw / (name + '.rgba.f32le')
        path.write_bytes(struct.pack(f'<{COUNT*4}f', *values))
        input_paths[name] = path
    exported = root / 'exported'
    result = export(exported, input_paths,
                    {'region_key': '0xabc', 'mip': 0, 'source_fingerprint': '0x555',
                     'composition_revision': 19})
    assert result['capture_stage'] == 'uncompressed_baker'
    assert len((exported / 'normal.f32le').read_bytes()) == COUNT*3*4
    assert struct.unpack('<3f', (exported / 'normal.f32le').read_bytes()[:12]) == (.25, .5, .75)
print('CS21 triple-stage composed-surface parity: PASS')
