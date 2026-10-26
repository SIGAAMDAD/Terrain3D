#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
switch_probe = (root / 'src/instances/terrain_3d_instancer_bringup.h').read_text(encoding='utf-8')
if '#define T3D_INSTANCER_RD_PORT 1' in switch_probe and '#define T3D_INSTANCER_MINIMAL_RENDER_PATH 0' in switch_probe:
    print('PASS: historical bring-up gate superseded by integrated instancer RD port')
    sys.exit(0)
switch = (root / 'src/instances/terrain_3d_instancer_bringup.h').read_text(encoding='utf-8')
instancer = (root / 'src/instances/runtime/terrain_3d_instancer.cpp').read_text(encoding='utf-8')
streaming = (root / 'src/streaming/integration/terrain_3d_streaming_integration.cpp').read_text(encoding='utf-8')
coordinator = (root / 'src/rendering/rd/core/terrain_3d_render_coordinator.cpp').read_text(encoding='utf-8')
renderer = (root / 'src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp').read_text(encoding='utf-8')


def fail(message: str) -> None:
    print(f'Instancer minimal render path verification FAIL: {message}')
    sys.exit(1)


def body(signature: str, text: str) -> str:
    start = text.find(signature)
    if start < 0:
        fail(f'missing {signature}')
    brace = text.find('{', start)
    if brace < 0:
        fail(f'missing body for {signature}')
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    fail(f'unterminated body for {signature}')
    return ''


if '#define T3D_INSTANCER_MINIMAL_RENDER_PATH 1' not in switch:
    fail('central bring-up switch is not enabled')

snapshot = body('bool Terrain3DInstancer::buildRdInstanceSnapshot(', instancer)
minimal_snapshot = snapshot.split('#else', 1)[0]
for token in (
        'getRegionPtrMapInternal()',
        'getResidentInstanceStoreInternal()',
        'asset->getMesh( 0 )',
        't3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, 0 )',
        'cell.m_nVisibilityLayers = UINT32_MAX;',
        'cell.m_fLodBegin = 0.0f;',
        'cell.m_fLodEnd = 0.0f;',
        'cell.m_fFadeMargin = 0.0f;',
        't3dPackRdInstanceRecord( transform, color )',
):
    if token not in minimal_snapshot:
        fail(f'direct authoritative-store snapshot is missing {token}')
if 'm_renderCells' in minimal_snapshot:
    fail('minimal snapshot still depends on render-cell materialization')

update = body('void Terrain3DInstancer::updateMmis(', instancer).split('#else', 1)[0]
if 'markRdPublicationDirty();' not in update or 'destroyRenderCells();' not in update:
    fail('authoritative mutations do not invalidate direct publication while dropping render cells')

service = body('void Terrain3DInstancer::servicePendingUpdates(', instancer).split('#else', 1)[0]
if 'loadPendingMeshes();' not in service or 'processUpdates();' in service:
    fail('minimal service path still drains the render-cell update queue')

for signature in ('bool Terrain3D::effectiveInstanceRegionCacheEnabled(', 'bool Terrain3D::effectiveInstancerCellStreamingEnabled('):
    function = body(signature, streaming).split('#else', 1)[0]
    if 'return false;' not in function:
        fail(f'{signature} is not disabled in bring-up mode')

hlod_helper = body('static Terrain3DHLODStreamer *t3dRdGetInstanceHlodStreamer(', coordinator).split('#else', 1)[0]
if 'return NULL;' not in hlod_helper:
    fail('HLOD is still combined into the minimal instance publication')
if 'const bool bInstanceDirectDebug = true;' not in coordinator:
    fail('minimal renderer is not forced onto CPU visibility/reference packet construction')
if 'const bool bInstancePassEnabled = rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE' not in coordinator:
    fail('minimal renderer still participates in depth/shadow/motion passes')
if 'const uint32_t instance_visibility_mask = UINT32_MAX;' not in coordinator:
    fail('minimal renderer still depends on camera visibility-layer state')

visible = body('static bool t3dRdInstanceCellVisible(', renderer)
minimal_visible = visible.split('#else', 1)[0]
if 'return true;' not in minimal_visible:
    fail('minimal CPU visibility still performs frustum/LOD rejection')

prepare = body('bool Terrain3DRdInstanceRenderer::prepareForwardPass(', renderer)
if 'buildPackets( *pScene' not in prepare:
    fail('CPU reference packet construction is missing')
if 'const bool bSkipFrustum = true;' in prepare:
    fail('CPU reference path incorrectly selects GPU-cull topology semantics')
if '#if !T3D_INSTANCER_MINIMAL_RENDER_PATH' not in prepare or 'prepareMotionHistory' not in prepare:
    fail('minimal render path does not isolate temporal history')
if 'const bool bHlodVisibilityReady = true;' not in prepare:
    fail('minimal render path still dispatches HLOD visibility')

draw_packet = body('bool Terrain3DRdInstanceRenderer::drawPacket(', renderer)
for token in (
        '#if T3D_INSTANCER_MINIMAL_RENDER_PATH',
        'getDirectSurfaceVertexArray',
        'getDirectSurfaceIndexArray',
        'draw_list_draw( nDrawList, true, rPacket.m_nVisibleCount, 0u );',
        'draw_list_draw_indirect',
):
    if token not in draw_packet:
        fail(f'minimal direct-surface / production indirect split is missing: {token}')
if draw_packet.find('draw_list_draw( nDrawList, true, rPacket.m_nVisibleCount, 0u );') > draw_packet.find('#else'):
    fail('minimal direct indexed draw is not isolated ahead of the production branch')

print('Instancer minimal render path verification PASS')
