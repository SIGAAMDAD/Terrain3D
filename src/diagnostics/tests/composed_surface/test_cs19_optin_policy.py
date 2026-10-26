#!/usr/bin/env python3
"""Source contract check for the CS19 cold shader opt-in policy."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[4]
lib=(root/'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
header=(root/'src/rendering/rd/shaders/terrain_3d_rd_shader_library.h').read_text()
raster=(root/'src/rendering/rd/world/visibility/terrain_3d_world_raster.cpp').read_text()
assert 'm_bComposedShaderExperiment = false' in lib
assert 'TERRAIN3D_COMPOSED_SHADER_EXPERIMENT' in lib
assert 'get_environment( "TERRAIN3D_COMPOSED_SHADER_EXPERIMENT" ) == "1"' in lib
assert 'isComposedShaderExperimentEnabledCold( void )' in header
assert 'id < T3D_RD_SHADER_WORLD_CS_BASE || Terrain3DRdShaderLibrary::isComposedShaderExperimentEnabledCold()' in lib
assert 'bEnable && !Terrain3DRdShaderLibrary::isComposedShaderExperimentEnabledCold()' in raster
assert 'm_bInitialized ||' in raster.split('configureComposedExperimentalCold( const bool bEnable )')[1].split('}',1)[0]
assert 'if ( !t3dRdShaderCatalogEntryEnabled( id ) )' in lib.split('bool Terrain3DRdShaderLibrary::resolveCatalogCold(')[1]
assert 'if ( !t3dRdShaderCatalogEntryEnabled( id ) || !t3dRdShaderNeedsStartupPhysical( id ) )' in lib
assert lib.count('t3dRdShaderCatalogEntryEnabled( ') >= 5
print('CS19 cold opt-in and legacy startup isolation: PASS')
