#!/usr/bin/env python3
"""C-03: no historical atlas allocation is presented as physical V1 residency."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
WINDOW = (ROOT / 'project/addons/terrain_3d/src/runtime_profiler_window.gd').read_text()
CPU = (ROOT / 'src/diagnostics/core/terrain_3d_runtime_diagnostics.cpp').read_text()
MEMORY = (ROOT / 'src/memory/policy/terrain_3d_memory_policy.cpp').read_text()


def verify():
    for key in ('far_height_rebuild_cpu_usec', 'far_color_rebuild_cpu_usec',
                'far_height_publication_cpu_usec', 'far_color_publication_cpu_usec'):
        # The native producer currently substitutes 0 for missing source keys;
        # the UI must not display that manufactured zero as a measurement.
        assert 'cpu["' + key + '"]' in CPU
        assert '"' + key + '"' not in WINDOW
    assert 'FAR / V1 cook and publication' in WINDOW
    for key in ('gpu_terrain_page_pool_estimated_bytes',
                'repository_and_authoritative_storage_bytes',
                'cpu_terrain_page_cache_bytes', 'tracked_current_residency_bytes',
                'current_known_bytes'):
        assert '"' + key + '"' in WINDOW
        assert '"' + key + '"' in MEMORY
    assert 'p_cpu.has(str(row[1])) else "unavailable"' in WINDOW
    assert '"source_io_cache_resident_bytes"' in WINDOW
    assert 'source_io_cache_resident_bytes' in (ROOT / 'src/streaming/cache/terrain_3d_gpu_region_cache.cpp').read_text()
    assert 'V1 VT physical-page bytes", "unavailable"' in WINDOW
    assert 'if region_gpu_available else "unavailable (deep snapshot)"' in WINDOW
    for stale in ('far_atlas_bytes', 'gpu_terrain_page_pool_bytes',
                  'far_atlas_publication_cpu_usec', 'far_atlas_rebuild_publication_cpu_usec'):
        assert stale not in WINDOW
    print('PASS C-03 profiler source-key provenance and missing V1 physical-byte state')


if __name__ == '__main__':
    verify()
