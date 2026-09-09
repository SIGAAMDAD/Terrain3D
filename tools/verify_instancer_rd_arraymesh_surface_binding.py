from pathlib import Path

source = Path('src/rendering/rd/terrain_3d_rd_mesh_cache.cpp').read_text()
constants = Path('src/Terrain3DConstants.h').read_text()

required = [
    '#include <godot_cpp/classes/array_mesh.hpp>',
    'Object::cast_to<ArrayMesh>( rMesh.ptr() )',
    'pArrayMesh->surface_get_primitive_type( surface )',
    'pArrayMesh->surface_get_array_len( surface )',
    'pArrayMesh->surface_get_array_index_len( surface )',
    'RD static mesh source is not an ArrayMesh',
    'RD static mesh source produced no usable triangle surfaces',
    'instancer-rd-arraymesh-surface-binding-p0-2026-09-09',
]

for token in required[:-1]:
    if token not in source:
        raise SystemExit(f'MISSING: {token}')
if required[-1] not in constants:
    raise SystemExit(f'MISSING: {required[-1]}')

for forbidden in (
    'rMesh->_surface_get_primitive_type(',
    'mesh->_surface_get_primitive_type(',
    'mesh->_surface_get_array_len(',
    'mesh->_surface_get_array_index_len(',
    'source.m_mesh->_surface_get_primitive_type(',
    'source.m_mesh->_surface_get_array_len(',
    'source.m_mesh->_surface_get_array_index_len(',
):
    if forbidden in source:
        raise SystemExit(f'FORBIDDEN: {forbidden}')

print('Instancer RD ArrayMesh surface binding verification PASS')
