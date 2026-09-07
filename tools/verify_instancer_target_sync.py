#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
terrain = (root / 'src/terrain_3d.cpp').read_text(encoding='utf-8')
integration = (root / 'src/rendering/terrain_3d_render_integration.cpp').read_text(encoding='utf-8')
instancer = (root / 'src/instances/terrain_3d_instancer.cpp').read_text(encoding='utf-8')
header = (root / 'src/instances/terrain_3d_instancer.h').read_text(encoding='utf-8')
policy = (root / 'src/streaming/terrain_3d_streaming_policy.h').read_text(encoding='utf-8')

errors = []

if 'return bConfiguredEnabled && !t3dIsEditorAuthoringMode' not in policy:
    errors.append('editor Authoring streaming policy contract changed; re-audit this regression gate')

physics_start = terrain.find('void Terrain3D::physicsProcess')
physics_end = terrain.find('\n}', physics_start)
physics = terrain[physics_start:physics_end + 2] if physics_start >= 0 and physics_end >= 0 else ''
if 'bInstanceTargetTracking = bInstanceCellStreaming || m_bInstancerSingleLodBuffers' not in physics:
    errors.append('physicsProcess does not service target tracking when only single-LOD instance buffers are active')
if 'm_pInstancer->updateStreaming( clipmap_target_position )' not in physics:
    errors.append('physicsProcess does not feed the resolved frame target to the instancer')

sync_start = integration.find('void Terrain3D::synchronizeClipmapNow')
sync_end = integration.find('\n}', sync_start)
sync = integration[sync_start:sync_end + 2] if sync_start >= 0 and sync_end >= 0 else ''
if 'm_pInstancer->synchronizeStreamingTarget( rTargetPosition )' not in sync:
    errors.append('camera/explicit-target synchronization does not resynchronize instance LOD selection')

method_start = instancer.find('void Terrain3DInstancer::synchronizeStreamingTarget')
method_end = instancer.find('\n}', method_start)
method = instancer[method_start:method_end + 2] if method_start >= 0 and method_end >= 0 else ''
if not method:
    errors.append('synchronizeStreamingTarget implementation is missing')
else:
    if 'm_lastStreamPosition = rTargetPosition' not in method or 'm_bStreamPositionValid = true' not in method:
        errors.append('instance target synchronization does not publish the authoritative target')
    if 'updateMmis( -1, s_v2iMax, false )' not in method:
        errors.append('instance target synchronization does not re-evaluate resident render cells')

if 'void synchronizeStreamingTarget( const Vector3 &rTargetPosition );' not in header:
    errors.append('synchronizeStreamingTarget declaration is missing')

if errors:
    for error in errors:
        print('FAIL:', error)
    sys.exit(1)
print('PASS: instance single-LOD target synchronization contract')
