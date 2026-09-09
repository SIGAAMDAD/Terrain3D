#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
stream = (ROOT / 'src/streaming/terrain_3d_streaming_integration.cpp').read_text(encoding='utf-8')
header = (ROOT / 'src/terrain_3d.h').read_text(encoding='utf-8')
constants = (ROOT / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
terrain = (ROOT / 'src/terrain_3d.cpp').read_text(encoding='utf-8')

def fail(msg):
    print('FAIL:', msg)
    sys.exit(1)

start = stream.find('void Terrain3D::normalizeRuntimeTierLayout( const bool bSyncDetailLods ) {')
end = stream.find('\nvoid Terrain3D::applyRuntimeTierLayout', start)
if start < 0 or end < 0:
    fail('normalizeRuntimeTierLayout body not found')
body = stream[start:end]

if 'Clamped detailed terrain range to ' in body:
    fail('capacity path still contains destructive tier clamp warning')
if 'm_nRuntimeNearShaderLods = supported_lods' in body or 'm_nGpuRegionDetailLods = supported_lods' in body:
    fail('capacity path still mutates render/detail tier counts')
if 'required_pages > capacity' not in body or 'markMemoryBudgetDirty();' not in body:
    fail('capacity shortfall no longer requests owner memory-policy reevaluation')
if 'preserving the render-tier layout' not in body:
    fail('capacity shortfall diagnostic does not describe non-destructive behavior')
if '!m_bGpuRegionCapacityWarningActive' not in body:
    fail('capacity warning is not latched and can spam every normalization')
if 'effectiveGpuRegionCacheEnabled()' not in body:
    fail('capacity validation runs even when GPU paging is disabled')
if 'm_bGpuRegionCapacityWarningActive = false;' not in header:
    fail('capacity warning latch missing')
if '#define s_pszNativeBuildId' not in constants or not any(
    tag in constants for tag in ('p07-', 'p08-', 'p09-', 'p10-', 'p11-', 'instancer-minimal-', 'instancer-fault-', 'instancer-direct-', 'instancer-rd-port-', 'memory-budget-zone-containment-p1-', 'instance-local-active-storage-p2-', 'forward-color-blend-attachment-contract-fix-', 'instancer-authoritative-visible-index-fix-')
):
    fail('unique native build provenance marker missing')
if '[build] native_build_id=' not in terrain:
    fail('startup build provenance log missing')

print('PASS: GPU page capacity is non-destructive to render-tier layout and build provenance is explicit')
