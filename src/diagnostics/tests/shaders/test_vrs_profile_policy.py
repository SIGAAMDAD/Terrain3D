#!/usr/bin/env python3
"""C-01: production viewport VRS policy, ownership and feature-gate contract.

Source gate only; hardware support and viewport restore also require a Godot
rendering-device smoke test in a complete editor/runtime checkout.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SOURCE = (ROOT / 'src/terrain_3d.cpp').read_text()
HEADER = (ROOT / 'src/terrain_3d.h').read_text()
CONSTANTS = (ROOT / 'src/Terrain3DConstants.h').read_text()
CLIPMAP = (ROOT / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()
LIBRARY = (ROOT / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
GPU = (ROOT / 'src/rendering/rd/vt/gpu/terrain_3d_vt_gpu.cpp').read_text()
PLAN = (ROOT / 'src/rendering/rd/vt/planning/terrain_3d_vt_resource_plan.cpp').read_text()


def section(source, start, stop):
    return source.split(start, 1)[1].split(stop, 1)[0]


def verify():
    acquire = section(SOURCE, 'void Terrain3D::applyViewportVrsProfile(',
                      'void Terrain3D::releaseViewportVrsProfile(')
    release = section(SOURCE, 'void Terrain3D::releaseViewportVrsProfile(',
                      '// Private functions')
    assert 'terrain3d/rendering/vrs_profile' in acquire
    assert 'has_setting( "terrain3d/rendering/vrs_profile" )' in acquire
    assert 'profile != "uniform" && profile != "conservative"' in acquire
    assert 'profile == "uniform"' in acquire
    assert 'get_current_rendering_method() == "gl_compatibility"' in acquire
    assert 'pViewport->get_vrs_mode() != Viewport::VRS_DISABLED' in acquire
    assert 'm_nVrsViewportId != 0u' in acquire
    assert 'm_nVrsViewportId = pViewport->get_instance_id();' in acquire
    assert 'm_hVrsPreviousTexture = pViewport->get_vrs_texture();' in acquire
    assert 'm_nPreviousVrsUpdateMode' in acquire
    assert 'pViewport->set_vrs_texture( density );' in acquire
    assert 'pViewport->set_vrs_mode( Viewport::VRS_TEXTURE );' in acquire
    assert 'pViewport->get_vrs_texture() == m_hVrsProfileTexture' in release
    assert 'pViewport->set_vrs_mode( Viewport::VRS_DISABLED );' in release
    assert 'pViewport->set_vrs_texture( m_hVrsPreviousTexture );' in release
    assert 'pViewport->set_vrs_update_mode(' in release
    assert SOURCE.count('releaseViewportVrsProfile();') >= 3
    assert 'applyViewportVrsProfile( void );' in HEADER
    assert 'releaseViewportVrsProfile( void );' in HEADER
    for source in (SOURCE, HEADER, CONSTANTS):
        assert 'TERRAIN3D_VRS_TRIAL' not in source
        assert 'T3D_DEFAULT_VRS_TRIAL' not in source
    # Optional native features stay behind one consistent build gate with a
    # real non-hint fallback. No promotion is authorized by a source-only test.
    assert '#define T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS ( 0 )' in CONSTANTS
    assert '#define T3D_ENABLE_COLD_MATERIAL_CLASSES ( 0 )' in CONSTANTS
    for path in (CLIPMAP, LIBRARY, GPU, PLAN):
        assert 'T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS' in path
    assert 'pSharedVTBindings->hAncestorHints.is_valid()' in CLIPMAP
    assert 'return false;' in section(CLIPMAP, 'static bool t3dRdCanonicalHintAvailable(',
                                      'bool Terrain3DRdClipmapRenderer::ensureCanonicalVtShaders(')
    assert 't3dRdTerrainEditorDiagnosticClass(' in CLIPMAP
    assert '#if !T3D_ENABLE_COLD_MATERIAL_CLASSES' in CLIPMAP
    assert '#if T3D_ENABLE_COLD_MATERIAL_CLASSES' in CLIPMAP
    print('PASS C-01 VRS project profile, viewport lifetime and guarded shader features')


if __name__ == '__main__':
    verify()
