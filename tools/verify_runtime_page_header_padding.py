#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
constants = (ROOT / 'src/Terrain3DConstants.h').read_text(encoding='utf-8')
store = (ROOT / 'src/streaming/runtime/terrain_3d_runtime_page_store.cpp').read_text(encoding='utf-8')

def macro(name: str) -> int:
    m = re.search(rf'#define\s+{re.escape(name)}\s+\(\s*(\d+)u\s*\)', constants)
    if not m:
        raise SystemExit(f'FAIL: unable to read {name}')
    return int(m.group(1))

header_bytes = macro('s_nRuntimePageHeaderBytes')
channel_count = macro('s_nRuntimePageChannelCount')
alignment = macro('s_nRuntimePageAlignment')
serialized_bytes = 40 + channel_count * 48 + 44

if header_bytes != 1248:
    raise SystemExit(f'FAIL: v7 header envelope changed unexpectedly: {header_bytes}')
if serialized_bytes != 1236:
    raise SystemExit(f'FAIL: serialized v7 field bytes changed unexpectedly: {serialized_bytes}')
if header_bytes - serialized_bytes != 12:
    raise SystemExit('FAIL: expected 12 bytes of reserved v7 header padding')
if header_bytes % alignment != 0:
    raise SystemExit('FAIL: runtime-page header is no longer payload-aligned')
if 'required_bytes = 40u + ( ( size_t )( s_nRuntimePageChannelCount ) * 48u ) + 44u' not in store:
    raise SystemExit('FAIL: encoder does not guard the concrete serialized header field size')
if 'memset( pWrite, 0, s_nRuntimePageHeaderBytes - written_bytes );' not in store:
    raise SystemExit('FAIL: encoder does not explicitly write reserved header padding')
if 'return ( size_t )( pWrite - pBegin ) == s_nRuntimePageHeaderBytes;' in store:
    raise SystemExit('FAIL: old impossible 1236 == 1248 encoder gate remains')
if 'runtime page header/final byte serialization failed' not in store:
    raise SystemExit('FAIL: final header assembly is still collapsed into the channel-staging error')

print('PASS: runtime-page v7 header keeps 1236 field bytes plus 12 explicit alignment padding bytes')
