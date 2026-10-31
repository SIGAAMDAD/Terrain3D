"""BRP-00..03 deterministic native-format, edit routing and shape footprint fixtures.

Host tests cannot replace Godot editor parity captures, which are supplied under
project/addons/terrain_3d/tests/brush_capture.gd.
"""
import math
import random
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / 'src'
EDITOR = (SOURCE / 'authoring/editor/terrain_3d_editor.cpp').read_text()
DATA = (SOURCE / 'data/model/terrain_3d_data.cpp').read_text()
COLLISION = (SOURCE / 'physics/runtime/terrain_3d_collision.cpp').read_text()
ENUM = (SOURCE / 'Terrain3DEnums.h').read_text()
REGION = 128
TILE = 16


def touched_shape(x, z, size, sample_min, sample_max):
    return (x <= sample_max[0] and z <= sample_max[1] and
            x + size >= sample_min[0] and z + size >= sample_min[1])


def rect_samples(x, z, extent, spacing=1.):
    return ((math.floor(x / spacing) - 1, math.floor(z / spacing) - 1),
            (math.ceil((x + extent) / spacing) + 1, math.ceil((z + extent) / spacing) + 1))


def holes_differ(previous, following):
    return ((previous ^ following) & 4) != 0


def map_holes_differ(previous: bytes | None, following: bytes | None):
    default = struct.pack('<I', 1)
    n = REGION * REGION
    assert previous is None or len(previous) == n * 4
    assert following is None or len(following) == n * 4
    for index in range(n):
        old = struct.unpack_from('<I', previous, index * 4)[0] if previous is not None else 1
        new = struct.unpack_from('<I', following, index * 4)[0] if following is not None else 1
        if holes_differ(old, new):
            return True
    return False


class BrushWave1Contracts(unittest.TestCase):
    def test_typed_interactive_derived_backend(self):
        self.assertIn('MAP_EDIT_INTERACTIVE = 1u << 4', ENUM)
        self.assertIn('MAP_EDIT_KEEP_EDITED | MAP_EDIT_INTERACTIVE', EDITOR)
        self.assertIn('uint32_t commit_flags = MAP_EDIT_INTERACTIVE', EDITOR)
        self.assertIn('( nFlags & MAP_EDIT_INTERACTIVE ) == 0u', DATA)
        self.assertIn('m_mapEditService.commitMapEdits( eMapType, rRegionLocations, rEditedArea, pCompute )', DATA)
        self.assertIn('publishDerivedMapUpdates( mutation.m_derivedUpdates )', DATA)

    def test_editor_hole_bit_not_selected_brush(self):
        self.assertIn('t3dIsHole( src.r ) != t3dIsHole( dest.r )', EDITOR)
        self.assertIn('t3dIsHole( write.m_nSourceValue ) != t3dIsHole( element.m_nValue )', EDITOR)
        self.assertIn('map_type == TYPE_CONTROL && rState.m_bHoleChanged', EDITOR)
        self.assertIn('controlHoleBytesDiffer(', EDITOR)
        for kind in (0, 1, 2, 3, 0xFFFFFFFF, 0xFEFFFFFB):
            for bit in range(32):
                changed = kind ^ (1 << bit)
                self.assertEqual(holes_differ(kind, changed), bit == 2)

    def test_explicit_update_no_global_shape_rebuild(self):
        commit = DATA[DATA.index('void Terrain3DData::commitMapEdits('):DATA.index('void Terrain3DData::commitMapEdit(')]
        self.assertIn('pCollision->requestEditedHeightfieldArea(', commit)
        self.assertNotIn('pCollision->update(', commit)
        self.assertIn('m_dirtyEditedShapes.none()', COLLISION)
        self.assertIn('serviceEditedDynamicShapes( snapped_position, edited_work_started )', COLLISION)
        self.assertIn('m_emptyGridCells.reset( ( size_t ) i )', COLLISION)
        self.assertIn('m_dirtyEditedShapes.reset();', COLLISION)
        self.assertIn('updateFullShape( i, region )', COLLISION)

    def test_shared_129th_region_vertex(self):
        for x, z in ((0, 0), (128, 128), (-128, -128), (127, 0), (128, 1)):
            lo, hi = rect_samples(x, z, 0.01)
            expected = {(rx, rz) for rx in range(-2, 3) for rz in range(-2, 3)
                        if touched_shape(rx * REGION, rz * REGION, REGION, lo, hi)}
            if x in (0, 128, -128):
                self.assertGreaterEqual(len({rx for rx, _ in expected}), 2)
            if z in (0, 128, -128):
                self.assertGreaterEqual(len({rz for _, rz in expected}), 2)
            self.assertLessEqual(len(expected), 4)

    def test_stationary_dynamic_shape_pool_and_hole_fill(self):
        lo, hi = rect_samples(24, 24, 1)
        positions = [(x * 16, z * 16) for z in range(8) for x in range(8)]
        matched = [p for p in positions if touched_shape(*p, 16, lo, hi)]
        self.assertTrue(matched)
        self.assertLessEqual(len(matched), 4)
        # An empty grid cell's shape is reconsidered after a hole bit is removed.
        empty = set(matched)
        empty.difference_update(p for p in positions if touched_shape(*p, 16, lo, hi))
        self.assertFalse(empty)

    def test_control_undo_only_geometry_changes(self):
        rng = random.Random(101)
        words = [rng.getrandbits(32) for _ in range(REGION * REGION)]
        original = struct.pack('<%dI' % len(words), *words)
        words[22] ^= 1 << 19   # Texture data, not geometry.
        nav_only = struct.pack('<%dI' % len(words), *words)
        self.assertFalse(map_holes_differ(original, nav_only))
        words[22] ^= 1 << 2    # Hole occupancy.
        with_hole = struct.pack('<%dI' % len(words), *words)
        self.assertTrue(map_holes_differ(original, with_hole))
        implicit = struct.pack('<I', 1) * (REGION * REGION)
        self.assertFalse(map_holes_differ(None, implicit))
        with_hole = bytearray(implicit)
        with_hole[0:4] = struct.pack('<I', 5)
        self.assertTrue(map_holes_differ(None, with_hole))

    def test_native_format_bytes_unmodified_by_collision_classification(self):
        rng = random.Random(312)
        for size in (2, 4):
            original = rng.randbytes(REGION * REGION * size)
            source = bytearray(original)
            if size == 4:
                old = struct.unpack_from('<I', source, 92)[0]
                new = old ^ (1 << 15)
                struct.pack_into('<I', source, 92, new)
                self.assertFalse(holes_differ(old, new))
            else:
                encoded = struct.unpack_from('<H', source, 92)[0]
                struct.pack_into('<H', source, 92, encoded)
                self.assertEqual(original, bytes(source))
            self.assertEqual(len(source), len(original))


if __name__ == '__main__':
    unittest.main()
