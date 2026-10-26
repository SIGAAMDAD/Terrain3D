#!/usr/bin/env python3
"""Static memory/ABI baseline, intentionally separate from a GPU benchmark."""
import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def collect():
    source = (ROOT / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
    v1 = (ROOT / 'src/rendering/rd/vt/core/terrain_3d_vt_contract.h').read_text(encoding='utf-8')
    main_abi = (ROOT / 'src/rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h').read_text(encoding='utf-8')
    assert '#define T3D_REGION_SLOTS' in source and '( 1024 )' in source
    assert '#define T3D_VT_PHYSICAL_TILE' in v1 and '136u' in v1
    assert 'sizeof( t3dRdTerrainRegionGpu_t ) == 48u' in main_abi
    inventory = json.loads((HERE / 'far_inventory_v1.json').read_text(encoding='utf-8'))
    shaders = json.loads((HERE / 'shader_pass_matrix_v1.json').read_text(encoding='utf-8'))
    physical = 136 * 136
    bc1 = (136 // 4) ** 2 * 8
    bc5 = bc7 = (136 // 4) ** 2 * 16
    composed_set = bc1 + bc5 + bc7
    return {
        'schema': 'ST00_STATIC_BASELINE_V1',
        'source': 'Terrain3D_FAR_Regional_V1_Contract_Cleanup_2026-10-25_Full_Source.zip',
        'units': 'bytes unless indicated',
        'observed_in_source': {
            'region_map_slots': 1024, 'old_gpu_region_record_bytes': 48,
            'old_region_bank_bytes': 1024 * 48,
            'vt_physical_tile_side': 136, 'vt_interior_side': 128,
            'composed_channels': ['BC1 albedo', 'BC5 normal', 'BC7 PBR'],
            'v1_logical_allocation_runtime_slots': 3,
            'far_explicit_reference_files': inventory['file_count'],
            'far_explicit_reference_occurrences': inventory['reference_count'],
            'shader_variant_count': shaders['variant_count'],
        },
        'calculated_projections_not_allocations': {
            'new_gpu_region_record_bytes': 64,
            'new_region_bank_bytes': 1024 * 64,
            'region_bank_delta_bytes': 1024 * (64 - 48),
            'bc1_physical_tile_bytes': bc1,
            'bc5_physical_tile_bytes': bc5,
            'bc7_physical_tile_bytes': bc7,
            'composed_three_channel_set_bytes': composed_set,
            'composed_160_set_payload_bytes': 160 * composed_set,
            'vhm_r16_68x68_page_bytes': 68 * 68 * 2,
            'vhm_144_page_payload_bytes': 144 * 68 * 68 * 2,
            'vhm_pinned_tail_atlas_max_bytes': 1664 * 640 * 2,
            'vhm_page_table_max_bytes': 1024 * 11 * 16,
            'mask_mixed_exact_pinned_worst_case_bytes': 1024 * 4 * 1536,
            'mask_page_table_max_bytes': 1024 * 4 * 16,
        },
        'live_metrics': {
            'status': 'NOT_CAPTURED: no running Godot scene, Vulkan device or MSVC DLL here',
            'camera_path': None, 'elapsed_s': None,
            'gpu_ms_p50': None, 'gpu_ms_p95': None,
            'process_ram_bytes': None, 'process_ram_peak_bytes': None,
            'gpu_vt_bank_bytes': None, 'gpu_vt_tail_bytes': None,
            'gpu_height_bytes': None, 'gpu_mask_bytes': None,
            'gpu_page_eviction_count': None, 'vt_resident_page_count': None,
            'cpu_allocations_per_frame': None, 'gpu_readbacks_per_frame': None,
        },
        'required_on_device_capture': [
            'Use same scene and camera trace pre/post; steady idle, continuous travel, teleport, slow uploads',
            'Run editor and runtime, count Vulkan allocation deltas and RD physical/virtual residency',
            'Record p50/p95 frame and terrain GPU pass timing plus process RSS and peak',
            'Log vt faults, page misses, pinned roots, decode/cook time, package sizes and upload bytes',
            'Compare production parity reference images across shader variants',
        ],
        'native_build_commands_windows': [
            'scons platform=windows target=editor arch=x86_64 optimize=speed_trace debug_symbols=yes use_static_cpp=yes -j8',
            'scons platform=windows target=template_release arch=x86_64 optimize=speed_trace debug_symbols=yes use_static_cpp=yes -j8',
        ],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    value = json.dumps(collect(), sort_keys=True, indent=2) + '\n'
    output = HERE / 'static_baseline_v1.json'
    if args.write:
        output.write_text(value, encoding='utf-8')
    elif not output.exists() or output.read_text(encoding='utf-8') != value:
        raise SystemExit('ST00 static baseline drift')
    print('ST00 static baseline OK (live GPU/CPU benchmarks not captured)')


if __name__ == '__main__':
    main()
