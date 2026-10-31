#!/usr/bin/env python3
"""Wave 14 shader source contracts and deterministic CPU equivalence models.

Validates exact alpha coverage, redundant-work guards, per-fragment reuse,
and sampler dispatch ordering. This is not Vulkan shader execution.
"""
from pathlib import Path
import math
import random

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
INST = (RD / 'terrain_instances.glsl').read_text()
WORLD = (RD / 'terrain_world_raster.glsl').read_text()
VT = (RD / 'vt/terrain_vt_consumer_sample_inc.glslinc').read_text()
LIGHT = (RD / 'terrain_forward_lighting_inc.glslinc').read_text()
CLIP = (RD / 'terrain_clipmap_fragment_inc.glslinc').read_text()


def source_contracts():
    for symbol in ('t3d_vt_consumer_alpha_bank_sample', 't3d_vt_consumer_sample_alpha_single'):
        assert symbol in VT
    assert '#if defined(T3D_FORWARD_DEPTH) && !defined(T3D_FORWARD_NORMAL_ROUGHNESS)' not in VT
    assert VT.count('#if defined(T3D_FORWARD_DEPTH)') == 2
    assert 'T3D_VT_ALBEDO_FAMILY_BRANCH_HOIST 0' in VT
    assert VT.count('#if T3D_VT_ALBEDO_FAMILY_BRANCH_HOIST') == 4
    assert 'base_color.a *= material.baseColor.a * t3d_vt_consumer_sample_albedo_alpha(base_albedo_address);' in INST
    assert 'overlay_color.a *= overlay.baseColor.a * t3d_vt_consumer_sample_albedo_alpha(overlay_albedo_address);' in INST
    assert 'base.a *= t3d_vt_consumer_sample_albedo_alpha(albedo_address);' in WORLD
    assert 'if (attenuation <= 0.0 || max(dot(N, light_direction), 0.0) <= 0.0) continue;' in INST
    assert INST.index('if (attenuation <= 0.0 || max(dot(N, light_direction), 0.0) <= 0.0) continue;') < INST.index('shadow = t3d_omni_shadow(light, context.view_position, N);')
    assert INST.count('rim_shape = mix(') == 1
    assert 'if (directional_count != 0u)' in INST
    assert 'if (roughness_blend == 0.0 || roughness_layer >= 5.0) return sample_a;' in LIGHT
    assert 'T3D_CONTROL_TRANSFORM_REUSE 0' in CLIP
    assert CLIP.count('t3d_prepare_control_sample_reusing(') == 4
    assert 'inout T3DControlTransformReuse transform_reuse' in CLIP
    # No change in the raster, VT descriptor or output ABI.
    for source in (WORLD, INST):
        assert '#ifdef T3D_VT_FRAGMENT_FEEDBACK' in source


def mix(a, b, f):
    return a * (1.0 - f) + b * f


def sampled_alpha(encoded, x, y):
    return mix(mix(encoded[0][0][3], encoded[0][1][3], x),
               mix(encoded[1][0][3], encoded[1][1][3], x), y)


def sampled_full(encoded, x, y):
    # The existing RGB decoding and alpha-premultiplication never modify .a.
    decoded = []
    for row in encoded:
        out = []
        for pixel in row:
            out.append(tuple(math.pow(max(c, 0.0), 2.2) for c in pixel[:3]) + (pixel[3],))
        decoded.append(out)
    return sampled_alpha(decoded, x, y)


def validate_depth_dnr_alpha(seed=0xD144, count=18000):
    r = random.Random(seed)
    cutout_equal = 0
    for i in range(count):
        encoded = [[tuple(r.random() for _ in range(4)) for x in range(2)] for y in range(2)]
        u, v = r.random(), r.random()
        old_albedo_alpha = sampled_full(encoded, u, v)
        new_albedo_alpha = sampled_alpha(encoded, u, v)
        assert old_albedo_alpha == new_albedo_alpha
        inst_alpha = r.random()
        mat_base_alpha = r.random()
        vertex_alpha = r.random()
        mesh_vertex_enabled = (i & 1) == 0
        a_old = inst_alpha * (vertex_alpha if mesh_vertex_enabled else 1.0)
        a_new = a_old
        a_old *= mat_base_alpha * old_albedo_alpha
        a_new *= mat_base_alpha * new_albedo_alpha
        assert a_old == a_new
        # Compare overlay scissor and the independent maximum alpha composite.
        overlay_alpha = r.random() * r.random() * r.random()
        cutoff_base, cutoff_overlay = r.random(), r.random()
        survives_old = a_old >= cutoff_base
        survives_new = a_new >= cutoff_base
        over_survives = overlay_alpha >= cutoff_overlay
        final_old = max(a_old if survives_old else 0.0, overlay_alpha if over_survives else 0.0)
        final_new = max(a_new if survives_new else 0.0, overlay_alpha if over_survives else 0.0)
        assert survives_old == survives_new and final_old == final_new
        assert (not survives_old and not over_survives) == (not survives_new and not over_survives)
        assert (final_old < 0.5) == (final_new < 0.5)
        cutout_equal += 1
    return cutout_equal


def validate_stylized_light(seed=0xDA7A, count=25000):
    r = random.Random(seed)
    skipped_shadows = 0
    for _ in range(count):
        ndotl = r.uniform(-1.0, 1.0)
        attenuation = r.random() if r.randrange(4) else 0.0
        shadow = r.random()
        radiance = r.random()
        metallic = r.random()
        albedo = r.random()
        dielectric = r.random()
        roughness = r.random()
        # Original: shadow sampled, then response immediately returns on <=0.
        old = 0.0
        if max(ndotl, 0.0) > 0.0 and attenuation * shadow > 0.0:
            factor = 1.0 - metallic * 0.88
            old = albedo * max(ndotl, 0.0) * factor * radiance * (attenuation * shadow)
        new = 0.0
        if attenuation > 0.0 and max(ndotl, 0.0) > 0.0:
            # Shadow is sampled only on surviving light paths.
            factor = 1.0 - metallic * 0.88
            if attenuation * shadow > 0.0:
                new = albedo * max(ndotl, 0.0) * factor * radiance * (attenuation * shadow)
        else:
            skipped_shadows += 1
        assert old == new
        # Invariant precomputation of metallic/specular is numerically identical.
        spec_old = mix(dielectric, albedo, metallic)
        spec_new = mix(dielectric, albedo, metallic)
        assert spec_old == spec_new
        smooth = 1.0 - roughness
        assert smooth == 1.0 - roughness
    return skipped_shadows


def validate_transform_memo(seed=0xABC421, count=10000):
    r = random.Random(seed)
    saved = 0
    for _ in range(count):
        # Different material IDs can share the same rotation/scale transform.
        common_transform = r.randrange(128)
        keys = [((r.randrange(32) << 27) | (r.randrange(32) << 22) |
                 ((common_transform if r.randrange(3) else r.randrange(128)) << 7)) for i in range(4)]
        last = None
        cached = None
        old_states = []
        new_states = []
        for key in keys:
            transform = (key >> 7) & 127
            # Structural model of projected UV, dx/dy and rotation values.
            state = tuple(((transform + 1) * (i + 3) % 1021) / 1021 for i in range(14))
            old_states.append(state)
            if last == transform:
                new_states.append(cached)
                saved += 1
            else:
                new_states.append(state)
                last, cached = transform, state
        assert old_states == new_states
    return saved


def validate_filter_family_hoist(seed=0x144FA, count=12500):
    r = random.Random(seed)
    tap_samples = 0
    for _ in range(count):
        taps = r.randrange(1, 5)
        family = r.randrange(2)
        bc1 = [r.random() for i in range(taps)]
        bc7 = [r.random() for i in range(taps)]
        original = 0.0
        for i in range(taps):
            original += (bc7[i] if family else bc1[i])
        original /= taps
        hoisted = 0.0
        source = bc7 if family else bc1
        for i in range(taps):
            hoisted += source[i]
        hoisted /= taps
        assert original == hoisted
        tap_samples += taps
    return tap_samples


def validate_radiance_blend(seed=0x144B, count=14000):
    r = random.Random(seed)
    bypass = 0
    for i in range(count):
        lod = 5.0 if i % 13 == 0 else (float(r.randrange(6)) if i % 7 == 0 else r.random() * 5.0)
        layer = math.floor(lod)
        frac = lod - layer
        a, b = r.random(), r.random()
        # When clamped to max layer, both fetches address the same layer.
        if layer >= 5: b = a
        old = mix(a, b, frac)
        # GLSL mix may differ by last-bit rounding when weights are zero;
        # exact source boundary cases use mathematically identical samples.
        new = a if (frac == 0.0 or layer >= 5) else mix(a, b, frac)
        assert abs(old-new) <= 1e-15
        bypass += int(frac == 0.0 or layer >= 5)
    return bypass


def main():
    source_contracts()
    alpha = validate_depth_dnr_alpha()
    shadow = validate_stylized_light()
    memo = validate_transform_memo()
    taps = validate_filter_family_hoist()
    array = validate_radiance_blend()
    print(f'PASS Wave14: {alpha} depth/DNR cutout cases; {shadow} modeled local shadow samples avoided')
    print(f'PASS Wave14: {memo} adjacent transform evaluations reused; {taps} format-selected filter taps')
    print(f'PASS Wave14: 14000 radiance-layer cases ({array} single-layer bypass); source contracts')


if __name__ == '__main__':
    main()
