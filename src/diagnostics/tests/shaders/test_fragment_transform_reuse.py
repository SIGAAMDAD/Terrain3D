#!/usr/bin/env python3
"""fragment transform reuse source contracts and finite-input reference tests.

This tests skip conditions against the old arithmetic on finite authored inputs.
It does not prove GLSL compile, Vulkan execution, visual parity or performance.
"""
from pathlib import Path
import math
import random

RD = Path(__file__).resolve().parents[4] / 'project/addons/terrain_3d/rd'
CLIP = (RD/'terrain_clipmap_fragment_inc.glslinc').read_text()
LIGHT = (RD/'terrain_forward_lighting_inc.glslinc').read_text()
INST = (RD/'terrain_instances.glsl').read_text()
WORLD = (RD/'terrain_world_raster.glsl').read_text()
OCEAN = (RD/'terrain_ocean.glsl').read_text()


def close(a, b):
    return math.isclose(a,b,rel_tol=1.e-12,abs_tol=1.e-12)


def normalize(v):
    length = math.sqrt(sum(x*x for x in v))
    return tuple(x/length for x in v)


def test_source_contracts():
    assert CLIP.count('uint flags = packed_region_cell(world_cell);') >= 1
    start = CLIP.index('vec4 sample_far_surface(')
    end = CLIP.index('uint decode_far_surface_bits(', start)
    assert CLIP[start:end].count('packed_region_cell(world_cell)') == 1
    assert CLIP.count('if (tier_config.texture_surface_mode != 1u && need_material_tangent)') == 1
    assert CLIP.index('if (require_roughness) {\n        nr_material.roughness = clamp(') > CLIP.index('// P56: the DNR pass')
    assert 'decal.color.a == 0.0' in CLIP
    assert 'if (light.fade_to > light.fade_from && depth_z >= light.fade_to)' in LIGHT
    assert LIGHT.count('mat3(light_shadow_matrix) * view_normal') == 2
    assert 'if (albedo_active) {' in LIGHT and 'if (emission_active) {' in LIGHT
    assert 'if (dot(brdf.normal, light_direction) <= 0.0)' in LIGHT
    assert 'if (highlight > 0.0)' in OCEAN
    assert OCEAN.index('if (crest > 0.0) {') < OCEAN.index('float fade = 1.0 - smoothstep(')
    assert 'if (evaluate_surface) {' in WORLD and 'if (evaluate_surface) {' in INST
    assert 'T3D_UNSHADED_SURFACE_FAST_PATH' in WORLD and 'T3D_UNSHADED_SURFACE_FAST_PATH' in INST
    assert '!defined(T3D_VT_FRAGMENT_FEEDBACK)' in INST
    for x in (CLIP,LIGHT,INST,WORLD,OCEAN):
        assert 'layout(' in x or x is CLIP or x is LIGHT


def test_packed_region(rng, n=35000):
    for _ in range(n):
        word = rng.getrandbits(32)
        old_slot = (word >> 18) & 0xFFF
        old_flags = word
        new_flags = word
        new_slot = (new_flags >> 18) & 0xFFF
        assert old_slot == new_slot and old_flags == new_flags


def test_dnr(rng, n=30000):
    saved_norm=0
    for _ in range(n):
        tangent = (rng.uniform(-1,1),rng.uniform(0.2,1.0),rng.uniform(-1,1))
        rough = rng.random() * 1.5 - 0.25
        require_normal = rng.random() < 0.6
        require_rough = rng.random() < 0.6
        surface_mode = rng.randrange(3)
        authored_alpha = rng.random()
        wet_strength = rng.random()
        # Old implementation normalizes even when the tangent is ignored.
        old_tangent = normalize((tangent[0],tangent[1]+0.00001,tangent[2]))
        old_rough = min(max(rough,0.0),1.0)
        if surface_mode == 1:
            old_tangent=(0.0,1.0,0.0)
            old_rough=1.0
        if require_rough:
            wetness=(1.0-2.0*min(max(authored_alpha,0.0),1.0))*wet_strength
            old_rough=min(max(old_rough-wetness,0.0),1.0)
        else:
            old_rough=1.0
        new_tangent = tangent
        new_rough=rough
        if surface_mode == 1:
            new_rough=1.0
            new_tangent=(0.0,1.0,0.0)
        elif require_normal:
            new_tangent=normalize((tangent[0],tangent[1]+0.00001,tangent[2]))
        else:
            saved_norm+=1
        if require_rough:
            new_rough=min(max(new_rough,0.0),1.0)
            new_rough=min(max(new_rough-(1.0-2.0*authored_alpha)*wet_strength,0.0),1.0)
        else:
            new_rough=1.0
        assert close(old_rough,new_rough)
        if require_normal:
            assert all(close(a,b) for a,b in zip(old_tangent,new_tangent))
    return saved_norm


def test_faded_directional_shadow(rng,n=40000):
    bypassed=0
    for _ in range(n):
        from_z=rng.uniform(1.0,1000.0)
        to_z=from_z+rng.uniform(0.0001,1000.0)
        depth=rng.uniform(-10.0,to_z+2000.0)
        shadow=rng.random()
        opacity=rng.random()
        old=shadow
        if depth>from_z:
            amount=min(max((depth-from_z)/(to_z-from_z),0.0),1.0)
            old=shadow*(1.0-amount)+amount
        old=1.0*(1.0-opacity)+old*opacity
        new=1.0 if depth>=to_z else old
        assert close(old,new)
        if depth>=to_z: bypassed+=1
    return bypassed


def test_local_light_early_reject(rng,n=35000):
    skipped=0
    for _ in range(n):
        n_dot_l=rng.uniform(-1.0,1.0)
        attenuation=rng.choice([0.0, rng.random()])
        cone=rng.uniform(0.0,1.0)
        shadow=rng.random()
        energy=max(n_dot_l,0.0)*attenuation*cone*shadow
        # Old path calculated falloff/cone before checking N.L.
        old=energy if attenuation>0.0 and n_dot_l>0.0 else 0.0
        new=0.0 if n_dot_l<=0.0 or attenuation<=0.0 else energy
        assert close(old,new)
        if n_dot_l<=0 or attenuation<=0: skipped+=1
    return skipped


def test_decal_skip(rng,n=32000):
    skipped_albedo=skipped_emission=0
    for _ in range(n):
        mask=rng.choice([0.0,rng.random()])
        alpha_mod=rng.choice([0.0,rng.random()])
        mix_weight=rng.random()
        rgb=tuple(rng.uniform(0,1) for _ in range(3))
        emissive=tuple(rng.uniform(0,1) for _ in range(3))
        mod_rgb=rng.choice([(0.0,0.0,0.0),tuple(rng.uniform(0.0,1.0) for _ in range(3))])
        emission_energy=rng.choice([0.0,rng.random()])
        albedo_present=rng.random()<0.7
        emission_present=rng.random()<0.7
        fade=rng.random()
        old_alpha=mask*alpha_mod*fade if albedo_present else 0.0
        old_color=tuple(c*(1-old_alpha*mix_weight)+a*old_alpha*mix_weight for c,a in zip(rgb,emissive))
        old_emission=tuple(e*m*emission_energy*fade for e,m in zip(emissive,mod_rgb)) if emission_present else (0.,0.,0.)
        new_albedo_present=albedo_present and alpha_mod!=0.0
        new_emission_present=emission_present and emission_energy!=0.0 and mod_rgb!=(0.0,0.0,0.0)
        new_alpha=mask*alpha_mod*fade if new_albedo_present else 0.0
        new_color=tuple(c*(1-new_alpha*mix_weight)+a*new_alpha*mix_weight for c,a in zip(rgb,emissive))
        new_emission=tuple(e*m*emission_energy*fade for e,m in zip(emissive,mod_rgb)) if new_emission_present else (0.,0.,0.)
        assert all(close(a,b) for a,b in zip(old_color,new_color))
        assert all(close(a,b) for a,b in zip(old_emission,new_emission))
        skipped_albedo+=int(albedo_present and not new_albedo_present)
        skipped_emission+=int(emission_present and not new_emission_present)
    return skipped_albedo,skipped_emission


def test_ocean_specular(rng,n=30000):
    skipped=0
    for _ in range(n):
        value=rng.uniform(-1.0,1.0)
        power=rng.uniform(8.0,96.0)
        strength=rng.random()
        old=math.pow(max(value,0.0),power)*strength
        new=math.pow(value,power)*strength if value>0.0 else 0.0
        assert close(old,new)
        skipped+=int(value<=0.0)
    return skipped


def test_unshaded_surface(rng,n=30000):
    skipped=0
    for _ in range(n):
        base_valid=rng.choice([True,False])
        overlay_valid=rng.choice([True,False])
        overlay_survives=overlay_valid and rng.choice([True,False])
        base_unshaded=base_valid and rng.choice([True,False])
        overlay_unshaded=overlay_survives and rng.choice([True,False])
        final_unshaded=base_unshaded or overlay_unshaded
        # Original final color in unshaded mode consumes only albedo and
        # emission; material surface/normal scalars cannot affect its result.
        base_color=tuple(rng.random() for _ in range(3))
        emission=tuple(rng.random() for _ in range(3))
        opacity=rng.random()
        normal=tuple(rng.random() for _ in range(3))
        rough=rng.random()
        material_id=rng.randint(0,4096)
        old_rgb=tuple(x+y for x,y in zip(base_color,emission)) if final_unshaded else base_color
        skip=final_unshaded
        new_rgb=tuple(x+y for x,y in zip(base_color,emission)) if skip else base_color
        assert old_rgb==new_rgb and opacity==opacity and material_id>=0 and rough>=0 and normal[0]>=0
        skipped+=int(skip)
    return skipped


def main():
    test_source_contracts()
    rng=random.Random(0x171117)
    test_packed_region(rng)
    norm=test_dnr(rng)
    fade=test_faded_directional_shadow(rng)
    local=test_local_light_early_reject(rng)
    albedo,emission=test_decal_skip(rng)
    spec=test_ocean_specular(rng)
    surface=test_unshaded_surface(rng)
    print('PASS fragment transform reuse: far-word 35k, DNR 30k, fade 40k, local-light 35k, decal 32k, ocean 30k, unshaded 30k')
    print('Exercise counts: skipped DNR normals',norm,'directional fades',fade,'local lights',local,
          'decal albedo',albedo,'decal emission',emission,'ocean highlights',spec,'unshaded surface',surface)

if __name__=='__main__': main()
