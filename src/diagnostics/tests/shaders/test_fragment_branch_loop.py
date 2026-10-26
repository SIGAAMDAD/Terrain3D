#!/usr/bin/env python3
"""BR01-BR05 offline finite-domain/control-flow equivalence and safety checks.

This is NOT a Vulkan rendering or GPU timing test.
"""
from pathlib import Path
import random
import re

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'


def representatives(keys, weights):
    mask = 0
    for i in range(4):
        if weights[i] > 0 and all(weights[j] <= 0 or keys[j] != keys[i] for j in range(i)):
            mask |= 1 << i
    return mask


def old_control_events(keys, overlays, weights):
    mask = representatives(keys, weights)
    events = []
    for corner in range(4):
        if not mask & (1 << corner):
            continue
        pair = overlays[corner]
        events.append(('base', corner, pair[0]))
        if pair[1] != pair[0]:
            events.append(('overlay', corner, pair[1]))
        for position in range(4):
            if weights[position] > 0 and keys[position] == keys[corner]:
                events.append(('accumulate', position, keys[corner]))
    return events


def new_control_events(keys, overlays, weights):
    remaining = representatives(keys, weights)
    events = []
    while remaining:
        low_bit = remaining & -remaining
        corner = low_bit.bit_length() - 1
        remaining &= remaining - 1
        base, overlay = overlays[corner]
        events.append(('base', corner, base))
        if overlay != base:
            events.append(('overlay', corner, overlay))
        for position in range(4):
            if weights[position] > 0 and keys[position] == keys[corner]:
                events.append(('accumulate', position, keys[corner]))
    return events


def old_taps(values, count):
    accum = 0.0
    for index in range(4):
        if index >= count:
            break
        accum += values[index]
    return accum / max(count, 1)


def new_taps(values, count):
    accum = 0.0
    accum += values[0]
    if count > 1:
        accum += values[1]
        if count > 2:
            accum += values[2]
            accum += values[3]
    return accum / max(count, 1)


def main():
    randomizer = random.Random(190019)
    for _ in range(65000):
        keys = [randomizer.randrange(8) for _ in range(4)]
        weights = [randomizer.choice((0.0, 0.25, 0.5, 0.75, 1.0)) for _ in range(4)]
        if not any(weights):
            weights[0] = 1.0
        overlays = [(randomizer.randrange(8), randomizer.randrange(8)) for _ in range(4)]
        assert old_control_events(keys, overlays, weights) == new_control_events(keys, overlays, weights)
    for _ in range(45000):
        taps = [randomizer.uniform(-100, 100) for _ in range(4)]
        count = randomizer.choice((1, 2, 4))
        assert old_taps(taps, count) == new_taps(taps, count)
    frag = (RD / 'terrain_clipmap_fragment_inc.glslinc').read_text()
    consumer = (RD / 'vt/terrain_vt_consumer_sample_inc.glslinc').read_text()
    lighting = (RD / 'terrain_forward_lighting_inc.glslinc').read_text()
    assert 'remaining_representatives &= remaining_representatives - 1u;' in frag
    assert 'uint corner = uint(findLSB(remaining_representatives));' in frag
    assert 't3d_sample_color_pair_reusing(' in frag
    assert consumer.count('#if T3D_VT_FIXED_FILTER_TAPS') == 18
    assert frag.count('#if T3D_VT_FIXED_FILTER_TAPS') == 4
    assert frag.count('for (int octave = 0; octave < 15; ++octave)') == 1
    assert all(x in lighting for x in ('t3d_light_response_lambert(', 't3d_light_response_blinn(',
                                       't3d_light_response_ggx('))
    assert 'layout(constant_id = 29)' in frag and 'layout(constant_id = 30)' in frag
    host = (ROOT / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()
    assert 'T3D_RD_DEBUG_HEIGHTMAP' in host and 'T3D_RD_DEBUG_CONTROL_TEXTURE' in host
    assert 'm_nEditorDiagnosticClass == rOther.m_nEditorDiagnosticClass' in (
        ROOT / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h').read_text()
    native = (ROOT / 'src/rendering/material/terrain_3d_material.cpp').read_text()
    assert '( !bColorPass &&' in native  # one-sun editor color is also a color pass
    constants = (ROOT / 'src/Terrain3DConstants.h').read_text()
    assert '#define T3D_ENABLE_COLD_MATERIAL_CLASSES ( 0 )' in constants
    assert 'if ( flags == T3D_RD_DEBUG_HEIGHTMAP )' in host
    assert 'if ( flags == T3D_RD_DEBUG_CONTROL_TEXTURE )' in host
    assert 'm_nWorldNoiseCacheEnabled != 0u' in host
    assert 'T3D_RD_MATERIAL_FEATURE_WORLD_NOISE_CACHE' in host
    print('PASS BR01-05: 65,000 active-key/source-trace + 45,000 fixed-tap cases; contract checks')


if __name__ == '__main__':
    main()
