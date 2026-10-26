#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
renderer = (root / 'src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp').read_text(encoding='utf-8')
coordinator = (root / 'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text(encoding='utf-8')
constants = (root / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text(encoding='utf-8')


def fail(message: str) -> None:
    print(f'Instancer authoritative visible-index verification FAIL: {message}')
    sys.exit(1)


start = renderer.find('template <bool t_bShadowPass>\nbool Terrain3DRdInstanceRenderer::buildPacketsForPass(')
end = renderer.find('\nbool Terrain3DRdInstanceRenderer::buildPackets(', start)
if start < 0 or end < 0:
    fail('could not isolate buildPacketsForPass')
body = renderer[start:end]

for token in (
    '#if T3D_INSTANCER_MINIMAL_RENDER_PATH',
    'const bool defer_visibility_output = false;',
    'const bool defer_visibility_output = bSkipFrustum;',
    'if ( !defer_visibility_output )',
    'visible_indices[write++] = entry.m_nFirstInstance + instance;',
    'slot.m_packet.m_command.m_nInstanceCount = defer_visibility_output ? 0u : slot.m_packet.m_nVisibleCount;',
):
    if token not in body:
        fail(f'missing visible-index/direct-draw contract token: {token}')

if 'if ( !bSkipFrustum ) {\n\t\tfor ( size_t iEntry' in body:
    fail('visible-index materialization is still incorrectly coupled to CPU frustum testing')

if 'Forward+ PREPARE produced zero draws for a non-empty published scene' in coordinator:
    fail('successful pass-specific zero-draw PREPARE is still reported as an error')
if '++m_nInstancePreparedWithoutDraws;' not in coordinator:
    fail('zero-draw diagnostic counter was removed instead of retained')
if '[RD instances] Forward+ PREPARE failed; pass=' not in coordinator:
    fail('actual PREPARE failures are no longer reported')

native_build = re.search(r's_pszNativeBuildId \( "([^"]+)" \)', constants)
plugin_build = re.search(r'EXPECTED_NATIVE_BUILD_ID: String = "([^"]+)"', plugin)
native_revision = re.search(r's_nNativeApiRevision \( (\d+)LL \)', constants)
plugin_revision = re.search(r'EXPECTED_NATIVE_API_REVISION: int = (\d+)', plugin)
if not native_build or not plugin_build or native_build.group(1) != plugin_build.group(1):
    fail('native/script build IDs are not synchronized')
if not native_revision or not plugin_revision or native_revision.group(1) != plugin_revision.group(1):
    fail('native/script API revisions are not synchronized')
if int(native_revision.group(1)) < 2026090901:
    fail('native/script API revision predates the integrated P3 lineage')

print('Instancer authoritative visible-index verification PASS')
