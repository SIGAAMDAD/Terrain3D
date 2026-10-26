#!/usr/bin/env python3
"""SHD-02: canonical control, prepared mip, feedback, hint ABI, fallback and selector route checks.
Uses clang preprocessing; Godot GLSL/SPIR-V compilation still requires the custom editor.
"""
import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
SRC = ROOT / 'src'
spec = importlib.util.spec_from_file_location('t3d_shd01_verify', Path(__file__).with_name('verify_shd01_shader_contract.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

CASES = ['color_native', 'color_specular_motion', 'color_native_one_sun',
         'depth', 'depth_normal_roughness', 'color_native_compact',
         'feedback_color_native', 'feedback_color_specular_motion',
         'feedback_color_native_one_sun', 'feedback_color_native_compact',
         'feedback_color_specular_motion_radiance_array_compact',
         'color_native_vt_hints',
         'feedback_color_native_vt_hints']


def ensure(value, label):
    if not value:
        raise AssertionError(label)


def main():
    layout_checks = 0
    for tier in ('near', 'middle', 'far'):
        shader = RD / ('terrain_clipmap_' + tier + '.glsl')
        names = module.versions(shader)
        for variant in CASES:
            ensure(variant in names, tier + ':' + variant + ' missing shader version')
            source = module.preprocess(shader, variant)
            label = tier + ':' + variant
            ensure(re.search(r'float near_derivative_scale\s*=\s*[^;]*\.reserved_3;', source) is not None,
                   label + ' bypassed prepared mip-bias producer')
            ensure('bool adaptive = footprint < 16.0;' in source,
                   label + ' bypassed adaptive footprint control')
            ensure('tier_config.sampling_mode == 2u' in source and
                   'tier_config.sampling_mode == 1u' in source,
                   label + ' dropped authored nearest/bilinear semantics')
            module.verify_layout(source, 0,
                {'TerrainVTPageTableBuffer': 27, 'TerrainVTAllocationBuffer': 26,
                 'TerrainVTMipLayoutBuffer': 37}, label)
            hint = '_hints' in variant or '_ancestor_hints' in variant
            has_hint = 'buffer TerrainVTAncestorHints' in source
            ensure(hint == has_hint, label + ' hint descriptor layout does not match variant')
            if hint:
                module.verify_layout(source, 0, {'TerrainVTAncestorHints': 43}, label)
                ensure('T3DVTAncestorHint hintHeader =' in source, label + ' hint resolver absent')
            feedback = variant.startswith('feedback_')
            has_feedback = 'buffer VtFragmentGridBuffer' in source
            ensure(feedback == has_feedback, label + ' feedback set-3 layout incorrect')
            if feedback:
                module.verify_layout(source, 3,
                    {'VtFragmentGridBuffer': 2, 'VtFragmentCounterBuffer': 3,
                     'VtFragmentFeedbackParams': 4}, label)
            if variant.endswith('_compact'):
                ensure('t3d_br21_cursor_begin' in source and 't3d_cluster_buffer' in source,
                       label + ' compact lighting path did not activate')
            layout_checks += 1
        # Nonshipping material modes retain original profile selection.
        compat = module.preprocess(shader, 'compat_color_native')
        ensure(re.search(r'float near_derivative_scale\s*=\s*exp2\([^;]*\.mipmap_bias\);', compat) is not None,
               tier + ': compatibility evaluator unexpectedly changed')

    resolver = (RD / 'vt/terrain_vt_resolve_core.glslinc').read_text()
    ensure('uint probeCap = min(T3D_VT_ANCESTOR_PROBE_CAP, allocationMipCount - requestedMip);' in resolver,
           'Bounded fallback probe cap was removed')
    ensure('hintHeader.resolvedPage == T3D_VT_HINT_ENABLED' in resolver and
           'requestedEntry.generation != allocation.generation' in resolver,
           'Hint validity/residency generation guard missing')
    # Validate all eight compact-feedback signatures for every terrain tier.
    for tier in ('near', 'middle', 'far'):
        shader = RD / ('terrain_clipmap_' + tier + '.glsl')
        for variant in ('native', 'specular', 'motion', 'specular_motion',
                        'native_radiance_array', 'specular_radiance_array',
                        'motion_radiance_array', 'specular_motion_radiance_array'):
            version = 'feedback_color_' + variant + '_compact'
            compiled = module.preprocess(shader, version)
            ensure('buffer VtFragmentGridBuffer' in compiled and
                   't3d_cluster_buffer' in compiled, tier + ':' + version + ' ABI mismatch')
            layout_checks += 1
    cpp = (SRC / 'rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()
    preparation = cpp[cpp.index('bool Terrain3DRdClipmapRenderer::prepareForwardPass('):]
    ensure('!ensureMaterialTrialShaders(' not in preparation, 'Material trial still takes production slot')
    ensure('!ensureVtSamplingTrialShaders(' not in preparation, 'VT trial still takes production slot')
    ensure('t3dRdCanonicalHintAvailable(' in preparation and 'ensureCanonicalVtShaders(' in preparation,
           'Missing canonical hint capability route')
    ensure('m_hszForwardCompactShaders[path][tier][variant]' in cpp and
           'm_hszForwardCompactShaders[shaderPath][nTier][variant]' in cpp,
           'Compact shader RID must not reuse cached noncompact shader')
    ensure('!m_bForwardFeedbackActive;' not in
           cpp[cpp.index('const bool compact = m_forwardLighting.compactRequested() && !one_sun_lighting &&'):],
           'Compact cluster set may not be disabled just because feedback is active')
    ensure('m_forwardLighting.ensureCompactSet( shader )' in cpp and
           'm_hszFeedbackCompactShaders[nTier][variant]' in cpp,
           'Compact feedback shader must receive the set-4 cluster descriptor')
    ensure('!bOneSunLighting && m_forwardLighting.compactRequested()' in cpp,
           'One-sun compact route must not shadow the one-sun shader')
    ensure('Terrain3DRdDescriptorLeaseTable< t3dSetCacheKey_t, 8u >' in
           (SRC / 'rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h').read_text(),
           'Three baseline plus three hint descriptor leases require at least six slots')
    catalog = (SRC / 'rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
    for name in ['feedback_color_native_compact', 'feedback_color_specular_compact',
                 'feedback_color_motion_compact', 'feedback_color_specular_motion_radiance_array_compact']:
        ensure(name in catalog, name + ' is not present in the compact-feedback catalog')
    builder = (SRC / 'rendering/rd/material/terrain_3d_material_state_builder.cpp').read_text()
    ensure('config.m_fReserved3 = exp2f( config.m_fMipmapBias );' in builder,
           'Prepared mip value does not have a CPU producer')
    print('PASS: shader paths and descriptor layouts: ' + str(layout_checks))
    print('PASS: authored nearest/bilinear, prepared mip and adaptive control in all shipping variants')
    print('PASS: hint generation checks, bounded fallback, independent feedback set and compact variants')
    print('PASS: production dispatch does not depend on material/VT experiment selectors')


if __name__ == '__main__':
    main()
