#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []


def text(rel):
    return (ROOT / rel).read_text(encoding='utf-8')


def require(condition, message):
    if not condition:
        failures.append(message)


data = text('src/data/model/terrain_3d_data.cpp')
cache_h = text('src/streaming/cache/terrain_3d_middle_height_region_cache.h')
cache_cpp = text('src/streaming/cache/terrain_3d_middle_height_region_cache.cpp')
sampler_h = text('src/data/sampling/terrain_3d_sampler.h')
sampler_cpp = text('src/data/sampling/terrain_3d_sampler.cpp')
builder = text('src/physics/build/terrain_3d_collision_surface_builder.cpp')
store = text('src/streaming/runtime/terrain_3d_runtime_page_store.cpp')

require('makeHeightImageForDivisor' in cache_h,
        'middle-height cache must expose an explicit divisor cook path')
require('const int divisor = nResolutionDivisor <= 1 ? 1 : ( nResolutionDivisor <= 2 ? 2 : 4 );' in cache_cpp,
        'explicit middle-height cook path must normalize to the supported 1/2/4 runtime divisors')
require('const t3dRuntimePageConfig_t runtime_page_config = getRuntimePageConfig();' in data,
        'runtime page cook must freeze one numeric/config contract per page')
require('makeHeightImageForDivisor( location, runtime_page_config.m_nMiddleHeightDivisor )' in data,
        'runtime page cook must generate Middle height using the runtime-page divisor, not the editor live-cache divisor')
require('m_runtimeArtifacts.writePage( pRegion, runtime_page_config, payload, pError )' in data,
        'runtime page writer must validate against the same frozen config used to generate Middle height')
require('makeHeightImage( location, true )' not in data,
        'runtime page cook must not route Middle height through the authoring cache resolution')

require('bool bRequireCompleteCoverage = true' in sampler_h,
        'sampler rectangle copy must retain strict coverage by default')
require('bHardFailure = bHardFailure || ( pRegion && !pRegion->isDeleted() );' in sampler_cpp,
        'partial rectangle mode must still reject unavailable authored regions')
require('return !bHardFailure && ( bComplete || !bRequireCompleteCoverage );' in sampler_cpp,
        'partial rectangle mode must only tolerate genuinely absent coverage')
require('copyHeightRect( rPosition.x, rPosition.y, width, width, pHeightScratch, width, true, false )' in builder,
        'canonical collision surface must tolerate an absent +X/+Z authored-world border')

require('middle-height page has an invalid size/format; expected=' in store,
        'Middle-height cook failures must report expected and actual layout details')

if failures:
    print('runtime-page authoring cook verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)
print('runtime-page authoring cook verification PASS')
