from pathlib import Path
r=Path(__file__).resolve().parents[4]
rd=r/'project/addons/terrain_3d/rd'
legacy=['terrain_clipmap_near.glsl','terrain_clipmap_middle.glsl','terrain_clipmap_far.glsl']
for name in legacy:
 assert not (rd/name).exists(), name
cat=(r/'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
for name in legacy: assert name not in cat
shader=(r/'project/addons/terrain_3d/rd/terrain_clipmap.glsl').read_text()
assert shader.count('#define T3D_UCMP_COOKED 1')==2
lib=(r/'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
assert 'm_bUnifiedCookedPreview = true;' in lib
assert 'id >= T3D_RD_SHADER_TERRAIN_FORWARD_BASE && id < T3D_RD_SHADER_TERRAIN_CULL' in lib
assert 'id >= T3D_RD_SHADER_TERRAIN_FEEDBACK_BASE && id < T3D_RD_SHADER_WORLD_CS_BASE' in lib
clip=(r/'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()
init=clip.split('bool Terrain3DRdClipmapRenderer::initialize(',1)[1].split('void Terrain3DRdClipmapRenderer::shutdown(',1)[0]
assert 'borrowSpirv( T3D_RD_SHADER_NEAR_ALIAS' not in init
assert 'borrowSpirv( T3D_RD_SHADER_UCMP_ALIAS' in init
ens=clip.split('bool Terrain3DRdClipmapRenderer::ensureShaders(',1)[1].split('bool Terrain3DRdClipmapRenderer::ensureForwardShaders(',1)[0]
assert 'T3D_RD_SHADER_NEAR_ALIAS' not in ens
assert 'T3D_RD_SHADER_UCMP_ALIAS' in ens
assert 'm_bUnifiedCookedMaterialActive = unified;' in clip
cache=(r/'src/streaming/cache/terrain_3d_gpu_region_cache.cpp').read_text()
assert 'if ( !Terrain3DRdShaderLibrary::isUnifiedCookedPreviewEnabledCold() &&' in cache
material=(r/'src/rendering/rd/material/terrain_3d_material_state_builder.cpp').read_text()
assert 'rSnapshot.m_nTerrainMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1;' in material
frag=(rd/'terrain_clipmap_fragment_inc.glslinc').read_text()
assert 'including auto controls and deterministic detiling' in frag
# No artificial approval of unverified GPU/material parity.
author=(r/'src/authoring/vt/integration/terrain_3d_render_authoring_companion.cpp').read_text()
assert 'result["composed_bake_parity_ready"] = false' in author
print('PASS: UCMP single-root activation, legacy resource retirement, V1 source fallback, GPU middle allocation disabled, parity provenance')

# Material ABI matches exactly one 48-byte tier record, not the old 3 x 48-byte model.
assert 'm_szTier[1]' in (r/'src/Terrain3DTypes.h').read_text()
assert 'sizeof( t3dRdMaterialConfig_t ) == 288u' in (r/'src/rendering/rd/core/terrain_3d_rd_types.h').read_text()
contract=(rd/'terrain_clipmap_contract.glslinc').read_text()
assert 'TierMaterialConfig tier[1];' in contract
assert 'TierMaterialConfig tier[3];' not in contract
for filename in ('terrain_clipmap_fragment_inc.glslinc','terrain_clipmap_vertex_inc.glslinc'):
 import re
 text=(rd/filename).read_text()
 assert not re.search(r'material_config\.tier\[(?!0\])',text),filename
print('PASS: 288-byte single-tier C++/GLSL material ABI, no physical legacy terrain root')
