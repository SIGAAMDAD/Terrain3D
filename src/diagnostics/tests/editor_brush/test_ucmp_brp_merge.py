"""UCMP/BRP shader ownership integration checks (source contract, not GPU parity)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


class UcmpBrpMergeContract(unittest.TestCase):
    def test_stable_nonoverlapping_shader_ids(self):
        enums = source('src/Terrain3DEnums.h')
        self.assertIn('T3D_RD_SHADER_EDITOR_BRUSH_TILES = T3D_RD_SHADER_UCMP_FEEDBACK_COMPACT_BASE + T3D_RD_FORWARD_COLOR_VARIANT_COUNT', enums)
        self.assertIn('T3D_RD_SHADER_COUNT = T3D_RD_SHADER_EDITOR_BRUSH_TILES + 1u', enums)
        cat = source('src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp')
        self.assertIn('id >= T3D_RD_SHADER_UCMP_ALIAS && id < T3D_RD_SHADER_EDITOR_BRUSH_TILES', cat)
        self.assertIn('case T3D_RD_SHADER_EDITOR_BRUSH_TILES:', cat)
        self.assertIn('terrain_brush_tiles.glsl', cat)
        lib = source('src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp')
        self.assertIn('id >= T3D_RD_SHADER_UCMP_ALIAS && id < T3D_RD_SHADER_EDITOR_BRUSH_TILES', lib)

    def test_unified_shader_editor_preview(self):
        root = 'project/addons/terrain_3d/rd/'
        shader = source(root + 'terrain_clipmap.glsl')
        overlay = source(root + 'terrain_clipmap_editor_brush_overlay.glslinc')
        fragment = source(root + 'terrain_clipmap_fragment_inc.glslinc')
        self.assertEqual(2, shader.count('#include "terrain_clipmap_editor_brush_overlay.glslinc"'))
        self.assertIn('#define T3D_EDITOR_BRUSH_FRAGMENT_STAGE 1', shader)
        self.assertIn('layout(set = 5, binding = 0', overlay)
        self.assertIn('layout(set = 5, binding = 1', overlay)
        self.assertIn('t3d_editor_brush_invalidates_composed', overlay)
        self.assertIn('&& !t3d_editor_brush_invalidates_composed(world_texel)', fragment)
        self.assertIn('composed_eligible = composed_eligible && !t3d_editor_brush_invalidates_composed(world_texel)', fragment)
        self.assertIn('t3d_editor_brush_lookup(texel, 1u, edited_control)', fragment)
        self.assertIn('t3d_editor_brush_lookup(texel, 2u, edited_color)', fragment)
        self.assertIn('t3d_editor_brush_lookup(texel, 0u, authored_height)', source(root + 'terrain_clipmap_vertex_inc.glslinc'))
        for obsolete in ('near', 'middle', 'far'):
            self.assertFalse((ROOT / (root + 'terrain_clipmap_' + obsolete + '.glsl')).exists())

    def test_renderer_uses_unified_editor_pipeline_set5(self):
        renderer = source('src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp')
        coordinator = source('src/rendering/rd/core/terrain_3d_render_coordinator.cpp')
        self.assertIn('t3dRdUnifiedTerrainId(', renderer)
        self.assertIn('m_lastForwardPipelineKey.m_shaderPath == T3D_TERRAIN_SHADER_PATH_EDITOR_DEBUG_MIXED', renderer)
        self.assertIn('getEditorBrushOverlaySetOnRenderThread( shader )', renderer)
        self.assertIn('draw_list_bind_uniform_set( draw_list, overlaySet, 5u )', renderer)
        self.assertIn('T3D_RD_SHADER_EDITOR_BRUSH_TILES', coordinator)
        self.assertIn('T3D_EDITOR_BRUSH_MAX_TILES', coordinator)


if __name__ == '__main__':
    unittest.main()
