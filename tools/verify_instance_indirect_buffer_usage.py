#!/usr/bin/env python3
"""Check the shared Instance indirect arena's allocation and offset paths."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8")
GLSL = (ROOT / "project/addons/terrain_3d/rd/terrain_instance_cull.glsl").read_text(encoding="utf-8")
SERVICE = (ROOT / "src/rendering/rd/infrastructure/terrain_3d_rd_resource_service.h").read_text(encoding="utf-8")

required_cpp = (
    'replaceBuffer( m_commandTable.m_hIndirectBuffer, m_commandTable.m_nIndirectArenaCapacityBytes,',
    '"T3D.Instance.IndirectArena", Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER,',
    'RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT',
    'm_commandTable.m_hShadowIndirectBuffer = m_commandTable.m_hIndirectBuffer;',
    'm_commandTable.m_nMainIndirectOffsetBytes = 0u;',
    'm_commandTable.m_nShadowIndirectOffsetBytes = ( uint32_t )aligned;',
    'push.m_nMainCommandBase = m_commandTable.m_nMainIndirectOffsetBytes / sizeof( t3dRdIndexedIndirectCommand_t );',
    'push.m_nShadowCommandBase = m_commandTable.m_nShadowIndirectOffsetBytes / sizeof( t3dRdIndexedIndirectCommand_t );',
    'updatePacked( m_commandTable.m_hIndirectBuffer, m_commandTable.m_nMainIndirectOffsetBytes, indirect_bytes',
    'updatePacked( m_commandTable.m_hShadowIndirectBuffer, m_commandTable.m_nShadowIndirectOffsetBytes, indirect_bytes',
    'updatePacked( m_commandTable.m_hShadowIndirectBuffer, m_commandTable.m_nShadowIndirectOffsetBytes, shadow_indirect_bytes',
    'bShadowPass ? m_commandTable.m_nShadowIndirectOffsetBytes : m_commandTable.m_nMainIndirectOffsetBytes, indirect_bytes',
    'shadow_pass ? m_commandTable.m_nShadowIndirectOffsetBytes : m_commandTable.m_nMainIndirectOffsetBytes,',
    '( shadow_pass ? m_commandTable.m_nShadowIndirectOffsetBytes : m_commandTable.m_nMainIndirectOffsetBytes ) + i * sizeof( t3dRdIndexedIndirectCommand_t )',
    'm_context.freeResource( m_commandTable.m_hIndirectBuffer, m_commandTable.m_nIndirectArenaCapacityBytes );',
)
required_shader = (
    'main_commands[cull_data.main_command_base + command_index]',
    'shadow_commands[cull_data.shadow_command_base + command_index]',
    'shadow_commands[cull_data.shadow_command_base + descriptor.command_index]',
)
missing = [token for token in required_cpp if token not in CPP]
missing += [token for token in required_shader if token not in GLSL]
if 'storage_buffer_create( nBytes, PackedByteArray(), usage )' not in SERVICE:
    missing.append('indirect-capable storage creation in the resource service')
if missing:
    print('FAIL: missing Instance indirect arena contract: ' + ', '.join(missing))
    sys.exit(1)

print('PASS: Instance main/shadow commands share an indirect-capable arena with separate offsets')
