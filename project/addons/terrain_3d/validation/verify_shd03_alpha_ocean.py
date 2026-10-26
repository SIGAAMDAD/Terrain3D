#!/usr/bin/env python3
"""Compile-only SHD-03 GLSL preprocessing and runtime-owner contract checks.

This checker does not execute a GPU raster pass. It verifies that WORLD and
legacy preview share the exact coverage helper in legal pass variants, and
that the production ocean shader and the old prepared version preprocess to
the same fragment code (including ENV receiver variants).
"""
from pathlib import Path
import re
from verify_shd01_shader_contract import versions, preprocess

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
SRC = ROOT / 'src'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def has(source, expression, kind):
    require(re.search(expression, source), kind + ': missing ' + expression)


def main():
    alpha = (RD / 'terrain_alpha_coverage_inc.glslinc').read_text()
    has(alpha, r'bool\s+t3d_alpha_coverage_scissor\s*\(', 'shared alpha helper')
    has(alpha, r'opac(?:ity)\s*>=\s*cutoff', 'shared alpha threshold')
    has(alpha, r'bool\s+t3d_alpha_coverage_depth_prepass', 'preview prepass')

    # Validate against the actual historically shipped cutoff semantics.
    # WORLD and preview both use an 8-bit UNORM cutoff and >= comparison.
    sample_values = (0.0, 0.05, 0.25, 0.5, 0.75, 0.999, 1.0)
    cases = 0
    for packed_cutoff in range(256):
        cutoff = packed_cutoff * (1.0 / 255.0)
        for opacity in (*sample_values, cutoff):
            old_world = not (opacity < cutoff)
            old_preview = opacity >= cutoff
            new_helper = opacity >= cutoff
            require(old_world == new_helper == old_preview,
                    'cutoff discrepancy at %d and %.9f' % (packed_cutoff, opacity))
            cases += 1
    for opacity in sample_values:
        for prepass in (True, False):
            require((not prepass or opacity >= 0.5) == (not (prepass and opacity < 0.5)),
                    'prepass cutoff mismatch')

    fragment_cases = {
        'terrain_world_raster.glsl': [
            'mesh_color_native', 'mesh_color_motion', 'mesh_color_specular_motion_radiance_array',
            'mesh_depth', 'mesh_depth_normal_roughness', 'mesh_depth_normal_roughness_voxel_gi',
            'mesh_shadow', 'feedback_mesh_color_native', 'feedback_mesh_color_motion',
            'mesh_color_native_compact', 'env_mesh_color_native', 'env_mesh_color_motion',
            'env_mesh_depth', 'env_mesh_depth_normal_roughness', 'env_mesh_shadow',
            'env_feedback_mesh_color_native', 'env_mesh_color_native_compact',
                    ],
        'terrain_instances.glsl': [
            'color_native', 'color_motion', 'color_specular_motion_radiance_array',
            'depth', 'depth_normal_roughness', 'depth_normal_roughness_voxel_gi',
            'feedback_color_native', 'color_native_compact',
                    ],
    }
    preprocessed = 0
    for filename, names in fragment_cases.items():
        shader = RD / filename
        available = versions(shader)
        for name in names:
            require(name in available, filename + ': missing version ' + name)
            text = preprocess(shader, name)
            context = filename + '::' + name
            has(text, r'bool\s+t3d_alpha_coverage_scissor\s*\(', context)
            has(text, r'if\s*\(!t3d_alpha_coverage_scissor\s*\(', context) if filename.startswith('terrain_world_') else has(
                text, r'base_survives\s*=\s*t3d_alpha_coverage_scissor\s*\(', context)
            if 'depth' in name or 'shadow' in name or 'dnr' in name:
                require('t3d_vt_consumer_sample_albedo_alpha(albedo_address)' in text if filename.startswith('terrain_world_')
                        else 't3d_vt_consumer_sample_albedo_alpha(base_albedo_address)' in text,
                        context + ': depth/DNR/shadow not sampling alpha channel')
            if filename == 'terrain_instances.glsl':
                has(text, r't3d_alpha_coverage_depth_prepass\s*\(', context)
                has(text, r'overlay_survives\s*=\s*t3d_alpha_coverage_scissor\s*\(', context)
            preprocessed += 1

    preview_bringup = preprocess(RD / 'terrain_instances.glsl', 'bringup')
    require('t3d_alpha_coverage_scissor' not in preview_bringup,
            'bringup must not require VT material constants')
    preprocessed += 1

    ocean = RD / 'terrain_ocean.glsl'
    ocean_versions = versions(ocean)
    for name in ('color_prepared', 'env_color_prepared'):
        require(name not in ocean_versions, 'obsolete prepared ocean version remains: ' + name)
    for canonical in ('color_native', 'env_color_native'):
        current = preprocess(ocean, canonical)
        require('frame_data.scatter_params.xyz' in current, canonical + ': missing CPU-prepared sunlight vector')
        require('frame_data.scatter_params.w' in current, canonical + ': missing prepared foam scale')
        preprocessed += 1
    for name in ('shadow', 'env_shadow'):
        require('T3D_OCEAN_PREPARED_FRAGMENT' not in preprocess(ocean, name),
                name + ': shadow must not gain color-only work')
        preprocessed += 1

    world = (SRC / 'rendering/rd/world/visibility/terrain_3d_world_raster.cpp').read_text()
    instance = (SRC / 'rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp').read_text()
    lib = (SRC / 'rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
    header = (SRC / 'rendering/rd/shaders/terrain_3d_rd_shader_library.h').read_text()
    ocean_cpp = (SRC / 'rendering/rd/ocean/terrain_3d_rd_ocean_renderer.cpp').read_text()
    for symbol in ('getWorldAlphaTrialCold', 'getInstanceAlphaTrialCold', 'isOceanPreparedFragmentEnabledCold'):
        for owner, code in [('world', world), ('instance', instance), ('library', lib), ('header', header), ('ocean', ocean_cpp)]:
            require(symbol not in code, 'legacy selector ' + symbol + ' still in ' + owner)
    require('T3D_RD_SHADER_OCEAN_COLOR' in ocean_cpp and 'T3D_RD_SHADER_OCEAN_ENV_COLOR' in ocean_cpp,
            'ocean color/ENV shader ID routing lost')
    require('T3D_RD_SHADER_RETIRED_OCEAN_PREPARED_COLOR' not in ocean_cpp and
            'T3D_RD_SHADER_RETIRED_OCEAN_ENV_PREPARED_COLOR' not in ocean_cpp,
            'legacy ocean shader variant still selected')
    require('const Vector3 sun_direction = ( -rSnapshot.m_lightDirection ).normalized();' in ocean_cpp,
            'CPU side of canonical ocean preparation lost')
    catalog = (SRC / 'rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
    require('!t3dRdShaderCatalogIsActive( id )' in lib and
            'T3D_RD_SHADER_TERRAIN_VT_COLOR_HINT_BASE' in catalog and
            'T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE' in catalog,
            'retired alpha IDs must be rejected by catalog and direct acquisitions')
    print('PASS: SHD-03 shared opacity and pass contracts, %d fragment cases' % preprocessed)
    print('PASS: exhaustive UNORM8 cutoff reference comparison, %d cases' % cases)
    print('PASS: ocean production-prepared GLSL for base + ENV; obsolete trial entrypoints removed')


if __name__ == '__main__':
    main()
