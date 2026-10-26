#!/usr/bin/env python3
"""P25/P26 pure-CPU reference tests for portable 64-lane compaction.

These checks cover set identity, capacity clipping, bucket statistics and scan
ranks. Hardware shader parity and GPU execution timings remain separate gates.
"""

import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
BUCKETS = 7
LANES = 64


def local_prefix_reference(visible):
    result = []
    for i, is_visible in enumerate(visible):
        if is_visible:
            result.append((i, sum(visible[:i])))
    return result, sum(visible)


def local_prefix_portable(visible):
    masks = [sum(int(visible[o * 8 + lane]) << lane for lane in range(8)) for o in range(8)]
    bases = []
    count = 0
    for mask in masks:
        bases.append(count)
        count += mask.bit_count()
    result = []
    for index, is_visible in enumerate(visible):
        if is_visible:
            octet = index >> 3
            rank = bases[octet] + (masks[octet] & ((1 << (index & 7)) - 1)).bit_count()
            result.append((index, rank))
    return result, count


def grouped_reference(entries, counts, caps):
    """Emulate sequential lane append and collect surviving records/counters."""
    output = [[] for _ in range(BUCKETS)]
    useful = mesh = hlod = static = 0
    overflow = False
    for bucket, kind, vertices, identifier in entries:
        if bucket == 7:
            continue
        index = counts[bucket]
        counts[bucket] += 1
        if index >= caps[bucket]:
            overflow = True
            continue
        output[bucket].append((identifier, kind, vertices))
        if bucket < 6:
            useful += vertices
        elif kind == 1:
            mesh += 1
        elif kind == 2:
            hlod += 1
        elif kind == 3:
            static += 1
    return output, counts, (useful, mesh, hlod, static), overflow


def grouped_batched(entries, counts, caps):
    output = [[] for _ in range(BUCKETS)]
    useful = mesh = hlod = static = 0
    overflow = False
    for bucket in range(BUCKETS):
        lane_entries = [entry for entry in entries if entry[0] == bucket]
        count = len(lane_entries)
        if not count:
            continue
        base = counts[bucket]
        counts[bucket] += count
        accepted = min(count, max(0, caps[bucket] - base))
        overflow |= accepted != count
        for entry in lane_entries[:accepted]:
            _, kind, vertices, identifier = entry
            output[bucket].append((identifier, kind, vertices))
            if bucket < 6:
                useful += vertices
            elif kind == 1:
                mesh += 1
            elif kind == 2:
                hlod += 1
            elif kind == 3:
                static += 1
    return output, counts, (useful, mesh, hlod, static), overflow


def test_instance_prefix():
    rng = random.Random(0x25C011)
    for active in (0, 1, 2, 8, 16, 31, 32, 33, 63, 64, 65, 127, 128, 129):
        for density in (0, 0.03125, 0.25, 0.5, 0.99, 1.0):
            indices = [i for i in range(active) if rng.random() < density]
            visible = set(indices)
            observed = []
            for chunk in range(0, active, LANES):
                flags = [(chunk + i) < active and (chunk + i) in visible for i in range(LANES)]
                before, old_count = local_prefix_reference(flags)
                after, new_count = local_prefix_portable(flags)
                assert before == after
                assert old_count == new_count
                observed.extend(chunk + i for i, _ in after)
            assert observed == sorted(visible)
    for _ in range(5000):
        flags = [bool(rng.getrandbits(1)) for _ in range(LANES)]
        assert local_prefix_reference(flags) == local_prefix_portable(flags)


def test_world_buckets():
    rng = random.Random(0x26A7)
    for size in (0, 1, 16, 32, 33, 63, 64, 65, 127, 128, 129, 255, 511):
        for density in (0.0, 0.05, 0.3, 0.65, 1.0):
            entries = []
            for i in range(size):
                bucket = rng.randrange(BUCKETS) if rng.random() < density else 7
                kind = 0 if bucket < 6 else rng.randrange(1, 4)
                entries.append((bucket, kind, rng.randrange(1, 373), i))
            capacities = [rng.randrange(0, size + 2) for _ in range(BUCKETS)]
            old_count = [rng.randrange(0, 5) for _ in range(BUCKETS)]
            # Model each 64-lane dispatch independently, then compare the
            # complete accepted set and per-kind counters across chunks.
            old_out = [[] for _ in range(BUCKETS)]
            new_out = [[] for _ in range(BUCKETS)]
            old_stats = new_stats = (0, 0, 0, 0)
            old_overflow = new_overflow = False
            new_count = list(old_count)
            for start in range(0, size, LANES):
                chunk = entries[start:start + LANES]
                out, old_count, stats, over = grouped_reference(chunk, old_count, capacities)
                opt, new_count, opt_stats, opt_over = grouped_batched(chunk, new_count, capacities)
                assert out == opt  # one simulated group, no concurrent scheduling
                assert old_count == new_count
                for b in range(BUCKETS):
                    old_out[b].extend(out[b])
                    new_out[b].extend(opt[b])
                old_stats = tuple(a + b for a, b in zip(old_stats, stats))
                new_stats = tuple(a + b for a, b in zip(new_stats, opt_stats))
                old_overflow |= over
                new_overflow |= opt_over
            assert old_out == new_out
            assert old_stats == new_stats
            assert old_overflow == new_overflow

    for _ in range(2000):
        count = rng.randrange(LANES + 1)
        entries = []
        for i in range(count):
            b = rng.randrange(8)
            entries.append((b, 0 if b < 6 else rng.randrange(1, 4), rng.randrange(373), i))
        caps = [rng.randrange(65) for _ in range(BUCKETS)]
        start = [rng.randrange(25) for _ in range(BUCKETS)]
        assert grouped_reference(entries, list(start), caps) == grouped_batched(entries, list(start), caps)


def test_source_gates():
    instances = (RD / 'terrain_instance_cull.glsl').read_text()
    world = (RD / 'terrain_world_visibility.glsl').read_text()
    assert 'grouped = "#define T3D_INSTANCE_CULL_PREFIX 1";' in instances
    assert 'shared_octet_masks[8]' in instances
    assert 'uint(bitCount(shared_octet_masks[octet] & lower_mask))' in instances
    assert 'cull_grouped = "#define T3D_WORLD_VIS_CULL' in world
    assert 'group_rank[lane] = count++;' in world
    assert 'atomicAdd(counter_storage[counter_base + counter], count)' in world
    assert 'uint admitted = reservation < capacity ? min(count, capacity - reservation) : 0u;' in world
    assert '#else\nvoid main()' in world
    shader_ids = (ROOT / 'src/Terrain3DEnums.h').read_text()
    catalog = (ROOT / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
    lib = (ROOT / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
    instance_cpp = (ROOT / 'src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp').read_text()
    world_cpp = (ROOT / 'src/rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.cpp').read_text()
    assert 'T3D_RD_SHADER_WORLD_CS_END' in shader_ids
    assert 'T3D_RD_SHADER_INSTANCE_CULL_GROUPED' in catalog
    assert 'T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED' in catalog
    assert 'TERRAIN3D_GPU_GROUPED_CULL' in lib
    assert 'isGroupedCullEnabledCold()' in instance_cpp and 'isGroupedCullEnabledCold()' in world_cpp
    assert 'push.m_nFlags = bHiZValid ? 1u : 0u;' in world_cpp


def main():
    test_instance_prefix()
    test_world_buckets()
    test_source_gates()
    print('P25/P26 portable workgroup prefix and grouped reserve: PASS')


if __name__ == '__main__':
    main()
