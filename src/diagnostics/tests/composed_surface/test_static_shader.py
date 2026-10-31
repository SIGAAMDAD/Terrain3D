#!/usr/bin/env python3
"""Static consistency checks for CS11's deliberately narrow safe baking subset."""
from pathlib import Path
root = Path(__file__).resolve().parents[4]
shader = (root / 'project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc').read_text()
native = (root / 'src/authoring/vt/build/terrain_3d_composed_surface_static_baseline.cpp').read_text()
contract = (root / 'src/authoring/vt/build/terrain_3d_composed_surface_static_baseline.h').read_text()
assert shader.count('t3d_comp_surf_static_baseline_runtime_eligible(tier_config)') == 2
assert shader.count('bool t3d_comp_surf_static_baseline_runtime_eligible(') == 1
for word in ['tier_config.detiling_mode == 2u', 'material_config.projection_mode == 0u',
             '!t3d_feature_dual_scale_enabled()', '!t3d_feature_auto_shader_enabled()',
             'tier_config.surface_response != 0u', 'tier_config.sampling_mode == 5u',
             'T3D_TERRAIN_MATERIAL_BACKEND_CANONICAL_VT']:
    assert word in shader, word
for word in ['s.m_bDualScale', 's.m_bSideProjection', 's.m_bDetiling',
             's.m_bAdaptiveSampling', 's.m_bAutoShader', 's.m_bImplicitControls',
             's.m_bViewDependentMipBias', 's.m_bCanonicalMaterialParityAccepted',
             't3d_comp_surf_linear_to_srgb', 't3d_comp_surf_normalize', 'm_pSampleSource']:
    assert word in native, word
assert 'Source reader' in contract or 'source reader' in contract
print('CS11: opt-in canonical shader eligibility / cold source contract: PASS')
