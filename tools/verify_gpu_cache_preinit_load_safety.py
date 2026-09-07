#!/usr/bin/env python3
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
gpu = (root / 'src/streaming/terrain_3d_gpu_region_cache.cpp').read_text()
stream = (root / 'src/streaming/terrain_3d_streaming_integration.cpp').read_text()
errors=[]

m=re.search(r'int Terrain3DGpuRegionCache::calculateGpuRegionPhysicalLayers\([^\)]*\) const \{(.*?)\n\}', gpu, re.S)
if not m:
    errors.append('calculateGpuRegionPhysicalLayers body missing')
else:
    body=m.group(1)
    if 'int maximum_layers = s_nGpuCacheRuntimeMaxLayers;' not in body or 'if ( m_pTerrain ) {' not in body:
        errors.append('physical-layer calculation is not null-safe before Terrain3DData activation')

m=re.search(r'void Terrain3D::setGpuRegionCachePageLimit\([^\)]*\) \{(.*?)\n\}', stream, re.S)
if not m:
    errors.append('setGpuRegionCachePageLimit body missing')
else:
    body=m.group(1)
    if 'm_bInitialized && m_pData && effectiveGpuRegionCacheEnabled()' not in body:
        errors.append('serialized page-limit setter may reconfigure Terrain3DData before runtime activation')

if errors:
    print('FAIL:')
    for e in errors:
        print(' -',e)
    raise SystemExit(1)
print('PASS: GPU page-limit deserialization and pre-activation capacity calculation are load-safe')
