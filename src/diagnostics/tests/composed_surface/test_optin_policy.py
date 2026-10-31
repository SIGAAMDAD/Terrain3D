#!/usr/bin/env python3
"""SHD-05: retired composed WORLD variants remain reserved without startup loads."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
lib = (root/'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
catalog = (root/'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
raster = (root/'src/rendering/rd/world/visibility/terrain_3d_world_raster.cpp').read_text()
assert 'id >= T3D_RD_SHADER_WORLD_CS_BASE && id < T3D_RD_SHADER_WORLD_CS_END' in lib
assert 'if ( id >= T3D_RD_SHADER_WORLD_CS_BASE && id < T3D_RD_SHADER_WORLD_CS_END )' not in catalog
assert 'TERRAIN3D_COMPOSED_SHADER_EXPERIMENT' not in lib
assert 'm_bInitialized || bEnable' in raster.split('configureComposedExperimentalCold( const bool bEnable )')[1].split('}', 1)[0]
print('SHD-05 composed WORLD shader tombstones: PASS')
