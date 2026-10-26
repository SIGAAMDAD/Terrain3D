#!/usr/bin/env python3
"""Deterministic contracts for the SIGGRAPH-inspired shader integration.

This tests logical equivalence of derived counters and alpha reconstruction.
It is not a substitute for Vulkan captures, driver execution, or GPU profiling.
"""

from __future__ import annotations

import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'


def mix(a: float, b: float, t: float) -> float:
    return a * (1.0 - t) + b * t


def reference_rgba_bilinear(corners: list[tuple[float, float, float, float]], u: float, v: float):
    # Original pipeline: decode RGB, premultiply, interpolate, then undo the
    # premultiply. In the source alpha is linear and its value is unchanged
    # by the decode and final unpremultiply operations.
    pre = [(r * a, g * a, b * a, a) for r, g, b, a in corners]
    blended = [mix(mix(pre[0][i], pre[1][i], u), mix(pre[2][i], pre[3][i], u), v)
               for i in range(4)]
    a = blended[3]
    if a > 0.00001:
        blended[:3] = [channel / a for channel in blended[:3]]
    else:
        blended[:3] = [0.0, 0.0, 0.0]
    return blended


def alpha_only_bilinear(corners: list[tuple[float, float, float, float]], u: float, v: float) -> float:
    return mix(mix(corners[0][3], corners[1][3], u),
               mix(corners[2][3], corners[3][3], u), v)


def test_alpha_equivalence() -> None:
    rng = random.Random(0x5A17)
    for _ in range(2000):
        n = rng.randrange(1, 5)  # emulate tap averages with 1-4 anisotropic taps
        rgba_samples = []
        alpha_samples = []
        for _ in range(n):
            corners = [tuple(rng.random() for _ in range(4)) for _ in range(4)]
            if rng.randrange(7) == 0:  # transparent texels
                corners[rng.randrange(4)] = (0.5, 0.7, 0.2, 0.0)
            uv = (rng.random(), rng.random())
            rgba_samples.append(reference_rgba_bilinear(corners, *uv))
            alpha_samples.append(alpha_only_bilinear(corners, *uv))
        original_alpha = sum(sample[3] for sample in rgba_samples) / len(rgba_samples)
        optimized_alpha = sum(alpha_samples) / len(alpha_samples)
        assert math.isclose(original_alpha, optimized_alpha, rel_tol=1e-13, abs_tol=1e-13)

        # Original trilinear code mixes complete filtered colors; the alpha
        # lane of that mix must be identical to an alpha-only mix.
        other = rng.random()
        t = rng.random()
        assert math.isclose(mix(original_alpha, other, t),
                            mix(optimized_alpha, other, t), rel_tol=1e-13, abs_tol=1e-13)

    instance = (RD / 'terrain_instances.glsl').read_text()
    world = (RD / 'terrain_world_raster.glsl').read_text()
    sampler = (RD / 'vt/terrain_vt_consumer_sample_inc.glslinc').read_text()
    assert 't3d_vt_consumer_sample_albedo_alpha(base_albedo_address)' in instance
    assert 't3d_vt_consumer_sample_albedo_alpha(albedo_address)' in world
    # fragment alpha optimization expands the exact alpha-only consumer to the depth/normal/roughness
    # pass; it now covers both depth-only and DNR without RGB reconstruction.
    assert sampler.count('#if defined(T3D_FORWARD_DEPTH)') == 2
    assert '#if defined(T3D_FORWARD_DEPTH) && !defined(T3D_FORWARD_NORMAL_ROUGHNESS)' not in sampler
    assert 't3d_vt_consumer_sample_alpha_single(pair.other)' in sampler
    assert 'texelFetch(bank' in sampler and 'texelFetch(tail' in sampler


def test_derived_visibility_counters() -> None:
    rng = random.Random(0xA117)
    for _ in range(3000):
        bucket_capacity = [rng.randrange(0, 512) for _ in range(9)]
        admitted = [rng.randrange(0, 1024) for _ in bucket_capacity]
        nonterrain_capacity = rng.randrange(0, 1024)
        nonterrain_count = rng.randrange(0, 2048)

        # Old atomic increments were issued only by admitted output lanes.
        old_terrain = sum(sum(1 for lane in range(n) if lane < cap)
                          for n, cap in zip(admitted, bucket_capacity))
        old_visible = old_terrain + sum(1 for lane in range(nonterrain_count)
                                      if lane < nonterrain_capacity)

        # New finalize stage derives counters from bucket reservations.
        new_terrain = sum(min(n, cap) for n, cap in zip(admitted, bucket_capacity))
        new_visible = new_terrain + min(nonterrain_count, nonterrain_capacity)
        assert old_terrain == new_terrain
        assert old_visible == new_visible

    source = (RD / 'terrain_world_visibility.glsl').read_text()
    gpu = (ROOT / 'src/rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.cpp').read_text()
    assert 'T3D_WORLD_VIS_COUNTER_CANDIDATE] = push_data.control.x;' in source
    assert 'T3D_WORLD_VIS_COUNTER_TERRAIN] = accepted_terrain;' in source
    assert 'T3D_WORLD_VIS_COUNTER_VISIBLE] = accepted_terrain + accepted_nonterrain;' in source
    assert 'push.m_nCandidateCount = candidate_count;' in gpu
    assert 'rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );' in gpu
    assert 'rd->compute_list_add_barrier( list );' in gpu


def test_light_and_editor_contracts() -> None:
    lights = (RD / 'terrain_forward_lighting_inc.glslinc').read_text()
    terrain = (RD / 'terrain_clipmap_fragment_inc.glslinc').read_text()
    feedback = (RD / 'vt/terrain_vt_feedback_inc.glslinc').read_text()
    consumers = (RD / 'vt/terrain_vt_consumer_sample_inc.glslinc').read_text()
    assert 'if (dot(brdf.normal, light_direction) <= 0.0)' in lights
    assert 'bool baked_only = i == 0u && directional_shadowmask_mode == 1u;' in lights
    for column in ('shadow_column0', 'shadow_column1', 'shadow_column2', 'shadow_column3'):
        assert f'vec4 {column};' in lights
    assert 'const uint exclusive_debug_modes' in terrain
    assert 'debug_modes != 0u && (debug_modes & ~exclusive_debug_modes) == 0u' in terrain
    assert 'apply_fog(apply_editor_decals(diagnostic))' in terrain
    assert 'T3DVTAllocation allocation,' in feedback
    assert 't3d_vt_fragment_logical_page_at_mip(allocation, uv' in feedback
    assert 't3d_vt_consumer_resolve_material_sibling_v1' in consumers


def main() -> None:
    test_alpha_equivalence()
    test_derived_visibility_counters()
    test_light_and_editor_contracts()
    print('SIGGRAPH shader integration: alpha, visibility counters and shader contracts PASS')


if __name__ == '__main__':
    main()
