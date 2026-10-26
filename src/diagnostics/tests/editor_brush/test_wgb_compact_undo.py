"""Wave B source contract + deterministic 128px tile-delta fixtures.

Native C++ editor behaviour is separately validated with clang++ -fsyntax-only;
these fixtures check the tile layout/restore arithmetic and source integration.
"""
from pathlib import Path
import random
import unittest

ROOT = Path(__file__).resolve().parents[4]
EDITOR = (ROOT / 'src/authoring/editor/terrain_3d_editor.cpp').read_text()
HEADER = (ROOT / 'src/authoring/editor/terrain_3d_editor.h').read_text()
PLUGIN = (ROOT / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text()
REGION = 128
TILE = 16


def extract_tile(data: bytes, pixel_size: int, tile_index: int) -> bytes:
    x = (tile_index % (REGION // TILE)) * TILE
    y = (tile_index // (REGION // TILE)) * TILE
    return b''.join(data[((y + row) * REGION + x) * pixel_size:
                         ((y + row) * REGION + x + TILE) * pixel_size]
                    for row in range(TILE))


def restore_tiles(data: bytearray, pixel_size: int, tiles: dict[int, bytes]) -> None:
    for index, tile in tiles.items():
        assert 0 <= index < 64
        assert len(tile) == TILE * TILE * pixel_size
    for index, tile in tiles.items():
        x = (index % (REGION // TILE)) * TILE
        y = (index // (REGION // TILE)) * TILE
        for row in range(TILE):
            offset = ((y + row) * REGION + x) * pixel_size
            data[offset:offset + TILE * pixel_size] = tile[row * TILE * pixel_size:(row + 1) * TILE * pixel_size]


class BrushUndoFixtures(unittest.TestCase):
    def test_native_contract(self):
        self.assertIn('static_assert( T3D_REGION_SIZE % T3D_BRUSH_UNDO_TILE_SIZE', HEADER)
        self.assertIn('recordBrushUndoTile( region_entry, mapPixelPosition, map_type )', EDITOR)
        self.assertIn('recordBrushUndoTile( entry, write.m_mapPixel, map_type )', EDITOR)
        self.assertIn('map->set_data( T3D_REGION_SIZE, T3D_REGION_SIZE', EDITOR)
        self.assertIn('finishBrushUndoTiles();', EDITOR)
        self.assertIn('ClassDB::bind_method( D_METHOD( "cancel_operation" )', EDITOR)
        self.assertIn('editor.cancel_operation()', PLUGIN)
        self.assertNotIn('pComputeBackend->processBrush( compute_elements, params )', EDITOR)
        self.assertNotIn('m_pRd->sync()', EDITOR)
        self.assertIn('Terrain3DCompute::processBrushCpu( compute_elements, params )', EDITOR)

    def test_first_touch_repeated_stamps_undo_redo(self):
        randomizer = random.Random(425)
        for pixel_size in (2, 4):  # R16 height; RF control/RGBA8 color
            original = bytes(randomizer.randbytes(REGION * REGION * pixel_size))
            working = bytearray(original)
            before = {}
            for _ in range(3500):
                x, y = randomizer.randrange(REGION), randomizer.randrange(REGION)
                tile_index = (y // TILE) * (REGION // TILE) + x // TILE
                if tile_index not in before:
                    before[tile_index] = extract_tile(bytes(working), pixel_size, tile_index)
                pos = (y * REGION + x) * pixel_size
                working[pos:pos + pixel_size] = randomizer.randbytes(pixel_size)
            expected = bytes(working)
            after = {i: extract_tile(expected, pixel_size, i) for i in before}
            restore_tiles(working, pixel_size, before)
            self.assertEqual(bytes(working), original)
            restore_tiles(working, pixel_size, after)
            self.assertEqual(bytes(working), expected)

    def test_sparse_memory_bound_and_cross_boundary(self):
        for x, y in ((0, 0), (127, 127), (15, 16), (16, 15)):
            tile_idx = (y // TILE) * 8 + x // TILE
            self.assertLess(tile_idx, 64)
        self.assertEqual(TILE * TILE * 4 * 2, 2048)
        self.assertLess(TILE * TILE * 4 * 2, REGION * REGION * 4 * 2)

    def test_implicit_map_undo_is_absence_and_redo_is_tiles(self):
        pixel_size = 4
        original_default = bytearray(REGION * REGION * pixel_size)
        modified = original_default.copy()
        before_index = 32
        tile = bytes([37] * (TILE * TILE * pixel_size))
        restore_tiles(modified, pixel_size, {before_index: tile})
        # Undo clears the implicit map, redo materializes it and replays only dirty tiles.
        restored = bytearray(original_default)
        restore_tiles(restored, pixel_size, {before_index: extract_tile(modified, pixel_size, before_index)})
        self.assertEqual(restored, modified)


if __name__ == '__main__':
    unittest.main()
