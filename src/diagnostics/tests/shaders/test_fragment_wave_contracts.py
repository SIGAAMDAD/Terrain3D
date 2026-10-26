#!/usr/bin/env python3
"""P00-P05 deterministic source-contract and capture-comparison regression tests."""
import csv
import json
import math
import struct
import tempfile
from pathlib import Path

from compare_fragment_captures import compare as compare_images
from compare_fragment_gpu_times import compare as compare_times, METRICS, COHORT

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'


def source_identity(material, rotation, scale, height):
    assert 0 <= material < 32 and 0 <= rotation < 16 and 0 <= scale < 8
    key = (rotation << 10) | (scale << 7)
    return (material & 31) | (((key >> 7) & 127) << 5) | ((1 << 12) if height else 0)


def test_keys():
    # Every material/rotation/scale/height encoding is injective. Blend and
    # opposite-layer IDs are intentionally excluded: they never change source UVs.
    observed = set()
    for height in (False, True):
        for material in range(32):
            for rotation in range(16):
                for scale in range(8):
                    v = source_identity(material, rotation, scale, height)
                    assert v not in observed
                    observed.add(v)
    assert len(observed) == 8192
    source = (RD / 'terrain_clipmap_fragment_inc.glslinc').read_text()
    assert '((control_key >> 7u) & 127u) << 5u' in source
    assert '(effective_height ? (1u << 12u) : 0u)' in source
    assert 'samples.same_material == 0u);' in source
    assert 'source_reuse.next_slot ^= 1u;' not in source  # struct access explicitly scoped
    assert 'reuse.next_slot ^= 1u;' in source


def test_filter():
    # Preserve the original decode -> premultiply -> bilinear -> unpremultiply.
    def mix(a, b, w):
        return [x * (1 - w) + y * w for x, y in zip(a, b)]
    def reference(corners, fraction):
        a, b, c, d = [list(v) for v in corners]
        for v in (a, b, c, d):
            v[:3] = [x * v[3] for x in v[:3]]
        result = mix(mix(a, b, fraction[0]), mix(c, d, fraction[0]), fraction[1])
        result[:3] = [x / result[3] for x in result[:3]] if result[3] > 0.00001 else [0.0] * 3
        return result
    values = [(0.1,0.9,0.2,0.0), (0.7,0.2,0.3,0.8),
              (0.9,0.8,0.2,1.0), (0.3,0.4,0.5,0.3)]
    for fy in (0.0,0.3,0.5,1.0):
        for fx in (0.0,0.25,0.8,1.0):
            expected = reference(values, (fx, fy))
            # Independent weighted sum of the four premultiplied corner values.
            weights = ((1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy)
            premult = [sum(values[k][axis] * values[k][3] * weights[k] for k in range(4))
                       for axis in range(3)]
            alpha = sum(values[k][3] * weights[k] for k in range(4))
            actual = [c / alpha for c in premult] if alpha > 0.00001 else [0.0] * 3
            actual.append(alpha)
            assert all(math.isclose(a, b, rel_tol=1e-13, abs_tol=1e-13)
                       for a,b in zip(expected, actual))
    src = (RD / 'vt/terrain_vt_consumer_sample_inc.glslinc').read_text()
    assert 't3d_vt_consumer_linear_bilinear' in src
    assert 'for (uint i = 0u; i < T3D_VT_MAX_ANISOTROPY; ++i)' in src


def test_feedback():
    # The only requests removed have identical (page, grid index) to the
    # immediately prior request from *this fragment invocation*.
    requests = [(7,14), (7,14), (9,14), (7,14), (7,15), (7,15), (8,15)]
    previous = (-1,-1)
    emitted = []
    for key in requests:
        if key != previous:
            emitted.append(key)
        previous = key
    assert emitted == [(7,14), (9,14), (7,14), (7,15), (8,15)]
    source = (RD / 'vt/terrain_vt_feedback_inc.glslinc').read_text()
    assert 'virtual_page == t3d_vt_feedback_previous_page && index == t3d_vt_feedback_previous_index' in source
    assert 'vt_feedback_grid.words[index] = virtual_page;' in source


def make_identity():
    return dict(renderer='Vulkan', device='Arc 140V', driver='test', godot_build='test',
                scene_sha256='a'*64, assets_sha256='b'*64, camera_state_sha256='c'*64,
                material_state_sha256='d'*64, vt_state_sha256='e'*64,
                variant='terrain_near_color', capture_case='looking_down',
                state='warm', frame_number=42, width=2, height=2)


def write_images(folder, value=0.25, page=4):
    folder.mkdir()
    attachments = {}
    for name, components in (('hdr_color',4), ('dnr',4), ('depth',1), ('coverage',1), ('vt_page_ids',1)):
        dtype = 'u8' if name == 'coverage' else 'u32le' if name == 'vt_page_ids' else 'f32le'
        filename = name + '.dat'
        length = components * 4
        if dtype == 'f32le':
            raw = struct.pack('<' + 'f'*length, *[value]*length)
        elif dtype == 'u8':
            raw = bytes([1]*length)
        else:
            raw = struct.pack('<' + 'I'*length, *[page]*length)
        (folder / filename).write_bytes(raw)
        attachments[name] = {'type': dtype, 'components': components, 'file': filename}
    (folder / 'capture.json').write_text(json.dumps({'schema':1, 'identity':make_identity(),
                                                        'attachments':attachments}))


def test_captures(root):
    a,b = root/'before', root/'after'
    write_images(a)
    write_images(b)
    limits = {name: {'max_abs':0, 'mae':0} for name in ('hdr_color','dnr','depth')}
    assert compare_images(a/'capture.json', b/'capture.json', limits)['accepted']
    (b/'vt_page_ids.dat').write_bytes(struct.pack('<4I', 4, 5, 4, 4))
    result = compare_images(a/'capture.json', b/'capture.json', limits)
    assert not result['accepted'] and result['attachments']['vt_page_ids']['different_elements'] == 1
    (b/'vt_page_ids.dat').write_bytes(struct.pack('<4I', 4,4,4,4))
    (b/'hdr_color.dat').write_bytes(struct.pack('<16f', *([0.26]+[0.25]*15)))
    result = compare_images(a/'capture.json', b/'capture.json', limits)
    assert not result['accepted'] and result['attachments']['hdr_color']['different_elements'] == 1
    (b/'hdr_color.dat').write_bytes(struct.pack('<16f', *([float('nan')]+[0.25]*15)))
    try:
        compare_images(a/'capture.json', b/'capture.json', limits)
        raise AssertionError('NaN attachment incorrectly accepted')
    except ValueError as error:
        assert 'non-finite' in str(error)


def write_times(path, value):
    with path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=COHORT + METRICS)
        writer.writeheader()
        for _ in range(10):
            row = dict(case='looking_down', resolution='full', warm_state='warm', shader_path='v3')
            row.update({name: value for name in METRICS})
            writer.writerow(row)


def test_times(root):
    a,b = root/'baseline.csv', root/'candidate.csv'
    write_times(a, 100)
    write_times(b, 95)
    result = compare_times(a, b, 0)
    assert result['accepted']
    assert result['comparisons'][0]['p95']['delta_percent'] == -5.0
    write_times(b, 120)
    assert not compare_times(a, b, 0)['accepted']


def main():
    test_keys()
    test_filter()
    test_feedback()
    with tempfile.TemporaryDirectory(prefix='t3d_shader_p00_') as temp:
        root = Path(temp)
        test_captures(root)
        test_times(root)
    print('P00-P05 shader contracts and capture comparators: PASS')


if __name__ == '__main__':
    main()
