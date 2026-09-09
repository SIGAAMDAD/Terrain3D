#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
bringup = (root / 'src/instances/terrain_3d_instancer_bringup.h').read_text()
instancer = (root / 'src/instances/terrain_3d_instancer.cpp').read_text()
constants = (root / 'src/Terrain3DConstants.h').read_text()
plugin = (root / 'project/addons/terrain_3d/src/editor_plugin.gd').read_text()

def fail(msg):
    print('Instancer authoritative-store publication verification FAIL:', msg)
    sys.exit(1)

if '#define T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT 1' not in bringup:
    fail('authoritative-store RD snapshot mode is not enabled')

start = instancer.find('bool Terrain3DInstancer::buildRdInstanceSnapshot(')
end = instancer.find('\nVector2i Terrain3DInstancer::getCellForLocalPosition', start)
if start < 0 or end < 0:
    fail('could not isolate buildRdInstanceSnapshot')
body = instancer[start:end]
branch_start = body.find('#if T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT')
branch_end = body.find('#elif T3D_INSTANCER_MINIMAL_RENDER_PATH', branch_start)
if branch_start < 0 or branch_end < 0:
    fail('authoritative-store snapshot branch is missing')
branch = body[branch_start:branch_end]

for token in (
    'getRegionPtrMapInternal()',
    'getResidentInstanceStoreInternal()',
    'pStore->meshes()',
    'sourceCell.m_transforms.size()',
    'sourceCell.getTransform( iInstance )',
    'transform.origin += regionOffset',
    't3dPackRdInstanceRecord( transform, color )',
    'transform.xform( sourceAabb )',
    'cell.m_nLod = 0',
    'cell.m_fLodBegin = 0.0f',
    'cell.m_fLodEnd = 0.0f',
    'cell.m_nVisibilityLayers = UINT32_MAX',
):
    if token not in branch:
        fail(f'authoritative snapshot is missing {token}')

if 'RD authoritative-store snapshot found resident instance data but published zero renderable records' not in branch:
    fail('authoritative publication does not report resident-but-unpublishable instance data')

if 'm_renderCells.empty()' in branch or 'for ( size_t iCell = 0u; iCell < ( size_t )( ( m_renderCells ).size() )' in branch:
    fail('authoritative RD publication still depends on m_renderCells')
if 'source_cell->m_cachedAabb' in branch or 'sourceCell.m_cachedAabb' in branch:
    fail('authoritative RD publication still trusts cached cell AABBs')

update_start = instancer.find('void Terrain3DInstancer::updateMmis(')
update_end = instancer.find('\n// -----------------------------------------------------------------------------\n// Protected functions', update_start)
update = instancer[update_start:update_end]
if '#if T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT' not in update or 'markRdPublicationDirty();' not in update:
    fail('authoritative store mutations do not invalidate RD publication directly')

renderer = (root / 'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp').read_text()
if '#if T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT' not in renderer or 'const bool skip_instance_frustum = true;' not in renderer or 'const uint32_t packet_visibility_mask = UINT32_MAX;' not in renderer:
    fail('authoritative isolation path still depends on camera frustum/visibility-layer gates')

build_id = 'instance-local-active-storage-p2-2026-09-09'
if build_id not in constants or build_id not in plugin:
    fail('native/script build IDs are not synchronized')

print('Instancer authoritative-store publication verification PASS')
