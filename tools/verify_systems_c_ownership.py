#!/usr/bin/env python3
from pathlib import Path
import re, sys
root=Path(__file__).resolve().parents[1]
src=root/'src'
errors=[]

def need(path, pattern, label):
    text=(src/path).read_text()
    if not re.search(pattern,text,re.S): errors.append(f'{path}: missing {label}')

def forbid(path, pattern, label):
    text=(src/path).read_text()
    if re.search(pattern,text,re.S): errors.append(f'{path}: forbidden {label}')

# Instance payload ownership must be slab-backed rather than per-cell vectors.
need(Path('instances/terrain_3d_instance_store.h'), r'Terrain3DSlabArray<uint32_t>\s+m_colors', 'slab-backed colors')
need(Path('instances/terrain_3d_instance_store.h'), r'Terrain3DSlabArray<uint32_t>\s+m_owners', 'slab-backed owners')
need(Path('instances/terrain_3d_instance_store.h'), r'Terrain3DHighWaterSlab<t3dPackedTransform_t>\s+m_transformSlab', 'shared transform slab')
forbid(Path('instances/terrain_3d_instance_store.h'), r'eastl::vector<\s*t3dPackedTransform_t', 'per-cell transform vector')

# RD publication must use explicit fixed-capacity POD storage.
need(Path('rendering/rd/terrain_3d_rd_instance_renderer.h'), r'Terrain3DFixedBuffer<t3dRdInstanceRecord_t>\s+m_instances', 'fixed instance publication')
need(Path('rendering/rd/terrain_3d_rd_instance_renderer.h'), r'Terrain3DFixedBuffer<t3dRdInstanceCell_t>\s+m_cells', 'fixed cell publication')

# Fixed buffer semantics: reset retains, shutdown frees; no ambiguous clear/swap aliases.
fixed=(src/'core/terrain_3d_fixed_buffer.h').read_text()
for token in ('void reset( void )','void shutdown( void )','memcpy(','memmove(','memset('):
    if token not in fixed: errors.append(f'core/terrain_3d_fixed_buffer.h: missing {token}')
for token in ('void clear( void )','void swap( Terrain3DFixedBuffer','void release( void )'):
    if token in fixed: errors.append(f'core/terrain_3d_fixed_buffer.h: ambiguous API retained: {token}')

# Old unused generic containers stay deleted.
for name in ('terrain_3d_array.h','terrain_3d_containers.h','terrain_3d_fixed_bitset.h','terrain_3d_flat_hash.h'):
    if (src/'core'/name).exists(): errors.append(f'core/{name}: obsolete custom container still exists')

# Pointer/count mesh-cache boundary.
need(Path('rendering/rd/terrain_3d_rd_mesh_cache.h'), r'const t3dRdExternalMeshSource_t \*pExternalMeshes,\s*size_t nExternalMeshCount', 'external mesh pointer/count API')
need(Path('instances/terrain_3d_instancer.h'), r'const Transform3D \*pTransforms,\s*size_t nTransformCount,\s*const Color \*pColors,\s*size_t nColorCount', 'placement pointer/count API')

if errors:
    print('\n'.join(errors)); print(f'FAIL: {len(errors)} ownership/layout violations'); sys.exit(1)
print('PASS: Terrain3D explicit ownership/layout gates')
