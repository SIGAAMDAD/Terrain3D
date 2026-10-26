#include "instances/storage/terrain_3d_instance_store.h"

#include "core/config/terrain_3d_compiler.h"

#include <EASTL/utility.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool t3dInstanceStoreValidateRawSlabRange(
	const void *const pData,
	const size_t	  nCount,
	const size_t	  nCapacity,
	const void *const pSlabData,
	const size_t	  nSlabUsed,
	const size_t	  nSlabCapacity,
	const size_t	  nElementBytes ) {
	if ( nCount > nCapacity || nSlabUsed > nSlabCapacity || nElementBytes == 0u ) {
		return false;
	}
	if ( nCapacity == 0u ) {
		return pData == NULL;
	}
	if ( !pData || !pSlabData || nCapacity > SIZE_MAX / nElementBytes ) {
		return false;
	}
	const uintptr_t base = ( uintptr_t ) ( pSlabData );
	const uintptr_t data = ( uintptr_t ) ( pData );
	if ( data < base ) {
		return false;
	}
	const uintptr_t byte_offset = data - base;
	if ( byte_offset % nElementBytes != 0u || byte_offset / nElementBytes > SIZE_MAX ) {
		return false;
	}
	const size_t offset = ( size_t ) ( byte_offset / nElementBytes );
	if ( offset > nSlabUsed || offset > nSlabCapacity ) {
		return false;
	}
	return nCapacity <= nSlabUsed - offset && nCapacity <= nSlabCapacity - offset;
}

static bool t3dInstanceStoreReserveTarget(
	const size_t nCurrentCapacity,
	const size_t nRequired,
	const bool	 bExact,
	size_t		&rTargetCapacity ) {
	rTargetCapacity = nCurrentCapacity;
	if ( nRequired <= nCurrentCapacity ) {
		return true;
	}
	if ( bExact ) {
		rTargetCapacity = nRequired;
		return true;
	}
	const size_t growth = MAX( ( size_t ) 32u, nCurrentCapacity / 2u );
	if ( nCurrentCapacity > SIZE_MAX - growth ) {
		return false;
	}
	rTargetCapacity = MAX( nRequired, nCurrentCapacity + growth );
	return true;
}

template< typename T >
static bool t3dInstanceStorePreflightSlabSegment(
	Terrain3DHighWaterSlab< T > &rSlab,
	const size_t				 nCurrentCapacity,
	const size_t				 nTargetCapacity ) {
	if ( nTargetCapacity <= nCurrentCapacity ) {
		return true;
	}
	if ( nTargetCapacity > SIZE_MAX - rSlab.used() ) {
		return false;
	}
	return rSlab.reserveCold( rSlab.used() + nTargetCapacity );
}

Terrain3DInstanceStore::Terrain3DInstanceStore(
	const t3dInstanceStoreAllocationClass_t eAllocationClass ) :
	m_eAllocationClass( eAllocationClass ) {
	for ( uint32_t i = 0u; i < T3D_MAX_MESH_ASSETS; ++i ) {
		m_szMeshSlots[i] = UINT16_MAX;
	}
	if ( m_eAllocationClass == TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED ) {
		m_transformSlab.requireRuntimeZone();
		m_colorSlab.requireRuntimeZone();
		m_ownerSlab.requireRuntimeZone();
	}
}

Terrain3DInstanceStore::Terrain3DInstanceStore(
	const Terrain3DInstanceStore &rOther ) :
	Terrain3DInstanceStore( rOther.m_eAllocationClass ) {
	if ( rOther.m_pRuntimeAllocatorZone ) {
		bindRuntimeAllocator( rOther.m_pRuntimeAllocatorZone, rOther.m_eRuntimeAllocatorOverflowPolicy );
	}
	( void ) copyFrom( rOther );
}

bool Terrain3DInstanceStore::bindRuntimeAllocator(
	Terrain3DMemoryZone *const		   pZone,
	const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone || ( m_pRuntimeAllocatorZone && m_pRuntimeAllocatorZone != pZone ) ) {
		return false;
	}
	if ( m_meshes.capacity() != 0u && m_meshes.get_allocator().zone() != pZone ) {
		return false;
	}
	for ( size_t i = 0u; i < m_meshes.size(); ++i ) {
		const auto &cells = m_meshes[i].m_cells;
		if ( ( !cells.empty() || cells.capacity() != 0u ) && cells.get_allocator().zone() != pZone ) {
			return false;
		}
	}
	if ( !m_transformSlab.canBindAllocatorZone( pZone ) || !m_colorSlab.canBindAllocatorZone( pZone ) ||
		 !m_ownerSlab.canBindAllocatorZone( pZone ) ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	if ( m_meshes.capacity() == 0u ) {
		m_meshes.set_allocator( allocator );
	}
	for ( size_t i = 0u; i < m_meshes.size(); ++i ) {
		if ( m_meshes[i].m_cells.capacity() == 0u ) {
			m_meshes[i].m_cells.set_allocator( allocator );
		}
	}
	m_transformSlab.requireRuntimeZone();
	m_colorSlab.requireRuntimeZone();
	m_ownerSlab.requireRuntimeZone();
	if ( !m_transformSlab.bindAllocatorZone( pZone, ePolicy ) || !m_colorSlab.bindAllocatorZone( pZone, ePolicy ) ||
		 !m_ownerSlab.bindAllocatorZone( pZone, ePolicy ) ) {
		return false;
	}
	m_eAllocationClass = TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED;
	m_pRuntimeAllocatorZone = pZone;
	m_eRuntimeAllocatorOverflowPolicy = ePolicy;
	return true;
}

Terrain3DInstanceStore::Terrain3DInstanceStore(
	Terrain3DInstanceStore &&rOther ) :
	Terrain3DInstanceStore( rOther.m_eAllocationClass ) {
	*this = eastl::move( rOther );
}

Terrain3DInstanceStore &Terrain3DInstanceStore::operator=(
	const Terrain3DInstanceStore &rOther ) {
	if ( this != &rOther ) {
		Terrain3DInstanceStore replacement( m_eAllocationClass );
		if ( m_pRuntimeAllocatorZone ) {
			replacement.bindRuntimeAllocator( m_pRuntimeAllocatorZone, m_eRuntimeAllocatorOverflowPolicy );
		}
		if ( replacement.copyFrom( rOther ) ) {
			*this = eastl::move( replacement );
		}
	}
	return *this;
}

Terrain3DInstanceStore &Terrain3DInstanceStore::operator=(
	Terrain3DInstanceStore &&rOther ) {
	if ( this == &rOther ) {
		return *this;
	}
	shutdown();
	m_meshes = eastl::move( rOther.m_meshes );
	m_transformSlab = eastl::move( rOther.m_transformSlab );
	m_colorSlab = eastl::move( rOther.m_colorSlab );
	m_ownerSlab = eastl::move( rOther.m_ownerSlab );
	m_layout = rOther.m_layout;
	m_eAllocationClass = rOther.m_eAllocationClass;
	m_pRuntimeAllocatorZone = rOther.m_pRuntimeAllocatorZone;
	m_eRuntimeAllocatorOverflowPolicy = rOther.m_eRuntimeAllocatorOverflowPolicy;
	rebuildActiveMeshIds();
	rebindCellStorage();
	rOther.m_meshes.clear();
	rOther.rebuildActiveMeshIds();
	rOther.m_eAllocationClass = TERRAIN3D_INSTANCE_STORE_PERSISTENT_RESOURCE;
	rOther.m_pRuntimeAllocatorZone = NULL;
	return *this;
}

void Terrain3DInstanceStore::rebuildActiveMeshIds(
	void ) {
	for ( uint32_t i = 0u; i < T3D_MAX_MESH_ASSETS; ++i ) {
		m_szMeshSlots[i] = UINT16_MAX;
	}
	for ( size_t slot = 0u; slot < m_meshes.size(); ++slot ) {
		const int id = m_meshes[slot].m_nId;
		if ( id >= 0 && id < T3D_MAX_MESH_ASSETS ) {
			m_szMeshSlots[id] = ( uint16_t ) slot;
		}
	}
	m_nActiveMeshCount = 0u;
	for ( int id = 0; id < T3D_MAX_MESH_ASSETS; ++id ) {
		if ( m_szMeshSlots[id] != UINT16_MAX ) {
			m_szActiveMeshIds[m_nActiveMeshCount++] = ( uint16_t ) id;
		}
	}
}

void Terrain3DInstanceStore::rebindCellStorage(
	void ) {
	for ( size_t iMesh = 0u; iMesh < m_meshes.size(); ++iMesh ) {
		auto &cells = m_meshes[iMesh].m_cells;
		for ( size_t i = 0u; i < cells.size(); ++i ) {
			cells[i].rebindStorage( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
		}
	}
}

bool Terrain3DInstanceStore::reserveMeshCapacityCold(
	const size_t nCapacity ) {
	if ( nCapacity > T3D_MAX_MESH_ASSETS || !runtimeAllocatorReady() || !canGrowCellVector() ) {
		return false;
	}
	return t3dContainerReserveCold( m_meshes, nCapacity );
}

bool Terrain3DInstanceStore::copyFrom(
	const Terrain3DInstanceStore &rOther ) {
	if ( !runtimeAllocatorReady() ) {
		return false;
	}
	clearRetainCapacity();
	m_layout = rOther.m_layout;
	for ( size_t iMesh = 0u; iMesh < ( size_t ) ( ( rOther.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &source_mesh = ( rOther.meshes() )[iMesh];
		if ( source_mesh.m_nId < 0 || source_mesh.m_nId >= T3D_MAX_MESH_ASSETS ) {
			clearRetainCapacity();
			return false;
		}
		t3dInstanceMesh_t *const pDestination = tryGetOrCreateMesh( source_mesh.m_nId );
		if ( !pDestination ) {
			clearRetainCapacity();
			return false;
		}
		t3dInstanceMesh_t &destination_mesh = *pDestination;
		if ( !t3dContainerReserveCold( destination_mesh.m_cells, source_mesh.m_cells.size() ) ) {
			clearRetainCapacity();
			return false;
		}
		destination_mesh.m_nId = source_mesh.m_nId;
		for ( size_t iCell = 0u; iCell < ( size_t ) ( ( source_mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &source_cell = ( source_mesh.m_cells )[iCell];
			t3dInstanceCell_s		 destination_cell;
			destination_cell.bindStorage( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
			destination_cell.m_location = source_cell.m_location;
			if ( !destination_cell.reserveExact( source_cell.m_transforms.size(),
												 source_cell.usesColors(),
												 source_cell.usesOwners() ) ||
				 !destination_cell.m_transforms.resize( source_cell.m_transforms.size() ) ) {
				clearRetainCapacity();
				return false;
			}
			if ( !source_cell.m_transforms.empty() ) {
				memcpy( destination_cell.m_transforms.data(),
						source_cell.m_transforms.data(),
						source_cell.m_transforms.size() * sizeof( t3dPackedTransform_s ) );
			}
			if ( source_cell.usesColors() ) {
				if ( !destination_cell.m_colors.resize( source_cell.m_colors.size() ) ) {
					clearRetainCapacity();
					return false;
				}
				memcpy( destination_cell.m_colors.data(),
						source_cell.m_colors.data(),
						source_cell.m_colors.size() * sizeof( uint32_t ) );
			}
			if ( source_cell.usesOwners() ) {
				if ( !destination_cell.m_owners.resize( source_cell.m_owners.size() ) ) {
					clearRetainCapacity();
					return false;
				}
				memcpy( destination_cell.m_owners.data(),
						source_cell.m_owners.data(),
						source_cell.m_owners.size() * sizeof( uint32_t ) );
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

bool Terrain3DInstanceStore::cellLess(
	const t3dInstanceCell_s &a,
	const Vector2i			&b ) {
	return a.m_location.x < b.x || ( a.m_location.x == b.x && a.m_location.y < b.y );
}

size_t Terrain3DInstanceStore::cellLowerBound(
	const eastl::vector< t3dInstanceCell_s, Terrain3DEastlAllocator > &cells,
	const Vector2i													  &location ) {
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

size_t Terrain3DInstanceStore::findCellIndex(
	const t3dInstanceMesh_t &mesh,
	const Vector2i			&location ) {
	const size_t cell_index = cellLowerBound( mesh.m_cells, location );
	return cell_index < mesh.m_cells.size() && mesh.m_cells[cell_index].m_location == location ? cell_index
																							   : mesh.m_cells.size();
}

void Terrain3DInstanceStore::reset(
	void ) {
	for ( size_t i = 0u; i < m_meshes.size(); ++i ) {
		m_meshes[i].m_cells.clear();
	}
	m_meshes.clear();
	rebuildActiveMeshIds();
	m_transformSlab.reset();
	m_colorSlab.reset();
	m_ownerSlab.reset();
}

void Terrain3DInstanceStore::shutdown(
	void ) {
	for ( size_t i = 0u; i < m_meshes.size(); ++i ) {
		t3dVectorRelease( m_meshes[i].m_cells );
	}
	t3dVectorRelease( m_meshes );
	rebuildActiveMeshIds();
	m_transformSlab.release();
	m_colorSlab.release();
	m_ownerSlab.release();
}

void Terrain3DInstanceStore::compactStorage(
	void ) {
	// Build the compacted image off to the side. The old store remains fully
	// valid until every mesh/cell/payload copy succeeds, so allocation failure
	// cannot leave live cells rebound to partially constructed replacement slabs.
	Terrain3DInstanceStore compacted( m_eAllocationClass );
	if ( m_pRuntimeAllocatorZone ) {
		compacted.bindRuntimeAllocator( m_pRuntimeAllocatorZone, m_eRuntimeAllocatorOverflowPolicy );
	}
	if ( !compacted.copyFrom( *this ) ) {
		return;
	}
	*this = eastl::move( compacted );
}

const t3dInstanceMesh_t *Terrain3DInstanceStore::findMesh(
	const int nMeshId ) const {
	return nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS || m_szMeshSlots[nMeshId] == UINT16_MAX
			   ? NULL
			   : &m_meshes[m_szMeshSlots[nMeshId]];
}

t3dInstanceMesh_t *Terrain3DInstanceStore::findMesh(
	const int nMeshId ) {
	return nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS || m_szMeshSlots[nMeshId] == UINT16_MAX
			   ? NULL
			   : &m_meshes[m_szMeshSlots[nMeshId]];
}

bool Terrain3DInstanceStore::canGrowCellVector(
	void ) const {
	return m_eAllocationClass != TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED ||
		   ( m_pRuntimeAllocatorZone && m_pRuntimeAllocatorZone->state() == T3D_MEMORY_ZONE_ACTIVE );
}

t3dInstanceMesh_t *Terrain3DInstanceStore::tryGetOrCreateMesh(
	const int	 nMeshId,
	const size_t nReserveCells ) {
	if ( nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS || !runtimeAllocatorReady() ) {
		return NULL;
	}
	t3dInstanceMesh_t *pMesh = findMesh( nMeshId );
	const bool		   bNewMesh = pMesh == NULL;
	if ( !pMesh ) {
		if ( !canGrowCellVector() || m_meshes.size() >= T3D_MAX_MESH_ASSETS ) {
			return NULL;
		}
		if ( m_meshes.size() == m_meshes.capacity() ) {
			const size_t capacity =
				MAX( ( size_t ) 4u, m_meshes.capacity() + MAX( ( size_t ) 4u, m_meshes.capacity() / 2u ) );
			if ( !reserveMeshCapacityCold( MIN( capacity, ( size_t ) T3D_MAX_MESH_ASSETS ) ) ) {
				return NULL;
			}
		}
		m_meshes.emplace_back();
		pMesh = &m_meshes.back();
		pMesh->m_nId = nMeshId;
		if ( m_pRuntimeAllocatorZone ) {
			pMesh->m_cells.set_allocator(
				Terrain3DEastlAllocator::zoneAllocator( m_pRuntimeAllocatorZone, m_eRuntimeAllocatorOverflowPolicy ) );
		}
		rebuildActiveMeshIds();
	}
	if ( nReserveCells > pMesh->m_cells.capacity() ) {
		if ( nReserveCells > SIZE_MAX / sizeof( t3dInstanceCell_s ) || !canGrowCellVector() ||
			 !t3dContainerReserveCold( pMesh->m_cells, nReserveCells ) ) {
			// A rejected reservation must not publish an otherwise empty new
			// descriptor or consume a logical mesh ID in this sparse table.
			if ( bNewMesh ) {
				eraseMesh( nMeshId );
			}
			return NULL;
		}
	}
	return pMesh;
}

bool Terrain3DInstanceStore::eraseMesh(
	const int nMeshId ) {
	if ( !findMesh( nMeshId ) ) {
		return false;
	}
	const size_t slot = m_szMeshSlots[nMeshId];
	m_meshes[slot].m_cells.clear();
	if ( slot != m_meshes.size() - 1u ) {
		m_meshes[slot] = eastl::move( m_meshes.back() );
	}
	m_meshes.pop_back();
	rebuildActiveMeshIds();
	return true;
}

void Terrain3DInstanceStore::swapMeshIds(
	const int nSrcId,
	const int nDstId ) {
	if ( nSrcId == nDstId || nSrcId < 0 || nDstId < 0 || nSrcId >= T3D_MAX_MESH_ASSETS ||
		 nDstId >= T3D_MAX_MESH_ASSETS ) {
		return;
	}
	if ( t3dInstanceMesh_t *pSource = findMesh( nSrcId ) ) {
		pSource->m_nId = nDstId;
	}
	if ( t3dInstanceMesh_t *pDestination = findMesh( nDstId ) ) {
		pDestination->m_nId = nSrcId;
	}
	rebuildActiveMeshIds();
}

const t3dInstanceCell_s *Terrain3DInstanceStore::findCell(
	const int		nMeshId,
	const Vector2i &cell ) const {
	const t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
	if ( !pMesh ) {
		return NULL;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->m_cells.size() ? &pMesh->m_cells[cell_index] : NULL;
}

t3dInstanceCell_s *Terrain3DInstanceStore::findCell(
	const int		nMeshId,
	const Vector2i &cell ) {
	t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
	if ( !pMesh ) {
		return NULL;
	}
	const size_t cell_index = findCellIndex( *pMesh, cell );
	return cell_index < pMesh->m_cells.size() ? &pMesh->m_cells[cell_index] : NULL;
}

t3dInstanceCell_s *Terrain3DInstanceStore::tryGetOrCreateCellInternal(
	const int		nMeshId,
	const Vector2i &cell,
	const size_t	nReserveInstances,
	const bool		bColorData,
	const bool		bOwnerData,
	const bool		bExact ) {
	if ( nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS || !runtimeAllocatorReady() ) {
		return NULL;
	}

	t3dInstanceMesh_t *const pMesh = tryGetOrCreateMesh( nMeshId );
	if ( !pMesh ) {
		return NULL;
	}
	t3dInstanceMesh_t &mesh = *pMesh;
	const bool		   bInactiveMesh = false;

	if ( !bInactiveMesh ) {
		const size_t existing_index = findCellIndex( mesh, cell );
		if ( existing_index < mesh.m_cells.size() ) {
			t3dInstanceCell_s &existing = mesh.m_cells[existing_index];
			size_t			   transform_capacity = existing.m_transforms.capacity();
			size_t			   color_capacity = existing.m_colors.capacity();
			size_t			   owner_capacity = existing.m_owners.capacity();
			if ( !t3dInstanceStoreReserveTarget( transform_capacity, nReserveInstances, bExact, transform_capacity ) ||
				 ( bColorData &&
				   !t3dInstanceStoreReserveTarget( color_capacity, nReserveInstances, bExact, color_capacity ) ) ||
				 ( bOwnerData &&
				   !t3dInstanceStoreReserveTarget( owner_capacity, nReserveInstances, bExact, owner_capacity ) ) ||
				 !t3dInstanceStorePreflightSlabSegment( m_transformSlab,
														existing.m_transforms.capacity(),
														transform_capacity ) ||
				 ( bColorData && !t3dInstanceStorePreflightSlabSegment( m_colorSlab,
																		existing.m_colors.capacity(),
																		color_capacity ) ) ||
				 ( bOwnerData && !t3dInstanceStorePreflightSlabSegment( m_ownerSlab,
																		existing.m_owners.capacity(),
																		owner_capacity ) ) ) {
				return NULL;
			}
			const bool bReserved = bExact ? existing.reserveExact( nReserveInstances, bColorData, bOwnerData )
										  : existing.reserve( nReserveInstances, bColorData, bOwnerData );
			return bReserved ? &existing : NULL;
		}
	}

	const size_t cell_index = cellLowerBound( mesh.m_cells, cell );
	const size_t required = mesh.m_cells.size() + 1u;
	if ( required == 0u ) {
		return NULL;
	}
	if ( required > mesh.m_cells.capacity() || mesh.m_cells.capacity() < 4u ) {
		size_t target_capacity = MAX( ( size_t ) 4u, required );
		if ( !bExact && mesh.m_cells.capacity() >= 4u ) {
			const size_t growth = MAX( ( size_t ) 4u, mesh.m_cells.capacity() / 2u );
			if ( mesh.m_cells.capacity() > SIZE_MAX - growth ) {
				return NULL;
			}
			target_capacity = MAX( target_capacity, mesh.m_cells.capacity() + growth );
		}
		if ( target_capacity > SIZE_MAX / sizeof( t3dInstanceCell_s ) || !canGrowCellVector() ||
			 !t3dContainerReserveCold( mesh.m_cells, target_capacity ) ) {
			return NULL;
		}
	}

	t3dInstanceCell_s value;
	value.bindStorage( &m_transformSlab, &m_colorSlab, &m_ownerSlab );
	value.m_location = cell;
	size_t transform_capacity = 0u;
	size_t color_capacity = 0u;
	size_t owner_capacity = 0u;
	if ( !t3dInstanceStoreReserveTarget( 0u, nReserveInstances, bExact, transform_capacity ) ||
		 ( bColorData && !t3dInstanceStoreReserveTarget( 0u, nReserveInstances, bExact, color_capacity ) ) ||
		 ( bOwnerData && !t3dInstanceStoreReserveTarget( 0u, nReserveInstances, bExact, owner_capacity ) ) ||
		 !t3dInstanceStorePreflightSlabSegment( m_transformSlab, 0u, transform_capacity ) ||
		 ( bColorData && !t3dInstanceStorePreflightSlabSegment( m_colorSlab, 0u, color_capacity ) ) ||
		 ( bOwnerData && !t3dInstanceStorePreflightSlabSegment( m_ownerSlab, 0u, owner_capacity ) ) ) {
		return NULL;
	}
	const bool bReserved = bExact ? value.reserveExact( nReserveInstances, bColorData, bOwnerData )
								  : value.reserve( nReserveInstances, bColorData, bOwnerData );
	if ( !bReserved ) {
		return NULL;
	}
	if ( !t3dVectorInsertWithinCapacity( mesh.m_cells, cell_index, eastl::move( value ) ) ) {
		return NULL;
	}
	// Mesh descriptor is admitted in tryGetOrCreateMesh().
	return &mesh.m_cells[cell_index];
}

t3dInstanceCell_s *Terrain3DInstanceStore::tryGetOrCreateCell(
	const int		nMeshId,
	const Vector2i &cell,
	const size_t	nReserveInstances,
	const bool		bColorData,
	const bool		bOwnerData ) {
	return tryGetOrCreateCellInternal( nMeshId, cell, nReserveInstances, bColorData, bOwnerData, false );
}

t3dInstanceCell_s *Terrain3DInstanceStore::tryGetOrCreateCellExact(
	const int		nMeshId,
	const Vector2i &cell,
	const size_t	nReserveInstances,
	const bool		bColorData,
	const bool		bOwnerData ) {
	return tryGetOrCreateCellInternal( nMeshId, cell, nReserveInstances, bColorData, bOwnerData, true );
}

bool Terrain3DInstanceStore::eraseCell(
	const int		nMeshId,
	const Vector2i &cell ) {
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

void Terrain3DInstanceStore::importDictionary(
	const Dictionary &instances ) {
	clearRetainCapacity();
	const Array mesh_ids = instances.keys();
	for ( int mesh_index = 0; mesh_index < mesh_ids.size(); ++mesh_index ) {
		const Variant mesh_variant = mesh_ids[mesh_index];
		const int	  mesh_id = ( int ) ( mesh_variant );
		if ( mesh_id < 0 || mesh_id >= T3D_MAX_MESH_ASSETS ) {
			continue;
		}
		const Dictionary cell_dict = instances[mesh_variant];
		const Array		 cell_keys = cell_dict.keys();
		if ( !tryGetOrCreateMesh( mesh_id, ( size_t ) ( cell_keys.size() ) ) ) {
			clearRetainCapacity();
			return;
		}
		for ( int cell_index = 0; cell_index < cell_keys.size(); ++cell_index ) {
			const Variant  cell_variant = cell_keys[cell_index];
			const Vector2i location = cell_variant;
			if ( !isValidCell( location ) ) {
				continue;
			}
			const Array triple = cell_dict[cell_variant];
			if ( triple.size() < 2 ) {
				continue;
			}
			const TypedArray< Transform3D > xforms = triple[0];
			const PackedColorArray			source_colors = triple[1];
			PackedInt32Array				source_owners;
			PackedInt32Array				source_behavior_flags;
			if ( triple.size() > 3 && Variant( triple[3] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				source_owners = triple[3];
			}
			if ( triple.size() > 4 && Variant( triple[4] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				source_behavior_flags = triple[4];
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
			t3dInstanceCell_s *const pTargetCell =
				tryGetOrCreateCell( mesh_id, location, ( size_t ) ( xforms.size() ), bHasNonwhite, bHasOwnerData );
			if ( !pTargetCell ) {
				clearRetainCapacity();
				return;
			}
			t3dInstanceCell_s &target_cell = *pTargetCell;
			if ( !target_cell.m_transforms.resize( ( size_t ) ( xforms.size() ) ) ) {
				clearRetainCapacity();
				return;
			}
			for ( int transform_index = 0; transform_index < xforms.size(); ++transform_index ) {
				const uint16_t nBehaviorFlags = transform_index < source_behavior_flags.size()
													? ( uint16_t ) ( source_behavior_flags[transform_index] )
													: T3D_INSTANCE_LEGACY_BEHAVIOR;
				target_cell.setTransformAndBehaviorFlags( ( size_t ) ( transform_index ),
														  xforms[transform_index],
														  nBehaviorFlags );
			}
			if ( bHasNonwhite ) {
				if ( !target_cell.m_colors.resize( ( size_t ) ( xforms.size() ) ) ) {
					clearRetainCapacity();
					return;
				}
				for ( size_t color_index = 0u; color_index < target_cell.m_colors.size(); ++color_index ) {
					target_cell.m_colors[color_index] = T3D_INSTANCE_WHITE_RGBA8;
				}
				const int color_count = MIN( source_colors.size(), xforms.size() );
				for ( int color_index = 0; color_index < color_count; ++color_index ) {
					target_cell.setColor( ( size_t ) ( color_index ), source_colors[color_index] );
				}
			}
			if ( bHasOwnerData ) {
				if ( !target_cell.m_owners.resize( ( size_t ) ( xforms.size() ) ) ) {
					clearRetainCapacity();
					return;
				}
				for ( size_t owner_index = 0u; owner_index < target_cell.m_owners.size(); ++owner_index ) {
					target_cell.m_owners[owner_index] = 0u;
				}
				const int owner_count = MIN( source_owners.size(), xforms.size() );
				for ( int owner_index = 0; owner_index < owner_count; ++owner_index ) {
					target_cell.m_owners[( size_t ) ( owner_index )] =
						( uint32_t ) ( MAX( source_owners[owner_index], 0 ) );
				}
			}
			target_cell.m_bModified = true;
		}
	}
}

bool Terrain3DInstanceStore::equalsDictionary(
	const Dictionary &instances ) const {
	if ( instances.size() != ( int ) ( m_nActiveMeshCount ) ) {
		return false;
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t ) ( ( meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( meshes() )[iMesh];
		if ( !instances.has( mesh.m_nId ) ) {
			return false;
		}
		const Dictionary cell_dict = instances[mesh.m_nId];
		if ( cell_dict.size() != ( int ) ( mesh.m_cells.size() ) ) {
			return false;
		}
		for ( size_t iCell = 0u; iCell < ( size_t ) ( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &instance_cell = ( mesh.m_cells )[iCell];
			if ( !cell_dict.has( instance_cell.m_location ) ) {
				return false;
			}
			const Array triple = cell_dict[instance_cell.m_location];
			if ( triple.size() < 2 ) {
				return false;
			}
			const TypedArray< Transform3D > transforms = triple[0];
			const PackedColorArray			colors = triple[1];
			PackedInt32Array				owners;
			PackedInt32Array				behavior_flags;
			if ( triple.size() > 3 && Variant( triple[3] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				owners = triple[3];
			}
			if ( triple.size() > 4 && Variant( triple[4] ).get_type() == Variant::PACKED_INT32_ARRAY ) {
				behavior_flags = triple[4];
			}
			if ( transforms.size() != ( int ) ( instance_cell.m_transforms.size() ) ||
				 colors.size() > transforms.size() || owners.size() > transforms.size() ||
				 behavior_flags.size() > transforms.size() ) {
				return false;
			}
			for ( int transform_index = 0; transform_index < transforms.size(); ++transform_index ) {
				const size_t index = ( size_t ) ( transform_index );
				if ( Transform3D( transforms[transform_index] ) != instance_cell.getTransform( index ) ) {
					return false;
				}
				const Color expected = instance_cell.getColor( index );
				const Color actual = transform_index < colors.size() ? colors[transform_index] : Color( 1, 1, 1, 1 );
				if ( t3dInstanceCell_s::packColor( actual ) != t3dInstanceCell_s::packColor( expected ) ) {
					return false;
				}
				const uint32_t expected_owner = instance_cell.getOwner( index );
				const uint32_t actual_owner =
					transform_index < owners.size() ? ( uint32_t ) ( MAX( owners[transform_index], 0 ) ) : 0u;
				if ( actual_owner != expected_owner ) {
					return false;
				}
				const uint16_t expected_behavior = instance_cell.getTransformBehaviorFlags( index );
				const uint16_t actual_behavior =
					transform_index < behavior_flags.size()
						? ( uint16_t ) ( behavior_flags[transform_index] ) & T3D_INSTANCE_BEHAVIOR_MASK
						: T3D_INSTANCE_LEGACY_BEHAVIOR;
				if ( actual_behavior != expected_behavior ) {
					return false;
				}
			}
		}
	}
	return true;
}

Dictionary Terrain3DInstanceStore::exportDictionary(
	void ) const {
	Dictionary result;
	for ( size_t iMesh = 0u; iMesh < ( size_t ) ( ( meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( meshes() )[iMesh];
		Dictionary				 cells;
		for ( size_t iCell = 0u; iCell < ( size_t ) ( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s	 &cell = ( mesh.m_cells )[iCell];
			TypedArray< Transform3D > xforms;
			xforms.resize( ( int ) ( cell.m_transforms.size() ) );
			for ( size_t transform_index = 0; transform_index < cell.m_transforms.size(); ++transform_index ) {
				xforms[( int ) ( transform_index )] = cell.getTransform( transform_index );
			}
			PackedColorArray colors;
			colors.resize( ( int ) ( cell.m_transforms.size() ) );
			if ( cell.usesColors() ) {
				for ( size_t color_index = 0; color_index < cell.m_colors.size(); ++color_index ) {
					colors.set( ( int ) ( color_index ), cell.getColor( color_index ) );
				}
			} else {
				colors.fill( Color( 1, 1, 1, 1 ) );
			}
			Array triple;
			triple.resize( 5 );
			triple[0] = xforms;
			triple[1] = colors;
			triple[2] = cell.m_bModified;
			if ( cell.usesOwners() ) {
				PackedInt32Array owners;
				owners.resize( ( int ) ( cell.m_transforms.size() ) );
				for ( size_t owner_index = 0; owner_index < cell.m_transforms.size(); ++owner_index ) {
					owners.set( ( int ) ( owner_index ), ( int32_t ) ( cell.getOwner( owner_index ) ) );
				}
				triple[3] = owners;
			}
			PackedInt32Array behavior_flags;
			behavior_flags.resize( ( int ) ( cell.m_transforms.size() ) );
			for ( size_t behavior_index = 0u; behavior_index < cell.m_transforms.size(); ++behavior_index ) {
				behavior_flags.set( ( int ) ( behavior_index ),
									( int32_t ) ( cell.getTransformBehaviorFlags( behavior_index ) ) );
			}
			triple[4] = behavior_flags;
			cells[cell.m_location] = triple;
		}
		result[mesh.m_nId] = cells;
	}
	return result;
}

size_t Terrain3DInstanceStore::instanceCount(
	const int nMeshId ) const {
	size_t count = 0u;
	if ( nMeshId >= 0 ) {
		const t3dInstanceMesh_t *const pMesh = findMesh( nMeshId );
		if ( !pMesh ) {
			return 0u;
		}
		for ( size_t iCell = 0u; iCell < ( size_t ) ( ( pMesh->m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &cell = ( pMesh->m_cells )[iCell];
			count += cell.m_transforms.size();
		}
		return count;
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t ) ( ( meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( meshes() )[iMesh];
		for ( size_t iCell = 0u; iCell < ( size_t ) ( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &cell = ( mesh.m_cells )[iCell];
			count += cell.m_transforms.size();
		}
	}
	return count;
}

bool Terrain3DInstanceStore::validateDeep(
	void ) const {
	if ( m_nActiveMeshCount > T3D_MAX_MESH_ASSETS || m_transformSlab.used() > m_transformSlab.capacity() ||
		 m_colorSlab.used() > m_colorSlab.capacity() || m_ownerSlab.used() > m_ownerSlab.capacity() ) {
		return false;
	}
	if ( ( m_transformSlab.capacity() > 0u && !m_transformSlab.data() ) ||
		 ( m_colorSlab.capacity() > 0u && !m_colorSlab.data() ) ||
		 ( m_ownerSlab.capacity() > 0u && !m_ownerSlab.data() ) || !m_transformSlab.runtimeBindingSatisfied() ||
		 !m_colorSlab.runtimeBindingSatisfied() || !m_ownerSlab.runtimeBindingSatisfied() ) {
		return false;
	}

	int previous_id = -1;
	for ( size_t iActive = 0u; iActive < m_nActiveMeshCount; ++iActive ) {
		const int mesh_id = ( int ) ( m_szActiveMeshIds[iActive] );
		if ( mesh_id < 0 || mesh_id >= T3D_MAX_MESH_ASSETS || mesh_id <= previous_id ||
			 m_szMeshSlots[mesh_id] == UINT16_MAX || m_meshes[m_szMeshSlots[mesh_id]].m_nId != mesh_id ) {
			return false;
		}
		previous_id = mesh_id;
	}

	size_t active_cursor = 0u;
	if ( m_meshes.size() != m_nActiveMeshCount ) {
		return false;
	}
	for ( int mesh_id = 0; mesh_id < T3D_MAX_MESH_ASSETS; ++mesh_id ) {
		const uint16_t slot = m_szMeshSlots[mesh_id];
		if ( slot == UINT16_MAX ) {
			continue;
		}
		if ( slot >= m_meshes.size() ) {
			return false;
		}
		const t3dInstanceMesh_t &mesh = m_meshes[slot];
		if ( mesh.m_nId != mesh_id || active_cursor >= m_nActiveMeshCount ||
			 m_szActiveMeshIds[active_cursor] != ( uint16_t ) mesh_id ) {
			return false;
		}
		++active_cursor;

		for ( size_t iCell = 0u; iCell < mesh.m_cells.size(); ++iCell ) {
			const t3dInstanceCell_s &cell = mesh.m_cells[iCell];
			if ( iCell > 0u && !cellLess( mesh.m_cells[iCell - 1u], cell.m_location ) ) {
				return false;
			}
			if ( cell.m_nRevision == 0u ||
				 ( cell.m_bCachedAabbValid &&
				   ( cell.m_nCachedAabbRevision != cell.m_nRevision || cell.m_nCachedAabbGeneration == 0u ) ) ) {
				return false;
			}
			if ( !t3dInstanceStoreValidateRawSlabRange( cell.m_transforms.data(),
														cell.m_transforms.size(),
														cell.m_transforms.capacity(),
														m_transformSlab.data(),
														m_transformSlab.used(),
														m_transformSlab.capacity(),
														sizeof( t3dPackedTransform_s ) ) ) {
				return false;
			}
			if ( !cell.m_colors.validateBinding( &m_colorSlab ) || !cell.m_owners.validateBinding( &m_ownerSlab ) ) {
				return false;
			}
			const size_t transform_count = cell.m_transforms.size();
			if ( ( !cell.m_colors.empty() && cell.m_colors.size() != transform_count ) ||
				 ( !cell.m_owners.empty() && cell.m_owners.size() != transform_count ) ) {
				return false;
			}
		}
	}
	return active_cursor == m_nActiveMeshCount;
}

size_t Terrain3DInstanceStore::residentBytes(
	void ) const {
	size_t bytes =
		sizeof( m_meshes ) + sizeof( m_szMeshSlots ) + sizeof( m_szActiveMeshIds ) + sizeof( m_nActiveMeshCount );
	bytes += t3dContainerResidentBytes( m_meshes );
	for ( size_t i = 0u; i < m_meshes.size(); ++i ) {
		bytes += t3dContainerResidentBytes( m_meshes[i].m_cells );
	}
	return bytes + m_transformSlab.residentBytes() + m_colorSlab.residentBytes() + m_ownerSlab.residentBytes();
}
