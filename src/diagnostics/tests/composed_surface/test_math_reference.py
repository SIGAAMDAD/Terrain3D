#!/usr/bin/env python3
"""CPU golden-value checks for GLSL shared control and height-blend algebra."""
import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
COMMON = (ROOT / 'project/addons/terrain_3d/rd/terrain_composed_material_math.glslinc').read_text()


def height_blend(blend, base_h, overlay_h, sharpness):
    b = math.pow(max(1.0 - blend + base_h, 1e-8), sharpness)
    o = math.pow(max(blend + overlay_h, 1e-8), sharpness)
    return o * (1.0 / max(b + o, 1e-5))


def encode_control(base, overlay, blend, angle=0, scale=0):
    return ((base & 31) << 27) | ((overlay & 31) << 22) | ((blend & 255) << 14) | ((angle & 15) << 10) | ((scale & 7) << 7)


def corner_weights(x, y):
    return [(1-x)*(1-y), x*(1-y), (1-x)*y, x*y]


def select_top_two(weights):
    order = sorted(range(4), key=lambda i: (-weights[i], i))[:2]
    selected = [w if i in order else 0.0 for i, w in enumerate(weights)]
    denom = max(sum(selected), 1e-5)
    return [w / denom for w in selected]


def normalize_active(weights):
    selected = [x if x > 1e-5 else 0.0 for x in weights]
    denom = max(sum(selected), 1e-5)
    return [x / denom for x in selected]


def blend_pair(base, overlay, b, sharp):
    t = height_blend(b, base[2], overlay[2], sharp)
    return (base[0]*(1-t) + overlay[0]*t,
            base[1]*(1-t) + overlay[1]*t,
            base[2]*(1-t) + overlay[2]*t)


def main():
    required = ['base_weight = pow(max((1.0 - blend) + base_height, 0.00000001), sharpness)',
                'overlay_weight = pow(max(blend + overlay_height, 0.00000001), sharpness)',
                'float pair_weight_inv = 1.0 / max(base_weight + overlay_weight, 0.00001)',
                'return overlay_weight * pair_weight_inv;',
                'weights.x = weights.x > 0.00001 ? weights.x : 0.0',
                'const uint T3D_CONTROL_SAMPLE_KEY_MASK',
                'if (second_weight > first_weight)']
    for line in required:
        assert line in COMMON, f'missing GLSL reference algebra: {line}'
    rng = random.Random(20261015)
    for _ in range(10000):
        b = rng.random()
        h1, h2 = rng.random(), rng.random()
        sharp = rng.uniform(4, 64)
        t = height_blend(b, h1, h2, sharp)
        assert 0 <= t <= 1
        mirror = height_blend(1-b, h2, h1, sharp)
        assert t + mirror <= 1.0 + 1e-9
        if (max(1.0 - b + h1, 1e-8)**sharp + max(b + h2, 1e-8)**sharp) >= 1e-5:
            assert math.isclose(t, 1.0 - mirror, abs_tol=1e-9)
        samples = [(rng.random(), rng.random(), rng.random()) for _ in range(8)]
        controls = [encode_control(rng.randrange(8), rng.randrange(8), rng.randrange(256)) for _ in range(4)]
        x,y = rng.random(), rng.random()
        weights = normalize_active(corner_weights(x,y))
        assert abs(sum(weights)-1) < 1e-10
        vals = []
        for c in controls:
            vals.append(blend_pair(samples[(c >> 27) & 31], samples[(c >> 22) & 31],
                                   ((c >> 14) & 255)/255, sharp))
        aggregate = [sum(v[k]*weights[i] for i,v in enumerate(vals)) for k in range(3)]
        if len(set(controls)) == 1:
            assert all(math.isclose(v, vals[0][i], abs_tol=1e-9) for i,v in enumerate(aggregate))
        top = select_top_two(corner_weights(x,y))
        assert sum(w > 0 for w in top) <= 2
        assert abs(sum(top)-1) < 1e-10
    # Sampling-semantics control: encoded-UNORM midpoint vs linear midpoint.
    half_encoded_then_decode = ((0.5 + 0.055)/1.055)**2.4
    assert abs(half_encoded_then_decode - 0.21404114048223255) < 1e-10
    print('CS02: 10,000 seeded control/height/weight reference cases: PASS')


if __name__ == '__main__':
    main()
