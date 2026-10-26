#!/usr/bin/env python3
"""Structural checks for CS10's render-owner integration, not an engine build."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
header = (root/'src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h').read_text()
owner = (root/'src/rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.cpp').read_text()
coordinator = (root/'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text()
gpu = (root/'src/rendering/rd/vt/gpu/terrain_3d_composed_surface_gpu.cpp').read_text()
shader = (root/'project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc').read_text()
assert 'Terrain3DComposedSurfaceGpu m_ComposedGpu;' in header
assert 'Terrain3DComposedSurfaceCache m_ComposedCache;' in header
assert 't3d_cs_open_sidecar_cold(' in owner
assert 'nExpectedSourceFingerprint' in owner
assert 'shutdownComposedSurfaceCold( rContext );' in owner
assert owner.index('shutdownComposedSurfaceCold( rContext );', owner.index('beginVtResourceReplacementCold(')) < owner.index('invalidateBoundDescriptorsCold( rContext );', owner.index('beginVtResourceReplacementCold('))
assert 'cache->invalidateDependencyCold( bindings.m_hPageTable' in owner
assert 'cache->invalidateDependencyCold( bindings.m_hChannels[i]' in owner
assert 'cache->currentSubmissionEpochHot()' in owner
assert 'cache->diagnostics().m_nCompletedSubmissionEpoch' in owner
assert 'bSharedTransferAdmitted' in owner
assert 'm_sharedVtRuntime.serviceComposedGpuCompletionsHot' in coordinator
assert coordinator.index('beginRenderFrameHot( rContext.m_nRenderFrame )') < coordinator.index('m_sharedVtRuntime.serviceComposedGpuCompletionsHot')
assert 'serviceCompletedUploadsHot' in gpu
assert 't3d_cs_gpu_epoch_completed' in gpu
assert 'PackedByteArray zeroTable' in gpu
assert 'T3D_CS_ACTIVE' in shader
assert '#define T3D_CS_ACTIVE' not in shader
print('CS10: shared VT lifecycle, epoch publication, descriptor invalidation: PASS')
