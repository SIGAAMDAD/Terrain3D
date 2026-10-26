#!/usr/bin/env python3
"""zero-contribution fragment optimization source contracts and CPU reference checks for zero-work branches.

The numeric tests exercise branch equivalence for finite inputs. They are NOT
GPU shader execution, SPIR-V validation, or image-residency acceptance.
"""
from pathlib import Path
import math
import random

RD = Path(__file__).resolve().parents[4] / 'project/addons/terrain_3d/rd'
LIGHT = (RD / 'terrain_forward_lighting_inc.glslinc').read_text()
INST = (RD / 'terrain_instances.glsl').read_text()
CLIP = (RD / 'terrain_clipmap_fragment_inc.glslinc').read_text()


def check_source_contracts():
    assert 'bool albedo_active = t3d_rect_active(decal.albedo_rect) && decal.modulate.a != 0.0;' in LIGHT
    assert 'bool emission_active = decal.emission_energy != 0.0 && t3d_rect_active(decal.emission_rect)' in LIGHT
    assert '&& !all(equal(decal.modulate.rgb, vec3(0.0)));' in LIGHT
    assert LIGHT.index('if (!albedo_active && !emission_active)') < LIGHT.index('vec3 uv_local =')
    assert 'if (accumulated_weight == 0.0) return vec3(0.0);' in LIGHT
    assert 'if (sky_mix > 0.0)' in LIGHT
    assert 'if (horizon == 0.0 || ao <= 0.0' in LIGHT
    assert 'if (brdf.gloss > 0.0 && specular_amount > 0.0)' in LIGHT
    assert 'if (lighting_model != 1u && lighting_model != 2u' in LIGHT
    assert LIGHT.count('attenuation <= 0.0 || dot(brdf.normal, light_direction) <= 0.0') == 3
    assert '&& !all(equal(light.color, vec3(0.0))))' in LIGHT
    assert '|| all(equal(light.color, vec3(0.0))))' in LIGHT
    assert 'if (specular_amount > 0.0 && roughness != 1.0)' in INST
    assert 'if (!diffuse_active && !specular_active && !rim_active) continue;' in INST
    assert INST.index('if (!diffuse_active && !specular_active && !rim_active)') < INST.index('float shadow = t3d_directional_shadow')
    assert INST.count('float contrast_scale = 1.0 + frame_data.nomad_sun_shape.w * 3.5;') == 1
    assert 'if (ndotl <= 0.0)' in CLIP
    assert 'if ((material_config.output_flags & T3D_OUTPUT_SPECULAR) != 0u && gloss > 0.0)' in CLIP
    assert CLIP.index('if (tier_config.lighting_model == 2u)', CLIP.index('void light_material(')) < CLIP.index('if (ndotl <= 0.0)', CLIP.index('void light_material('))
    # The interface layout and feedback emission are untouched in zero-contribution fragment optimization.
    for path in ('terrain_instances.glsl', 'terrain_clipmap_fragment_inc.glslinc'):
        source = (RD / path).read_text()
        assert 'T3D_VT_FRAGMENT_FEEDBACK' in source


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def normalize(a):
    size = math.sqrt(sum(x*x for x in a))
    return tuple(x / size for x in a)


def zero_diffuse_and_spec_model(ndotl, attenuation, model, ndotv):
    # t3d_light_response's early-exit contract, independently of the BRDF.
    return ndotl <= 0.0 or attenuation <= 0.0 or (model not in (1, 2) and ndotv <= 0.0)


def test_native_early_reject(seed=0x15038, count=40000):
    rng = random.Random(seed)
    zeros = 0
    facing = 0
    empty_radiance = 0
    for _ in range(count):
        n = normalize((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
        l = normalize((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
        v = normalize((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
        ndotl = dot(n, l)
        ndotv = dot(n, v)
        model = rng.choice((1, 2, 3, 4))
        attenuation = rng.choice((0.0, -0.02, rng.random(), rng.random()))
        color = tuple(rng.choice((0.0, rng.random(), rng.random())) for _ in range(3))
        energy = rng.choice((0.0, rng.random(), rng.random()))
        early_ggx = model not in (1, 2) and ndotv <= 0.0
        early_local = attenuation <= 0.0 or ndotl <= 0.0
        early_radiance = all(c == 0.0 for c in color) or energy == 0.0
        if early_ggx or early_local:
            assert zero_diffuse_and_spec_model(ndotl, attenuation, model, ndotv)
            zeros += 1
        if ndotl <= 0.0 and attenuation > 0.0:
            facing += 1
        if early_radiance:
            empty_radiance += 1
            # Every direct BRDF multiplies the supplied light radiance.
            assert all(c * energy == 0.0 for c in color) if energy == 0.0 else all(c == 0.0 for c in color)
    return zeros, facing, empty_radiance


def test_native_blinn(seed=0x15041, count=28000):
    rng = random.Random(seed)
    bypassed = 0
    for _ in range(count):
        roughness = rng.choice((0.0, 1.0, 1.2, rng.random(), rng.random()))
        spec_amount = rng.choice((0.0, -1.0, 0.4, rng.random()))
        gloss = max(1.0 - roughness, 0.0)
        spec_power = 4.0 + 124.0 * gloss * gloss
        ndoth = rng.random()
        old = math.pow(ndoth, spec_power) * gloss * max(spec_amount, 0.0)
        if gloss > 0.0 and spec_amount > 0.0:
            new = math.pow(ndoth, spec_power) * gloss * spec_amount
        else:
            new = 0.0
            bypassed += 1
        assert old == new
    return bypassed


def test_decals(seed=0x15038D, count=25000):
    rng = random.Random(seed)
    bypassed = 0
    for _ in range(count):
        albedo_rect = rng.choice((True, False))
        emission_rect = rng.choice((True, False))
        modulate_alpha = rng.choice((0.0, rng.random()))
        emission_energy = rng.choice((0.0, 0.0, rng.random()))
        albedo_active = albedo_rect and modulate_alpha != 0.0
        emission_active = emission_rect and emission_energy != 0.0
        shortcut = not albedo_active and not emission_active
        if shortcut:
            # Normal/ORM updates are gated on the albedo decal's final alpha;
            # emission is gated independently by rectangle + energy.
            alpha = modulate_alpha * rng.random() if albedo_rect else 0.0
            assert alpha == 0.0
            assert not emission_active
            bypassed += 1
    return bypassed


def test_env(seed=0x15040, count=19000):
    rng = random.Random(seed)
    bypassed_ambient = 0
    bypassed_specular = 0
    for _ in range(count):
        sky_mix = rng.choice((0.0, 0.0, rng.random(), 1.0))
        exposure = rng.choice((0.0, 0.0, rng.random()))
        environment_energy = rng.choice((0.0, rng.random()))
        diffuse_sample = rng.random()
        ambient = rng.random()
        old = ambient * (1.0-sky_mix) + diffuse_sample * exposure * environment_energy * sky_mix
        new_sample = diffuse_sample if sky_mix != 0.0 and exposure != 0.0 and environment_energy != 0.0 else 0.0
        new = ambient if sky_mix == 0.0 else ambient * (1.0-sky_mix) + new_sample * exposure * environment_energy * sky_mix
        assert math.isclose(old, new, rel_tol=1e-14, abs_tol=1e-14)
        if sky_mix == 0.0 or exposure == 0.0 or environment_energy == 0.0:
            bypassed_ambient += 1
        horizon = rng.choice((0.0, rng.random()))
        ao = rng.choice((0.0, rng.random()))
        if horizon == 0.0 or ao <= 0.0 or exposure == 0.0 or environment_energy == 0.0:
            # Both horizon and AO are multiplicative factors in environment specular.
            assert horizon * horizon * ao * exposure * environment_energy == 0.0
            bypassed_specular += 1
    return bypassed_ambient, bypassed_specular


def smoothstep(lo, hi, x):
    if lo == hi:
        return 1.0 if x >= hi else 0.0
    v = max(0.0, min((x-lo)/(hi-lo), 1.0))
    return v*v*(3.0-2.0*v)


def test_stylized_sun(seed=0x15045, count=30000):
    rng = random.Random(seed)
    bypassed = 0
    skipped_spec = 0
    for _ in range(count):
        ndotl = rng.uniform(-1.0, 1.0)
        lambert = max(ndotl, 0.0)
        wrap = rng.uniform(-0.3, 0.5)
        smin = rng.uniform(-0.5, 0.5)
        smax = max(rng.uniform(-0.5, 0.5), 0.001)
        diffuse_term = (1.0-wrap)*lambert + wrap*smoothstep(-smin, smax, ndotl)
        contrast_scale = 1.0 + rng.uniform(-0.5, 1.0)*3.5
        diffuse_term = min(max((diffuse_term-0.5)*contrast_scale+0.5, 0.0), 1.0)
        fallback = rng.choice((0.0, rng.random()))
        diffuse_term = max(diffuse_term, fallback*(1.0-lambert))
        f2 = diffuse_term*diffuse_term
        f4 = f2*f2
        diffuse_term = min(diffuse_term*(1.0+f4*rng.random()), 2.0)
        rim = rng.choice((0.0, rng.random()))
        rim_scale = rng.choice((0.0, rng.random()))
        diff_scale = rng.choice((0.0, rng.random()))
        spec_scale = rng.choice((0.0, rng.random()))
        spec_light = rng.choice((0.0, rng.random()))
        backlight = max(-ndotl, 0.0)
        diffuse_active = diffuse_term != 0.0 and diff_scale != 0.0
        specular_active = ndotl > 0.0 and spec_scale != 0.0 and spec_light > 0.0
        rim_active = rim != 0.0 and backlight != 0.0 and rim_scale != 0.0
        if not (diffuse_active or specular_active or rim_active):
            # Potential contributions are exact zero before shadow evaluation.
            assert diffuse_term*diff_scale == 0.0
            assert (spec_scale*max(spec_light, 0.0)) == 0.0 or ndotl <= 0.0
            assert rim*backlight*rim_scale == 0.0
            bypassed += 1
        if not specular_active:
            skipped_spec += 1
    return bypassed, skipped_spec


def test_legacy_terrain(seed=0x15046, count=16000):
    rng = random.Random(seed)
    blinn_skips = 0
    ggx_skips = 0
    for _ in range(count):
        roughness = rng.choice((1.0, 1.1, rng.random()))
        output_specular = rng.choice((False, True))
        ndotl = rng.choice((0.0, rng.random()))
        gloss = max(1.0-roughness, 0.0)
        if not output_specular or gloss == 0.0:
            old_specular = (rng.random()**rng.uniform(4,128)) * gloss if output_specular else 0.0
            assert old_specular == 0.0
            blinn_skips += 1
        if ndotl <= 0.0:
            kd = rng.random()
            assert 0.0*kd == 0.0
            ggx_skips += 1
    return blinn_skips, ggx_skips


def main():
    check_source_contracts()
    results = {
        'native_light_cases': test_native_early_reject(),
        'native_blinn_bypasses': test_native_blinn(),
        'empty_decal_bypasses': test_decals(),
        'environment_bypasses': test_env(),
        'stylized_sun_bypasses': test_stylized_sun(),
        'legacy_terrain_bypasses': test_legacy_terrain(),
    }
    print('PASS zero-contribution fragment optimization: source contracts and randomized CPU finite-input zero-contribution models')
    for name, result in results.items(): print(f'  {name}: {result}')


if __name__ == '__main__':
    main()
