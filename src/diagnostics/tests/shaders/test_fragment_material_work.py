#!/usr/bin/env python3
"""fragment material optimization source contracts and deterministic CPU reference models.

Not a substitute for glslang/SPIR-V, live image captures or Vulkan GPU timing.
Run from any working directory: python test_fragment_material_work.py
"""
from __future__ import annotations

import math
from pathlib import Path
import random
import re

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
CLIP = (RD / 'terrain_clipmap_fragment_inc.glslinc').read_text()
CONSUMER = (RD / 'vt/terrain_vt_consumer_sample_inc.glslinc').read_text()


def check_source_contracts():
    assert 'vec2 ddx_uv = dFdx(uv), ddy_uv = dFdy(uv);' not in CLIP
    assert 'vec2 ddx_uv = world_ddx_xz * record.megavt_world_to_uv.xy;' in CLIP
    assert 'vec2 ddy_uv = world_ddy_xz * record.megavt_world_to_uv.xy;' in CLIP
    assert CLIP.count('sample_megavt_macro(cooked_macro, ') == 3  # fragment sample reuse: early override + fallback are mutually exclusive
    assert '#define T3D_COLOR_TWO_KEY_SPECIALIZATION 0' in CLIP
    assert '#define T3D_DNR_SOURCE_REUSE 0' in CLIP
    assert CLIP.count('if (bitCount(representatives) == 2)') == 1
    assert 'if (identity == reuse.key0) return reuse.sample0;' in CLIP
    assert 'if (identity == reuse.key1) return reuse.sample1;' in CLIP
    assert '#if !T3D_DNR_SOURCE_REUSE || defined(T3D_VT_TERRAIN_FRAGMENT_FEEDBACK_ACTIVE)' in CLIP
    assert '#if !T3D_FRAGMENT_SOURCE_REUSE || defined(T3D_VT_TERRAIN_FRAGMENT_FEEDBACK_ACTIVE)' in CLIP
    assert CLIP.count('fwidth(') == 1
    early = CLIP.index('instancer_grid_footprint = fwidth(grid_distance);')
    discard = CLIP.index('if (outside_region && material_config.background_mode == 0u')
    assert early < discard
    assert CLIP.count('debug_visualization(') == 4  # two declarations, two calls
    for name in ('albedo', 'albedo_alpha', 'normal_v3', 'surface_v3', 'orm_v3'):
        pattern = r'(?:vec4|float) t3d_vt_consumer_sample_' + name + r'\([^)]*\)\s*\{'
        m = re.search(pattern, CONSUMER)
        assert m is not None, name
        body = CONSUMER[m.end():CONSUMER.index('\n}', m.end())]
        assert 'if (pair.blend == 0u) return' in body, name
        assert body.index('if (pair.blend == 0u) return') < body.index('pair.other)'), name


def key(base, overlay, angle, scale):
    return (base << 27) | (overlay << 22) | (angle << 10) | (scale << 7)


def unpack(k):
    return ((k >> 27) & 31, (k >> 22) & 31)


def identity(material_id, k):
    return (material_id & 31) | (((k >> 7) & 127) << 5)


def source_value(material_id, k):
    # Stable synthetic height/roughness/normal; identical identities must
    # produce identical data independently of base-vs-overlay use.
    seed = identity(material_id, k)
    return (
        ((seed * 127 + 137) % 65521) / 65521.0,
        ((seed * 409 + 11) % 65519) / 65519.0,
        ((seed * 853 + 17) % 65497) / 65497.0,
        ((seed * 199 + 43) % 65521) / 65521.0,
    )


class Reuse:
    def __init__(self, enabled, events):
        self.enabled = enabled
        self.events = events
        self.keys = [None, None]
        self.values = [None, None]
        self.slot = 0

    def fetch(self, material_id, k):
        ident = identity(material_id, k)
        if self.enabled:
            for i in range(2):
                if self.keys[i] == ident:
                    return self.values[i]
        self.events.append((material_id, k))
        value = source_value(material_id, k)
        if self.enabled:
            self.keys[self.slot] = ident
            self.values[self.slot] = value
            self.slot ^= 1
        return value


def mix4(a, b, t):
    return tuple(x * (1.0 - t) + y * t for x, y in zip(a, b))


def add4(a, b):
    return tuple(x + y for x, y in zip(a, b))


def mul4(a, amount):
    return tuple(x * amount for x in a)


def evaluate_pair(k, blend, reuse):
    base_id, overlay_id = unpack(k)
    a = reuse.fetch(base_id, k)
    if base_id == overlay_id:
        return a
    b = reuse.fetch(overlay_id, k)
    # Height-aware, nontrivial deterministic blend to exercise reused data.
    height_blend = max(0.0, min(1.0, blend + (b[3] - a[3]) * 0.30))
    return mix4(a, b, height_blend)


def representatives(keys, weights):
    return [i for i in range(4) if weights[i] > 0.0
            and all(weights[j] <= 0.0 or keys[i] != keys[j] for j in range(i))]


def evaluate_grouped(keys, weights, blends, cache_enabled, specialized_two_keys=False):
    events = []
    cache = Reuse(cache_enabled, events)
    reps = representatives(keys, weights)
    if specialized_two_keys and len(reps) == 2:
        # findLSB(representatives) and findLSB(representatives & (representatives-1))
        mask = sum(1 << i for i in reps)
        first = (mask & -mask).bit_length() - 1
        second_mask = mask & (mask - 1)
        second = (second_mask & -second_mask).bit_length() - 1
        selected = [first, second]
    else:
        selected = reps
    result = (0.0, 0.0, 0.0, 0.0)
    for i in selected:
        k = keys[i]
        # Pair values are sampled only once per representative key.
        a_id, b_id = unpack(k)
        a = cache.fetch(a_id, k)
        b = a if a_id == b_id else cache.fetch(b_id, k)
        for j in range(4):
            if weights[j] <= 0.0 or keys[j] != k:
                continue
            blend = blends[j]
            amount = blend if a_id == b_id else max(0.0, min(1.0, blend + (b[3] - a[3]) * 0.30))
            material = a if a_id == b_id else mix4(a, b, amount)
            result = add4(result, mul4(material, weights[j]))
    return result, events


def validate_material_order_and_cache():
    rng = random.Random(0xD13C0DE)
    reference_cases = 9000
    matched_two_key = 0
    cache_savings = 0
    for n in range(reference_cases):
        palette = [key(rng.randrange(16), rng.randrange(16), rng.randrange(16), rng.randrange(8))
                   for _ in range(rng.randrange(1, 5))]
        if n % 3 == 0:
            # Deliberately share base material and transformation across pairs.
            a, b = rng.randrange(16), rng.randrange(16)
            ang, scale = rng.randrange(16), rng.randrange(8)
            palette = [key(a, b, ang, scale), key(a, (b + 1) % 16, ang, scale)]
        keys = [rng.choice(palette) for _ in range(4)]
        weights = [rng.random() if rng.randrange(5) else 0.0 for _ in range(4)]
        if all(w == 0.0 for w in weights):
            weights[rng.randrange(4)] = 1.0
        total = sum(weights)
        weights = [w / total for w in weights]
        blends = [rng.random() for _ in range(4)]
        baseline, baseline_events = evaluate_grouped(keys, weights, blends, False)
        # P16 must issue the same logical sampling sequence as the original
        # generic algorithm, with and without existing P02 source cache.
        old_on, old_events = evaluate_grouped(keys, weights, blends, True)
        new_on, new_events = evaluate_grouped(keys, weights, blends, True, True)
        assert baseline == old_on == new_on, n
        assert old_events == new_events, n
        # In a feedback variant caching is disabled, so no VT requests vanish.
        fb_ref, fb_events = evaluate_grouped(keys, weights, blends, False, False)
        fb_new, fb_new_events = evaluate_grouped(keys, weights, blends, False, True)
        assert fb_ref == fb_new and fb_events == fb_new_events, n
        if len(representatives(keys, weights)) == 2:
            matched_two_key += 1
        # P20 DNR reuses sources that identical pairs would evaluate twice.
        dnr_on, dnr_events = evaluate_grouped(keys, weights, blends, True)
        assert dnr_on == baseline, n
        assert len(dnr_events) <= len(baseline_events), n
        cache_savings += len(baseline_events) - len(dnr_events)
    assert matched_two_key > 1000, matched_two_key
    assert cache_savings > 0, cache_savings
    return reference_cases, matched_two_key, cache_savings


def validate_gradient_reuse():
    rng = random.Random(0xA5515)
    for _ in range(2200):
        origin = [rng.uniform(-1e4, 1e4) for _ in range(2)]
        dx = [rng.uniform(-20, 20) for _ in range(2)]
        dy = [rng.uniform(-20, 20) for _ in range(2)]
        scale = [rng.uniform(1e-5, 50.0) for _ in range(2)]
        shift = [rng.uniform(-100, 100) for _ in range(2)]
        uv = [origin[i] * scale[i] + shift[i] for i in range(2)]
        uvx = [(origin[i] + dx[i]) * scale[i] + shift[i] for i in range(2)]
        uvy = [(origin[i] + dy[i]) * scale[i] + shift[i] for i in range(2)]
        for i in range(2):
            tolerance = 3e-9 * max(1.0, abs(uvx[i]), abs(uvy[i]), abs(uv[i]))
            assert abs((uvx[i] - uv[i]) - dx[i] * scale[i]) <= tolerance
            assert abs((uvy[i] - uv[i]) - dy[i] * scale[i]) <= tolerance
    return 2200


def validate_trilinear_selection():
    rng = random.Random(0x7177)
    samples = 0
    for channels in (1, 4):
        for _ in range(1300):
            base = tuple(rng.random() for _ in range(channels))
            other = tuple(rng.random() for _ in range(channels))
            weight = rng.random()
            for blend in (0, 1):
                visits = []
                def fetch_base():
                    visits.append('base')
                    return base
                def fetch_other():
                    visits.append('other')
                    return other
                start = fetch_base()
                output = start if blend == 0 else mix4(start, fetch_other(), weight)
                expected = start if blend == 0 else mix4(start, other, weight)
                assert output == expected
                assert visits == (['base'] if blend == 0 else ['base', 'other'])
                samples += 1
    return samples


def main():
    check_source_contracts()
    n, two, saved = validate_material_order_and_cache()
    gradient_count = validate_gradient_reuse()
    sample_count = validate_trilinear_selection()
    print(f'PASS fragment material optimization: {n} seeded material cases; {two} two-key patterns; {saved} DNR sample evaluations avoided in model')
    print(f'PASS fragment material optimization: {gradient_count} affine UV gradient pairs; {sample_count} trilinear branch cases')
    print('PASS fragment material optimization: GLSL source and opt-in/feedback contracts')


if __name__ == '__main__':
    main()
