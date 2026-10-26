#!/usr/bin/env python3
"""fragment precalculation source contracts and finite-input CPU reference models.

These checks compare skip predicates and CPU scalar equivalents. They do not
validate GLSL semantics, shader hardware instructions, GPU timings, or images.
"""
from pathlib import Path
import math
import random

ROOT = Path(__file__).resolve().parents[4] / 'project/addons/terrain_3d/rd'
CLIP = (ROOT/'terrain_clipmap_fragment_inc.glslinc').read_text()
LIGHT = (ROOT/'terrain_forward_lighting_inc.glslinc').read_text()
INST = (ROOT/'terrain_instances.glsl').read_text()
WORLD = (ROOT/'terrain_world_raster.glsl').read_text()
OCEAN = (ROOT/'terrain_ocean.glsl').read_text()


def check_source_contracts():
    for channel in ('albedo', 'normal', 'surface', 'height', 'pbr'):
        start = CLIP.index(f'terrain_vt_sample_{channel}(TerrainVTResolvedAddress address)')
        end = CLIP.index('\n}', start)
        part = CLIP[start:end]
        assert part.index('if (pair.blend == 0u) return base_sample;') < part.index(f'terrain_vt_sample_{channel}_single(pair.other)')
        assert 'return mix(base_sample, other_sample, pair.other_weight);' in part
    assert 'bool sample1 = any(notEqual(macro1, vec3(1.0)));' in CLIP
    assert 'bool sample2 = any(notEqual(macro2, vec3(1.0)));' in CLIP
    assert CLIP.index('if (!sample1 && !sample2) return albedo;') < CLIP.index('float noise1 =')
    assert 'if (sample1) {' in CLIP and 'if (sample2) {' in CLIP
    assert 'if (nd == 0.0) return 0.0;' in LIGHT
    assert LIGHT.index('if (nd == 0.0) return 0.0;') < LIGHT.index('return nd * pow(')
    assert 'if (ao != 0.0 && clamp(metallic, 0.0, 1.0) < 1.0)' in LIGHT
    assert 'if (probe.exposure_normalization != 0.0) {' in LIGHT
    assert LIGHT.index('accumulated_weight += weight;') > LIGHT.index('if (probe.exposure_normalization != 0.0) {')
    assert 'float ndotv = max(dot(brdf.normal, brdf.view_direction), 0.0);' in LIGHT
    a = LIGHT.index('    // P54: evaluate view-facing eligibility')
    assert LIGHT.index('if (ndotv <= 0.0)',a) < LIGHT.index('vec3 half_sum = light_direction + brdf.view_direction',a)
    assert 'material_alpha' not in LIGHT
    assert WORLD.index('if (base.a < alpha_threshold) discard;') < WORLD.index('float normal_scale = unpackHalf2x16(')
    assert 'if ((material.flags & T3D_VT_MATERIAL_ALPHA_SCISSOR) != 0u) {' in INST
    assert 'if ((overlay.flags & T3D_VT_MATERIAL_ALPHA_SCISSOR) != 0u) {' in INST
    assert 'if (frame_data.wave_params.w > 0.0)' in OCEAN
    assert 'if (shading_mode > 0 && frame_data.surface_params.y > 0.0)' in OCEAN
    assert 'if (scatter_mode > 0 && frame_data.foam_params0.w != 0.0)' in OCEAN
    assert OCEAN.index('float crest = clamp(') < OCEAN.index('float n = noise21(',OCEAN.index('    if (foam_mode > 0)'))
    # The complete floating, coverage, VT feedback, and resource contracts remain.
    for source in (CLIP, INST, WORLD):
        assert 'T3D_VT_FRAGMENT_FEEDBACK' in source


def close(a,b):
    return math.isclose(a,b,rel_tol=2e-13,abs_tol=2e-13)


def test_terrain_trilinear(seed=0x1647,count=25000):
    rng=random.Random(seed)
    skipped=0
    for _ in range(count):
        base=rng.uniform(-4,4)
        other=rng.uniform(-4,4)
        blend=rng.choice((0,0,0,1))
        w=rng.random()
        old=(base*(1-w)+other*w) if blend else base
        new=base if not blend else base*(1-w)+other*w
        assert close(old,new)
        if not blend: skipped+=1
    return skipped


def test_alpha_scissor(seed=0x1648,count=45000):
    rng=random.Random(seed)
    nonscissored=0
    discarded=0
    for _ in range(count):
        for _layer in range(2):
            alpha=rng.choice((0.0,0.5,1.0,rng.random()))
            threshold=rng.choice((0.0,0.5,1.0,rng.random()))
            enabled=rng.choice((False,False,True))
            old=(not enabled) or alpha>=threshold
            new=True
            if enabled: new=alpha>=threshold
            assert old==new
            if not enabled: nonscissored+=1
            elif not new: discarded+=1
    return nonscissored,discarded


def attenuation(distance,inv_range,decay):
    nd=distance*inv_range
    nd*=nd
    nd*=nd
    nd=max(1.0-nd,0.0)
    return nd*nd*math.pow(max(distance,0.0001),-decay)


def attenuation_fast(distance,inv_range,decay):
    nd=distance*inv_range
    nd*=nd
    nd*=nd
    nd=max(1.0-nd,0.0)
    if nd==0.0: return 0.0
    return nd*nd*math.pow(max(distance,0.0001),-decay)


def test_attenuation(seed=0x1649,count=30000):
    rng=random.Random(seed)
    skip=0
    for _ in range(count):
        d=rng.uniform(0.01,150.0)
        inverse=rng.uniform(0.002,3.0)
        decay=rng.uniform(-2,3)
        a=attenuation(d,inverse,decay)
        b=attenuation_fast(d,inverse,decay)
        assert close(a,b)
        if d*inverse>=1: skip+=1
    return skip


def test_ambient(seed=0x1650,count=35000):
    rng=random.Random(seed)
    skips=0
    for _ in range(count):
        a=rng.uniform(0,2)
        albedo=rng.uniform(0,1)
        metallic=rng.choice((0,1,rng.uniform(-0.5,1.5)))
        ao=rng.choice((0,rng.random()))
        metal=max(0,min(1,metallic))
        old=a*albedo*(1-metal)*ao
        new=0.0
        if ao!=0 and metal<1:
            new=a*albedo*(1-metal)*ao
        else: skips+=1
        assert close(old,new)
    return skips


def test_reflection_probe_weights(seed=0x1651,count=15000):
    rng=random.Random(seed)
    missing_samples=0
    for _ in range(count):
        probes=[(rng.random(),rng.choice((0,0,0,rng.random())),rng.random(),rng.random()) for _ in range(rng.randint(1,8))]
        old=0.0
        new=0.0
        w_old=w_new=0.0
        for weight,exposure,intensity,texture_value in probes:
            if w_old>=0.999: break
            dw=max(0,weight-w_old)
            if dw<=0:continue
            old+=texture_value*exposure*intensity*dw
            w_old+=dw
        for weight,exposure,intensity,texture_value in probes:
            if w_new>=0.999:break
            dw=max(0,weight-w_new)
            if dw<=0:continue
            if exposure!=0:
                new+=texture_value*exposure*intensity*dw
            else:missing_samples+=1
            w_new+=dw
        assert close(old,new) and w_old==w_new
    return missing_samples


def test_macro_neutral(seed=0x1652,count=20000):
    rng=random.Random(seed)
    skips=0
    for _ in range(count):
        m1=rng.choice((1.0,1.0,rng.random()*2))
        m2=rng.choice((1.0,1.0,rng.random()*2))
        n1=rng.random()
        n2=rng.random()
        influence=rng.random()
        albedo=rng.random()
        old1=m1*(1-n1)+n1
        old2=m2*(1-n2)+n2
        old=albedo*((1-influence)+(old1*old2)*influence)
        do1=m1!=1
        do2=m2!=1
        if not do1 and not do2:
            new=albedo
        else:
            a=m1*(1-n1)+n1 if do1 else 1.0
            b=m2*(1-n2)+n2 if do2 else 1.0
            new=albedo*((1-influence)+(a*b)*influence)
        assert close(old,new)
        if not do1:skips+=1
        if not do2:skips+=1
    return skips


def test_ocean_zero_work(seed=0x1653,count=30000):
    rng=random.Random(seed)
    skipped_fresnel=skipped_highlight=skipped_noise=0
    for _ in range(count):
        amplitude=rng.choice((0.0,-1.0,rng.random()))
        ndotv=rng.uniform(-1,1)
        power=rng.uniform(0.01,4)
        old=max(0,min(1,amplitude*math.pow(max(1-ndotv,0.0),power)))
        new=0.0 if amplitude<=0 else max(0,min(1,amplitude*math.pow(max(1-ndotv,0.0),power)))
        assert close(old,new)
        if amplitude<=0:skipped_fresnel+=1
        specular_strength=rng.choice((0,-1,rng.random()))
        old_specular=rng.random()**rng.uniform(8,96)*max(specular_strength,0)
        new_specular=old_specular if specular_strength>0 else 0.0
        assert close(old_specular,new_specular)
        if specular_strength<=0:skipped_highlight+=1
        crest=rng.choice((0.0,rng.random()))
        fade=rng.choice((0.0,rng.random()))
        noise=rng.random()
        old_foam=crest*noise*fade
        new_foam=crest*noise*fade if crest>0 and fade>0 else 0.0
        assert close(old_foam,new_foam)
        if crest==0 or fade==0:skipped_noise+=1
    return skipped_fresnel,skipped_highlight,skipped_noise


def main():
    check_source_contracts()
    results={
        'terrain_trilinear':test_terrain_trilinear(),
        'alpha_scissor':test_alpha_scissor(),
        'attenuation':test_attenuation(),
        'ambient':test_ambient(),
        'reflection_probes':test_reflection_probe_weights(),
        'macro_neutral':test_macro_neutral(),
        'ocean_zero_work':test_ocean_zero_work(),
    }
    print('PASS fragment precalculation: source contracts and finite-input CPU reference models')
    for name,value in results.items():print(' ',name,value)

if __name__=='__main__':main()
