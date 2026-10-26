#!/usr/bin/env python3
"""SHD-05: retired shader entries, preserved IDs and active source closure.

Uses only source text, C++ contract fixtures and Clang GLSL preprocessing;
full Godot RDShaderFile/SPIR-V compilation requires the custom editor.
"""
from pathlib import Path
import re
from verify_shd01_shader_contract import versions, preprocess

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
SRC = ROOT / 'src'
CAT = (SRC / 'rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
LIB = (SRC / 'rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
IDS = (SRC / 'Terrain3DEnums.h').read_text()


def require(result, name):
    if not result:
        raise AssertionError(name)


def run():
    retired_sources = (
        RD / 'terrain_clipmap_screen_space.glsl',
        RD / 'terrain_vt_feedback_multi_writer.glsl',
    )
    for path in retired_sources:
        require(not path.exists(), 'orphan source still included: ' + str(path))
        require(path.name not in CAT, 'catalog still references ' + path.name)

    retired_trials = ('authored_exact_native', 'authored_hardware_native', 'planar_native',
        'two_key_native', 'active_key_native', 'fixed_taps_native', 'transform_reuse_native',
        'authored_corner_native', 'megavt_early_native', 'silhouette_native',
        'silhouette_only_native', 'control_nearest_native', 'control_bilinear_native',
        'control_adaptive_native', 'prepared_mip_bias_native', 'vt_ancestor_hints_native',
        'vt_ancestor_hints_prepared_bias_native')
    retired_alpha = ('alpha_gather_depth', 'alpha_edge_depth', 'alpha_gather_dnr', 'alpha_edge_dnr')
    retained_shader_cases = {
        'terrain_clipmap.glsl': ('color_native', 'feedback_color_native',
            'color_native_compact', 'feedback_color_native_compact', 'color_native_vt_hints',
            'feedback_color_native_vt_hints', 'depth', 'depth_normal_roughness',
            'color_specular_motion', 'feedback_color_specular_motion'),
        'terrain_instances.glsl': ('color_native', 'color_native_compact', 'feedback_color_native',
            'depth', 'depth_normal_roughness', 'bringup'),
        'terrain_world_raster.glsl': ('mesh_color_native', 'mesh_shadow', 'mesh_depth',
            'mesh_depth_normal_roughness', 'feedback_mesh_color_native',
            'mesh_color_native_compact', 'terrain_strip_color_native',
            'terrain_strip_depth_normal_roughness', 'env_mesh_color_native',
            'env_mesh_shadow', 'env_terrain_strip_color_native'),
        'terrain_ocean.glsl': ('color_native', 'env_color_native', 'shadow', 'env_shadow'),
    }
    preprocess_count = 0
    shader_count = 0
    for name, required in retained_shader_cases.items():
        path = RD / name
        table = versions(path)
        shader_count += len(table)
        for variant in required:
            require(variant in table, name + ' missing ' + variant)
            text = preprocess(path, variant)
            require('subgroupAllEqual' not in text, name + ':' + variant + ' retired subgroup still active')
            if '_vt_hints' in variant:
                require('buffer TerrainVTAncestorHints' in text,
                        name + ':' + variant + ' missing the canonical hint binding')
            preprocess_count += 1
        for name_inactive in retired_trials:
            if name.startswith('terrain_clipmap_'):
                require(name_inactive not in table, name + ' still compiles ' + name_inactive)
        if name in ('terrain_world_raster.glsl', 'terrain_instances.glsl'):
            for old in retired_alpha:
                require(old not in table, name + ' still compiles ' + old)
        if name == 'terrain_world_raster.glsl':
            require(not any(v.startswith('cs_terrain_strip_') for v in table),
                    'unreachable composed WORLD versions remain')
        if name == 'terrain_ocean.glsl':
            require('color_prepared' not in table and 'env_color_prepared' not in table,
                    'redundant ocean trial versions remain')
    # Append-stable ID slots must be reserved for dead sources, and the canonical
    # hinted routes reuse the historically allocated hint-layout positions.
    for symbol in ('T3D_RD_SHADER_RETIRED_MATERIAL_BASE',
                   'T3D_RD_SHADER_RETIRED_INSTANCE_ALPHA_BASE',
                   'T3D_RD_SHADER_RETIRED_WORLD_ALPHA_BASE',
                   'T3D_RD_SHADER_TERRAIN_VT_COLOR_HINT_BASE',
                   'T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE',
                   'T3D_RD_SHADER_RETIRED_SCREEN_SPACE_COVERAGE',
                   'T3D_RD_SHADER_RETIRED_VT_FEEDBACK_MULTI',
                   'T3D_RD_SHADER_RETIRED_OCEAN_ENV_PREPARED_COLOR'):
        require(symbol in IDS, 'shader ID slot dropped: ' + symbol)
    require('t3dRdUnifiedTerrainId(' in (SRC/'rendering/rd/shaders/terrain_3d_rd_shader_catalog.h').read_text(),
            'missing authoritative UCMP shader route')
    for hint in ('color_native_vt_hints', 'feedback_color_native_vt_hints'):
        require(hint in versions(RD / 'terrain_clipmap.glsl'),
                'missing optional UCMP resident-hint shader variant: ' + hint)
    for prefix in ('TERRAIN3D_MATERIAL_SHADER_TRIAL', 'TERRAIN3D_VT_SAMPLING_TRIAL',
                   'TERRAIN3D_INSTANCE_ALPHA_TRIAL', 'TERRAIN3D_WORLD_ALPHA_TRIAL',
                   'TERRAIN3D_OCEAN_PREPARED_FRAGMENT', 'TERRAIN3D_COMPOSED_SHADER_EXPERIMENT',
                   'TERRAIN3D_SCREEN_SPACE_PILOT', 'TERRAIN3D_FRAGMENT_SUBGROUP_EXPERIMENT'):
        require(prefix not in LIB, 'retired dispatch flag still read: ' + prefix)
    require('!t3dRdShaderCatalogIsActive( id )' in LIB and
            'id >= T3D_RD_SHADER_WORLD_CS_BASE && id < T3D_RD_SHADER_WORLD_CS_END' in CAT,
            'composed shader tombstones were reactivated')
    require('T3D_RD_SHADER_RETIRED_SCREEN_SPACE_COVERAGE' in CAT,
            'screen-space shader slots were reactivated')
    require('T3D_RD_SHADER_RETIRED_VT_FEEDBACK_MULTI' in CAT,
            'multi-feedback shader slot was reactivated')
    require('T3D_RD_SHADER_TERRAIN_VT_COLOR_HINT_BASE' in CAT and
            'T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE' in CAT,
            'canonical hint catalog mapping was dropped')
    # Resource and format authority are outside SHD-05.
    require('T3D_VT_ANCESTOR_PROBE_CAP' in (RD / 'vt/terrain_vt_resolve_core.glslinc').read_text(),
            'canonical bounded VT ancestor fallback was dropped')
    print(f'SHD-05 PASS: {preprocess_count} canonical GLSL preprocessor cases, {shader_count} active versions in selected shader families')
    print('SHD-05 PASS: legacy shader/catalog tombstones, ID routes, retained fallback and ENV variant closure')


if __name__ == '__main__':
    run()
