#!/usr/bin/env python3
"""CS18 cold opt-in raster and descriptor ABI validation (no GPU execution)."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[4]
shader = (root / 'project/addons/terrain_3d/rd/terrain_world_raster.glsl').read_text()
shader_old = shader.split('#[vertex]')[0]
versions = dict(re.findall(r'^(\w+) = "([^"\n]*)";', shader_old, re.M))
composed = {k: v for k,v in versions.items() if k.startswith('cs_terrain_strip_')}
assert len(composed) == 10
assert len(set(composed)) == 10
for k, val in composed.items():
    assert val == versions[k[3:]] + r'\n#define T3D_CS_ACTIVE', k
for name, definition in versions.items():
    if not name.startswith('cs_'):
        assert 'T3D_CS_ACTIVE' not in definition, name
assert not any(name.startswith('cs_terrain_strip_depth =') for name in shader_old.splitlines())
cat = (root/'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
ids = (root/'src/Terrain3DEnums.h').read_text()
assert 'T3D_RD_SHADER_WORLD_CS_BASE' in cat and 'T3D_RD_SHADER_WORLD_CS_BASE' in ids
ordered = re.findall(r'"(cs_terrain_strip_[a-z0-9_]+)"', cat)
assert ordered == list(composed)
assert 'value < T3D_RD_SHADER_WORLD_CS_BASE' in cat
cpp = (root/'src/rendering/rd/world/visibility/terrain_3d_world_raster.cpp').read_text()
assert 'm_bExperimentalComposed &&' in cpp
assert '( uint32_t ) variant != 8u' in cpp
assert 'T3D_RD_SHADER_WORLD_CS_BASE + ( uint32_t ) variant' in cpp
assert 'pComposed->m_hPageTable' in cpp
assert 'for ( uint32_t i = 0u; i < T3D_CS_CHANNEL_COUNT; ++i )' in cpp
assert 'samplerTexture( 14 + ( int ) i' in cpp
assert 'UNIFORM_TYPE_STORAGE_BUFFER, pComposed->m_hPageTable' in cpp
assert 'pComposed->m_nDeviceGeneration << 32u' in cpp
assert 'return false;' in cpp.split('configureComposedExperimentalCold')[1]
preparer = (root/'src/rendering/rd/forward/terrain_3d_forward_pass_preparer.cpp').read_text()
assert 'acquireComposedSurfaceBindingsHot( composed )' in preparer
assert 'nFailureStage = 8u' in preparer
assert 'pComposed ) )' in preparer
assert 'isComposedExperimentalEnabled()' in preparer
runtime = (root/'src/rendering/rd/world/runtime/terrain_3d_world_runtime.cpp').read_text()
assert '!m_bInitialized && m_raster.configureComposedExperimentalCold( bEnable )' in runtime
assert 'pFeedbackOwner, pComposed' in runtime
print('CS18 shader variants, GPU binding contract, default-off and fail-closed wiring: PASS')
