"""Static integration guards for U01 conventional mesh LOD cooking.

Godot editor render/serialization behavior requires the actual custom engine build.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSET = (ROOT / "src/assets/resources/terrain_3d_mesh_asset.cpp").read_text()
HEADER = (ROOT / "src/assets/resources/terrain_3d_mesh_asset.h").read_text()
COOK = (ROOT / "src/authoring/meshes/terrain_3d_meshopt_cook.cpp").read_text()
EDITOR = (ROOT / "project/addons/terrain_3d/src/editor_plugin.gd").read_text()
PANEL = (ROOT / "project/addons/terrain_3d/src/inspector/terrain_mesh_lod_inspector_plugin.gd").read_text()


def test_editor_contract() -> None:
    assert "T3D_EDITOR_NATIVE" in ASSET
    assert "generateMissingLodsPending();" in ASSET
    assert "capturePendingSurfaceRenderingMaterialsCold()" in ASSET
    assert "m_bMeshoptLodsEnabled = false" in ASSET
    assert "m_nAuthoredLodCount" in HEADER
    assert "m_pendingMeshes = oldPending" in ASSET
    assert "m_bMeshoptLodsEnabled = oldEnabled" in ASSET
    assert "staged( m_pendingMeshes.duplicate() )" in ASSET
    assert "if ( t3dMeshoptCook::hasEmbeddedLods( source ) ) { continue; }" in ASSET
    assert "pServer->mesh_get_surface( rSource->get_rid(), s )" in COOK
    assert 'surface.has( "lods" )' in COOK
    assert "meshopt_SimplifyErrorAbsolute" in COOK
    assert "meshopt_SimplifyLockBorder" in COOK
    assert "rSource->surface_get_material( s )" in COOK
    assert "m_fMeshoptLodError[lod] = errors[lod]" in ASSET
    assert "mesh_lod_inspector_plugin" in EDITOR
    assert "Bake Missing LODs" in PANEL
    assert "SubViewport.UPDATE_ONCE" in PANEL
    assert "get_meshopt_lod_stats" in PANEL
    print("U01 mesh cook source contract checks passed")


if __name__ == "__main__":
    test_editor_contract()
