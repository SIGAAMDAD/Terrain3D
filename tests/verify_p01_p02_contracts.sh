#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D P01+P02 contract gate: FAIL: $1" >&2; exit 1; }

CONSTANTS="$ROOT/src/Terrain3DConstants.h"
MEMORY_POLICY="$ROOT/src/memory/terrain_3d_memory_policy.cpp"
MEMORY_BUDGET="$ROOT/src/memory/terrain_3d_memory_budget.h"
GPU_POLICY="$ROOT/src/streaming/terrain_3d_gpu_cache_policy.h"
GPU_CACHE_H="$ROOT/src/streaming/terrain_3d_gpu_region_cache.h"
GPU_CACHE_CPP="$ROOT/src/streaming/terrain_3d_gpu_region_cache.cpp"
DATA_H="$ROOT/src/data/terrain_3d_data.h"
REGION_H="$ROOT/src/data/terrain_3d_region.h"
REGION_CPP="$ROOT/src/data/terrain_3d_region.cpp"
REPO_H="$ROOT/src/data/terrain_3d_region_repository.h"
REPO_CPP="$ROOT/src/data/terrain_3d_region_repository.cpp"
WORLD_H="$ROOT/src/data/terrain_3d_runtime_world_manifest.h"
WORLD_CPP="$ROOT/src/data/terrain_3d_runtime_world_manifest.cpp"
HLOD_H="$ROOT/src/hlod/terrain_3d_hlod_manifest.h"
HLOD_CPP="$ROOT/src/hlod/terrain_3d_hlod_manifest.cpp"
HLOD_STREAMER="$ROOT/src/hlod/terrain_3d_hlod_streamer.cpp"
BINDINGS="$ROOT/src/core/terrain_3d_bindings.cpp"
CS_TERRAIN="$ROOT/project/addons/terrain_3d/csharp/Terrain3D.cs"
CS_REGION="$ROOT/project/addons/terrain_3d/csharp/Terrain3DRegion.cs"
HLOD_BAKER="$ROOT/project/addons/terrain_3d/tools/hlod_baker.gd"

# P01: one owner for far-atlas bytes; page-cache grant remains exact bytes.
grep -q 'const uint64_t fixed_far_atlas_bytes = getGpuFarAtlasBytes();' "$MEMORY_POLICY" || fail "far atlas is not explicitly owned by fixed memory"
grep -q 'const uint64_t managed_gpu_page_budget_bytes = rResult.m_szSubsystems\[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES\].m_nEffectiveBytes;' "$MEMORY_POLICY" || fail "managed GPU page grant is not taken directly from coordinator bytes"
apply_body="$(sed -n '/void Terrain3D::applyMemoryBudgetResult/,/void Terrain3D::serviceMemoryBudgetPolicy/p' "$MEMORY_POLICY")"
if grep -q 'getGpuFarAtlasBytes' <<<"$apply_body"; then fail "far atlas is recombined into managed GPU page grant"; fi
grep -q 'uint64_t m_nGpuRegionCacheBudgetBytes' "$GPU_CACHE_H" || fail "GPU cache does not store exact byte budget"
if grep -q 'm_nGpuRegionCacheBudgetMb' "$GPU_CACHE_H" "$GPU_CACHE_CPP"; then fail "GPU cache still owns an internal MiB budget"; fi
grep -q 'uint64_t nBudgetBytes' "$DATA_H" || fail "data/cache boundary does not accept byte budget"
if grep -Eq 'FarSurface|far_surface|far_atlas|FarAtlas' "$GPU_POLICY"; then fail "managed GPU cache policy still estimates far-atlas storage"; fi
grep -q 'uint64_t m_nCommittedBytes' "$MEMORY_BUDGET" || fail "budget result does not expose committed bytes"
grep -q 'result.m_nCommittedBytes > rInput.m_nTotalBudgetBytes' "$MEMORY_BUDGET" || fail "known-residency total is not checked against total budget"
grep -q 'physical_layers <= minimum_spares' "$GPU_POLICY" || fail "page cache can still manufacture capacity below legal minimum"
grep -q 't3dGpuCacheEstimateResidentLayerBytes' "$GPU_POLICY" || fail "managed GPU capacity does not use complete resident-layer accounting"

# P02: one immutable 128 samples x 1 metre geometry contract.
grep -q '#define s_nRegionSize ( 128 )' "$CONSTANTS" || fail "region size is not fixed at 128"
grep -q '#define s_nRegionVertexSpacingMeters ( 1u )' "$CONSTANTS" || fail "integer vertex spacing contract is not fixed at 1 metre"
grep -q '#define s_fRegionVertexSpacing ( 1.0f )' "$CONSTANTS" || fail "floating vertex spacing contract is not fixed at 1 metre"
grep -q '#define s_nRegionWorldSizeMeters ( s_nRegionSize \* s_nRegionVertexSpacingMeters )' "$CONSTANTS" || fail "128 metre physical width is not derived from the fixed geometry contract"
grep -q '#define s_nRuntimeWorldManifestVersion ( 5u )' "$CONSTANTS" || fail "world manifest was not bumped to v5"
grep -q '#define s_fDataCurrentVersion ( 0.95f )' "$CONSTANTS" || fail "region persistence version was not bumped to 0.95"
if grep -Eq 'setRegionSize|setVertexSpacing|m_fVertexSpacing' "$REGION_H"; then fail "Terrain3DRegion still exposes/stores mutable geometry"; fi
grep -q 'hasValidGeometryContractInternal' "$REGION_H" || fail "region geometry validity latch is missing"
grep -q 'Rejected region data with legacy geometry: region_size must be exactly 128 samples' "$REGION_CPP" || fail "setData does not reject non-128 legacy geometry"
grep -q 'Rejected region data with legacy geometry: vertex_spacing must be exactly 1.0 metre' "$REGION_CPP" || fail "setData does not reject non-1m legacy geometry"
if grep -q 'dict\["region_size"\]\|dict\["vertex_spacing"\]' "$REGION_CPP"; then fail "new region dictionary persistence still serializes mutable geometry"; fi
if grep -Eq 'configureGeometry|setVertexSpacing|m_fVertexSpacing' "$REPO_H" "$REPO_CPP"; then fail "repository still stores or mutates vertex spacing"; fi
grep -q 's_nRegionWorldSizeMeters' "$REPO_CPP" || fail "repository world addressing is not fixed to the 128 metre region width"
if grep -Eq 'm_fVertexSpacing|setVertexSpacing' "$WORLD_H"; then fail "world manifest still stores mutable vertex spacing state"; fi
grep -q 'serialized_vertex_spacing != s_fRegionVertexSpacing' "$WORLD_CPP" || fail "world manifest does not reject spacing other than exactly 1 metre"
grep -q 's_nRegionGeometrySignature' "$WORLD_CPP" || fail "world manifest does not validate fixed geometry signature"
grep -q 'int m_nFormat = 3;' "$HLOD_H" || fail "HLOD resource format is not 3"
if grep -Eq 'vertex_spacing|VertexSpacing' "$HLOD_H" "$HLOD_CPP" "$HLOD_STREAMER" "$HLOD_BAKER"; then fail "HLOD path still persists or compares mutable vertex spacing"; fi
grep -q 'const BUILDER_VERSION: int = 5' "$HLOD_BAKER" || fail "HLOD builder version is not 5"
grep -q 'geometry=128x1m' "$HLOD_BAKER" || fail "HLOD build signature does not seal physical geometry"
if grep -q 'D_METHOD( "set_vertex_spacing"' "$BINDINGS"; then fail "Terrain3D still binds a public vertex-spacing setter"; fi
if grep -q 'D_METHOD( "set_region_size"' "$BINDINGS"; then fail "Terrain3D still binds a public region-size setter"; fi
if grep -q 'set => Set(GDExtensionPropertyName.RegionSize' "$CS_TERRAIN" "$CS_REGION"; then fail "generated C# region-size setter remains"; fi
if grep -q 'set => Set(GDExtensionPropertyName.VertexSpacing' "$CS_TERRAIN" "$CS_REGION"; then fail "generated C# vertex-spacing setter remains"; fi

echo "Terrain3D P01+P02 contract gate: PASS"
