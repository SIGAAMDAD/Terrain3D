#include "instances/terrain_3d_instance_store.h"

#include "core/terrain_3d_compiler.h"

#include <EASTL/utility.h>
#include <assert.h>
#include <string.h>

Terrain3DInstanceStore::Terrain3DInstanceStore( Terrain3DInstanceStore &&rOther ) :
        m_transformSlab( eastl::move( rOther.m_transformSlab ) ),
        m_colorSlab( eastl::move( rOther.m_colorSlab ) ),
        m_ownerSlab( eastl::move( rOther.m_ownerSlab ) ),
        m_layout( rOther.m_layout ) {
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		m_szMeshes[nMeshId] = eastl::move( rOther.m_szMeshes[nMeshId] );
	}
	rebindCellStorage();
	rebuildActiveMeshIds();
	rOther.m_nActiveMeshCount = 0u;
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		rOther.m_szMeshes[nMeshId].m_nId = -1;
	}
}

Terrain3DInstanceStore &Terrain3DInstanceStore::operator=( const Terrain3DInstanceStore &rOther ) {
	if ( this != &rOther ) {
		Terrain3DInstanceStore replacement;
		if ( replacement.copyFrom( rOther ) ) {
			*this = eastl::move( replacement );
		}
	}
	return *this;
}

Terrain3DInstanceStore &Terrain3DInstanceStore::operator=( Terrain3DInstanceStore &&rOther ) {
	if ( this == &rOther ) {
		return *this;
	}
	shutdown();
	m_transformSlab = eastl::move( rOther.m_transformSlab );
	m_colorSlab = eastl::move( rOther.m_colorSlab );
	m_ownerSlab = eastl::move( rOther.m_ownerSlab );
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		m_szMeshes[nMeshId] = eastl::move( rOther.m_szMeshes[nMeshId] );
	}
	m_layout = rOther.m_layout;
	rebindCellStorage();
	rebuildActiveMeshIds();
	rOther.m_nActiveMeshCount = 0u;
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		rOther.m_szMeshes[nMeshId].m_nId = -1;
	}
	return *this;
}

void Terrain3DInstanceStore::rebuildActiveMeshIds( void ) {
	m_nActiveMeshCount = 0u;
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		if ( m_szMeshes[nMeshId].m_nId == nMeshId && m_nActiveMeshCount < s_nMaxMeshAssets ) {
			m_szActiveMeshIds[m_nActiveMeshCount++] = ( uint16_t )( nMeshId );
		}
	}
}

void Terrain3DInstanceStore::rebindCellStorage( void ) {
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		eastl::vector<t3dInstanceCell_t, Terrain3DEastlAllocator> &rCells = m_szMeshes[nMeshId].m_cells;
		for ( size_t iCell = 0u; iCell < rCells.size(); iCell++ ) {
			rCells[iCell].rebindStorage( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
		}
	}
}

bool Terrain3DInstanceStore::copyFrom( const Terrain3DInstanceStore &rOther ) {
	clearRetainCapacity();
	m_layout = rOther.m_layout;
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( rOther.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &source_mesh = ( rOther.meshes() )[iMesh];
		if ( source_mesh.m_nId < 0 || source_mesh.m_nId >= s_nMaxMeshAssets ) {
			clearRetainCapacity();
			return false;
		}
		t3dInstanceMesh_t &destination_mesh = m_szMeshes[source_mesh.m_nId];
		if ( !t3dContainerReserveCold( destination_mesh.m_cells, source_mesh.m_cells.size() ) ) {
			clearRetainCapacity();
			return false;
		}
		destination_mesh.m_nId = source_mesh.m_nId;
		for ( size_t iCell = 0u; iCell < ( size_t )( ( source_mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &source_cell = ( source_mesh.m_cells )[iCell];
			t3dInstanceCell_t destination_cell;
			destination_cell.bindStorage( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
			destination_cell.m_location = source_cell.m_location;
			if ( !destination_cell.reserveExact( source_cell.m_transforms.size(), source_cell.usesColors(), source_cell.usesOwners() )
			        || !destination_cell.m_transforms.resize( source_cell.m_transforms.size() ) ) {
				clearRetainCapacity();
				return false;
			}
			if ( !source_cell.m_transforms.empty() ) {
				memcpy( destination_cell.m_transforms.data(),
				        source_cell.m_transforms.data(),
				        source_cell.m_transforms.size() * sizeof( t3dPackedTransform_t ) );
			}
			if ( source_cell.usesColors() ) {
				if ( !destination_cell.m_colors.resize( source_cell.m_colors.size() ) ) {
					clearRetainCapacity();
					return false;
				}
				memcpy( destination_cell.m_colors.data(), source_cell.m_colors.data(), source_cell.m_colors.size() * sizeof( uint32_t ) );
			}
			if ( source_cell.usesOwners() ) {
				if ( !destination_cell.m_owners.resize( source_cell.m_owners.size() ) ) {
					clearRetainCapacity();
					return false;
				}
				memcpy( destination_cell.m_owners.data(), source_cell.m_owners.data(), source_cell.m_owners.size() * sizeof( uint32_t ) );
			}
			destination_cell.m_nRevision = source_cell.m_nRevision;
			destination_cell.m_bModified = source_cell.m_bModified;
			destination_cell.m_cachedAabb = source_cell.m_cachedAabb;
			destination_cell.m_cachedSourceMeshAabb = source_cell.m_cachedSourceMeshAabb;
			destination_cell.m_nCachedAabbRevision = source_cell.m_nCachedAabbRevision;
			destination_cell.m_nCachedAabbGeneration = source_cell.m_nCachedAabbGeneration;
			destination_cell.m_bCachedAabbValid = source_cell.m_bCachedAabbValid;
			if ( !t3dVectorPushBackWithinCapacity( destination_mesh.m_cells, eastl::move( destination_cell ) ) ) {
				clearRetainCapacity();
				return false;
			}
		}
	}
	rebuildActiveMeshIds();
	return true;
}

bool Terrain3DInstanceStore::cellLess( const t3dInstanceCell_t &a, const Vector2i &b ) {
	return a.m_location.x < b.x || ( a.m_location.x == b.x && a.m_location.y < b.y );
}

size_t Terrain3DInstanceStore::cellLowerBound(
        const eastl::vector<t3dInstanceCell_t, Terrain3DEastlAllocator> &cells, const Vector2i &location ) {
	size_t low = 0u;
	size_t high = cells.size();
	while ( low < high ) {
		const size_t middle = low + ( high - low ) / 2u;
		if ( cellLess( cells[middle], location ) ) {
			low = middle + 1u;
		} else {
			high = middle;
		}
	}
	return low;
}

size_t Terrain3DInstanceStore::findCellIndex( const t3dInstanceMesh_t &mesh, const Vector2i &location ) {
	const size_t cell_index = cellLowerBound( mesh.m_cells, location );
	return cell_index < mesh.m_cells.size() && mesh.m_cells[cell_index].m_location == location ? cell_index : mesh.m_cells.size();
}

void Terrain3DInstanceStore::reset( void ) {
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		m_szMeshes[nMeshId].m_cells.clear();
		m_szMeshes[nMeshId].m_nId = -1;
	}
	m_nActiveMeshCount = 0u;
	m_transformSlab.reset();
	m_colorSlab.reset();
	m_ownerSlab.reset();
}

void Terrain3DInstanceStore::shutdown( void ) {
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		t3dVectorRelease( m_szMeshes[nMeshId].m_cells );
		m_szMeshes[nMeshId].m_nId = -1;
	}
	m_nActiveMeshCount = 0u;
	m_transformSlab.release();
	m_colorSlab.release();
	m_ownerSlab.release();
}

void Terrain3DInstanceStore::compactStorage( void ) {
	// Build the compacted image off to the side. The old store remains fully
	// valid until every mesh/cell/payload copy succeeds, so allocation failure
	// cannot leave live cells rebound to partially constructed replacement slabs.
	Terrain3DInstanceStore compacted;
	if ( !compacted.copyFrom( *this ) ) {
		return;
	}
	*this = eastl::move( compacted );
}

const t3dInstanceMesh_t *Terrain3DInstanceStore::findMesh( const int nMeshId ) const {
	return nMeshId >= 0 && nMeshId < s_nMaxMeshAssets && m_szMeshes[nMeshId].m_nId == nMeshId ? &m_szMeshes[nMeshId] : NULL;
}

t3dInstanceMesh_t *Terrain3DInstanceStore::findMesh( const int nMeshId ) {
	return nMeshId >= 0 && nMeshId < s_nMaxMeshAssets && m_szMeshes[nMeshId].m_nId == nMeshId ? &m_szMeshes[nMeshId] : NULL;
}

t3dInstanceMesh_t &Terrain3DInstanceStore::getOrCreateMesh( const int nMeshId, const size_t nReserveCells ) {
	assert( nMeshId >= 0 && nMeshId < s_nMaxMeshAssets );
	const int mesh_id = CLAMP( nMeshId, 0, s_nMaxMeshAssets - 1 );
	t3dInstanceMesh_t &mesh = m_szMeshes[mesh_id];
	const bool bWasInactive = mesh.m_nId != mesh_id;
	if ( nReserveCells > mesh.m_cells.capacity() && !t3dContainerReserveCold( mesh.m_cells, nReserveCells ) ) {
		return mesh;
	}
	if ( bWasInactive ) {
		mesh.m_nId = mesh_id;
		rebuildActiveMeshIds();
	}
	return mesh;
}

bool Terrain3DInstanceStore::eraseMesh( const int nMeshId ) {
	t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
	if ( !pMesh ) {
		return false;
	}
	pMesh->m_cells.clear();
	pMesh->m_nId = -1;
	rebuildActiveMeshIds();
	return true;
}

void Terrain3DInstanceStore::swapMeshIds( const int nSrcId, const int nDstId ) {
	if ( nSrcId == nDstId || nSrcId < 0 || nDstId < 0 || nSrcId >= s_nMaxMeshAssets || nDstId >= s_nMaxMeshAssets ) {
		return;
	}
	eastl::swap( m_szMeshes[nSrcId], m_szMeshes[nDstId] );
	if ( m_szMeshes[nSrcId].m_nId >= 0 ) {
		m_szMeshes[nSrcId].m_nId = nSrcId;
	}
	if ( m_szMeshes[nDstId].m_nId >= 0 ) {
		m_szMeshes[nDstId].m_nId = nDstId;
	}
	rebuildActiveMeshIds();
}

const t3dInstanceCell_t *Terrain3DInstanceStore::findCell( const int nMeshId, const Vector2i &cell ) const {
	const t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
	if ( !pMesh ) {
		return NULL;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->m_cells.size() ? &pMesh->m_cells[cell_index] : NULL;
}

t3dInstanceCell_t *Terrain3DInstanceStore::findCell( const int nMeshId, const Vector2i &cell ) {
	t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
	if ( !pMesh ) {
		return NULL;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->m_cells.size() ? &pMesh->m_cells[cell_index] : NULL;
}

t3dInstanceCell_t &Terrain3DInstanceStore::getOrCreateCell(
        const int nMeshId, const Vector2i &cell, const size_t reserve_instances, const bool bColorData, const bool bOwnerData ) {
	t3dInstanceMesh_t &mesh = getOrCreateMesh( nMeshId, 4u );
	size_t cell_index = findCellIndex( mesh, cell );
	if ( cell_index >= mesh.m_cells.size() ) {
		cell_index = cellLowerBound( mesh.m_cells, cell );
		if ( mesh.m_cells.size() >= mesh.m_cells.capacity() ) {
			const size_t required = mesh.m_cells.size() + 1u;
			const size_t growth = MAX( 4u, mesh.m_cells.capacity() / 2u );
			if ( !t3dContainerReserveCold( mesh.m_cells, MAX( required, mesh.m_cells.capacity() + growth ) ) ) {
				assert( false && "Terrain3D instance cell capacity exhausted" );
				return mesh.m_cells[0];
			}
		}
		t3dInstanceCell_t value;
		value.bindStorage( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
		value.m_location = cell;
		value.reserve( reserve_instances, bColorData, bOwnerData );
		if ( !t3dVectorInsertWithinCapacity( mesh.m_cells, cell_index, eastl::move( value ) ) ) {
			assert( false && "Terrain3D instance cell insertion failed" );
		}
	} else {
		mesh.m_cells[cell_index].reserve( reserve_instances, bColorData, bOwnerData );
	}
	return mesh.m_cells[cell_index];
}

t3dInstanceCell_t &Terrain3DInstanceStore::getOrCreateCellExact(
        const int nMeshId, const Vector2i &cell, const size_t reserve_instances, const bool bColorData, const bool bOwnerData ) {
	t3dInstanceMesh_t &mesh = getOrCreateMesh( nMeshId, 4u );
	size_t cell_index = findCellIndex( mesh, cell );
	if ( cell_index >= mesh.m_cells.size() ) {
		cell_index = cellLowerBound( mesh.m_cells, cell );
		if ( mesh.m_cells.size() >= mesh.m_cells.capacity() ) {
			const size_t required = mesh.m_cells.size() + 1u;
			if ( !t3dContainerReserveCold( mesh.m_cells, required ) ) {
				assert( false && "Terrain3D exact instance cell capacity exhausted" );
				return mesh.m_cells[0];
			}
		}
		t3dInstanceCell_t value;
		value.bindStorage( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
		value.m_location = cell;
		value.reserveExact( reserve_instances, bColorData, bOwnerData );
		if ( !t3dVectorInsertWithinCapacity( mesh.m_cells, cell_index, eastl::move( value ) ) ) {
			assert( false && "Terrain3D exact instance cell insertion failed" );
		}
	} else {
		mesh.m_cells[cell_index].reserveExact( reserve_instances, bColorData, bOwnerData );
	}
	return mesh.m_cells[cell_index];
}

bool Terrain3DInstanceStore::eraseCell( const int nMeshId, const Vector2i &cell ) {
	t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
	if ( !pMesh ) {
		return false;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	if ( cell_index >= pMesh->m_cells.size() ) {
		return false;
	}
	t3dVectorEraseOrdered( pMesh->m_cells, cell_index );
	if ( pMesh->m_cells.empty() ) {
		eraseMesh( nMeshId );
	}
	return true;
}

void Terrain3DInstanceStore::importDictionary( const Dictionary &instances ) {
	clearRetainCapacity();
	const Array mesh_ids = instances.keys();
	for ( int mesh_index = 0; mesh_index < mesh_ids.size(); ++mesh_index ) {
		const Variant mesh_variant = mesh_ids[mesh_index];
		const int mesh_id = ( int )( mesh_variant );
		if ( mesh_id < 0 || mesh_id >= s_nMaxMeshAssets ) {
			continue;
		}
		const Dictionary cell_dict = instances[mesh_variant];
		const Array cell_keys = cell_dict.keys();
		getOrCreateMesh( mesh_id, ( size_t )( cell_keys.size() ) );
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
			t3dInstanceCell_t &target_cell = getOrCreateCell( mesh_id, location, ( size_t )( xforms.size() ), bHasNonwhite, bHasOwnerData );
			if ( !target_cell.m_transforms.resize( ( size_t )( xforms.size() ) ) ) {
				clearRetainCapacity();
				return;
			}
			for ( int transform_index = 0; transform_index < xforms.size(); ++transform_index ) {
				target_cell.setTransform( ( size_t )( transform_index ), xforms[transform_index] );
			}
			if ( bHasNonwhite ) {
				if ( !target_cell.m_colors.resize( ( size_t )( xforms.size() ) ) ) {
					clearRetainCapacity();
					return;
				}
				for ( size_t color_index = 0u; color_index < target_cell.m_colors.size(); ++color_index ) {
					target_cell.m_colors[color_index] = s_nInstanceWhiteRgba8;
				}
				const int color_count = MIN( source_colors.size(), xforms.size() );
				for ( int color_index = 0; color_index < color_count; ++color_index ) {
					target_cell.setColor( ( size_t )( color_index ), source_colors[color_index] );
				}
			}
			if ( bHasOwnerData ) {
				if ( !target_cell.m_owners.resize( ( size_t )( xforms.size() ) ) ) {
					clearRetainCapacity();
					return;
				}
				for ( size_t owner_index = 0u; owner_index < target_cell.m_owners.size(); ++owner_index ) {
					target_cell.m_owners[owner_index] = 0u;
				}
				const int owner_count = MIN( source_owners.size(), xforms.size() );
				for ( int owner_index = 0; owner_index < owner_count; ++owner_index ) {
					target_cell.m_owners[( size_t )( owner_index )] = ( uint32_t )( MAX( source_owners[owner_index], 0 ) );
				}
			}
			target_cell.m_bModified = true;
		}
	}
}

bool Terrain3DInstanceStore::equalsDictionary( const Dictionary &instances ) const {
	if ( instances.size() != ( int )( m_nActiveMeshCount ) ) {
		return false;
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( meshes() )[iMesh];
		if ( !instances.has( mesh.m_nId ) ) {
			return false;
		}
		const Dictionary cell_dict = instances[mesh.m_nId];
		if ( cell_dict.size() != ( int )( mesh.m_cells.size() ) ) {
			return false;
		}
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &instance_cell = ( mesh.m_cells )[iCell];
			if ( !cell_dict.has( instance_cell.m_location ) ) {
				return false;
			}
			const Array triple = cell_dict[instance_cell.m_location];
			if ( triple.size() < 2 ) {
				return false;
			}
			const TypedArray<Transform3D> transforms = triple[0];
			const PackedColorArray colors = triple[1];
			PackedInt32Array owners;
			if ( triple.size() > 3 && Variant( triple[3] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				owners = triple[3];
			}
			if ( transforms.size() != ( int )( instance_cell.m_transforms.size() ) || colors.size() > transforms.size()
			        || owners.size() > transforms.size() ) {
				return false;
			}
			for ( int transform_index = 0; transform_index < transforms.size(); ++transform_index ) {
				const size_t index = ( size_t )( transform_index );
				if ( Transform3D( transforms[transform_index] ) != instance_cell.getTransform( index ) ) {
					return false;
				}
				const Color expected = instance_cell.getColor( index );
				const Color actual = transform_index < colors.size() ? colors[transform_index] : Color( 1, 1, 1, 1 );
				if ( t3dInstanceCell_t::packColor( actual ) != t3dInstanceCell_t::packColor( expected ) ) {
					return false;
				}
				const uint32_t expected_owner = instance_cell.getOwner( index );
				const uint32_t actual_owner = transform_index < owners.size() ? ( uint32_t )( MAX( owners[transform_index], 0 ) ) : 0u;
				if ( actual_owner != expected_owner ) {
					return false;
				}
			}
		}
	}
	return true;
}

Dictionary Terrain3DInstanceStore::exportDictionary( void ) const {
	Dictionary result;
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( meshes() )[iMesh];
		Dictionary cells;
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( mesh.m_cells )[iCell];
			TypedArray<Transform3D> xforms;
			xforms.resize( ( int )( cell.m_transforms.size() ) );
			for ( size_t transform_index = 0; transform_index < cell.m_transforms.size(); ++transform_index ) {
				xforms[( int )( transform_index )] = cell.getTransform( transform_index );
			}
			PackedColorArray colors;
			colors.resize( ( int )( cell.m_transforms.size() ) );
			if ( cell.usesColors() ) {
				for ( size_t color_index = 0; color_index < cell.m_colors.size(); ++color_index ) {
					colors.set( ( int )( color_index ), cell.getColor( color_index ) );
				}
			} else {
				colors.fill( Color( 1, 1, 1, 1 ) );
			}
			Array triple;
			triple.resize( cell.usesOwners() ? 4 : 3 );
			triple[0] = xforms;
			triple[1] = colors;
			triple[2] = cell.m_bModified;
			if ( cell.usesOwners() ) {
				PackedInt32Array owners;
				owners.resize( ( int )( cell.m_transforms.size() ) );
				for ( size_t owner_index = 0; owner_index < cell.m_transforms.size(); ++owner_index ) {
					owners.set( ( int )( owner_index ), ( int32_t )( cell.getOwner( owner_index ) ) );
				}
				triple[3] = owners;
			}
			cells[cell.m_location] = triple;
		}
		result[mesh.m_nId] = cells;
	}
	return result;
}

size_t Terrain3DInstanceStore::instanceCount( const int nMeshId ) const {
	size_t count = 0u;
	if ( nMeshId >= 0 ) {
		const t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
		if ( !pMesh ) {
			return 0u;
		}
		for ( size_t iCell = 0u; iCell < ( size_t )( ( pMesh->m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( pMesh->m_cells )[iCell];
			count += cell.m_transforms.size();
		}
		return count;
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( meshes() )[iMesh];
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( mesh.m_cells )[iCell];
			count += cell.m_transforms.size();
		}
	}
	return count;
}

size_t Terrain3DInstanceStore::residentBytes( void ) const {
	size_t nBytes = sizeof( m_szMeshes ) + sizeof( m_szActiveMeshIds ) + sizeof( m_nActiveMeshCount );
	for ( int nMeshId = 0; nMeshId < s_nMaxMeshAssets; nMeshId++ ) {
		nBytes += t3dContainerResidentBytes( m_szMeshes[nMeshId].m_cells );
	}
	nBytes += m_transformSlab.residentBytes();
	nBytes += m_colorSlab.residentBytes();
	nBytes += m_ownerSlab.residentBytes();
	return nBytes;
}
