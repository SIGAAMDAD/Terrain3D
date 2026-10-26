#!/usr/bin/env python3
"""Wave 4 P24b/P28a deterministic world-visibility reference tests.

CPU checks model the complete 20-word counter ABI, bucket slots, rejection
accounting and overflow semantics, including the <=64-candidate single-owner
path. GPU execution and cross-workgroup scheduling require Vulkan captures.
"""
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BUCKETS = 7
LANES = 64
# Counter offsets from rd/terrain_world_contract.glslinc.
VISIBLE, CANDIDATE, FRUSTUM, LOD, CONE, HIZ, OVERFLOW = range(7)
TERRAIN, MESH, HLOD, STATIC, NONTERRAIN = range(7, 12)
BUCKET_BASE, USEFUL, SUBMITTED = 12, 18, 19
REJECT_COUNT = (LOD, CONE, FRUSTUM, HIZ)


def model_serial(entries, capacity):
    """Serial lane-order reference for one or multiple 64-thread groups."""
    counters = [0] * 20
    counters[CANDIDATE] = len(entries)
    output = [[] for _ in range(BUCKETS)]
    for group_begin in range(0, len(entries), LANES):
        for bucket, kind, vertices, rejection, ident in entries[group_begin:group_begin + LANES]:
            if rejection < 4:
                counters[REJECT_COUNT[rejection]] += 1
            if bucket >= BUCKETS:
                continue
            slot = BUCKET_BASE + bucket if bucket < 6 else NONTERRAIN
            index = counters[slot]
            counters[slot] += 1
            if index >= capacity[bucket]:
                counters[OVERFLOW] = 1
                continue
            output[bucket].append(ident)
            if bucket < 6:
                counters[USEFUL] += vertices
            elif kind == 1:
                counters[MESH] += 1
            elif kind == 2:
                counters[HLOD] += 1
            elif kind == 3:
                counters[STATIC] += 1
    counters[TERRAIN] = sum(min(counters[BUCKET_BASE + b], capacity[b]) for b in range(6))
    counters[VISIBLE] = counters[TERRAIN] + min(counters[NONTERRAIN], capacity[6])
    # The static submitted-vertex metric depends on bucket-specific strip
    # vertices; tests use explicit synthetic padded bucket counts.
    counters[SUBMITTED] = sum(min(counters[BUCKET_BASE + b], capacity[b]) * (b * 16 + 24)
                              for b in range(6))
    return counters, output


def model_grouped(entries, capacity):
    """Seven bucket leaders, four rejection leaders, one vertex-sum leader."""
    counters = [0] * 20
    counters[CANDIDATE] = len(entries)
    output = [[] for _ in range(BUCKETS)]
    for offset in range(0, len(entries), LANES):
        group = entries[offset:offset + LANES]
        # P24b: rejection classification carries one exclusive reason per lane.
        for r, ctr in enumerate(REJECT_COUNT):
            count = sum(1 for x in group if x[3] == r)
            counters[ctr] += count
        group_terrain_vertices = [0] * 6
        for bucket in range(BUCKETS):
            eligible = [x for x in group if x[0] == bucket]
            slot = BUCKET_BASE + bucket if bucket < 6 else NONTERRAIN
            start = counters[slot]
            counters[slot] += len(eligible)
            admitted = min(len(eligible), max(0, capacity[bucket] - start))
            counters[OVERFLOW] |= int(admitted != len(eligible))
            for _, kind, vertices, _, ident in eligible[:admitted]:
                output[bucket].append(ident)
                if bucket < 6:
                    group_terrain_vertices[bucket] += vertices
                elif kind == 1:
                    counters[MESH] += 1
                elif kind == 2:
                    counters[HLOD] += 1
                elif kind == 3:
                    counters[STATIC] += 1
        counters[USEFUL] += sum(group_terrain_vertices)
    counters[TERRAIN] = sum(min(counters[BUCKET_BASE + b], capacity[b]) for b in range(6))
    counters[VISIBLE] = counters[TERRAIN] + min(counters[NONTERRAIN], capacity[6])
    counters[SUBMITTED] = sum(min(counters[BUCKET_BASE + b], capacity[b]) * (b * 16 + 24)
                              for b in range(6))
    return counters, output


def test_parity():
    rng = random.Random(0x02824B)
    cases = 0
    for size in (0, 1, 2, 6, 7, 11, 12, 31, 32, 63, 64, 65, 66, 127, 128, 129, 255, 512):
        for _ in range(150):
            capacities = [rng.randrange(size + 3) for _ in range(BUCKETS)]
            entries = []
            for i in range(size):
                bucket = rng.choice((0, 1, 2, 3, 4, 5, 6, 7, 7, 7))
                reject = rng.randrange(5) if bucket == 7 else 4
                kind = 0 if bucket < 6 else rng.randrange(1, 4)
                entries.append((bucket, kind, rng.randrange(1, 800), reject, i))
            assert model_serial(entries, capacities) == model_grouped(entries, capacities), (size, capacities)
            cases += 1
    print('Wave4 world counters + compacted records:', cases, 'CPU parity cases PASS')


def test_source_contracts():
    shader = (ROOT / 'project/addons/terrain_3d/rd/terrain_world_visibility.glsl').read_text()
    cpp = (ROOT / 'src/rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.cpp').read_text()
    catalog = (ROOT / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
    lib = (ROOT / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
    assert 'cull_small = "#define T3D_WORLD_VIS_CULL' in shader
    assert 'group_rejection[local_index] = rejection' in shader
    assert 'group_terrain_vertex_sums[local_index]' in shader
    assert 'counter_storage[counter_base + counter] = count;' in shader
    assert 'T3D_RD_SHADER_WORLD_VISIBILITY_CULL_SMALL' in catalog
    assert 'TERRAIN3D_GPU_WORLD_SMALL_CULL' in lib
    assert 'candidate_count <= 64u' in cpp
    assert 'PIPELINE_CULL_SMALL' in cpp
    assert 'compute_list_add_barrier' in cpp
    assert 'm_hSet[PIPELINE_CULL_SMALL]' in cpp
    print('Wave4 shader/catalog/cold-select contract: PASS')


if __name__ == '__main__':
    test_parity()
    test_source_contracts()
