#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
bringup = (root / 'src/instances/terrain_3d_instancer_bringup.h').read_text()
instancer = (root / 'src/instances/runtime/terrain_3d_instancer.cpp').read_text()
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

builder_h = (root / 'src/instances/storage/terrain_3d_instance_publication_builder.h').read_text()
builder = (root / 'src/instances/storage/terrain_3d_instance_publication_builder.cpp').read_text()

if 'Terrain3DInstancePublicationBuilder::build( context, rSnapshot, m_rdPublicationDiagnostics )' not in branch:
    fail('authoritative snapshot does not delegate to the immutable publication builder')
for token in (
    'getRegionPtrMapInternal()',
    'getResidentInstanceStoreInternalMut()',
    'pStore->meshesMut()',
    'source_cell.m_transforms.size()',
    'source_cell.getTransform( iInstance )',
    'transform.origin += region_offset',
    't3dInstancePublicationPackRecord( transform, color )',
):
    if token not in builder:
        fail(f'authoritative publication builder is missing {token}')

if 'm_renderCells' in builder:
    fail('authoritative RD publication builder depends on m_renderCells')
for token in (
    't3dInstancePublicationBuildConservativeSourceAabb',
    't3dInstancePublicationEnsureCellBounds',
    'rCell.m_nCachedAabbRevision == rCell.m_nRevision',
    'rCell.m_cachedSourceMeshAabb.is_equal_approx( rConservativeSourceAabb )',
    'rCell.m_cachedAabb',
    'm_nPackedRecordsReused',
    'm_bInstanceLayoutChanged',
):
    if token not in builder:
        fail(f'authoritative publication derived-cache contract is missing {token}')

if 'm_renderCells.empty()' in branch or 'for ( size_t iCell = 0u; iCell < ( size_t )( ( m_renderCells ).size() )' in branch:
    fail('authoritative RD publication still depends on m_renderCells')
if 'source_cell->m_cachedAabb' in branch or 'sourceCell.m_cachedAabb' in branch:
    fail('authoritative RD publication still trusts cached cell AABBs')

update_start = instancer.find('void Terrain3DInstancer::updateMmis(')
update_end = instancer.find('\n// -----------------------------------------------------------------------------\n// Protected functions', update_start)
update = instancer[update_start:update_end]
if '#if T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT' not in update or 'markRdPublicationDirty();' not in update:
    fail('authoritative store mutations do not invalidate RD publication directly')

renderer = (root / 'src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp').read_text()
if 'const bool skip_instance_frustum = false;' not in renderer or 'const uint32_t packet_visibility_mask = nVisibilityMask;' not in renderer:
    fail('authoritative CPU reference path does not apply camera frustum/visibility-layer gates')

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

print('Instancer authoritative-store publication verification PASS')
