#!/usr/bin/env python3
"""Static startup/authoring integration checks for ST06 transition.

These are source-contract checks; they do not claim a Vulkan smoke test.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
coordinator = (ROOT / 'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text()
exporter = (ROOT / 'src/authoring/data/terrain_3d_data_cooking.cpp').read_text()
publication = (ROOT / 'src/authoring/vt/build/terrain_3d_vt_demand_cooker.cpp').read_text()


def at(source, fragment):
    index = source.find(fragment)
    assert index != -1, f'cold integration missing {fragment!r}'
    return index


# Approved authoring builds composed-first, with no FAR fragment, and publishes
# package + height/mask transaction + demand sidecars in one rollback unit.
approved = coordinator[at(coordinator, 'static bool t3dRdPrepareComposedShippingCold'):]
approved = approved[:at(approved, 'bool Terrain3DRenderCoordinator::cookComposedSurfaceSidecarEditorCold')]
at(approved, 'pAccount )')
at(approved, 't3d_st_cook_region_package_cold')
at(coordinator, 't3d_vt_demand_publish_quartet_cold')
assert 'cookRegionalV1FragmentsCold' not in exporter
assert 'farFragment' not in exporter
at(exporter, 'region_height_cutout=terrain3d_surface.t3dst')
at(publication, 'Terrain3DFilePublication')

# CPU validate and prepare before signaling GPU-stage work. Render resources
# must be committed before the first published WORLD snapshot references them.
prepare_start = at(coordinator, 'bool Terrain3DRenderCoordinator::prepareStartupCold(')
prepare_end = at(coordinator, 'void Terrain3DRenderCoordinator::shutdown(')
prepare = coordinator[prepare_start:prepare_end]
assert at(prepare, 'loadRuntimeVirtualTexturePackageFileCold') < at(prepare, 'prepareSuperTerrainPackageOwnerCold')
assert at(prepare, 'prepareSuperTerrainPackageOwnerCold') < at(prepare, 'm_bStartupColdPrepared = true')

activation = coordinator[at(coordinator, 'bool Terrain3DRenderCoordinator::prepareStartupActivationCold('):]
activation = activation[:at(activation, 'bool Terrain3DRenderCoordinator::commitStartupActivationCold(')]
assert at(activation, 'commitSuperTerrainCandidateOwnerCold') < at(activation, 'publishSnapshot()')

stage = coordinator[at(coordinator, 'case T3D_STARTUP_GPU_COMPLETE:'):]
# The first occurrence is the stage-name function; choose the executable GPU switch.
stage = stage[at(stage, 'case T3D_STARTUP_GPU_COMPLETE:',):]
at(coordinator, '!uploadSuperTerrainCandidateOnRenderThread( m_stStartupCandidate, 128u )')
at(coordinator, 'm_stStartupCandidate.swap( empty )')
print('ST06: shipping no-FAR producer, atomic sidecars and ordered startup publication: PASS')
