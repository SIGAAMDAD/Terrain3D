#!/usr/bin/env python3
"""Regression gate for UCMP material reuse and startup world capacity."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
material = (ROOT / "rendering/rd/material/terrain_3d_material_state_builder.cpp").read_text()
types = (ROOT / "rendering/rd/core/terrain_3d_rd_types.h").read_text()
publication = (ROOT / "rendering/rd/core/terrain_3d_render_publication_builder.cpp").read_text()
header = (ROOT / "rendering/rd/core/terrain_3d_render_publication_builder.h").read_text()
coordinator = (ROOT / "rendering/rd/core/terrain_3d_render_coordinator.cpp").read_text()

start = material.index("bool Terrain3DMaterialStateBuilder::publishSnapshotState(")
end = material.index("void Terrain3DMaterialStateBuilder::resetPublicationState(", start)
method = material[start:end]
cache_branch = method.index("} else {")
backend_restore = method.rfind("snapshot.m_nTerrainMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED;")
assert backend_restore > cache_branch, "Cache-hit publication must restore UCMP backend"
assert 'm_nTerrainMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED;' in types
assert 'snapshot.m_nTerrainMaterialBackend != T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED' in publication
assert 'isWorldPublicationCapacityPending(' in header
assert coordinator.count('m_publicationBuilder.isWorldPublicationCapacityPending()') >= 3
print('PASS UCMP snapshot cache-hit, default backend, mandatory publication, world cold reserve')
