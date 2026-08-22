#include "instances/terrain_3d_instance_store.h"

#include "core/terrain_3d_compiler.h"

template <typename T> static void t3dInstanceStoreCompactMoveVector( eastl::vector<T> &rValues ) {
	if ( rValues.size() == rValues.capacity() ) {
		return;
	}
	eastl::vector<T> compacted;
	compacted.reserve( rValues.size() );
	for ( T &value : rValues ) {
		compacted.push_back( eastl::move( value ) );
	}
	rValues.swap( compacted );
}

Terrain3DInstanceStore::Terrain3DInstanceStore( Terrain3DInstanceStore &&rOther ) :
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

Terrain3DInstanceStore &Terrain3DInstanceStore::operator=( const Terrain3DInstanceStore &rOther ) {
	if ( this != &rOther ) {
		copyFrom( rOther );
	}
	return *this;
}

Terrain3DInstanceStore &Terrain3DInstanceStore::operator=( Terrain3DInstanceStore &&rOther ) {
	if ( this == &rOther ) {
		return *this;
	}
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

void Terrain3DInstanceStore::rebindPayloadPools( void ) {
	for ( t3dInstanceMesh_t &mesh : m_meshes ) {
		for ( t3dInstanceCell_t &cell : mesh.m_cells ) {
			cell.rebindPayloadPools( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
		}
	}
}

void Terrain3DInstanceStore::copyFrom( const Terrain3DInstanceStore &rOther ) {
	clearRetainCapacity();
	m_layout = rOther.m_layout;
	size_t transform_count = 0u;
	size_t color_count = 0u;
	size_t owner_count = 0u;
	for ( const t3dInstanceMesh_t &source_mesh : rOther.m_meshes ) {
		for ( const t3dInstanceCell_t &source_cell : source_mesh.m_cells ) {
			transform_count += source_cell.m_transforms.size();
			color_count += source_cell.m_colors.size();
			owner_count += source_cell.m_owners.size();
		}
	}
	reserveMeshCapacity( rOther.m_meshes.size() );
	reservePayloadCapacity( transform_count, color_count, owner_count );
	for ( const t3dInstanceMesh_t &source_mesh : rOther.m_meshes ) {
		t3dInstanceMesh_t &destination_mesh = getOrCreateMesh( source_mesh.m_nId, source_mesh.m_cells.size() );
		for ( const t3dInstanceCell_t &source_cell : source_mesh.m_cells ) {
			t3dInstanceCell_t &destination_cell = getOrCreateCellExact( source_mesh.m_nId,
					source_cell.m_location,
					source_cell.m_transforms.size(),
					source_cell.usesColors(),
					source_cell.usesOwners() );
			destination_cell.m_transforms.resize( source_cell.m_transforms.size() );
			if ( !source_cell.m_transforms.empty() ) {
				eastl::copy_n( source_cell.m_transforms.data(),
						source_cell.m_transforms.size(),
						destination_cell.m_transforms.data() );
			}
			if ( source_cell.usesColors() ) {
				destination_cell.m_colors.resize( source_cell.m_colors.size() );
				eastl::copy_n(
						source_cell.m_colors.data(), source_cell.m_colors.size(), destination_cell.m_colors.data() );
			}
			if ( source_cell.usesOwners() ) {
				destination_cell.m_owners.resize( source_cell.m_owners.size() );
				eastl::copy_n(
						source_cell.m_owners.data(), source_cell.m_owners.size(), destination_cell.m_owners.data() );
			}
			destination_cell.m_nRevision = source_cell.m_nRevision;
			destination_cell.m_bModified = source_cell.m_bModified;
			destination_cell.m_cachedAabb = source_cell.m_cachedAabb;
			destination_cell.m_cachedSourceMeshAabb = source_cell.m_cachedSourceMeshAabb;
			destination_cell.m_nCachedAabbRevision = source_cell.m_nCachedAabbRevision;
			destination_cell.m_nCachedAabbGeneration = source_cell.m_nCachedAabbGeneration;
			destination_cell.m_bCachedAabbValid = source_cell.m_bCachedAabbValid;
		}
	}
	rebuildMeshLookup();
}

bool Terrain3DInstanceStore::cellLess( const t3dInstanceCell_t &a, const Vector2i &b ) {
	return a.m_location.x < b.x || ( a.m_location.x == b.x && a.m_location.y < b.y );
}

bool Terrain3DInstanceStore::meshLess( const t3dInstanceMesh_t &a, const int b ) {
	return a.m_nId < b;
}

size_t Terrain3DInstanceStore::cellLowerBound( const eastl::vector<t3dInstanceCell_t> &cells, const Vector2i &location ) {
	return ( size_t )( eastl::lower_bound( cells.begin(), cells.end(), location, cellLess ) - cells.begin() );
}

EA_FORCE_INLINE static bool t3dInstanceStoreInstanceMeshIdLess( const t3dInstanceMesh_t &rA,
		const t3dInstanceMesh_t &rB ) {
	return rA.m_nId < rB.m_nId;
}

size_t Terrain3DInstanceStore::findCellIndex( const t3dInstanceMesh_t &mesh, const Vector2i &location ) {
	const size_t cell_index = cellLowerBound( mesh.m_cells, location );
	return cell_index < mesh.m_cells.size() && mesh.m_cells[cell_index].m_location == location ? cell_index
																							   : mesh.m_cells.size();
}

size_t Terrain3DInstanceStore::meshLowerBound( const int nMeshId ) const {
	return ( size_t )( eastl::lower_bound( m_meshes.begin(), m_meshes.end(), nMeshId, meshLess ) - m_meshes.begin() );
}

void Terrain3DInstanceStore::rebuildMeshLookup( void ) const {
	for ( uint16_t &slot : m_szMeshSlots ) {
		slot = s_nInvalidMeshSlot;
	}
	for ( size_t i = 0u; i < m_meshes.size(); ++i ) {
		const int id = m_meshes[i].m_nId;
		if ( id >= 0 && id < s_nMaxMeshAssets && i < s_nInvalidMeshSlot ) {
			m_szMeshSlots[id] = ( uint16_t )i;
		}
	}
	m_bMeshLookupDirty = false;
}

void Terrain3DInstanceStore::clearRetainCapacity( void ) {
	m_meshes.clear();
	m_transformSlab.reset();
	m_colorSlab.reset();
	m_ownerSlab.reset();
	invalidateMeshLookup();
}

void Terrain3DInstanceStore::releaseStorage( void ) {
	eastl::vector<t3dInstanceMesh_t>().swap( m_meshes );
	m_transformSlab.release();
	m_colorSlab.release();
	m_ownerSlab.release();
	rebuildMeshLookup();
}

void Terrain3DInstanceStore::compactStorage( void ) {
	Terrain3DHighWaterSlab<t3dPackedTransform_t> compact_transforms;
	Terrain3DHighWaterSlab<uint32_t> compact_colors;
	Terrain3DHighWaterSlab<uint32_t> compact_owners;
	for ( t3dInstanceMesh_t &mesh : m_meshes ) {
		for ( t3dInstanceCell_t &cell : mesh.m_cells ) {
			cell.relocatePayloadsExact( &compact_transforms, &compact_colors, &compact_owners );
		}
		t3dInstanceStoreCompactMoveVector( mesh.m_cells );
	}
	compact_transforms.compactBacking();
	compact_colors.compactBacking();
	compact_owners.compactBacking();
	m_transformSlab.swap( compact_transforms );
	m_colorSlab.swap( compact_colors );
	m_ownerSlab.swap( compact_owners );
	t3dInstanceStoreCompactMoveVector( m_meshes );
	rebindPayloadPools();
}

const t3dInstanceMesh_t *Terrain3DInstanceStore::findMesh( const int nMeshId ) const {
	if ( m_bMeshLookupDirty ) {
		rebuildMeshLookup();
	}
	if ( nMeshId >= 0 && nMeshId < s_nMaxMeshAssets ) {
		const uint16_t slot = m_szMeshSlots[nMeshId];
		return slot != s_nInvalidMeshSlot && slot < m_meshes.size() ? &m_meshes[slot] : nullptr;
	}
	const size_t mesh_index = meshLowerBound( nMeshId );
	return mesh_index < m_meshes.size() && m_meshes[mesh_index].m_nId == nMeshId ? &m_meshes[mesh_index] : nullptr;
}

t3dInstanceMesh_t *Terrain3DInstanceStore::findMesh( const int nMeshId ) {
	if ( m_bMeshLookupDirty ) {
		rebuildMeshLookup();
	}
	if ( nMeshId >= 0 && nMeshId < s_nMaxMeshAssets ) {
		const uint16_t slot = m_szMeshSlots[nMeshId];
		return slot != s_nInvalidMeshSlot && slot < m_meshes.size() ? &m_meshes[slot] : nullptr;
	}
	const size_t mesh_index = meshLowerBound( nMeshId );
	return mesh_index < m_meshes.size() && m_meshes[mesh_index].m_nId == nMeshId ? &m_meshes[mesh_index] : nullptr;
}

t3dInstanceMesh_t &Terrain3DInstanceStore::getOrCreateMesh( const int nMeshId, const size_t nReserveCells ) {
	const size_t mesh_index = meshLowerBound( nMeshId );
	if ( mesh_index >= m_meshes.size() || m_meshes[mesh_index].m_nId != nMeshId ) {
		t3dInstanceMesh_t mesh;
		mesh.m_nId = nMeshId;
		mesh.m_cells.reserve( nReserveCells );
		m_meshes.insert( m_meshes.begin() + mesh_index, eastl::move( mesh ) );
		rebuildMeshLookup();
	} else if ( nReserveCells > m_meshes[mesh_index].m_cells.capacity() ) {
		m_meshes[mesh_index].m_cells.reserve( nReserveCells );
	}
	return m_meshes[mesh_index];
}

bool Terrain3DInstanceStore::eraseMesh( const int nMeshId ) {
	const size_t mesh_index = meshLowerBound( nMeshId );
	if ( mesh_index >= m_meshes.size() || m_meshes[mesh_index].m_nId != nMeshId ) {
		return false;
	}
	m_meshes.erase( m_meshes.begin() + mesh_index );
	rebuildMeshLookup();
	return true;
}

void Terrain3DInstanceStore::swapMeshIds( const int nSrcId, const int nDstId ) {
	t3dInstanceMesh_t *pSrc = findMesh( nSrcId );
	t3dInstanceMesh_t *pDst = findMesh( nDstId );
	if ( pSrc != nullptr ) {
		pSrc->m_nId = nDstId;
	}
	if ( pDst != nullptr ) {
		pDst->m_nId = nSrcId;
	}
	eastl::sort( m_meshes.begin(), m_meshes.end(), t3dInstanceStoreInstanceMeshIdLess );
	rebuildMeshLookup();
}

const t3dInstanceCell_t *Terrain3DInstanceStore::findCell( const int nMeshId, const Vector2i &cell ) const {
	const t3dInstanceMesh_t *pMesh = findMesh( nMeshId );
	if ( pMesh == nullptr ) {
		return nullptr;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->m_cells.size() ? &pMesh->m_cells[cell_index] : nullptr;
}

t3dInstanceCell_t *Terrain3DInstanceStore::findCell( const int nMeshId, const Vector2i &cell ) {
	t3dInstanceMesh_t *pMesh = findMesh( nMeshId );
	if ( pMesh == nullptr ) {
		return nullptr;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->m_cells.size() ? &pMesh->m_cells[cell_index] : nullptr;
}

t3dInstanceCell_t &Terrain3DInstanceStore::getOrCreateCell( const int nMeshId,
		const Vector2i &cell,
		const size_t reserve_instances,
		const bool bColorData,
		const bool bOwnerData ) {
	t3dInstanceMesh_t &mesh = getOrCreateMesh( nMeshId, 4 );
	size_t cell_index = findCellIndex( mesh, cell );
	if ( cell_index >= mesh.m_cells.size() ) {
		cell_index = cellLowerBound( mesh.m_cells, cell );
		t3dInstanceCell_t value;
		value.m_location = cell;
		bindCellPayloads( value );
		value.reserve( reserve_instances, bColorData, bOwnerData );
		mesh.m_cells.insert( mesh.m_cells.begin() + cell_index, eastl::move( value ) );
	} else {
		mesh.m_cells[cell_index].reserve( reserve_instances, bColorData, bOwnerData );
	}
	return mesh.m_cells[cell_index];
}

t3dInstanceCell_t &Terrain3DInstanceStore::getOrCreateCellExact( const int nMeshId,
		const Vector2i &cell,
		const size_t reserve_instances,
		const bool bColorData,
		const bool bOwnerData ) {
	t3dInstanceMesh_t &mesh = getOrCreateMesh( nMeshId, 4 );
	size_t cell_index = findCellIndex( mesh, cell );
	if ( cell_index >= mesh.m_cells.size() ) {
		cell_index = cellLowerBound( mesh.m_cells, cell );
		t3dInstanceCell_t value;
		value.m_location = cell;
		bindCellPayloads( value );
		value.reserveExact( reserve_instances, bColorData, bOwnerData );
		mesh.m_cells.insert( mesh.m_cells.begin() + cell_index, eastl::move( value ) );
	} else {
		mesh.m_cells[cell_index].reserveExact( reserve_instances, bColorData, bOwnerData );
	}
	return mesh.m_cells[cell_index];
}

bool Terrain3DInstanceStore::eraseCell( const int nMeshId, const Vector2i &cell ) {
	t3dInstanceMesh_t *pMesh = findMesh( nMeshId );
	if ( pMesh == nullptr ) {
		return false;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	if ( cell_index >= pMesh->m_cells.size() ) {
		return false;
	}
	pMesh->m_cells.erase( pMesh->m_cells.begin() + cell_index );
	if ( pMesh->m_cells.empty() ) {
		eraseMesh( nMeshId );
	}
	return true;
}

void Terrain3DInstanceStore::importDictionary( const Dictionary &instances ) {
	clearRetainCapacity();
	const Array mesh_ids = instances.keys();
	m_meshes.reserve( mesh_ids.size() );
	for ( int mesh_index = 0; mesh_index < mesh_ids.size(); ++mesh_index ) {
		const Variant mesh_variant = mesh_ids[mesh_index];
		const int mesh_id = ( int )( mesh_variant );
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
			t3dInstanceCell_t &cell = getOrCreateCell( mesh_id, location, xforms.size(), bHasNonwhite, bHasOwnerData );
			cell.m_transforms.resize( xforms.size() );
			for ( int transform_index = 0; transform_index < xforms.size(); ++transform_index ) {
				cell.m_transforms[( size_t )( transform_index )] = xforms[transform_index];
			}
			if ( bHasNonwhite ) {
				cell.m_colors.resize( xforms.size(), s_nInstanceWhiteRgba8 );
				const int color_count = eastl::min( source_colors.size(), xforms.size() );
				for ( int color_index = 0; color_index < color_count; ++color_index ) {
					cell.setColor( ( size_t )( color_index ), source_colors[color_index] );
				}
			}
			if ( bHasOwnerData ) {
				cell.m_owners.resize( xforms.size(), 0u );
				const int owner_count = eastl::min( source_owners.size(), xforms.size() );
				for ( int owner_index = 0; owner_index < owner_count; ++owner_index ) {
					cell.m_owners[( size_t )( owner_index )] = ( uint32_t )( MAX( source_owners[owner_index], 0 ) );
				}
			}
			cell.m_bModified = true; // Import/undo replaces storage; force one GPU upload.
		}
	}
	rebuildMeshLookup();
}

bool Terrain3DInstanceStore::equalsDictionary( const Dictionary &instances ) const {
	if ( instances.size() != ( int )( m_meshes.size() ) ) {
		return false;
	}
	for ( size_t mesh_index = 0; mesh_index < m_meshes.size(); ++mesh_index ) {
		const t3dInstanceMesh_t &mesh = m_meshes[mesh_index];
		if ( !instances.has( mesh.m_nId ) ) {
			return false;
		}
		const Dictionary cell_dict = instances[mesh.m_nId];
		if ( cell_dict.size() != ( int )( mesh.m_cells.size() ) ) {
			return false;
		}
		for ( size_t cell_index = 0; cell_index < mesh.m_cells.size(); ++cell_index ) {
			const t3dInstanceCell_t &cell = mesh.m_cells[cell_index];
			if ( !cell_dict.has( cell.m_location ) ) {
				return false;
			}
			const Array triple = cell_dict[cell.m_location];
			if ( triple.size() < 2 ) {
				return false;
			}
			const TypedArray<Transform3D> transforms = triple[0];
			const PackedColorArray colors = triple[1];
			PackedInt32Array owners;
			if ( triple.size() > 3 && Variant( triple[3] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				owners = triple[3];
			}
			if ( transforms.size() != ( int )( cell.m_transforms.size() ) || colors.size() > transforms.size() ||
					owners.size() > transforms.size() ) {
				return false;
			}
			for ( int transform_index = 0; transform_index < transforms.size(); ++transform_index ) {
				const size_t index = ( size_t )( transform_index );
				if ( Transform3D( transforms[transform_index] ) != cell.m_transforms[index] ) {
					return false;
				}
				const Color expected = cell.getColor( index );
				const Color actual = transform_index < colors.size() ? colors[transform_index] : Color( 1, 1, 1, 1 );
				if ( t3dInstanceCell_t::packColor( actual ) != t3dInstanceCell_t::packColor( expected ) ) {
					return false;
				}
				const uint32_t expected_owner = cell.getOwner( index );
				const uint32_t actual_owner =
						transform_index < owners.size() ? ( uint32_t )( MAX( owners[transform_index], 0 ) ) : 0u;
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
	for ( size_t mesh_index = 0; mesh_index < m_meshes.size(); ++mesh_index ) {
		const t3dInstanceMesh_t &mesh = m_meshes[mesh_index];
		Dictionary cells;
		for ( size_t cell_index = 0; cell_index < mesh.m_cells.size(); ++cell_index ) {
			const t3dInstanceCell_t &cell = mesh.m_cells[cell_index];
			TypedArray<Transform3D> xforms;
			xforms.resize( cell.m_transforms.size() );
			for ( size_t transform_index = 0; transform_index < cell.m_transforms.size(); ++transform_index ) {
				xforms[( int )( transform_index )] = cell.m_transforms[transform_index];
			}
			PackedColorArray colors;
			colors.resize( cell.m_transforms.size() );
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
				owners.resize( cell.m_transforms.size() );
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
	size_t count = 0;
	if ( nMeshId >= 0 ) {
		const t3dInstanceMesh_t *pMesh = findMesh( nMeshId );
		if ( pMesh == nullptr ) {
			return 0;
		}
		for ( size_t cell_index = 0; cell_index < pMesh->m_cells.size(); ++cell_index ) {
			count += pMesh->m_cells[cell_index].m_transforms.size();
		}
		return count;
	}
	for ( size_t mesh_index = 0; mesh_index < m_meshes.size(); ++mesh_index ) {
		const t3dInstanceMesh_t &pMesh = m_meshes[mesh_index];
		for ( size_t cell_index = 0; cell_index < pMesh.m_cells.size(); ++cell_index ) {
			count += pMesh.m_cells[cell_index].m_transforms.size();
		}
	}
	return count;
}
size_t Terrain3DInstanceStore::residentBytes( void ) const {
	size_t bytes = m_meshes.capacity() * sizeof( t3dInstanceMesh_t );
	for ( const t3dInstanceMesh_t &mesh : m_meshes ) {
		bytes += mesh.m_cells.capacity() * sizeof( t3dInstanceCell_t );
	}
	bytes += m_transformSlab.residentBytes();
	bytes += m_colorSlab.residentBytes();
	bytes += m_ownerSlab.residentBytes();
	return bytes;
}
