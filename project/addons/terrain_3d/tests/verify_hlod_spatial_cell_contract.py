"""Static regression guard for independently culled instanced HLOD WORLD cells."""
from pathlib import Path

root = Path(__file__).resolve().parents[4]
streamer_h = (root / "src/hlod/runtime/terrain_3d_hlod_streamer.h").read_text()
streamer_c = (root / "src/hlod/runtime/terrain_3d_hlod_streamer.cpp").read_text()
baker = (root / "project/addons/terrain_3d/tools/hlod_baker.gd").read_text()
panel = (root / "project/addons/terrain_3d/tools/hlod_panel.gd").read_text()
publication = (root / "src/rendering/rd/core/terrain_3d_render_publication_builder.cpp").read_text()
codec = (root / "src/hlod/format/terrain_3d_hlod_runtime_codec.cpp").read_text()
checks = {
    "separate cell reservation": "getRdPublicationCellCapacity( void )" in streamer_h and
        "entry.m_groupCells.size()" in streamer_h,
    "separate prototype reservation": "getRdPublicationSourceCapacity( void )" in streamer_h and
        "entry.m_groupPrototypes.size()" in streamer_h,
    "old packages retain original grouping": "getRecipeVersion() >= 2" in streamer_c and
        "if ( !spatialCells )" in streamer_c,
    "cold-load spatial grouping": "entry.m_groupCells.push_back" in streamer_c and
        "entry.m_groupInstances.swap( decoded.m_instances )" in streamer_c,
    "separate snapshot culling cells": "const size_t groupCellCount" in streamer_c and
        "spatial->m_nFirstInstance + g" in streamer_c,
    "shared source handles": "rdMeshSourceKey( i, p )" in streamer_c,
    "coordinator reserves all three dimensions": "getRdPublicationCellCapacity()" in publication and
        "getRdPublicationInstanceCapacity()" in publication and
        "getRdPublicationSourceCapacity()" in publication,
    "bake accounts for culling cells": "spatial_cell_keys.size() * 192" in baker,
    "new recipe invalidates old package": "package.recipe_version = 2" in baker and
        "loaded.recipe_version == 2" in baker,
    "editor displays actual cell count": "active_spatial_cells" in panel,
    "spatial hierarchy accepts old decoder": "wire_version != 2u && wire_version != 3u && wire_version != 4u && wire_version != T3D_HLOD_RUNTIME_VERSION" in codec,
}
for name, result in checks.items():
    if not result:
        raise AssertionError(name)
    print("PASS:", name)
