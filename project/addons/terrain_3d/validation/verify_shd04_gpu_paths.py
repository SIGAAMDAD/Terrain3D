#!/usr/bin/env python3
"""SHD-04 production GPU route contracts; CPU models do not exercise a Vulkan device."""

from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[4]
SRC = ROOT / 'src/rendering/rd'
TESTS = ROOT / 'src/diagnostics/tests/shaders'
sys.path.insert(0, str(TESTS))
from test_world_visibility_scan import model_scan
from test_world_culling import model_serial
from test_page_reduction import check_reduction


def require(value, detail):
    if not value:
        raise AssertionError(detail)


def route(count, small_ready=True, scan_ready=True):
    if count == 0:
        return 'reset/finalize'
    if small_ready and count <= 64:
        return 'small'
    if scan_ready and count <= 65536:
        return 'scan'
    return 'grouped'


def validate():
    lib = (SRC / 'shaders/terrain_3d_rd_shader_library.cpp').read_text()
    world = (SRC / 'world/visibility/terrain_3d_world_visibility_gpu.cpp').read_text()
    header = (SRC / 'world/visibility/terrain_3d_world_visibility_gpu.h').read_text()
    page = (SRC / 'terrain_pages/terrain_3d_page_compute.cpp').read_text()
    light = (SRC / 'forward/terrain_3d_rd_forward_lighting.cpp').read_text()
    inst = (SRC / 'instances/terrain_3d_rd_instance_renderer.cpp').read_text()
    catalog = (SRC / 'shaders/terrain_3d_rd_shader_catalog.cpp').read_text()

    retired = ('TERRAIN3D_GPU_GROUPED_CULL', 'TERRAIN3D_GPU_WORLD_SMALL_CULL',
               'TERRAIN3D_GPU_WORLD_SCAN', 'TERRAIN3D_GPU_PAGE_REDUCE',
               'TERRAIN3D_GPU_COMPACT_CLUSTERS', 'TERRAIN3D_GPU_INSTANCE_COMMAND_OWNER',
               'TERRAIN3D_GPU_INSTANCE_EXCLUSIVE')
    for name in retired:
        require(name not in lib and name not in light, name + ' remains a user dispatch flag')
    for func in ('isGroupedCullEnabledCold', 'isWorldSmallCullEnabledCold',
                 'isWorldScanEnabledCold', 'isPageReductionEnabledCold'):
        require('bool Terrain3DRdShaderLibrary::' + func in lib, func + ' unavailable')
    require('pSettings->has_setting( "terrain3d/rendering/compact_clusters" )' in lib,
            'compact profile owner missing')
    require('m_bCompactRequested = Terrain3DRdShaderLibrary::isCompactClustersEnabledCold();' in light,
            'compact producer and shader load have different profiles')
    require('!m_compact.prepare( rContext )' in light,
            'unsupported compact Forward+ input may silently render missing lights')
    require('m_bSmallAvailable && candidate_count <= 64u' in world and
            'm_bScanAvailable &&' in world and 'candidate_count <= SCAN_MAX_CANDIDATES' in world,
            'bounded WORLD workload routing changed')
    require('SCAN_MAX_CANDIDATES = 65536u' in header and
            'SCAN_WORDS = SCAN_MAX_CANDIDATES * 4u + SCAN_MAX_GROUPS * 22u' in header,
            'WORLD scan scratch contract changed')
    require('PIPELINE_CULL_GROUPED' not in world, 'WORLD now has competing grouped draw owner')
    require('m_bSmallAvailable = false;' in world and 'm_bScanAvailable = false;' in world,
            'WORLD lacks optional-kernel fallback')
    require('width <= 32u && height <= 32u' in page and
            'width <= T3D_PAGE_PARTIAL_MAX_EXTENT && height <= T3D_PAGE_PARTIAL_MAX_EXTENT' in page,
            'page reduction bounds changed')
    require('const bool reductionOperation = operation == HASH || operation == COMPARE;' in page,
            'edit operation can be routed to hash-only kernel')
    require('&& !reduce && !partial' in page, 'atomic reset fallback removed')
    require('m_reducePipeline.releaseTracked( context );' in page and
            'm_partialBuffer.releaseTracked( context );' in page,
            'page pipeline allocation failure has no fallback')
    require('T3D_RD_SHADER_INSTANCE_CULL_GROUPED' in catalog and
            'T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED' in catalog,
            'existing grouped catalog variants missing')
    require('Terrain3DRdShaderLibrary::isInstanceCommandOwnerEnabledCold()' in inst,
            'legacy preview compatibility boundary removed ahead of SHD-05')

    routes = 0
    for small, scan in ((True, True), (False, True), (True, False), (False, False)):
        for count in (0, 1, 2, 63, 64, 65, 127, 128, 1024, 65535, 65536, 65537, 131072):
            choice = route(count, small, scan)
            expected = ('reset/finalize' if not count else
                        'small' if small and count <= 64 else
                        'scan' if scan and count <= 65536 else 'grouped')
            require(choice == expected, 'wrong WORLD workload choice')
            routes += 1

    rng = random.Random(0x5D04)
    parities = 0
    for count in (0, 1, 2, 63, 64, 65, 66, 127, 129, 1024, 4097, 65536):
        entries = []
        for idx in range(count):
            bucket = rng.choice(range(8))
            kind = rng.randrange(1, 4) if bucket == 6 else 0
            reject = rng.randrange(4) if bucket == 7 else 4
            entries.append((bucket, kind, rng.randrange(1, 4000), reject, idx))
        caps = [rng.randrange(count + 2) for _ in range(7)]
        require(model_scan(entries, caps) == model_serial(entries, caps),
                'WORLD scan differs from grouped/serial semantics at ' + str(count))
        parities += 1

    reduction_parities = check_reduction()
    print('SHD-04 PASS: %d dispatch boundaries, %d scan/serial parities, '
          '%d integer page-reduction parities, 7 retired flags, compact/profile contract' %
          (routes, parities, reduction_parities))


if __name__ == '__main__':
    validate()
