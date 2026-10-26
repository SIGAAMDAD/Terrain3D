#!/usr/bin/env python3
from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')

def fail(msg):
    print('FAIL:', msg, file=sys.stderr)
    raise SystemExit(1)

constants = read('src/Terrain3DConstants.h')
header = read('src/terrain_3d.h')
stream = read('src/streaming/integration/terrain_3d_streaming_integration.cpp')
cache = read('src/streaming/cache/terrain_3d_gpu_region_cache.cpp')
renderer = read('src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp')
bindings = read('src/integration/godot/terrain_3d_bindings.cpp')
manifest = read('src/streaming/policy/terrain_3d_performance_profile_manifest.inc')
plugin = read('project/addons/terrain_3d/src/editor_plugin.gd')
metadata = json.loads(read('project/addons/terrain_3d/setting_metadata.json'))['properties']
csharp = read('project/addons/terrain_3d/csharp/Terrain3D.cs')
workbench = read('project/addons/terrain_3d/tools/region_workbench.gd')

for needle, label in [
    ('#define s_nGpuRegionCacheMaxBudgetMb ( 512 )', '512 MiB GPU budget ceiling'),
    ('#define s_nGpuCacheRuntimeMaxLayers ( 256 )', '256 runtime physical layer ceiling'),
    ('int m_nGpuRegionCacheBudgetMb = 64;', '64 MiB default GPU manual request'),
    ('int m_nGpuRegionCachePageLimit = 128;', '128-page runtime default'),
    ('int m_nTotalMemoryBudgetMb = 512;', '512 MiB total default'),
    ('int m_nMemoryTransientReserveMb = 32;', '32 MiB transient default'),
]:
    if needle not in constants + header:
        fail('missing ' + label)

if 'm_nGpuRegionDetailLods <= 0 ? MAX( 1, m_nRuntimeNearShaderLods )' not in stream:
    fail('zero detailed-LOD override does not follow the Near tier in Terrain3D normalization/range reporting')
if 'detail_lods = MAX( 1, m_pTerrain->getRuntimeNearShaderLods() );' not in cache:
    fail('GPU cache zero detailed-LOD override does not follow Near tier')
if 'required_pages = MIN( required_pages, active_regions );' not in cache:
    fail('required detailed page count is not bounded by actual active topology')
if 'calculateRequiredCorePagesForLods( 0 )' not in cache:
    fail('automatic GPU minimum does not reserve the configured Near footprint')
if 'getGpuRegionCachePageLimit()' not in cache or 'getEditorGpuRegionCachePages()' not in cache:
    fail('runtime/editor physical page ceilings are not independently honored')

if 'set_gpu_region_cache_page_limit' not in bindings or 'get_gpu_region_cache_page_limit' not in bindings:
    fail('runtime physical page ceiling is not exposed to GDScript/ClassDB')
if 'gpu_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,512,1,suffix: MiB"' not in bindings:
    fail('GPU memory budget inspector range is not 1-512 MiB')
if 'gpu_region_cache_page_limit", PROPERTY_HINT_RANGE, "16,256,1,suffix: pages"' not in bindings:
    fail('GPU page limit inspector range is not 16-256 pages')
if '0 = Near tier' not in bindings or '0 = all' in bindings:
    fail('ClassDB detailed-LOD sentinel metadata is stale')

if 'T3D_PROFILE_TERRAIN_INT( "total_memory_budget_mb", 512 )' not in manifest:
    fail('profile still restores the old 128 MiB total budget')
if 'T3D_PROFILE_TERRAIN_INT( "gpu_region_cache_page_limit", 128 )' not in manifest:
    fail('profile does not own the runtime page ceiling')
if 'T3D_PROFILE_TERRAIN_INT( "editor_gpu_region_cache_pages", 128 )' not in manifest:
    fail('profile still restores the old editor page ceiling')

# Height source data is the functional minimum, but the Near GPU array is a
# residency resource and may be transiently unavailable while cache publication
# converges. Both raster paths must retain a valid descriptor and fall through to
# Middle/Far/background until physical Near mappings can be republished.
if renderer.count('height = m_hFallbackArrayTexture;') < 2:
    fail('both raster preparation paths do not provide a transient Near-height descriptor fallback')
if 'Terrain3D direct RenderingDevice height texture is unavailable' in renderer:
    fail('transient Near-height residency is still renderer-fatal')
if 'Terrain3D direct RenderingDevice textures are incomplete' in renderer:
    fail('all-or-nothing optional texture abort remains in RD clipmap renderer')
for token in ['if ( !control.is_valid() )', 'if ( !color_map.is_valid() )', 'normal = m_hFallbackArrayTexture;', 'surface = m_hFallbackArrayTexture;']:
    if renderer.count(token) < 2:
        fail('optional terrain texture fallback missing: ' + token)


for key in ['configured_page_limit', 'required_core_pages', 'functional_minimum_bytes', 'height_texture_ready', 'control_texture_ready']:
    if f'stats["{key}"]' not in cache:
        fail('GPU cache diagnostics missing ' + key)

if 'm_bGpuRegionCacheTexturesReady = bHeightOk;' not in cache:
    fail('GPU page cache still requires optional texture arrays before becoming renderable')
if 'Height page cache is ready with optional channels degraded' not in cache:
    fail('optional-channel degraded cache state is not diagnosed')

if 'GpuRegionCachePageLimit = "gpu_region_cache_page_limit"' not in csharp:
    fail('C# API mirror missing GPU page ceiling')
if 'gpu_region_cache_page_limit' not in workbench:
    fail('Region Workbench does not expose GPU page ceiling')
if 'Terrain3D.gpu_region_cache_page_limit' not in metadata:
    fail('GDScript setting metadata missing GPU page ceiling')
if 'Zero follows runtime_near_shader_lods' not in metadata['Terrain3D.gpu_region_detail_lods']['tooltip']:
    fail('GDScript metadata still describes zero as all LODs')

native_rev = re.search(r's_nNativeApiRevision \( ([0-9]+)LL \)', constants)
script_rev = re.search(r'EXPECTED_NATIVE_API_REVISION: int = ([0-9]+)', plugin)
if not native_rev or not script_rev or native_rev.group(1) != script_rev.group(1):
    fail('native/GDScript API revision mismatch')
native_build = re.search(r's_pszNativeBuildId \( "([^"]+)" \)', constants)
script_build = re.search(r'EXPECTED_NATIVE_BUILD_ID: String = "([^"]+)"', plugin)
if not native_build or not script_build or native_build.group(1) != script_build.group(1):
    fail('native/GDScript build ID mismatch')

print('PASS: GPU budgets/page ceilings are adjustable, detailed residency follows Near, and height-only rendering degrades optional channels')
