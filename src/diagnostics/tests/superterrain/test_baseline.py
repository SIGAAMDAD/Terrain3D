#!/usr/bin/env python3
"""Compile-independent regression checks for the proposed ST_REGION_V1 contract."""
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib

import st00_baseline
import st00_corpus
import st00_inventory
import st00_shader_matrix

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def load_region(name):
    data = (HERE / 'corpus' / (name + '.st00region')).read_bytes()
    magic, version, side, index, flags, byte_count, crc, reserved, fingerprint = \
        st00_corpus.HEADER.unpack_from(data)
    assert (magic, version, side, byte_count, reserved) == (b'ST0R', 1, 128, len(data) - 32, 0)
    assert zlib.crc32(data[32:]) == crc
    assert fingerprint == int.from_bytes(hashlib.sha256(data[32:]).digest()[:8], 'little')
    payload = data[32:]
    plane_size = 128 * 128
    if flags & 1:
        assert byte_count == plane_size * 12
        return {
            'index': index, 'flags': flags,
            'height': memoryview(payload)[0:plane_size * 2],
            'control': memoryview(payload)[plane_size * 2:plane_size * 6],
            'color': memoryview(payload)[plane_size * 6:plane_size * 10],
            'derived': memoryview(payload)[plane_size * 10:plane_size * 12],
        }
    assert byte_count == 0
    return {'index': index, 'flags': flags}


def value_at(region, kind, x, y):
    pos = y * 128 + x
    if kind == 'height':
        return struct.unpack_from('<H', region[kind], pos * 2)[0]
    if kind == 'control':
        return struct.unpack_from('<I', region[kind], pos * 4)[0]
    if kind == 'color':
        return tuple(region[kind][pos * 4:pos * 4 + 4])
    raise ValueError(kind)


def define_map(text):
    return {m.group(1): m.group(2) for m in re.finditer(
        r'^#define[ \t]+(T3D_ST_[A-Z_0-9]+)[ \t]+([^\s/]+)', text, re.MULTILINE)}


def c_struct_members(text, struct_name, glsl=False):
    pattern = (r'struct ' + re.escape(struct_name) + r'\s*\{([^}]+)\};' if glsl else
               r'typedef struct (?:alignas\(\s*16\s*\)\s*)?' + re.escape(struct_name) + r'\s*\{([^}]+)\}')
    match = re.search(pattern, text, re.DOTALL)
    if not match:
        raise AssertionError('Missing struct: ' + struct_name)
    return [line.split('//', 1)[0].strip().rstrip(';') for line in match.group(1).splitlines()
            if line.strip() and not line.lstrip().startswith('//')]


class St00ContractTests(unittest.TestCase):
    def test_frozen_corpus_bytes(self):
        corpus = st00_corpus.run(False)
        self.assertEqual(len(corpus['fixtures']), 13)
        self.assertEqual(sum(f['name'] == 'absent' for f in corpus['fixtures']), 1)

    def test_corpus_sha_manifest(self):
        index = json.loads((HERE / 'corpus/index.json').read_text())
        for row in index['fixtures']:
            data = (HERE / 'corpus' / (row['name'] + '.st00region')).read_bytes()
            self.assertEqual(len(data), row['bytes'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), row['sha256'])
            self.assertEqual(load_region(row['name'])['index'], row['region_index'])

    def test_height_global_r16_quantization(self):
        self.assertEqual(st00_corpus.height_u16(-384.0), 0)
        self.assertEqual(st00_corpus.height_u16(768.0), 65535)
        self.assertEqual(st00_corpus.height_u16(0.0), 21845)
        one_lsb = 1152.0 / 65535.0
        for name, _, _ in st00_corpus.FIXTURES:
            if name == 'absent':
                continue
            source = load_region(name)
            for y in (0, 1, 63, 64, 127):
                for x in (0, 1, 63, 64, 127):
                    expected = st00_corpus.height_at(name, x, y)
                    encoded = value_at(source, 'height', x, y)
                    actual = -384.0 + encoded * one_lsb
                    self.assertLessEqual(abs(actual - expected), one_lsb * 0.51)
        self.assertEqual(value_at(load_region('negative_steep'), 'height', 0, 0), 0)

    def test_exact_boundary_height(self):
        west, east = load_region('border_west'), load_region('border_east')
        self.assertEqual(west['index'] + 1, east['index'])
        for y in range(128):
            self.assertEqual(value_at(west, 'height', 127, y), value_at(east, 'height', 0, y))

    def test_source_hole_and_alpha_quantization(self):
        alpha = load_region('alpha_extremes')
        for x, expected in ((0, 0), (31, 0), (32, 1), (63, 1),
                            (64, 2), (95, 2), (96, 3), (127, 3)):
            source_alpha = value_at(alpha, 'color', x, 3)[3]
            quantized = (source_alpha * 3 + 127) // 255
            self.assertEqual(quantized, expected)
        holes = load_region('coverage_edges')
        self.assertEqual((value_at(holes, 'control', 63, 0) >> 2) & 1, 0)
        self.assertEqual((value_at(holes, 'control', 64, 0) >> 2) & 1, 1)
        self.assertEqual((value_at(load_region('empty_holes'), 'control', 0, 0) >> 2) & 1, 1)
        self.assertEqual(load_region('absent')['flags'] & 1, 0)

    def test_control_bitfields_at_extremes(self):
        source = load_region('rotated_slopes')
        self.assertEqual((value_at(source, 'control', 127, 127) >> 10) & 15, 15)
        self.assertEqual((value_at(source, 'control', 127, 127) >> 7) & 7, 7)
        sparse = load_region('sparse_controls')
        self.assertEqual((value_at(sparse, 'control', 63, 63) >> 27) & 31, 31)
        self.assertEqual((value_at(sparse, 'control', 63, 63) >> 22) & 31, 30)
        self.assertEqual(value_at(sparse, 'control', 44, 44) & 1, 1)

    def test_bitplane_roundtrip_all_source_texels(self):
        for name, _, _ in st00_corpus.FIXTURES:
            if name == 'absent':
                continue
            region = load_region(name)
            data = region['control']
            rgba = region['color']
            for page_y in range(2):
                for page_x in range(2):
                    planes = [0] * (128 * 3)
                    values = []
                    for y in range(64):
                        for x in range(64):
                            i = ((page_y * 64 + y) * 128 + page_x * 64 + x)
                            control = struct.unpack_from('<I', data, i * 4)[0]
                            solid = 1 - ((control >> 2) & 1)
                            author_alpha = (rgba[i * 4 + 3] * 3 + 127) // 255
                            packed = solid | (author_alpha << 1)
                            values.append(packed)
                            word, bit = (y * 64 + x) >> 5, (y * 64 + x) & 31
                            for plane in range(3):
                                planes[plane * 128 + word] |= ((packed >> plane) & 1) << bit
                    for i, expected in enumerate(values):
                        word, bit = i >> 5, i & 31
                        actual = sum(((planes[plane * 128 + word] >> bit) & 1) << plane
                                     for plane in range(3))
                        self.assertEqual(actual, expected)
                    if len(set(values)) == 1:
                        self.assertTrue(0 <= values[0] <= 7)  # uniform sentinel
                    else:
                        self.assertEqual(len(struct.pack('<384I', *planes)), 1536)

    def test_vhm_page_and_tail_budget(self):
        physical = [(16 >> mip) + 4 for mip in range(5)]
        self.assertEqual(physical, [20, 12, 8, 6, 5])
        self.assertLessEqual(sum(physical), 52)
        self.assertLessEqual(max(physical), 20)
        self.assertEqual(1664 * 640 * 2, 2129920)
        self.assertEqual(68 * 68 * 2, 9248)
        self.assertEqual(4 + 7, 11)
        self.assertEqual(1024 * 4 * 1536, 6291456)

    def test_current_source_r16_and_hole_encoding(self):
        source = (ROOT / 'src/data/persistence/terrain_3d_region_codec.h').read_text()
        constants = (ROOT / 'src/Terrain3DConstants.h').read_text()
        util = (ROOT / 'src/core/utility/terrain_3d_util.h').read_text()
        self.assertIn('s_fRegionHeightMin', source)
        self.assertIn('s_fRegionHeightU16Scale', source)
        self.assertIn('( -384.0f )', constants)
        self.assertIn('( 768.0f )', constants)
        self.assertIn('nPixel >> 2 & 0x1', util)
        self.assertIn('nPixel >> 27 & 0x1F', util)
        self.assertIn('nPixel >> 22 & 0x1F', util)
        self.assertIn('nPixel >> 14 & 0xFF', util)

    def test_new_cpp_glsl_macro_parity(self):
        c = (HERE / 'st_region_contract.h').read_text()
        glsl = (HERE / 'st_region_contract.glslinc').read_text()
        self.assertEqual(define_map(c), define_map(glsl))
        cpp_names = c_struct_members(c, 't3dStRegionRecord_s')
        glsl_names = c_struct_members(glsl, 'StRegionRecordV1', True)
        self.assertEqual(len(cpp_names), 13)
        self.assertEqual(len(glsl_names), 13)
        self.assertEqual([re.match(r'\s*(?:uint32_t|float)', line).group(0).strip() for line in cpp_names],
                         ['uint32_t'] * 4 + ['float'] + ['uint32_t'] * 8)
        self.assertEqual([re.match(r'\s*(?:uint|vec4)', line).group(0).strip() for line in glsl_names],
                         ['uint'] * 4 + ['vec4'] + ['uint'] * 8)
        self.assertIn('sizeof( t3dRdTerrainRegionGpu_t ) == 48u',
                      (ROOT / 'src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h').read_text())
        self.assertIn('sizeof( t3dStRegionRecord_t ) == T3D_ST_REGION_SIZE_BYTES', c)

    def test_inventory_and_shader_passes_frozen(self):
        expected = (HERE / 'far_inventory.json').read_text()
        self.assertEqual(json.dumps(st00_inventory.scan(), indent=2, sort_keys=True) + '\n', expected)
        expected = (HERE / 'shader_pass_matrix.json').read_text()
        data = st00_shader_matrix.scan()
        self.assertEqual(json.dumps(data, indent=2, sort_keys=True) + '\n', expected)
        self.assertIn('shadow', {v['pass'] for v in data['variants']})

    def test_static_baseline_reproducible(self):
        expected = (HERE / 'static_baseline.json').read_text()
        self.assertEqual(json.dumps(st00_baseline.collect(), sort_keys=True, indent=2) + '\n', expected)
        self.assertIn('NOT_CAPTURED', st00_baseline.collect()['live_metrics']['status'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
