#!/usr/bin/env python3
"""Editor candidate source/persistence contract; not an in-engine parity test."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
READ = lambda path: (ROOT / path).read_text(encoding='utf-8')
source = READ('src/authoring/vt/integration/terrain_3d_composed_surface_editor_source.cpp')
pipeline = READ('src/authoring/vt/build/terrain_3d_composed_surface_baseline_cold_pipeline.cpp')
owner = READ('src/authoring/vt/integration/terrain_3d_render_authoring_companion.cpp')
ui = READ('project/addons/terrain_3d/menu/terrain_menu.gd')

# The cooker consumes actual package bytes, material assets and sampled control
# cells; no pre-baked constant raster pages or fabricated GPU parity flags.
for token in ('rData.getControls( positions )', 'getTextureAsset( int( i ) )',
              'request.m_pSourcePackage = rPack.ptr()', 'tile.m_nControlsRevision',
              'request.m_bReferenceOnly = true',
              'request.m_bPageLocalCompositionIdentity = true'):
    assert token in source, token
assert 'terrain_composed.t3sc.candidate' in source
assert 't3d_comp_surf_validate_sidecar_bytes( previous.ptr()' in source
assert 't3d_comp_surf_validate_sidecar_bytes( committed.ptr()' in source
assert 't3d_comp_surf_find_page( priorView' in source
assert 'unsupported_region_page_layout' in source
assert 'missing_interior_control' in source
assert 'm_nBoundaryClampedPages' in source
assert '( r.m_bPageLocalCompositionIdentity && !r.m_bReferenceOnly )' in pipeline
assert '( r.m_bReferenceOnly && r.m_bPageLocalCompositionIdentity )' in pipeline

# Explicit user bake schedules the candidate only after source package success.
assert 'm_pAuthoring->m_composedCandidateSourcePack = shippingPackage;' in owner
assert 'serviceComposedReferenceBakeCold();' in owner
assert 'm_composedSurfaceEdits.statsCold()' in owner
assert 'reference_stale_after_edit' in owner
# A completed prior patch replaced the old literal `false` with the
# accepted-capture gate. The candidate must not turn that gate on by itself.
assert 'composed_parity_supported"] = approvedComposed' in owner
assert 'composed_bake_parity_ready"] = false' in owner
assert 'composed_cache_enabled"] = approvedComposed' in owner
assert 'composed_live_fallback_reason' in owner
assert 'Composed candidate:' in ui and 'viewport still uses live materials' in ui

# This source overlay does NOT enable unvalidated composed render cutover.
assert 'configureComposedExperimentalCold( true' not in owner
assert 'prepareComposedSurfaceCold(' not in owner
print('CM editor candidate snapshot/reuse/CRC/fail-closed contract: PASS')
