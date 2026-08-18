#include "instances/terrain_3d_instance_store.h"

T3DInstanceStore::T3DInstanceStore( void ) {
}

T3DInstanceStore::~T3DInstanceStore( void ) {
}

T3DInstanceStore::T3DInstanceStore( const T3DInstanceStore &rOther ) :
		m_meshes( rOther.m_meshes ),
		m_layout( rOther.m_layout ) {
}

T3DInstanceStore::T3DInstanceStore( T3DInstanceStore &&rOther ) :
		m_meshes( eastl::move( rOther.m_meshes ) ),
		m_layout( rOther.m_layout ) {
}

T3DInstanceStore &T3DInstanceStore::operator=( const T3DInstanceStore &rOther ) {
	if ( this != &rOther ) {
		m_meshes = rOther.m_meshes;
		m_layout = rOther.m_layout;
	}
	return *this;
}

T3DInstanceStore &T3DInstanceStore::operator=( T3DInstanceStore &&rOther ) {
	if ( this != &rOther ) {
		m_meshes = eastl::move( rOther.m_meshes );
		m_layout = rOther.m_layout;
	}
	return *this;
}

bool T3DInstanceStore::cellLess( const t3dinstanceCell_t &a, const Vector2i &b ) {
	return a.location.x < b.x || ( a.location.x == b.x && a.location.y < b.y );
}

bool T3DInstanceStore::meshLess( const t3dinstanceMesh_t &a, const int b ) {
	return a.id < b;
}

size_t T3DInstanceStore::cellLowerBound(
	const eastl::vector<t3dinstanceCell_t> &cells,
	const Vector2i &location
) {
	return ( size_t ) ( eastl::lower_bound( cells.begin(), cells.end(), location, cellLess ) - cells.begin() );
}

namespace {
static bool terrain3dInstanceMeshIdLess( const t3dinstanceMesh_t &rA, const t3dinstanceMesh_t &rB ) {
	return rA.id < rB.id;
}
}

size_t T3DInstanceStore::findCellIndex( const t3dinstanceMesh_t &mesh, const Vector2i &location ) {
	const size_t cell_index = cellLowerBound( mesh.cells, location );
	return cell_index < mesh.cells.size() && mesh.cells[cell_index].location == location ? cell_index : mesh.cells.size();
}

size_t T3DInstanceStore::meshLowerBound( const int nMeshId ) const {
	return ( size_t ) ( eastl::lower_bound( m_meshes.begin(), m_meshes.end(), nMeshId, meshLess ) - m_meshes.begin() );
}

void T3DInstanceStore::clear( void ) {
	m_meshes.clear();
	m_meshes.shrink_to_fit();
}

void T3DInstanceStore::compactStorage( void ) {
	for ( t3dinstanceMesh_t &mesh : m_meshes ) {
		for ( t3dinstanceCell_t &cell : mesh.cells ) {
			cell.compactStorage();
		}
		mesh.cells.shrink_to_fit();
	}
	m_meshes.shrink_to_fit();
}

const t3dinstanceMesh_t *T3DInstanceStore::findMesh( const int nMeshId ) const {
	const size_t mesh_index = meshLowerBound( nMeshId );
	if ( mesh_index >= m_meshes.size() || m_meshes[mesh_index].id != nMeshId ) {
		return nullptr;
	}
	return &m_meshes[mesh_index];
}

t3dinstanceMesh_t *T3DInstanceStore::findMesh( const int nMeshId ) {
	const size_t mesh_index = meshLowerBound( nMeshId );
	if ( mesh_index >= m_meshes.size() || m_meshes[mesh_index].id != nMeshId ) {
		return nullptr;
	}
	return &m_meshes[mesh_index];
}

t3dinstanceMesh_t &T3DInstanceStore::getOrCreateMesh( const int nMeshId, const size_t nReserveCells ) {
	const size_t mesh_index = meshLowerBound( nMeshId );
	if ( mesh_index >= m_meshes.size() || m_meshes[mesh_index].id != nMeshId ) {
		t3dinstanceMesh_t mesh;
		mesh.id = nMeshId;
		mesh.cells.reserve( nReserveCells );
		m_meshes.insert( m_meshes.begin() + mesh_index, eastl::move( mesh ) );
	} else if ( nReserveCells > m_meshes[mesh_index].cells.capacity() ) {
		m_meshes[mesh_index].cells.reserve( nReserveCells );
	}
	return m_meshes[mesh_index];
}

bool T3DInstanceStore::eraseMesh( const int nMeshId ) {
	const size_t mesh_index = meshLowerBound( nMeshId );
	if ( mesh_index >= m_meshes.size() || m_meshes[mesh_index].id != nMeshId ) {
		return false;
	}
	m_meshes.erase( m_meshes.begin() + mesh_index );
	return true;
}

void T3DInstanceStore::swapMeshIds( const int nSrcId, const int nDstId ) {
	t3dinstanceMesh_t *pSrc = findMesh( nSrcId );
	t3dinstanceMesh_t *pDst = findMesh( nDstId );
	if ( pSrc != nullptr ) {
		pSrc->id = nDstId;
	}
	if ( pDst != nullptr ) {
		pDst->id = nSrcId;
	}
	eastl::sort( m_meshes.begin(), m_meshes.end(), terrain3dInstanceMeshIdLess );
}

const t3dinstanceCell_t *T3DInstanceStore::findCell( const int nMeshId, const Vector2i &cell ) const {
	const t3dinstanceMesh_t *pMesh = findMesh( nMeshId );
	if ( pMesh == nullptr ) {
		return nullptr;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->cells.size() ? &pMesh->cells[cell_index] : nullptr;
}

t3dinstanceCell_t *T3DInstanceStore::findCell( const int nMeshId, const Vector2i &cell ) {
	t3dinstanceMesh_t *pMesh = findMesh( nMeshId );
	if ( pMesh == nullptr ) {
		return nullptr;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->cells.size() ? &pMesh->cells[cell_index] : nullptr;
}

t3dinstanceCell_t &T3DInstanceStore::getOrCreateCell(
	const int nMeshId, const Vector2i &cell,
	const size_t reserve_instances, const bool bColorData, const bool bOwnerData
) {
	t3dinstanceMesh_t &mesh = getOrCreateMesh( nMeshId, 4 );
	size_t cell_index = findCellIndex( mesh, cell );
	if ( cell_index >= mesh.cells.size() ) {
		cell_index = cellLowerBound( mesh.cells, cell );
		t3dinstanceCell_t value;
		value.location = cell;
		value.reserve( reserve_instances, bColorData, bOwnerData );
		mesh.cells.insert( mesh.cells.begin() + cell_index, eastl::move( value ) );
	} else {
		mesh.cells[cell_index].reserve( reserve_instances, bColorData, bOwnerData );
	}
	return mesh.cells[cell_index];
}

bool T3DInstanceStore::eraseCell( const int nMeshId, const Vector2i &cell ) {
	t3dinstanceMesh_t *pMesh = findMesh( nMeshId );
	if ( pMesh == nullptr ) {
		return false;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	if ( cell_index >= pMesh->cells.size() ) {
		return false;
	}
	pMesh->cells.erase( pMesh->cells.begin() + cell_index );
	if ( pMesh->cells.empty() ) {
		eraseMesh( nMeshId );
	}
	return true;
}

void T3DInstanceStore::importDictionary( const Dictionary &instances ) {
	clear();
	const Array mesh_ids = instances.keys();
	m_meshes.reserve( mesh_ids.size() );
	for ( int mesh_index = 0; mesh_index < mesh_ids.size(); ++mesh_index ) {
		const Variant mesh_variant = mesh_ids[mesh_index];
		const int mesh_id = ( int ) ( mesh_variant );
		const Dictionary cell_dict = instances[mesh_variant];
		const Array cell_keys = cell_dict.keys();
		getOrCreateMesh( mesh_id, cell_keys.size() );
		for ( int cell_index = 0; cell_index < cell_keys.size(); ++cell_index ) {
			const Variant cell_variant = cell_keys[cell_index];
			const Vector2i location = cell_variant;
			if ( !isValidCell( location ) ) {
				continue;
			}
			const Array triple = cell_dict[cell_variant];
			if ( triple.size() < 2 ) {
				continue;
			}
			const TypedArray<Transform3D> xforms = triple[0];
			const PackedColorArray source_colors = triple[1];
			PackedInt32Array source_owners;
			if ( triple.size() > 3 && Variant( triple[3] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				source_owners = triple[3];
			}
			bool bHasNonwhite = false;
			for ( int color_index = 0; color_index < source_colors.size(); ++color_index ) {
				if ( source_colors[color_index] != Color( 1, 1, 1, 1 ) ) {
					bHasNonwhite = true;
					break;
				}
			}
			bool bHasOwnerData = false;
			for ( int owner_index = 0; owner_index < source_owners.size(); ++owner_index ) {
				if ( source_owners[owner_index] != 0 ) {
					bHasOwnerData = true;
					break;
				}
			}
			t3dinstanceCell_t &cell =
				getOrCreateCell( mesh_id, location, xforms.size(), bHasNonwhite, bHasOwnerData );
			cell.transforms.resize( xforms.size() );
			for ( int transform_index = 0; transform_index < xforms.size(); ++transform_index ) {
				cell.transforms[( size_t ) ( transform_index )] = xforms[transform_index];
			}
			if ( bHasNonwhite ) {
				cell.colors.resize( xforms.size(), TERRAIN3D_INSTANCE_WHITE_RGBA8 );
				const int color_count = eastl::min( source_colors.size(), xforms.size() );
				for ( int color_index = 0; color_index < color_count; ++color_index ) {
					cell.setColor( ( size_t ) ( color_index ), source_colors[color_index] );
				}
			}
			if ( bHasOwnerData ) {
				cell.owners.resize( xforms.size(), 0u );
				const int owner_count = eastl::min( source_owners.size(), xforms.size() );
				for ( int owner_index = 0; owner_index < owner_count; ++owner_index ) {
					cell.owners[( size_t ) ( owner_index )] = ( uint32_t ) ( MAX( source_owners[owner_index], 0 ) );
				}
			}
			cell.bModified = true; // Import/undo replaces storage; force one GPU upload.
		}
	}
	compactStorage();
}

bool T3DInstanceStore::equalsDictionary( const Dictionary &instances ) const {
	if ( instances.size() != ( int ) ( m_meshes.size() ) ) {
		return false;
	}
	for ( size_t mesh_index = 0; mesh_index < m_meshes.size(); ++mesh_index ) {
		const t3dinstanceMesh_t &mesh = m_meshes[mesh_index];
		if ( !instances.has( mesh.id ) ) {
			return false;
		}
		const Dictionary cell_dict = instances[mesh.id];
		if ( cell_dict.size() != ( int ) ( mesh.cells.size() ) ) {
			return false;
		}
		for ( size_t cell_index = 0; cell_index < mesh.cells.size(); ++cell_index ) {
			const t3dinstanceCell_t &cell = mesh.cells[cell_index];
			if ( !cell_dict.has( cell.location ) ) {
				return false;
			}
			const Array triple = cell_dict[cell.location];
			if ( triple.size() < 2 ) {
				return false;
			}
			const TypedArray<Transform3D> transforms = triple[0];
			const PackedColorArray colors = triple[1];
			PackedInt32Array owners;
			if ( triple.size() > 3 && Variant( triple[3] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				owners = triple[3];
			}
			if ( transforms.size() != ( int ) ( cell.transforms.size() ) || colors.size() > transforms.size() ||
				owners.size() > transforms.size() ) {
				return false;
			}
			for ( int transform_index = 0; transform_index < transforms.size(); ++transform_index ) {
				const size_t index = ( size_t ) ( transform_index );
				if ( Transform3D( transforms[transform_index] ) != cell.transforms[index] ) {
					return false;
				}
				const Color expected = cell.getColor( index );
				const Color actual = transform_index < colors.size() ? colors[transform_index] : Color( 1, 1, 1, 1 );
				if ( t3dinstanceCell_t::packColor( actual ) != t3dinstanceCell_t::packColor( expected ) ) {
					return false;
				}
				const uint32_t expected_owner = cell.getOwner( index );
				const uint32_t actual_owner =
					transform_index < owners.size() ? ( uint32_t ) ( MAX( owners[transform_index], 0 ) ) : 0u;
				if ( actual_owner != expected_owner ) {
					return false;
				}
			}
		}
	}
	return true;
}

Dictionary T3DInstanceStore::exportDictionary( void ) const {
	Dictionary result;
	for ( size_t mesh_index = 0; mesh_index < m_meshes.size(); ++mesh_index ) {
		const t3dinstanceMesh_t &mesh = m_meshes[mesh_index];
		Dictionary cells;
		for ( size_t cell_index = 0; cell_index < mesh.cells.size(); ++cell_index ) {
			const t3dinstanceCell_t &cell = mesh.cells[cell_index];
			TypedArray<Transform3D> xforms;
			xforms.resize( cell.transforms.size() );
			for ( size_t transform_index = 0; transform_index < cell.transforms.size(); ++transform_index ) {
				xforms[( int ) ( transform_index )] = cell.transforms[transform_index];
			}
			PackedColorArray colors;
			colors.resize( cell.transforms.size() );
			if ( cell.usesColors() ) {
				for ( size_t color_index = 0; color_index < cell.colors.size(); ++color_index ) {
					colors.set( ( int ) ( color_index ), cell.getColor( color_index ) );
				}
			} else {
				colors.fill( Color( 1, 1, 1, 1 ) );
			}
			Array triple;
			triple.resize( cell.usesOwners() ? 4 : 3 );
			triple[0] = xforms;
			triple[1] = colors;
			triple[2] = cell.bModified;
			if ( cell.usesOwners() ) {
				PackedInt32Array owners;
				owners.resize( cell.transforms.size() );
				for ( size_t owner_index = 0; owner_index < cell.transforms.size(); ++owner_index ) {
					owners.set( ( int ) ( owner_index ), ( int32_t ) ( cell.getOwner( owner_index ) ) );
				}
				triple[3] = owners;
			}
			cells[cell.location] = triple;
		}
		result[mesh.id] = cells;
	}
	return result;
}

size_t T3DInstanceStore::instanceCount( const int nMeshId ) const {
	size_t count = 0;
	if ( nMeshId >= 0 ) {
		const t3dinstanceMesh_t *pMesh = findMesh( nMeshId );
		if ( pMesh == nullptr ) {
			return 0;
		}
		for ( size_t cell_index = 0; cell_index < pMesh->cells.size(); ++cell_index ) {
			count += pMesh->cells[cell_index].transforms.size();
		}
		return count;
	}
	for ( size_t mesh_index = 0; mesh_index < m_meshes.size(); ++mesh_index ) {
		const t3dinstanceMesh_t &pMesh = m_meshes[mesh_index];
		for ( size_t cell_index = 0; cell_index < pMesh.cells.size(); ++cell_index ) {
			count += pMesh.cells[cell_index].transforms.size();
		}
	}
	return count;
}
size_t T3DInstanceStore::residentBytes( void ) const {
	size_t bytes = m_meshes.capacity() * sizeof( t3dinstanceMesh_t );
	for ( const t3dinstanceMesh_t &mesh : m_meshes ) {
		bytes += mesh.cells.capacity() * sizeof( t3dinstanceCell_t );
		for ( const t3dinstanceCell_t &cell : mesh.cells ) {
			bytes += cell.transforms.capacity() * sizeof( t3dpackedTransform_t );
			bytes += cell.colors.capacity() * sizeof( uint32_t );
			bytes += cell.owners.capacity() * sizeof( uint32_t );
		}
	}
	return bytes;
}
