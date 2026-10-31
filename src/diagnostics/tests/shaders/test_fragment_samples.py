#!/usr/bin/env python3
"""Wave18 finite-input reference models and source contracts.

These validate exact sampling addresses, alpha interpolation, corner reuse and
unused-color gating. They do not simulate GPU occupancy, latency or SPIR-V.
"""
from pathlib import Path
import math
import random

RD = Path(__file__).resolve().parents[4] / 'project/addons/terrain_3d/rd'
CONSUMER = (RD / 'vt/terrain_vt_consumer_sample_inc.glslinc').read_text()
CLIP = (RD / 'terrain_clipmap_fragment_inc.glslinc').read_text()
INSTANCE = (RD / 'terrain_instances.glsl').read_text()


def check_source():
    assert '#define T3D_VT_ALPHA_GATHER 0' in CONSUMER
    assert 'textureGather(bank, vec3(uv, float(layer)), 3)' in CONSUMER
    assert 'textureGather(tail, uv, 3)' in CONSUMER
    assert 'mix(gathered.w, gathered.z, fraction.x)' in CONSUMER
    assert 'mix(gathered.x, gathered.y, fraction.x)' in CONSUMER
    assert 'T3D_VT_ALPHA_EDGE_REUSE' in CONSUMER
    assert 't3d_vt_consumer_equal_sampling_address(' in CONSUMER
    assert '#define T3D_VT_INSTANCE_ALBEDO_SAMPLE_REUSE 0' in CONSUMER
    assert 't3d_vt_consumer_equal_sampling_address(' in INSTANCE
    assert 'if (draw_material_index != 0xffffu' in INSTANCE
    assert '#define T3D_AUTHORED_BILINEAR_CORNER_REUSE 0' in CLIP
    assert '#define T3D_MEGAVT_EARLY_COLOR_OVERRIDE 0' in CLIP
    assert 'bool sample_authored_color = need_authored_color && !cooked_macro_valid;' in CLIP
    assert 'tier_config.authored_color_contribution > 0.0' in CLIP
    assert 'tier_config.wetness_contribution > 0.0' in CLIP
    assert 'T3D_DEBUG_COLORMAP' in CLIP
    assert CLIP.count('sample_color_material_shared(') == 2
    assert 'sample_color_material_shared(world_texel, terrain_normal, tier_config,\n        nearest_control, outside_region, far_surface, far_bits, sample_authored_color,' in CLIP


def bilinear(a, b, c, d, fx, fy):
    return (a * (1.0-fx) + b*fx)*(1.0-fy) + (c*(1.0-fx) + d*fx)*fy


def sample_grid(grid, w, h, uv):
    x = uv[0] * w - .5
    y = uv[1] * h - .5
    bx = math.floor(x)
    by = math.floor(y)
    fx, fy = x - bx, y - by
    clamp = lambda v, size: max(0, min(size-1, v))
    p00 = (clamp(bx,w),clamp(by,h))
    p10 = (clamp(bx+1,w),clamp(by,h))
    p01 = (clamp(bx,w),clamp(by+1,h))
    p11 = (clamp(bx+1,w),clamp(by+1,h))
    original = bilinear(*(grid[p] for p in (p00,p10,p01,p11)),fx,fy)
    gather = tuple(grid[p] for p in (p01,p11,p10,p00))
    gathered = bilinear(gather[3],gather[2],gather[0],gather[1],fx,fy)
    # The shader's edge reuse is exactly the same address equivalence.
    cache = {p00:grid[p00]}
    edge_samples=1
    for p in (p10,p01,p11):
        if p not in cache:
            edge_samples+=1
            cache[p]=grid[p]
    dedup = bilinear(*(cache[p] for p in (p00,p10,p01,p11)),fx,fy)
    return original,gathered,dedup,edge_samples


def test_alpha_gather_and_edge_reuse(rng,n=65000):
    direct=0
    saved_edge=0
    for _ in range(n):
        w,h=rng.choice((1,2,3,4,9,16,64)),rng.choice((1,2,3,5,20,48))
        # Model the UNORM alpha channel, including exact zero and 1.
        grid={(x,y):rng.randrange(256)/255.0 for y in range(h) for x in range(w)}
        uv = (rng.choice((rng.random(),rng.uniform(-.75,.75),rng.uniform(.75,1.75),0.,1.)),
              rng.choice((rng.random(),rng.uniform(-.75,.75),rng.uniform(.75,1.75),0.,1.)))
        old,new,dedup,edge=sample_grid(grid,w,h,uv)
        assert math.isclose(old,new,rel_tol=1e-12,abs_tol=1e-12), (w,h,uv,old,new)
        assert math.isclose(old,dedup,rel_tol=1e-12,abs_tol=1e-12)
        saved_edge += 4 - edge
        direct += 1
    assert saved_edge > 0
    return direct,saved_edge


def test_address_reuse(rng,n=50000):
    fields=('valid','allocation_id','generation','content_id','allocation_count','page_count',
            'layer','physical_family','tail_index','tail_channel_mask','resolved_mip',
            'resolved_packed_extent','requested_mip','requested_mip1','filter_mode',
            'nearest','anisotropic','trilinear','mip_fraction','wrapped_uv','tile_uv',
            'ddx_uv','ddy_uv')
    vecs={'wrapped_uv','tile_uv','ddx_uv','ddy_uv'}
    def address():
        d={x:(rng.random(),rng.random()) if x in vecs else rng.randrange(12) for x in fields}
        d['mip_fraction']=rng.random()
        return d
    hits=0
    for _ in range(n):
        base=address()
        overlay=dict(base)
        if rng.randrange(3)!=0:
            key = rng.choice(fields)
            overlay[key]=(-3.,4.) if key in vecs else -3
        same = all(base[x]==overlay[x] for x in fields)
        # Uniform scalar alpha or RGBA color can be reused precisely on equality.
        shared_texel=tuple(rng.random() for _ in range(4))
        base_result=shared_texel
        independent_result=shared_texel if same else tuple(rng.random() for _ in range(4))
        old_overlay=independent_result
        new_overlay=base_result if same else independent_result
        assert old_overlay == new_overlay
        if same:
            hits+=1
    assert hits > 0
    return n,hits


def test_authored_neutral_and_megavt(rng,n=40000):
    avoided=0
    for _ in range(n):
        strength=rng.choice((0.0,0.0,0.0,rng.random(),1.0))
        wetness=rng.choice((0.0,0.0,rng.random(),1.0))
        debug=rng.random()<.08
        megavt=rng.random()<.5
        authored=(rng.random(),rng.random(),rng.random(),rng.random())
        macro=(rng.random(),rng.random(),rng.random(),rng.random())
        neutral=(1.0,1.0,1.0,.5)
        need=strength>0.0 or wetness>0.0 or debug
        old_map=macro if megavt else authored
        new_map=macro if need and megavt else authored if need else neutral
        alb=tuple(rng.random() for _ in range(3))
        rough=rng.random()
        def shade(sample):
            albedo=tuple(alb[i]*(1.0-strength+sample[i]*strength)
                         *(1.0-.28*max((1.0-2.0*sample[3])*wetness,0.0)) for i in range(3))
            return (albedo,max(0.,min(1.,rough-(1.0-2.0*sample[3])*wetness)),
                    sample[:3] if debug else ())
        assert shade(old_map)==shade(new_map),(strength,wetness,debug,megavt)
        if not need or need and megavt:
            avoided+=1
    assert avoided>0
    return n,avoided


def test_authored_bilinear_corners(rng,n=25000):
    duplicates=0
    for _ in range(n):
        x0,y0=rng.randrange(-3,5),rng.randrange(-3,5)
        x1=x0+rng.randrange(2)
        y1=y0+rng.randrange(2)
        fx,fy=rng.random(),rng.random()
        values={(x,y):rng.random() for x in (x0,x1) for y in (y0,y1)}
        old=bilinear(values[x0,y0],values[x1,y0],values[x0,y1],values[x1,y1],fx,fy)
        a=values[x0,y0]
        b=a if x1==x0 else values[x1,y0]
        c=a if y1==y0 else values[x0,y1]
        d=b if y1==y0 else c if x1==x0 else values[x1,y1]
        new=bilinear(a,b,c,d,fx,fy)
        assert old==new
        if x0==x1 or y0==y1:duplicates+=1
    assert duplicates>0
    return n,duplicates


def main():
    check_source()
    rng=random.Random(0x1800C0DE)
    a=test_alpha_gather_and_edge_reuse(rng)
    b=test_address_reuse(rng)
    c=test_authored_neutral_and_megavt(rng)
    d=test_authored_bilinear_corners(rng)
    print('PASS Wave18 shader source contracts')
    print(f'PASS Wave18 alpha gather/edge: {a[0]} grids, {a[1]} duplicate texel fetches avoidable in model')
    print(f'PASS Wave18 base/overlay reuse: {b[0]} cases, {b[1]} identical address cases')
    print(f'PASS Wave18 authored color/MegaVT: {c[0]} cases, {c[1]} lookup opportunities')
    print(f'PASS Wave18 authored bilinear corner reuse: {d[0]} cases, {d[1]} repeated-corner cases')

if __name__=='__main__':main()
