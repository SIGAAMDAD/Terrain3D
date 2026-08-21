#include "core/terrain_3d_compiler.h"
#include "instances/terrain_3d_instance_store.h"

T3DInstanceStore::T3DInstanceStore( T3DInstanceStore &&rOther ) :
		m_meshes( eastl::move( rOther.m_meshes ) ),
		m_transformSlab( eastl::move( rOther.m_transformSlab ) ),
		m_colorSlab( eastl::move( rOther.m_colorSlab ) ),
		m_ownerSlab( eastl::move( rOther.m_ownerSlab ) ),
		m_layout( rOther.m_layout ) {
	rebindPayloadPools();
	rebuildMeshLookup();
	rOther.m_meshes.clear();
	rOther.m_transformSlab.reset();
	rOther.m_colorSlab.reset();
	rOther.m_ownerSlab.reset();
	rOther.rebuildMeshLookup();
}

T3DInstanceStore &T3DInstanceStore::operator=( const T3DInstanceStore &rOther ) {
	if ( this != &rOther ) copyFrom( rOther );
	return *this;
}

T3DInstanceStore &T3DInstanceStore::operator=( T3DInstanceStore &&rOther ) {
	if ( this == &rOther ) return *this;
	m_meshes = eastl::move( rOther.m_meshes );
	m_transformSlab = eastl::move( rOther.m_transformSlab );
	m_colorSlab = eastl::move( rOther.m_colorSlab );
	m_ownerSlab = eastl::move( rOther.m_ownerSlab );
	m_layout = rOther.m_layout;
	rebindPayloadPools();
	rebuildMeshLookup();
	rOther.m_meshes.clear();
	rOther.m_transformSlab.reset();
	rOther.m_colorSlab.reset();
	rOther.m_ownerSlab.reset();
	rOther.rebuildMeshLookup();
	return *this;
}

void T3DInstanceStore::rebindPayloadPools( void ) {
	for ( t3dinstanceMesh_t &mesh : m_meshes ) {
		for ( t3dinstanceCell_t &cell : mesh.cells ) cell.rebindPayloadPools( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
	}
}

void T3DInstanceStore::copyFrom( const T3DInstanceStore &rOther ) {
	clearRetainCapacity();
	m_layout = rOther.m_layout;
	size_t transform_count = 0u;
	size_t color_count = 0u;
	size_t owner_count = 0u;
	for ( const t3dinstanceMesh_t &source_mesh : rOther.m_meshes ) {
		for ( const t3dinstanceCell_t &source_cell : source_mesh.cells ) {
			transform_count += source_cell.transforms.size();
			color_count += source_cell.colors.size();
			owner_count += source_cell.owners.size();
		}
	}
	reserveMeshCapacity( rOther.m_meshes.size() );
	reservePayloadCapacity( transform_count, color_count, owner_count );
	for ( const t3dinstanceMesh_t &source_mesh : rOther.m_meshes ) {
		t3dinstanceMesh_t &destination_mesh = getOrCreateMesh( source_mesh.id, source_mesh.cells.size() );
		for ( const t3dinstanceCell_t &source_cell : source_mesh.cells ) {
			t3dinstanceCell_t &destination_cell = getOrCreateCellExact( source_mesh.id, source_cell.location, source_cell.transforms.size(), source_cell.usesColors(), source_cell.usesOwners() );
			destination_cell.transforms.resize( source_cell.transforms.size() );
			if ( !source_cell.transforms.empty() ) eastl::copy_n( source_cell.transforms.data(), source_cell.transforms.size(), destination_cell.transforms.data() );
			if ( source_cell.usesColors() ) { destination_cell.colors.resize( source_cell.colors.size() ); eastl::copy_n( source_cell.colors.data(), source_cell.colors.size(), destination_cell.colors.data() ); }
			if ( source_cell.usesOwners() ) { destination_cell.owners.resize( source_cell.owners.size() ); eastl::copy_n( source_cell.owners.data(), source_cell.owners.size(), destination_cell.owners.data() ); }
			destination_cell.nRevision = source_cell.nRevision;
			destination_cell.bModified = source_cell.bModified;
			destination_cell.cachedAabb = source_cell.cachedAabb;
			destination_cell.cachedSourceMeshAabb = source_cell.cachedSourceMeshAabb;
			destination_cell.nCachedAabbRevision = source_cell.nCachedAabbRevision;
			destination_cell.nCachedAabbGeneration = source_cell.nCachedAabbGeneration;
			destination_cell.bCachedAabbValid = source_cell.bCachedAabbValid;
		}
	}
	rebuildMeshLookup();
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
EA_FORCE_INLINE static bool terrain3dInstanceMeshIdLess( const t3dinstanceMesh_t &rA, const t3dinstanceMesh_t &rB ) { return rA.id < rB.id; }
}

size_t T3DInstanceStore::findCellIndex( const t3dinstanceMesh_t &mesh, const Vector2i &location ) {
	const size_t cell_index = cellLowerBound( mesh.cells, location );
	return cell_index < mesh.cells.size() && mesh.cells[cell_index].location == location ? cell_index : mesh.cells.size();
}

size_t T3DInstanceStore::meshLowerBound( const int nMeshId ) const {
	return ( size_t ) ( eastl::lower_bound( m_meshes.begin(), m_meshes.end(), nMeshId, meshLess ) - m_meshes.begin() );
}

void T3DInstanceStore::rebuildMeshLookup( void ) const {
	for ( uint16_t &slot : m_szMeshSlots ) slot = INVALID_MESH_SLOT;
	for ( size_t i = 0u; i < m_meshes.size(); ++i ) { const int id = m_meshes[i].id; if ( id >= 0 && id < TERRAIN3D_MAX_MESH_ASSETS && i < INVALID_MESH_SLOT ) m_szMeshSlots[id] = ( uint16_t ) i; }
	m_bMeshLookupDirty = false;
}

void T3DInstanceStore::clearRetainCapacity( void ) {
	m_meshes.clear();
	m_transformSlab.reset();
	m_colorSlab.reset();
	m_ownerSlab.reset();
	invalidateMeshLookup();
}

void T3DInstanceStore::releaseStorage( void ) {
	m_meshes.clear();
	m_meshes.shrink_to_fit();
	m_transformSlab.release();
	m_colorSlab.release();
	m_ownerSlab.release();
	rebuildMeshLookup();
}

void T3DInstanceStore::compactStorage( void ) {
	T3DHighWaterSlab<t3dpackedTransform_t> compact_transforms;
	T3DHighWaterSlab<uint32_t> compact_colors;
	T3DHighWaterSlab<uint32_t> compact_owners;
	for ( t3dinstanceMesh_t &mesh : m_meshes ) {
		for ( t3dinstanceCell_t &cell : mesh.cells ) cell.relocatePayloadsExact( &compact_transforms, &compact_colors, &compact_owners );
		mesh.cells.shrink_to_fit();
	}
	compact_transforms.compactBacking();
	compact_colors.compactBacking();
	compact_owners.compactBacking();
	m_transformSlab.swap( compact_transforms );
	m_colorSlab.swap( compact_colors );
	m_ownerSlab.swap( compact_owners );
	rebindPayloadPools();
	m_meshes.shrink_to_fit();
}

const t3dinstanceMesh_t *T3DInstanceStore::findMesh( const int nMeshId ) const {
	if ( m_bMeshLookupDirty ) rebuildMeshLookup();
	if ( nMeshId >= 0 && nMeshId < TERRAIN3D_MAX_MESH_ASSETS ) { const uint16_t slot = m_szMeshSlots[nMeshId]; return slot != INVALID_MESH_SLOT && slot < m_meshes.size() ? &m_meshes[slot] : nullptr; }
	const size_t mesh_index = meshLowerBound( nMeshId ); return mesh_index < m_meshes.size() && m_meshes[mesh_index].id == nMeshId ? &m_meshes[mesh_index] : nullptr;
}

t3dinstanceMesh_t *T3DInstanceStore::findMesh( const int nMeshId ) {
	if ( m_bMeshLookupDirty ) rebuildMeshLookup();
	if ( nMeshId >= 0 && nMeshId < TERRAIN3D_MAX_MESH_ASSETS ) { const uint16_t slot = m_szMeshSlots[nMeshId]; return slot != INVALID_MESH_SLOT && slot < m_meshes.size() ? &m_meshes[slot] : nullptr; }
	const size_t mesh_index = meshLowerBound( nMeshId ); return mesh_index < m_meshes.size() && m_meshes[mesh_index].id == nMeshId ? &m_meshes[mesh_index] : nullptr;
}

t3dinstanceMesh_t &T3DInstanceStore::getOrCreateMesh( const int nMeshId, const size_t nReserveCells ) {
	const size_t mesh_index = meshLowerBound( nMeshId );
	if ( mesh_index >= m_meshes.size() || m_meshes[mesh_index].id != nMeshId ) {
		t3dinstanceMesh_t mesh;
		mesh.id = nMeshId;
		mesh.cells.reserve( nReserveCells );
		m_meshes.insert( m_meshes.begin() + mesh_index, eastl::move( mesh ) );
		rebuildMeshLookup();
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
	m_meshes.erase( m_meshes.begin() + mesh_index ); rebuildMeshLookup();
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
	eastl::sort( m_meshes.begin(), m_meshes.end(), terrain3dInstanceMeshIdLess ); rebuildMeshLookup();
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
		bindCellPayloads( value );
		value.reserve( reserve_instances, bColorData, bOwnerData );
		mesh.cells.insert( mesh.cells.begin() + cell_index, eastl::move( value ) );
	} else {
		mesh.cells[cell_index].reserve( reserve_instances, bColorData, bOwnerData );
	}
	return mesh.cells[cell_index];
}

t3dinstanceCell_t &T3DInstanceStore::getOrCreateCellExact(
	const int nMeshId, const Vector2i &cell, const size_t reserve_instances,
	const bool bColorData, const bool bOwnerData
) {
	t3dinstanceMesh_t &mesh = getOrCreateMesh( nMeshId, 4 );
	size_t cell_index = findCellIndex( mesh, cell );
	if ( cell_index >= mesh.cells.size() ) {
		cell_index = cellLowerBound( mesh.cells, cell );
		t3dinstanceCell_t value;
		value.location = cell;
		bindCellPayloads( value );
		value.reserveExact( reserve_instances, bColorData, bOwnerData );
		mesh.cells.insert( mesh.cells.begin() + cell_index, eastl::move( value ) );
	} else {
		mesh.cells[cell_index].reserveExact( reserve_instances, bColorData, bOwnerData );
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
	clearRetainCapacity();
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
	rebuildMeshLookup();
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
	for ( const t3dinstanceMesh_t &mesh : m_meshes ) bytes += mesh.cells.capacity() * sizeof( t3dinstanceCell_t );
	bytes += m_transformSlab.residentBytes();
	bytes += m_colorSlab.residentBytes();
	bytes += m_ownerSlab.residentBytes();
	return bytes;
}
