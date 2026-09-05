#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D 128 m specialization phases 10+11+12 gate: FAIL: $1" >&2; exit 1; }

CONSTANTS="$ROOT/Terrain3DConstants.h"
EROSION_H="$ROOT/authoring/terrain_3d_erosion_job.h"
EROSION_CPP="$ROOT/authoring/terrain_3d_erosion_job.cpp"
AUTHORING_DIR="$ROOT/authoring"
WORLD_H="$ROOT/data/terrain_3d_runtime_world_manifest.h"
HLOD_STREAMER_H="$ROOT/hlod/terrain_3d_hlod_streamer.h"
HLOD_CODEC_H="$ROOT/hlod/terrain_3d_hlod_runtime_codec.h"
CPU_H="$ROOT/streaming/terrain_3d_cpu_region_cache.h"
INSTANCE_H="$ROOT/streaming/terrain_3d_instance_region_cache.h"
MIDDLE_H="$ROOT/streaming/terrain_3d_middle_height_region_cache.h"
GPU_H="$ROOT/streaming/terrain_3d_gpu_region_cache.h"
ACTIVE_H="$ROOT/streaming/terrain_3d_active_height_residency.h"

# Phase 10: authoring operates on the same fixed 128/32/1024 geometry as runtime.
if grep -q 'm_nSourceRegionSize' "$EROSION_H" "$EROSION_CPP"; then fail "erosion still owns mutable source-region size"; fi
if grep -q 's_nErosionRegionLookupCapacity' "$CONSTANTS" "$EROSION_H"; then fail "historical 4096-entry erosion lookup capacity remains"; fi
grep -q 'int16_t m_szRegionLookup\[s_nRegionSlots\]' "$EROSION_H" || fail "erosion lookup is not 1024-entry int16 storage"
grep -q 'int16_t m_ids\[s_nErosionDirectionCount\]' "$EROSION_H" || fail "erosion neighbor IDs are not narrowed to int16"
if grep -R -E 'getRegionSize\(\)|getRegionSizeInternal\(\)|getRegionMapSizeInternal\(\)' "$AUTHORING_DIR" --include='*.cpp' --include='*.h' | grep -v 'getSimulationRegionSize' >/dev/null; then
    fail "authoring C++ still re-reads immutable region geometry"
fi

# Phase 11: native world/HLOD runtime metadata uses packed region coordinates.
grep -q 't3dRegionCoord_t m_worldRegionMin' "$WORLD_H" || fail "world manifest minimum bound is not packed"
grep -q 't3dRegionCoord_t m_worldRegionMax' "$WORLD_H" || fail "world manifest maximum bound is not packed"
for file in "$HLOD_STREAMER_H" "$HLOD_CODEC_H"; do
    grep -q 't3dRegionCoord_t m_cluster' "$file" || fail "HLOD cluster coordinate is not packed in $file"
    grep -q 't3dRegionCoord_t m_regionMin' "$file" || fail "HLOD region minimum is not packed in $file"
    grep -q 't3dRegionCoord_t m_regionMax' "$file" || fail "HLOD region maximum is not packed in $file"
done

# Phase 12: bounded cache-coordinate banks use packed storage.
grep -q 't3dRegionCoord_t m_szLocations\[s_nCpuCacheMaxLayers\]' "$CPU_H" || fail "CPU cache locations are not packed"
grep -q 't3dRegionCoord_t m_szLocations\[s_nInstanceCacheMaxPages\]' "$INSTANCE_H" || fail "instance cache locations are not packed"
grep -q 't3dRegionCoord_t m_szLoaded\[s_nInstanceCacheMaxPages\]' "$INSTANCE_H" || fail "instance loaded journal is not packed"
grep -q 't3dRegionCoord_t m_szEvicted\[s_nInstanceCacheMaxPages\]' "$INSTANCE_H" || fail "instance evicted journal is not packed"
grep -q 't3dRegionCoord_t m_szLocations\[s_nGpuHeightCacheMaxLayers\]' "$MIDDLE_H" || fail "middle-height cache locations are not packed"
grep -q 't3dRegionCoord_t m_szGpuRegionPriorityLocations\[s_nGpuCacheMaxLayers\]' "$GPU_H" || fail "GPU priority locations are not packed"
grep -q 't3dRegionCoord_t m_szGpuRegionLocations\[s_nGpuCacheMaxLayers\]' "$GPU_H" || fail "GPU resident locations are not packed"
grep -q 't3dRegionCoord_t m_location = t3dInvalidRegionCoord()' "$GPU_H" || fail "GPU pending upload location is not packed"
grep -q 't3dRegionCoord_t m_victimLocation = t3dInvalidRegionCoord()' "$GPU_H" || fail "GPU pending victim location is not packed"
grep -q 't3dRegionCoord_t m_szCpuSourceRequests\[s_nCpuRequestCapacity\]' "$ACTIVE_H" || fail "active-height request journal is not packed"

echo "Terrain3D 128 m specialization phases 10+11+12 gate: PASS"
