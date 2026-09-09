#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / 'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp'
text = CPP.read_text(encoding='utf-8')

needle = '''} else if ( type == Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER ) {'''
pos = text.find(needle)
if pos < 0:
    print('FAIL: instance renderer indirect-buffer allocation branch missing')
    sys.exit(1)
block = text[pos:text.find('\n\t} else {', pos)]
if 'STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT' not in block:
    print('FAIL: instance indirect buffers are created as plain storage buffers')
    sys.exit(1)
if 'storage_buffer_create' not in block:
    print('FAIL: instance indirect-buffer allocation no longer uses RenderingDevice storage buffer creation')
    sys.exit(1)

# Both main and shadow command buffers must continue to flow through the typed branch.
required = [
    'ensureBuffer( m_hIndirectBuffer,',
    'Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER',
    'ensureBuffer( m_hShadowIndirectBuffer,',
    'draw_list_draw_indirect',
]
for item in required:
    if item not in text:
        print(f'FAIL: missing expected instance indirect path token: {item}')
        sys.exit(1)

print('PASS: instance main/shadow indirect buffers carry RenderingDevice indirect usage')
