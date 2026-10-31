#!/usr/bin/env python3
"""P28b: exact CPU model of bounded three-stage world visibility scan.

Tests bucket-order prefixes, capacity clipping, diagnostics, rejected lanes,
partial 64-thread groups, and zero-atomic stage ownership. GPU tests are a
separate acceptance gate; shader compilation alone cannot prove GPU parity.
"""
import random
from pathlib import Path
from test_world_culling import model_serial

ROOT = Path(__file__).resolve().parents[4]
LANES = 64
MAX_CANDIDATES = 65536
MAX_GROUPS = MAX_CANDIDATES // LANES
WORDS = MAX_CANDIDATES * 4 + MAX_GROUPS * 22
REJECT_COUNTER = (3, 4, 2, 5)


def model_scan(entries, capacity):
    counts = []
    rejects = []
    records = []
    # 1: classify once per candidate and assign each surviving candidate a
    # deterministic local rank in one of seven independently owned buckets.
    for group_start in range(0, len(entries), LANES):
        group = entries[group_start:group_start + LANES]
        local = [0] * 7
        rej = [0] * 4
        for bucket, kind, vertices, reject, ident in group:
            if reject < 4:
                rej[reject] += 1
            if bucket < 7:
                records.append((bucket, local[bucket], kind, vertices, ident))
                local[bucket] += 1
            else:
                records.append((7, 0, kind, 0, ident))
        counts.append(local)
        rejects.append(rej)
    # 2: one prefix writer per bucket. Group prefixes are monotonically
    # increasing; every scatter target has a unique (bucket, rank) pair.
    totals = [0] * 7
    prefixes = []
    for counts_in_group in counts:
        prefixes.append(list(totals))
        for bucket in range(7):
            totals[bucket] += counts_in_group[bucket]
    counters = [0] * 20
    counters[1] = len(entries)
    for bucket in range(7):
        counters[12 + bucket if bucket < 6 else 11] = totals[bucket]
    for reject in range(4):
        counters[REJECT_COUNTER[reject]] = sum(group[reject] for group in rejects)
    counters[6] = int(any(totals[i] > capacity[i] for i in range(7)))
    # 3: unique output ownership, reduction of accepted material-class counters
    # and useful vertices per workgroup, then the finalizer folds those sums.
    outputs = [[] for _ in range(7)]
    accepted_totals = [0] * 4
    for index, (bucket, rank, kind, vertices, ident) in enumerate(records):
        if bucket >= 7:
            continue
        global_rank = prefixes[index // LANES][bucket] + rank
        if global_rank >= capacity[bucket]:
            continue
        outputs[bucket].append((global_rank, ident))
        if bucket < 6:
            accepted_totals[0] += vertices
        elif 1 <= kind <= 3:
            accepted_totals[kind] += 1
    counters[18] = accepted_totals[0]
    counters[8:11] = accepted_totals[1:]
    counters[7] = sum(min(totals[b], capacity[b]) for b in range(6))
    counters[0] = counters[7] + min(totals[6], capacity[6])
    counters[19] = sum(min(totals[b], capacity[b]) * (b * 16 + 24) for b in range(6))
    for bucket in range(7):
        outputs[bucket] = [item for _, item in sorted(outputs[bucket])]
    return counters, outputs


def test_scan_parity():
    random_generator = random.Random(0x528B)
    cases = 0
    for count in (0, 1, 2, 63, 64, 65, 66, 127, 128, 129, 511, 512, 513, 1023, 1024,
                  1025, 4095, 4096, 4097, 65535, 65536):
        repetitions = 70 if count <= 1025 else (5 if count <= 4097 else 1)
        for _ in range(repetitions):
            entries = []
            for index in range(count):
                bucket = random_generator.choice((0, 1, 2, 3, 4, 5, 6, 6, 7, 7, 7))
                reason = random_generator.randrange(5) if bucket == 7 else 4
                kind = 0 if bucket < 6 else random_generator.randrange(1, 4)
                entries.append((bucket, kind, random_generator.randrange(1, 65000), reason, index))
            capacity = [random_generator.randrange(count + 2) for _ in range(7)]
            baseline = model_serial(entries, capacity)
            assert baseline == model_scan(entries, capacity), (count, capacity)
            cases += 1
    # Exclusive all-in-one-bucket and zero-capacity paths are especially
    # sensitive to off-by-one errors in the global prefix and admitted counts.
    for count in (1, 64, 65, 4097, 65536):
        for bucket in range(8):
            entries = [(bucket, 1 if bucket == 6 else 0, 400, 4, index) for index in range(count)]
            for caps in ([0] * 7, [count] * 7, [count // 2] * 7):
                assert model_scan(entries, caps) == model_serial(entries, caps)
                cases += 1
    print('P28b bounded scan parity:', cases, 'cases PASS')


def test_scan_contract():
    assert WORDS == 284672
    assert MAX_GROUPS == 1024
    shader = (ROOT / 'project/addons/terrain_3d/rd/terrain_world_visibility.glsl').read_text()
    cpp = (ROOT / 'src/rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.cpp').read_text()
    header = (ROOT / 'src/rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.h').read_text()
    lib = (ROOT / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
    for version in ('scan_classify', 'scan_prefix', 'scan_scatter', 'scan_finalize'):
        assert version + ' = ' in shader
    assert 'T3D_WORLD_SCAN_CANDIDATE_LIMIT 65536u' in shader
    assert 'T3D_WORLD_SCAN_GROUP_LIMIT 1024u' in shader
    assert 'scan_scratch.words[T3D_WORLD_SCAN_PREFIX_BASE' in shader
    assert 'isWorldScanEnabledCold( void )' in lib
    assert 'T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_CLASSIFY' in lib
    assert 'T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_FINALIZE' in lib
    assert 'return t3dRdShaderSharedStateGet() != NULL;' in lib
    assert 'const bool use_scan = !use_small && m_bScanAvailable &&' in cpp
    assert 'candidate_count <= SCAN_MAX_CANDIDATES' in cpp
    assert 'm_scanScratch.releaseTracked( m_context )' in cpp
    assert 'SCAN_WORDS = SCAN_MAX_CANDIDATES * 4u + SCAN_MAX_GROUPS * 22u' in header
    assert 'PIPELINE_SCAN_FINALIZE' in cpp
    assert 'const uint32_t scan_pass_count = use_scan ? 3u : 1u;' in cpp
    assert 'rd->compute_list_add_barrier( list );' in cpp
    print('P28b source + scratch-layout contracts PASS')


if __name__ == '__main__':
    test_scan_parity()
    test_scan_contract()
