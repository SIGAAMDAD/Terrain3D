// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "instances/terrain_3d_instancer.h"

#include "Terrain3DConstants.h"
#include "authoring/terrain_3d_editor.h"
#include "core/logger.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "data/terrain_3d_sampler.h"
#include "simd/terrain_3d_simd.h"

#include <algorithm>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>

static_assert( s_nInstanceRegionTableCapacity == ( size_t )( s_nMaxRegionSlots ),
		"Instancer region tables must cover every T3DData region slot" );

static bool
t3dBeginInstancePageAccess( T3DInstancer *pInstancer, T3DData *pData, const Vector2i &rLocation, bool &bTemporary ) {
	bTemporary = false;
	if ( !pData ) {
		return false;
	}
	const bool bCacheEnabled = pData->isInstanceRegionCacheEnabled();
	const bool bWasResident = bCacheEnabled && pData->isInstanceRegionResident( rLocation );
	if ( !pData->ensureInstanceRegionResident( rLocation ) ) {
		return false;
	}
	bTemporary = bCacheEnabled && !bWasResident;
	T3DRegion *pRegion = pData->getRegionPtr( rLocation );
	if ( pRegion && pInstancer && !pInstancer->ensureRegionLayout( pRegion ) ) {
		if ( bTemporary ) {
			pData->evictInstanceRegion( rLocation );
			bTemporary = false;
		}
		return false;
	}
	return true;
}

static void t3dEndInstancePageAccess( T3DData *pData, const Vector2i &rLocation, const bool bTemporary ) {
	if ( bTemporary && pData ) {
		pData->evictInstanceRegion( rLocation );
	}
}

static uint64_t t3dRepositoryMeshInstanceCount( const T3DData *pData, const int nMeshId ) {
	if ( !pData || nMeshId < 0 || nMeshId >= s_nMaxMeshAssets ) {
		return 0u;
	}
	uint64_t total = 0u;
	T3DRegion *const *ppRegionPtrs = pData->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		const T3DRegion *pRegion = ppRegionPtrs[region_index];
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const uint64_t count = ( uint64_t )( pRegion->getInstanceCount( nMeshId ) );
		total = UINT64_MAX - total < count ? UINT64_MAX : total + count;
	}
	return total;
}

// -----------------------------------------------------------------------------
// Private functions

t3dRenderCellKey_t T3DInstancer::makeRenderKey( const t3dRenderCell_t &rCell ) {
	return t3dRenderCellKey_t{ rCell.m_region, rCell.m_cell, rCell.m_nMeshId, rCell.m_nLod };
}

int T3DInstancer::getRegionTableIndex( const Vector2i &vRegion ) {
	return T3DData::getRegionMapIndex( vRegion );
}

uint32_t T3DInstancer::allocateRenderRegionNode( const size_t iRenderCell, const uint16_t nRegionIndex ) {
	uint32_t node_index = UINT32_MAX;
	if ( m_nRenderRegionFreeHead != UINT32_MAX ) {
		node_index = m_nRenderRegionFreeHead;
		m_nRenderRegionFreeHead = m_renderRegionNodes[( size_t )( node_index )].m_nNext;
	} else {
		node_index = ( uint32_t )( m_renderRegionNodes.size() );
		m_renderRegionNodes.push_back( t3dRenderRegionNode_t{} );
	}
	t3dRegionList_t &list = m_szRegionRenderLists[nRegionIndex];
	t3dRenderRegionNode_t &node = m_renderRegionNodes[( size_t )( node_index )];
	node.m_iRenderCell = iRenderCell;
	node.m_nPrev = UINT32_MAX;
	node.m_nNext = list.m_nHead;
	if ( list.m_nHead != UINT32_MAX ) {
		m_renderRegionNodes[( size_t )( list.m_nHead )].m_nPrev = node_index;
	}
	list.m_nHead = node_index;
	++list.m_nCount;
	m_regionRenderMask.set( nRegionIndex );
	return node_index;
}

void T3DInstancer::releaseRenderRegionNode( const uint32_t nNode, const uint16_t nRegionIndex ) {
	if ( nNode == UINT32_MAX || nNode >= m_renderRegionNodes.size() ||
			nRegionIndex >= s_nInstanceRegionTableCapacity ) {
		return;
	}
	t3dRegionList_t &list = m_szRegionRenderLists[nRegionIndex];
	t3dRenderRegionNode_t &node = m_renderRegionNodes[( size_t )( nNode )];
	if ( node.m_nPrev != UINT32_MAX ) {
		m_renderRegionNodes[( size_t )( node.m_nPrev )].m_nNext = node.m_nNext;
	} else if ( list.m_nHead == nNode ) {
		list.m_nHead = node.m_nNext;
	}
	if ( node.m_nNext != UINT32_MAX ) {
		m_renderRegionNodes[( size_t )( node.m_nNext )].m_nPrev = node.m_nPrev;
	}
	if ( list.m_nCount > 0u ) {
		--list.m_nCount;
	}
	if ( list.m_nCount == 0u ) {
		list.m_nHead = UINT32_MAX;
		m_regionRenderMask.reset( nRegionIndex );
	}
	node.m_nPrev = UINT32_MAX;
	node.m_nNext = m_nRenderRegionFreeHead;
	m_nRenderRegionFreeHead = nNode;
}

uint32_t T3DInstancer::allocateLogicalRegionNode( const t3dCellUpdateKey_t &rKey, const uint16_t nRegionIndex ) {
	uint32_t node_index = UINT32_MAX;
	if ( m_nLogicalRegionFreeHead != UINT32_MAX ) {
		node_index = m_nLogicalRegionFreeHead;
		m_nLogicalRegionFreeHead = m_logicalRegionNodes[( size_t )( node_index )].m_nNext;
	} else {
		node_index = ( uint32_t )( m_logicalRegionNodes.size() );
		m_logicalRegionNodes.push_back( t3dLogicalRegionNode_t{} );
	}
	t3dRegionList_t &list = m_szRegionLogicalLists[nRegionIndex];
	t3dLogicalRegionNode_t &node = m_logicalRegionNodes[( size_t )( node_index )];
	node.m_key = rKey;
	node.m_nPrev = UINT32_MAX;
	node.m_nNext = list.m_nHead;
	if ( list.m_nHead != UINT32_MAX ) {
		m_logicalRegionNodes[( size_t )( list.m_nHead )].m_nPrev = node_index;
	}
	list.m_nHead = node_index;
	++list.m_nCount;
	m_regionLogicalMask.set( nRegionIndex );
	return node_index;
}

void T3DInstancer::releaseLogicalRegionNode( const uint32_t nNode, const uint16_t nRegionIndex ) {
	if ( nNode == UINT32_MAX || nNode >= m_logicalRegionNodes.size() ||
			nRegionIndex >= s_nInstanceRegionTableCapacity ) {
		return;
	}
	t3dRegionList_t &list = m_szRegionLogicalLists[nRegionIndex];
	t3dLogicalRegionNode_t &node = m_logicalRegionNodes[( size_t )( nNode )];
	if ( node.m_nPrev != UINT32_MAX ) {
		m_logicalRegionNodes[( size_t )( node.m_nPrev )].m_nNext = node.m_nNext;
	} else if ( list.m_nHead == nNode ) {
		list.m_nHead = node.m_nNext;
	}
	if ( node.m_nNext != UINT32_MAX ) {
		m_logicalRegionNodes[( size_t )( node.m_nNext )].m_nPrev = node.m_nPrev;
	}
	if ( list.m_nCount > 0u ) {
		--list.m_nCount;
	}
	if ( list.m_nCount == 0u ) {
		list.m_nHead = UINT32_MAX;
		m_regionLogicalMask.reset( nRegionIndex );
	}
	node.m_nPrev = UINT32_MAX;
	node.m_nNext = m_nLogicalRegionFreeHead;
	m_nLogicalRegionFreeHead = nNode;
}

uint32_t T3DInstancer::allocateTransitionCellNode( const t3dCellUpdateKey_t &rKey, const uint32_t nNext ) {
	uint32_t node_index = UINT32_MAX;
	if ( m_nTransitionCellFreeHead != UINT32_MAX ) {
		node_index = m_nTransitionCellFreeHead;
		m_nTransitionCellFreeHead = m_transitionCellNodes[( size_t )( node_index )].m_nNext;
	} else {
		node_index = ( uint32_t )( m_transitionCellNodes.size() );
		m_transitionCellNodes.push_back( t3dTransitionCellNode_t{} );
	}
	t3dTransitionCellNode_t &node = m_transitionCellNodes[( size_t )( node_index )];
	node.m_key = rKey;
	node.m_nNext = nNext;
	return node_index;
}

void T3DInstancer::releaseTransitionCellNode( const uint32_t nNode ) {
	if ( nNode == UINT32_MAX || nNode >= m_transitionCellNodes.size() ) {
		return;
	}
	m_transitionCellNodes[( size_t )( nNode )].m_nNext = m_nTransitionCellFreeHead;
	m_nTransitionCellFreeHead = nNode;
}

void T3DInstancer::clearRegionRenderLists( void ) {
	for ( size_t index = 0; index < s_nInstanceRegionTableCapacity; ++index ) {
		m_szRegionRenderLists[index] = t3dRegionList_t{};
	}
	m_renderRegionNodes.clear();
	m_nRenderRegionFreeHead = UINT32_MAX;
	m_regionRenderMask.reset();
}

void T3DInstancer::clearRegionLogicalLists( void ) {
	for ( size_t index = 0; index < s_nInstanceRegionTableCapacity; ++index ) {
		m_szRegionLogicalLists[index] = t3dRegionList_t{};
	}
	m_logicalRegionNodes.clear();
	m_nLogicalRegionFreeHead = UINT32_MAX;
	m_regionLogicalMask.reset();
}

t3dRenderCell_t *
T3DInstancer::findRenderCell( const Vector2i &region, const int nMeshId, const int nLod, const Vector2i &cell ) {
	const t3dRenderCellKey_t key{ region, cell, nMeshId, nLod };
	const size_t *const pIndex = m_iRenderCellIndex.find( key );
	if ( !pIndex || *pIndex >= m_renderCells.size() ) {
		return nullptr;
	}
	return &m_renderCells[*pIndex];
}

t3dRenderCell_t *
T3DInstancer::getOrCreateRenderCell( const Vector2i &region, const int nMeshId, const int nLod, const Vector2i &cell ) {
	const t3dRenderCellKey_t key{ region, cell, nMeshId, nLod };
	if ( const size_t *const pIndex = m_iRenderCellIndex.find( key ) ) {
		return *pIndex < m_renderCells.size() ? &m_renderCells[*pIndex] : nullptr;
	}
	const int region_index = getRegionTableIndex( region );
	if ( region_index < 0 || region_index >= ( int )( s_nInstanceRegionTableCapacity ) ) {
		LOG( ERROR, "Instance cell region is outside the fixed terrain map: ", region );
		return nullptr;
	}
	t3dRenderCell_t value;
	value.m_region = region;
	value.m_cell = cell;
	value.m_nMeshId = nMeshId;
	value.m_nLod = nLod;
	const size_t index = m_renderCells.size();
	m_renderCells.push_back( eastl::move( value ) );
	t3dRenderCell_t &render_cell = m_renderCells.back();
	render_cell.m_nRegionListNode = allocateRenderRegionNode( index, ( uint16_t )( region_index ) );
	if ( !m_iRenderCellIndex.insertOrAssign( key, index ) ) {
		releaseRenderRegionNode( render_cell.m_nRegionListNode, ( uint16_t )( region_index ) );
		m_renderCells.pop_back();
		return nullptr;
	}
	return &render_cell;
}

void T3DInstancer::eraseRenderCellAt( const size_t iIndex ) {
	if ( iIndex >= m_renderCells.size() ) {
		return;
	}
	t3dRenderCell_t &dead = m_renderCells[iIndex];
	m_iRenderCellIndex.erase( makeRenderKey( dead ) );
	const int dead_region_index = getRegionTableIndex( dead.m_region );
	if ( dead_region_index >= 0 && dead_region_index < ( int )( s_nInstanceRegionTableCapacity ) ) {
		releaseRenderRegionNode( dead.m_nRegionListNode, ( uint16_t )( dead_region_index ) );
	}
	if ( dead.m_hMmi.is_valid() ) {
		RS->free_rid( dead.m_hMmi );
	}
	if ( dead.m_nLod != s_nMeshShadowLodId && dead.m_hMultimesh.is_valid() ) {
		RS->free_rid( dead.m_hMultimesh );
	}

	const size_t last = m_renderCells.size() - 1u;
	if ( iIndex != last ) {
		m_renderCells[iIndex] = eastl::move( m_renderCells[last] );
		m_iRenderCellIndex.insertOrAssign( makeRenderKey( m_renderCells[iIndex] ), iIndex );
		const uint32_t moved_node = m_renderCells[iIndex].m_nRegionListNode;
		if ( moved_node != UINT32_MAX && moved_node < m_renderRegionNodes.size() ) {
			m_renderRegionNodes[( size_t )( moved_node )].m_iRenderCell = iIndex;
		}
	}
	m_renderCells.pop_back();
}

void T3DInstancer::eraseRenderCell( const Vector2i &region, const int nMeshId, const int nLod, const Vector2i &cell ) {
	const t3dRenderCellKey_t key{ region, cell, nMeshId, nLod };
	if ( const size_t *const pIndex = m_iRenderCellIndex.find( key ) ) {
		eraseRenderCellAt( *pIndex );
	}
}

void T3DInstancer::destroyRenderCells( void ) {
	while ( !m_renderCells.empty() ) {
		eraseRenderCellAt( m_renderCells.size() - 1 );
	}
	m_iRenderCellIndex.clear();
	clearRegionRenderLists();
	clearSingleLodTransitionBuckets();
}

void T3DInstancer::markLogicalMeshCountDirty( const int nMeshId ) {
	if ( nMeshId < 0 || nMeshId >= s_nMaxMeshAssets || !m_pTerrain || !m_pTerrain->getAssets().is_valid() ) {
		return;
	}
	m_dirtyLogicalMeshCounts.set( ( size_t )( nMeshId ) );
}

void T3DInstancer::flushLogicalMeshCounts( void ) {
	if ( m_bPublishingLogicalMeshCounts || m_dirtyLogicalMeshCounts.none() || !m_pTerrain ||
			!m_pTerrain->getAssets().is_valid() ) {
		return;
	}
	// Reset before emitting. A synchronous callback may mutate instance storage
	// and mark a count dirty again; that follow-up publication remains queued for
	// the next safe flush instead of recursively re-entering this loop.
	const eastl::bitset<s_nMaxMeshAssets> dirty_counts = m_dirtyLogicalMeshCounts;
	m_dirtyLogicalMeshCounts.reset();
	m_bPublishingLogicalMeshCounts = true;
	for ( int mesh_id = 0; mesh_id < s_nMaxMeshAssets; ++mesh_id ) {
		if ( !dirty_counts.test( ( size_t )( mesh_id ) ) ) {
			continue;
		}
		const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( mesh_id );
		if ( pAsset.is_null() ) {
			continue;
		}
		const T3DData *pData = m_pTerrain->getData();
		const uint64_t total = pData && pData->isInstanceRegionCacheEnabled()
				? t3dRepositoryMeshInstanceCount( pData, mesh_id )
				: m_szLogicalMeshInstanceTotals[( size_t )( mesh_id )];
		pAsset->setInstanceCount( total > ( uint64_t )( UINT32_MAX ) ? UINT32_MAX : ( uint32_t )( total ) );
	}
	m_bPublishingLogicalMeshCounts = false;
	if ( m_dirtyLogicalMeshCounts.any() ) {
		scheduleUpdateProcessing();
	}
}

void T3DInstancer::publishLogicalMeshCount( const int nMeshId ) {
	markLogicalMeshCountDirty( nMeshId );
	if ( !m_bMaterializingUpdates && !m_bProcessingPendingUpdates && !m_bPublishingLogicalMeshCounts ) {
		flushLogicalMeshCounts();
	}
}

void T3DInstancer::setLogicalCellCount( const Vector2i &vRegion,
		const int nMeshId,
		const Vector2i &vCell,
		const size_t nCount ) {
	if ( nMeshId < 0 || nMeshId >= s_nMaxMeshAssets ) {
		return;
	}
	if ( nCount == 0u ) {
		eraseLogicalCellCount( vRegion, nMeshId, vCell );
		return;
	}
	const int region_index = getRegionTableIndex( vRegion );
	if ( region_index < 0 || region_index >= ( int )( s_nInstanceRegionTableCapacity ) ) {
		LOG( ERROR, "Logical instance cell region is outside the fixed terrain map: ", vRegion );
		return;
	}
	const uint32_t new_count = nCount > ( size_t )( UINT32_MAX ) ? UINT32_MAX : ( uint32_t )( nCount );
	const t3dCellUpdateKey_t key{ vRegion, vCell, nMeshId };
	t3dLogicalCellState_t *pState = m_logicalCellCounts.find( key );
	const uint32_t old_count = pState ? pState->m_nCount : 0u;
	if ( old_count == new_count ) {
		publishLogicalMeshCount( nMeshId );
		return;
	}
	if ( !pState ) {
		t3dLogicalCellState_t state;
		state.m_nCount = new_count;
		state.m_nRegionListNode = allocateLogicalRegionNode( key, ( uint16_t )( region_index ) );
		pState = m_logicalCellCounts.insertOrAssign( key, state );
		if ( !pState ) {
			releaseLogicalRegionNode( state.m_nRegionListNode, ( uint16_t )( region_index ) );
			return;
		}
	} else {
		pState->m_nCount = new_count;
	}
	uint64_t &mesh_total = m_szLogicalMeshInstanceTotals[( size_t )( nMeshId )];
	mesh_total = mesh_total >= ( uint64_t )( old_count ) ? mesh_total - ( uint64_t )( old_count ) : 0u;
	mesh_total =
			UINT64_MAX - mesh_total < ( uint64_t )( new_count ) ? UINT64_MAX : mesh_total + ( uint64_t )( new_count );
	publishLogicalMeshCount( nMeshId );
}

void T3DInstancer::eraseLogicalCellCount( const Vector2i &vRegion, const int nMeshId, const Vector2i &vCell ) {
	const t3dCellUpdateKey_t key{ vRegion, vCell, nMeshId };
	untrackSingleLodCell( key );
	t3dLogicalCellState_t *const pState = m_logicalCellCounts.find( key );
	const int region_index = getRegionTableIndex( vRegion );
	uint32_t node_index = pState ? pState->m_nRegionListNode : UINT32_MAX;
	if ( node_index == UINT32_MAX && region_index >= 0 && region_index < ( int )( s_nInstanceRegionTableCapacity ) ) {
		for ( uint32_t node = m_szRegionLogicalLists[( size_t )( region_index )].m_nHead; node != UINT32_MAX;
				node = m_logicalRegionNodes[( size_t )( node )].m_nNext ) {
			if ( m_logicalRegionNodes[( size_t )( node )].m_key == key ) {
				node_index = node;
				break;
			}
		}
	}
	if ( node_index != UINT32_MAX && region_index >= 0 ) {
		releaseLogicalRegionNode( node_index, ( uint16_t )( region_index ) );
	}
	if ( !pState ) {
		return;
	}
	const uint32_t old_count = pState->m_nCount;
	m_logicalCellCounts.erase( key );
	uint64_t &mesh_total = m_szLogicalMeshInstanceTotals[( size_t )( nMeshId )];
	mesh_total = mesh_total >= ( uint64_t )( old_count ) ? mesh_total - ( uint64_t )( old_count ) : 0u;
	publishLogicalMeshCount( nMeshId );
}

void T3DInstancer::eraseLogicalCellsByRegion( const Vector2i &vRegion ) {
	const int region_index = getRegionTableIndex( vRegion );
	if ( region_index < 0 || region_index >= ( int )( s_nInstanceRegionTableCapacity ) ) {
		return;
	}
	while ( m_szRegionLogicalLists[( size_t )( region_index )].m_nHead != UINT32_MAX ) {
		const uint32_t node = m_szRegionLogicalLists[( size_t )( region_index )].m_nHead;
		const t3dCellUpdateKey_t key = m_logicalRegionNodes[( size_t )( node )].m_key;
		eraseLogicalCellCount( key.m_region, key.m_nMeshId, key.m_cell );
	}
}

void T3DInstancer::eraseLogicalCellsByLocation( const Vector2i &vRegion, const int nMeshId ) {
	const int region_index = getRegionTableIndex( vRegion );
	if ( region_index < 0 || region_index >= ( int )( s_nInstanceRegionTableCapacity ) ) {
		return;
	}
	uint32_t node = m_szRegionLogicalLists[( size_t )( region_index )].m_nHead;
	while ( node != UINT32_MAX ) {
		const uint32_t next = m_logicalRegionNodes[( size_t )( node )].m_nNext;
		const t3dCellUpdateKey_t key = m_logicalRegionNodes[( size_t )( node )].m_key;
		if ( key.m_nMeshId == nMeshId ) {
			eraseLogicalCellCount( key.m_region, key.m_nMeshId, key.m_cell );
		}
		node = next;
	}
}

void T3DInstancer::clearLogicalCellCounts( void ) {
	m_logicalCellCounts.clear();
	clearRegionLogicalLists();
	std::fill_n( m_szLogicalMeshInstanceTotals, s_nMaxMeshAssets, ( uint64_t )( 0 ) );
	if ( !m_pTerrain || !m_pTerrain->getAssets().is_valid() ) {
		return;
	}
	for ( int mesh_id = 0; mesh_id < m_pTerrain->getAssets()->getMeshCount(); ++mesh_id ) {
		markLogicalMeshCountDirty( mesh_id );
	}
	if ( !m_bMaterializingUpdates && !m_bProcessingPendingUpdates ) {
		flushLogicalMeshCounts();
	}
}

void T3DInstancer::rebuildLogicalCountsFromStore( void ) {
	if ( !m_pTerrain || !m_pTerrain->getData() ) {
		return;
	}
	const bool bWasMaterializing = m_bMaterializingUpdates;
	m_bMaterializingUpdates = true;
	clearLogicalCellCounts();
	T3DRegion *const *ppRegionPtrs = m_pTerrain->getData()->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		T3DRegion *const pRegion = ppRegionPtrs[region_index];
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const Vector2i regionLocation = pRegion->getLocation();
		if ( m_pTerrain->getData()->isInstanceRegionCacheEnabled() &&
				!m_pTerrain->getData()->isInstanceRegionResident( regionLocation ) ) {
			continue;
		}
		for ( const t3dInstanceMesh_t &mesh : pRegion->getInstanceStore().meshes() ) {
			if ( mesh.m_nId < 0 || mesh.m_nId >= s_nMaxMeshAssets ) {
				continue;
			}
			for ( const t3dInstanceCell_t &cell : mesh.m_cells ) {
				setLogicalCellCount( regionLocation, mesh.m_nId, cell.m_location, cell.m_transforms.size() );
			}
		}
	}
	m_bMaterializingUpdates = bWasMaterializing;
	if ( !bWasMaterializing && !m_bProcessingPendingUpdates ) {
		flushLogicalMeshCounts();
	}
}

void T3DInstancer::reconcileLogicalRegion( T3DRegion *const pRegion, const int nMeshId ) {
	const bool bWasMaterializing = m_bMaterializingUpdates;
	m_bMaterializingUpdates = true;
	if ( !pRegion || pRegion->isDeleted() ) {
		if ( pRegion ) {
			if ( nMeshId >= 0 ) {
				eraseLogicalCellsByLocation( pRegion->getLocation(), nMeshId );
			} else {
				eraseLogicalCellsByRegion( pRegion->getLocation() );
			}
		}
	} else if ( m_pTerrain && m_pTerrain->getData()->isInstanceRegionCacheEnabled() &&
			!m_pTerrain->getData()->isInstanceRegionResident( pRegion->getLocation() ) ) {
		if ( nMeshId >= 0 ) {
			eraseLogicalCellsByLocation( pRegion->getLocation(), nMeshId );
		} else {
			eraseLogicalCellsByRegion( pRegion->getLocation() );
		}
	} else {
		const Vector2i regionLocation = pRegion->getLocation();
		const T3DInstanceStore &store = pRegion->getInstanceStore();
		if ( nMeshId >= 0 ) {
			eraseLogicalCellsByLocation( regionLocation, nMeshId );
			const t3dInstanceMesh_t *pMesh = store.findMesh( nMeshId );
			if ( pMesh ) {
				for ( const t3dInstanceCell_t &cell : pMesh->m_cells ) {
					setLogicalCellCount( regionLocation, nMeshId, cell.m_location, cell.m_transforms.size() );
				}
			}
		} else {
			eraseLogicalCellsByRegion( regionLocation );
			for ( const t3dInstanceMesh_t &pMesh : store.meshes() ) {
				if ( pMesh.m_nId < 0 || pMesh.m_nId >= s_nMaxMeshAssets ) {
					continue;
				}
				for ( const t3dInstanceCell_t &cell : pMesh.m_cells ) {
					setLogicalCellCount( regionLocation, pMesh.m_nId, cell.m_location, cell.m_transforms.size() );
				}
			}
		}
	}
	m_bMaterializingUpdates = bWasMaterializing;
	if ( !bWasMaterializing && !m_bProcessingPendingUpdates ) {
		flushLogicalMeshCounts();
	}
}

void T3DInstancer::swapLogicalMeshIds( const int nSrcId, const int nDstId ) {
	if ( nSrcId == nDstId || nSrcId < 0 || nDstId < 0 || nSrcId >= s_nMaxMeshAssets || nDstId >= s_nMaxMeshAssets ) {
		return;
	}
	if ( m_logicalCellCounts.empty() ) {
		std::swap( m_szLogicalMeshInstanceTotals[( size_t )( nSrcId )],
				m_szLogicalMeshInstanceTotals[( size_t )( nDstId )] );
		publishLogicalMeshCount( nSrcId );
		publishLogicalMeshCount( nDstId );
		return;
	}
	typedef struct t3dLogicalSwapEntry_s {
		t3dCellUpdateKey_t m_key;
		uint32_t m_nCount;
	} t3dLogicalSwapEntry_t;
	eastl::vector<t3dLogicalSwapEntry_t> entries;
	entries.reserve( m_logicalCellCounts.size() );
	m_logicalCellCounts.forEach( [&]( const t3dCellUpdateKey_t &rKey, const t3dLogicalCellState_t &rState ) {
		t3dCellUpdateKey_t key = rKey;
		if ( key.m_nMeshId == nSrcId ) {
			key.m_nMeshId = nDstId;
		} else if ( key.m_nMeshId == nDstId ) {
			key.m_nMeshId = nSrcId;
		}
		entries.push_back( t3dLogicalSwapEntry_t{ key, rState.m_nCount } );
	} );
	m_logicalCellCounts.clear();
	clearRegionLogicalLists();
	m_logicalCellCounts.reserve( entries.size() );
	for ( const t3dLogicalSwapEntry_t &entry : entries ) {
		const int region_index = getRegionTableIndex( entry.m_key.m_region );
		if ( region_index < 0 ) {
			continue;
		}
		t3dLogicalCellState_t state;
		state.m_nCount = entry.m_nCount;
		state.m_nRegionListNode = allocateLogicalRegionNode( entry.m_key, ( uint16_t )( region_index ) );
		m_logicalCellCounts.insertOrAssign( entry.m_key, state );
	}
	std::swap(
			m_szLogicalMeshInstanceTotals[( size_t )( nSrcId )], m_szLogicalMeshInstanceTotals[( size_t )( nDstId )] );
	publishLogicalMeshCount( nSrcId );
	publishLogicalMeshCount( nDstId );
}

void T3DInstancer::reconcileRegionRenderCells( T3DRegion *const pRegion, const int nMeshId ) {
	if ( !pRegion ) {
		return;
	}
	if ( m_pTerrain && m_pTerrain->getData()->isInstanceRegionCacheEnabled() &&
			!m_pTerrain->getData()->isInstanceRegionResident( pRegion->getLocation() ) ) {
		destroyMmisByRegion( pRegion->getLocation() );
		return;
	}
	const Vector2i regionLocation = pRegion->getLocation();
	const int region_index = getRegionTableIndex( regionLocation );
	if ( region_index < 0 ) {
		return;
	}
	const T3DInstanceStore &store = pRegion->getInstanceStore();
	uint32_t logical_node = m_szRegionLogicalLists[( size_t )( region_index )].m_nHead;
	while ( logical_node != UINT32_MAX ) {
		const uint32_t next = m_logicalRegionNodes[( size_t )( logical_node )].m_nNext;
		const t3dCellUpdateKey_t key = m_logicalRegionNodes[( size_t )( logical_node )].m_key;
		if ( nMeshId < 0 || key.m_nMeshId == nMeshId ) {
			const t3dInstanceCell_t *pCell = store.findCell( key.m_nMeshId, key.m_cell );
			if ( !pCell || pCell->m_transforms.empty() ) {
				eraseLogicalCellCount( key.m_region, key.m_nMeshId, key.m_cell );
			}
		}
		logical_node = next;
	}
	uint32_t render_node = m_szRegionRenderLists[( size_t )( region_index )].m_nHead;
	while ( render_node != UINT32_MAX ) {
		const uint32_t next = m_renderRegionNodes[( size_t )( render_node )].m_nNext;
		const size_t render_index = m_renderRegionNodes[( size_t )( render_node )].m_iRenderCell;
		if ( render_index >= m_renderCells.size() ) {
			releaseRenderRegionNode( render_node, ( uint16_t )( region_index ) );
			render_node = next;
			continue;
		}
		const t3dRenderCell_t &render = m_renderCells[render_index];
		if ( nMeshId < 0 || render.m_nMeshId == nMeshId ) {
			const t3dInstanceCell_t *pCell = store.findCell( render.m_nMeshId, render.m_cell );
			if ( !pCell || pCell->m_transforms.empty() ) {
				eraseRenderCellAt( render_index );
			}
		}
		render_node = next;
	}
}

void T3DInstancer::clearRegionUpdates( const bool bActive ) {
	t3dRegionUpdateState_t *pStates = bActive ? m_szActiveRegionUpdates : m_szRegionUpdates;
	eastl::bitset<s_nInstanceRegionUpdateCapacity> &mask = bActive ? m_activeRegionUpdateMask : m_regionUpdateMask;
	uint16_t *const pIndices = bActive ? m_szActiveRegionUpdateIndices : m_szRegionUpdateIndices;
	int &count = bActive ? m_nActiveRegionUpdateCount : m_nRegionUpdateCount;
	for ( int list_index = 0; list_index < count; ++list_index ) {
		const uint16_t index = pIndices[list_index];
		pStates[( size_t )( index )] = t3dRegionUpdateState_t{};
	}
	count = 0;
	mask.reset();
}

void T3DInstancer::snapshotRegionUpdates( void ) {
	clearRegionUpdates( true );
	if ( m_nRegionUpdateCount > ( int )( s_nInstanceRegionUpdateCapacity ) ) {
		LOG( ERROR, "Region update count exceeded fixed capacity during snapshot" );
		m_nRegionUpdateCount = ( int )( s_nInstanceRegionUpdateCapacity );
	}
	for ( int list_index = 0; list_index < m_nRegionUpdateCount; ++list_index ) {
		const uint16_t index = m_szRegionUpdateIndices[list_index];
		m_szActiveRegionUpdates[( size_t )( index )] = m_szRegionUpdates[( size_t )( index )];
		m_activeRegionUpdateMask.set( ( size_t )( index ) );
		m_szActiveRegionUpdateIndices[m_nActiveRegionUpdateCount++] = index;
		m_szRegionUpdates[( size_t )( index )] = t3dRegionUpdateState_t{};
	}
	m_nRegionUpdateCount = 0;
	m_regionUpdateMask.reset();
}

t3dRegionUpdateState_t *T3DInstancer::getRegionUpdate( const Vector2i &vRegion ) {
	const int index = T3DData::getRegionMapIndex( vRegion );
	if ( index < 0 ) {
		return nullptr;
	}
	if ( !m_regionUpdateMask.test( ( size_t )( index ) ) ) {
		if ( m_nRegionUpdateCount >= ( int )( s_nInstanceRegionUpdateCapacity ) ) {
			LOG( ERROR, "Region update queue exceeded fixed terrain region capacity" );
			return nullptr;
		}
		m_szRegionUpdateIndices[m_nRegionUpdateCount] = ( uint16_t )( index );
		++m_nRegionUpdateCount;
		m_regionUpdateMask.set( ( size_t )( index ) );
	}
	return &m_szRegionUpdates[( size_t )( index )];
}

const t3dRegionUpdateState_t *T3DInstancer::findActiveRegionUpdate( const Vector2i &vRegion ) const {
	const int index = T3DData::getRegionMapIndex( vRegion );
	return index >= 0 && m_activeRegionUpdateMask.test( ( size_t )( index ) )
			? &m_szActiveRegionUpdates[( size_t )( index )]
			: nullptr;
}

Vector2i T3DInstancer::regionLocationFromUpdateIndex( const uint16_t iIndex ) {
	const int x = ( int )( iIndex ) % s_nActiveMaxRegionMapSize - s_nActiveMaxRegionMapSize / 2;
	const int y = ( int )( iIndex ) / s_nActiveMaxRegionMapSize - s_nActiveMaxRegionMapSize / 2;
	return Vector2i( x, y );
}

bool T3DInstancer::hasPendingUpdates( void ) const {
	return m_iPendingCellUpdateCursor < m_pendingCellUpdates.size() || m_bRebuildAll || m_bUpdateAll ||
			m_globalMeshUpdates.any() || m_nRegionUpdateCount > 0 || !m_streamingUpdates.empty() ||
			m_dirtyLogicalMeshCounts.any();
}

void T3DInstancer::markPendingRegionWork( const Vector2i &rRegion ) {
	const int region_index = getRegionTableIndex( rRegion );
	if ( region_index >= 0 ) {
		m_pendingRegionWorkMask.set( ( size_t )( region_index ) );
	}
}

void T3DInstancer::rebuildPendingRegionWorkMask( void ) {
	m_pendingRegionWorkMask.reset();
	m_streamingUpdates.forEach( [&]( const t3dCellUpdateKey_t &rKey ) {
		markPendingRegionWork( rKey.m_region );
	} );
	m_activeStreamingUpdates.forEach( [&]( const t3dCellUpdateKey_t &rKey ) {
		markPendingRegionWork( rKey.m_region );
	} );
	for ( size_t i = m_iPendingCellUpdateCursor; i < m_pendingCellUpdates.size(); ++i ) {
		markPendingRegionWork( m_pendingCellUpdates[i].m_region );
	}
	for ( const t3dCellUpdateKey_t &key : m_buildingCellUpdates ) {
		markPendingRegionWork( key.m_region );
	}
}

bool T3DInstancer::hasPendingUpdatesForRegion( const Vector2i &rRegion ) const {
	// Global renderer changes can affect every detailed HLOD source. For ordinary
	// streaming churn, however, readiness must be scoped to the region being
	// handed off or a moving camera elsewhere could prevent a proxy from ever
	// retiring.
	if ( m_bRebuildAll || m_bUpdateAll || m_globalMeshUpdates.any() ) {
		return true;
	}
	const int region_index = getRegionTableIndex( rRegion );
	if ( region_index < 0 ) {
		return false;
	}
	if ( m_regionUpdateMask.test( ( size_t )region_index ) ||
			m_activeRegionUpdateMask.test( ( size_t )region_index ) ) {
		return true;
	}
	return m_pendingRegionWorkMask.test( ( size_t )region_index );
}

void T3DInstancer::scheduleUpdateProcessing( void ) {
	const Callable callback = callable_mp( this, &T3DInstancer::processUpdates );
	if ( !RS->is_connected( "frame_pre_draw", callback ) ) {
		RS->connect( "frame_pre_draw", callback );
	}
}

void T3DInstancer::refreshStreamingCache( void ) {
	if ( !m_bStreamingCacheDirty || !m_pTerrain || !m_pTerrain->getAssets().is_valid() ) {
		return;
	}
	std::fill_n( m_szMeshStreamingRadius, s_nMaxMeshAssets, ( real_t )( 0.0f ) );
	m_nFiniteStreamingMeshCount = 0;
	m_fMaxStreamingRadius = 0.0f;
	const real_t fCellWorldSize = m_nCellSize * m_pTerrain->getVertexSpacing();
	const int mesh_count = MIN( m_pTerrain->getAssets()->getMeshCount(), s_nMaxMeshAssets );
	for ( int mesh_id = 0; mesh_id < mesh_count; ++mesh_id ) {
		const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( mesh_id );
		if ( pAsset.is_null() || pAsset->getLastLod() < 0 ) {
			continue;
		}
		const real_t fVisibilityEnd = pAsset->getLodRangeEnd( pAsset->getLastLod() );
		// Zero is Terrain3D/Godot's unlimited-range convention. Keep a zero
		// cached radius so residency checks compile down to an early return.
		if ( fVisibilityEnd < 0.001f ) {
			continue;
		}
		const real_t fRadius = fVisibilityEnd + m_fCellStreamingMargin + fCellWorldSize * 0.70710678f;
		m_szMeshStreamingRadius[mesh_id] = fRadius;
		if ( m_nFiniteStreamingMeshCount >= s_nMaxMeshAssets ) {
			LOG( ERROR, "Finite streaming mesh list exceeded asset capacity" );
			break;
		}
		m_szFiniteStreamingMeshIds[m_nFiniteStreamingMeshCount++] = ( uint16_t )( mesh_id );
		m_fMaxStreamingRadius = MAX( m_fMaxStreamingRadius, fRadius );
	}
	m_bStreamingCacheDirty = false;
}

bool T3DInstancer::cellShouldBeResident( const Vector2i &region, const Vector2i &cell, const int nMeshId ) const {
	if ( !m_bStreamPositionValid ) {
		return true;
	}
	return cellShouldBeResidentAt( region, cell, nMeshId, m_lastStreamPosition );
}

bool T3DInstancer::cellShouldBeResidentAt( const Vector2i &region,
		const Vector2i &cell,
		const int nMeshId,
		const Vector3 &rTargetPosition ) const {
	if ( nMeshId < 0 || nMeshId >= s_nMaxMeshAssets ) {
		return true;
	}
	// HLOD is a representation decision above ordinary cell streaming. It must
	// suppress represented detail even when runtime cell streaming is disabled
	// (including the editor HLOD preview, which intentionally keeps authoring
	// residency semantics for the rest of Terrain3D).
	if ( m_pTerrain ) {
		const uint8_t hlod_min = m_pTerrain->getHlodInstancerMinimumLevel( region, nMeshId );
		if ( hlod_min > 0 && m_pTerrain->isHlodRegionSuppressed( region, hlod_min ) ) {
			return false;
		}
	}
	if ( !m_bCellStreamingEnabled ) {
		return true;
	}
	// Asset visibility still decides which cells inside a resident region are
	// worth drawing, but the region streaming map is the authoritative outer
	// residency envelope. Ordinary instances do not survive into COLD/UNLOADED.
	if ( m_pTerrain->getRegionStreamingStateInternal( region ) < TERRAIN3D_REGION_STREAMING_HOT ) {
		return false;
	}
	const real_t fConservativeRadius = m_szMeshStreamingRadius[nMeshId];
	if ( fConservativeRadius <= 0.0f ) {
		return true;
	}
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	const real_t fRegionWorldSize = ( real_t )( m_pTerrain->getRegionSize() ) * fSpacing;
	const real_t fCellWorldSize = m_nCellSize * fSpacing;
	const Vector2 center( region.x * fRegionWorldSize + ( cell.x + 0.5f ) * fCellWorldSize,
			region.y * fRegionWorldSize + ( cell.y + 0.5f ) * fCellWorldSize );
	const Vector2 target( rTargetPosition.x, rTargetPosition.z );
	return center.distance_squared_to( target ) <= fConservativeRadius * fConservativeRadius;
}

int T3DInstancer::selectCellLod( const Vector2i &vRegion,
		const Vector2i &vCell,
		const Ref<T3DMeshAsset> &rAsset,
		const Vector3 &rTargetPosition ) const {
	if ( rAsset.is_null() || rAsset->getLastLod() < 0 ) {
		return -1;
	}
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	const real_t fRegionWorldSize = ( real_t )( m_pTerrain->getRegionSize() ) * fSpacing;
	const real_t fCellWorldSize = m_nCellSize * fSpacing;
	const Vector2 cellCenter( vRegion.x * fRegionWorldSize + ( vCell.x + 0.5f ) * fCellWorldSize,
			vRegion.y * fRegionWorldSize + ( vCell.y + 0.5f ) * fCellWorldSize );
	const Vector2 target( rTargetPosition.x, rTargetPosition.z );
	const real_t fDistanceSquared = cellCenter.distance_squared_to( target );
	int fallback_lod = -1;
	for ( int lod = 0; lod <= rAsset->getLastLod(); ++lod ) {
		if ( rAsset->getCastShadows() == s_shadowCastingOnly &&
				( lod > rAsset->getLastShadowLod() || lod < rAsset->getShadowImpostor() ) ) {
			continue;
		}
		fallback_lod = lod;
		const real_t fRangeEnd = rAsset->getLodRangeEnd( lod );
		if ( fRangeEnd < 0.001f || fDistanceSquared <= fRangeEnd * fRangeEnd ) {
			return lod;
		}
	}
	return fallback_lod;
}

Vector2i T3DInstancer::getGlobalCell( const Vector2i &vRegion, const Vector2i &vCell ) const {
	const int cells_per_region = getCellsPerRegionAxis();
	if ( cells_per_region <= 0 ) {
		return vCell;
	}
	return Vector2i( vRegion.x * cells_per_region + vCell.x, vRegion.y * cells_per_region + vCell.y );
}

t3dTransitionBucketKey_t T3DInstancer::getTransitionBucketKey( const t3dCellUpdateKey_t &rKey ) const {
	const Vector2i global_cell = getGlobalCell( rKey.m_region, rKey.m_cell );
	return t3dTransitionBucketKey_t{ Vector2i( floorDivCell( global_cell.x, s_nInstanceTransitionBucketCellSpan ),
											 floorDivCell( global_cell.y, s_nInstanceTransitionBucketCellSpan ) ),
		rKey.m_nMeshId };
}

void T3DInstancer::trackSingleLodCell( const t3dCellUpdateKey_t &rKey ) {
	if ( !m_bSingleLodInstanceBuffers || rKey.m_nMeshId < 0 || rKey.m_nMeshId >= s_nMaxMeshAssets ) {
		return;
	}
	const t3dTransitionBucketKey_t bucket_key = getTransitionBucketKey( rKey );
	t3dTransitionBucketState_t *const pBucket = m_singleLodTransitionBuckets.getOrInsert( bucket_key );
	if ( !pBucket ) {
		return;
	}
	for ( uint32_t node = pBucket->m_nHead; node != UINT32_MAX;
			node = m_transitionCellNodes[( size_t )( node )].m_nNext ) {
		if ( m_transitionCellNodes[( size_t )( node )].m_key == rKey ) {
			return;
		}
	}
	pBucket->m_nHead = allocateTransitionCellNode( rKey, pBucket->m_nHead );
	++pBucket->m_nCount;
}

void T3DInstancer::untrackSingleLodCell( const t3dCellUpdateKey_t &rKey ) {
	const t3dTransitionBucketKey_t bucket_key = getTransitionBucketKey( rKey );
	t3dTransitionBucketState_t *const pBucket = m_singleLodTransitionBuckets.find( bucket_key );
	if ( !pBucket ) {
		return;
	}
	uint32_t previous = UINT32_MAX;
	uint32_t node = pBucket->m_nHead;
	while ( node != UINT32_MAX ) {
		const uint32_t next = m_transitionCellNodes[( size_t )( node )].m_nNext;
		if ( m_transitionCellNodes[( size_t )( node )].m_key == rKey ) {
			if ( previous == UINT32_MAX ) {
				pBucket->m_nHead = next;
			} else {
				m_transitionCellNodes[( size_t )( previous )].m_nNext = next;
			}
			releaseTransitionCellNode( node );
			if ( pBucket->m_nCount > 0u ) {
				--pBucket->m_nCount;
			}
			if ( pBucket->m_nCount == 0u ) {
				m_singleLodTransitionBuckets.erase( bucket_key );
			}
			return;
		}
		previous = node;
		node = next;
	}
}

void T3DInstancer::clearSingleLodTransitionBuckets( void ) {
	m_singleLodTransitionBuckets.clear();
	m_transitionCellNodes.clear();
	m_nTransitionCellFreeHead = UINT32_MAX;
}

EA_FORCE_INLINE static real_t
t3dPointRectMinDistanceSquared( const Vector2 &rPoint, const Vector2 &rMin, const Vector2 &rMax ) {
	const real_t fDx = rPoint.x < rMin.x ? rMin.x - rPoint.x : ( rPoint.x > rMax.x ? rPoint.x - rMax.x : 0.0f );
	const real_t fDy = rPoint.y < rMin.y ? rMin.y - rPoint.y : ( rPoint.y > rMax.y ? rPoint.y - rMax.y : 0.0f );
	return fDx * fDx + fDy * fDy;
}

EA_FORCE_INLINE static real_t
t3dPointRectMaxDistanceSquared( const Vector2 &rPoint, const Vector2 &rMin, const Vector2 &rMax ) {
	const real_t fDx = MAX( Math::abs( rPoint.x - rMin.x ), Math::abs( rPoint.x - rMax.x ) );
	const real_t fDy = MAX( Math::abs( rPoint.y - rMin.y ), Math::abs( rPoint.y - rMax.y ) );
	return fDx * fDx + fDy * fDy;
}

bool T3DInstancer::transitionBucketMayCrossBoundary( const t3dTransitionBucketKey_t &rBucket,
		const Ref<T3DMeshAsset> &rAsset,
		const Vector3 &rPreviousPosition,
		const Vector3 &rTargetPosition ) const {
	if ( rAsset.is_null() || rAsset->getLastLod() <= 0 || !m_pTerrain ) {
		return false;
	}
	const real_t fCellWorldSize = ( real_t )( m_nCellSize )*m_pTerrain->getVertexSpacing();
	const int first_x = rBucket.m_bucket.x * s_nInstanceTransitionBucketCellSpan;
	const int first_z = rBucket.m_bucket.y * s_nInstanceTransitionBucketCellSpan;
	const int last_x = first_x + s_nInstanceTransitionBucketCellSpan - 1;
	const int last_z = first_z + s_nInstanceTransitionBucketCellSpan - 1;
	const Vector2 bucketMin(
			( ( real_t )( first_x ) + 0.5f ) * fCellWorldSize, ( ( real_t )( first_z ) + 0.5f ) * fCellWorldSize );
	const Vector2 bucketMax(
			( ( real_t )( last_x ) + 0.5f ) * fCellWorldSize, ( ( real_t )( last_z ) + 0.5f ) * fCellWorldSize );
	const Vector2 previous( rPreviousPosition.x, rPreviousPosition.z );
	const Vector2 target( rTargetPosition.x, rTargetPosition.z );
	const real_t fPreviousMin = t3dPointRectMinDistanceSquared( previous, bucketMin, bucketMax );
	const real_t fPreviousMax = t3dPointRectMaxDistanceSquared( previous, bucketMin, bucketMax );
	const real_t fTargetMin = t3dPointRectMinDistanceSquared( target, bucketMin, bucketMax );
	const real_t fTargetMax = t3dPointRectMaxDistanceSquared( target, bucketMin, bucketMax );

	int valid_lods[s_nMeshMaxLodCount] = {};
	int valid_count = 0;
	for ( int lod = 0; lod <= rAsset->getLastLod(); ++lod ) {
		if ( rAsset->getCastShadows() == s_shadowCastingOnly &&
				( lod > rAsset->getLastShadowLod() || lod < rAsset->getShadowImpostor() ) ) {
			continue;
		}
		valid_lods[valid_count++] = lod;
	}
	for ( int index = 0; index + 1 < valid_count; ++index ) {
		const real_t fRangeEnd = rAsset->getLodRangeEnd( valid_lods[index] );
		if ( fRangeEnd < 0.001f ) {
			break;
		}
		const real_t fRadiusSquared = fRangeEnd * fRangeEnd;
		if ( ( fPreviousMin <= fRadiusSquared && fTargetMax > fRadiusSquared ) ||
				( fTargetMin <= fRadiusSquared && fPreviousMax > fRadiusSquared ) ) {
			return true;
		}
	}
	return false;
}

const t3dRenderCell_t *T3DInstancer::findSingleLodRenderCell( const t3dCellUpdateKey_t &rKey ) const {
	for ( int lod = 0; lod < s_nMeshMaxLodCount; ++lod ) {
		const t3dRenderCellKey_t render_key{ rKey.m_region, rKey.m_cell, rKey.m_nMeshId, lod };
		const size_t *const pIndex = m_iRenderCellIndex.find( render_key );
		if ( pIndex && *pIndex < m_renderCells.size() ) {
			return &m_renderCells[*pIndex];
		}
	}
	return nullptr;
}

void T3DInstancer::queueSingleLodTransitions( const Vector3 &rPreviousPosition, const Vector3 &rTargetPosition ) {
	m_nTransitionCellsExaminedLastScan = 0u;
	m_nTransitionLodEvaluationsLastScan = 0u;
	m_nTransitionsQueuedLastScan = 0u;
	m_nTransitionBucketsExaminedLastScan = 0u;
	m_nTransitionCandidateBucketsLastScan = 0u;
	const uint64_t scan_started_usec = Time::get_singleton()->get_ticks_usec();
	if ( !m_bSingleLodInstanceBuffers || !m_pTerrain || !m_pTerrain->getAssets().is_valid() ) {
		m_nTransitionScanUsecLastScan = Time::get_singleton()->get_ticks_usec() - scan_started_usec;
		return;
	}
	m_singleLodTransitionBuckets.forEach( [&]( const t3dTransitionBucketKey_t &bucket_key,
												  const t3dTransitionBucketState_t &rBucketState ) {
		++m_nTransitionBucketsExaminedLastScan;
		if ( bucket_key.m_nMeshId < 0 || bucket_key.m_nMeshId >= s_nMaxMeshAssets ) {
			return;
		}
		const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( bucket_key.m_nMeshId );
		if ( !transitionBucketMayCrossBoundary( bucket_key, pAsset, rPreviousPosition, rTargetPosition ) ) {
			return;
		}
		++m_nTransitionCandidateBucketsLastScan;
		for ( uint32_t node = rBucketState.m_nHead; node != UINT32_MAX;
				node = m_transitionCellNodes[( size_t )( node )].m_nNext ) {
			const t3dCellUpdateKey_t &cell_key = m_transitionCellNodes[( size_t )( node )].m_key;
			++m_nTransitionCellsExaminedLastScan;
			const t3dRenderCell_t *pRender = findSingleLodRenderCell( cell_key );
			if ( !pRender ) {
				continue;
			}
			++m_nTransitionLodEvaluationsLastScan;
			const int selected_lod = selectCellLod( cell_key.m_region, cell_key.m_cell, pAsset, rTargetPosition );
			if ( selected_lod != pRender->m_nLod && m_streamingUpdates.insert( cell_key ) ) {
				markPendingRegionWork( cell_key.m_region );
				++m_nTransitionsQueuedLastScan;
			}
		}
	} );
	m_nTransitionScanUsecLastScan = Time::get_singleton()->get_ticks_usec() - scan_started_usec;
}

void T3DInstancer::cancelPendingCellUpdates( void ) {
	if ( m_bMaterializingUpdates || m_bProcessingPendingUpdates ) {
		m_bPendingCancelRequested = true;
		return;
	}
	m_pendingCellUpdates.clear();
	m_buildingCellUpdates.clear();
	m_iPendingCellUpdateCursor = 0;
	m_bPendingCancelRequested = false;
	rebuildPendingRegionWorkMask();
}

void T3DInstancer::enqueueRegionMeshUpdates( T3DRegion *const pRegion, const int nMeshId ) {
	const Vector2i regionLocation = pRegion->getLocation();
	T3DInstanceStore &store = pRegion->getInstanceStoreInternal();
	const int mesh_count = MIN( m_pTerrain->getAssets()->getMeshCount(), s_nMaxMeshAssets );
	reconcileRegionRenderCells( pRegion, nMeshId );
	t3dInstanceMesh_t *const pNativeMesh = store.findMesh( nMeshId );
	if ( !pNativeMesh ) {
		eraseLogicalCellsByLocation( regionLocation, nMeshId );
		destroyMmiByLocation( regionLocation, nMeshId );
		return;
	}
	for ( const t3dInstanceCell_t &cell : pNativeMesh->m_cells ) {
		setLogicalCellCount( regionLocation, nMeshId, cell.m_location, cell.m_transforms.size() );
	}
	const Ref<T3DMeshAsset> pAsset = nMeshId >= 0 && nMeshId < mesh_count
			? m_pTerrain->getAssets()->getMeshAsset( nMeshId )
			: Ref<T3DMeshAsset>();
	if ( pAsset.is_null() || !pAsset->isEnabled() || pAsset->getLodCount() == 0 ) {
		destroyMmiByLocation( regionLocation, nMeshId );
		return;
	}
	m_buildingCellUpdates.reserve( m_buildingCellUpdates.size() + pNativeMesh->m_cells.size() );
	for ( const t3dInstanceCell_t &cell : pNativeMesh->m_cells ) {
		m_buildingCellUpdates.push_back( t3dCellUpdateKey_t{ regionLocation, cell.m_location, nMeshId } );
	}
	if ( !pNativeMesh->m_cells.empty() ) {
		markPendingRegionWork( regionLocation );
	}
}

void T3DInstancer::enqueueRegionCellUpdates( T3DRegion *const pRegion, const int nMeshId ) {
	if ( !pRegion || pRegion->isDeleted() ) {
		return;
	}
	if ( m_pTerrain->getData()->isInstanceRegionCacheEnabled() &&
			!m_pTerrain->getData()->isInstanceRegionResident( pRegion->getLocation() ) ) {
		return;
	}
	if ( nMeshId >= 0 ) {
		enqueueRegionMeshUpdates( pRegion, nMeshId );
		return;
	}
	for ( const t3dInstanceMesh_t &mesh : pRegion->getInstanceStore().meshes() ) {
		enqueueRegionMeshUpdates( pRegion, mesh.m_nId );
	}
}

void T3DInstancer::enqueueAllRegions( T3DData *const pData, const int nMeshId ) {
	const Vector2i *pSlots = pData->getRegionSlotsInternal();
	const int slot_count = pData->getRegionSlotCountInternal();
	for ( int slot = 0; slot < slot_count; ++slot ) {
		const Vector2i location = pSlots[slot];
		if ( location == s_v2iMax ) {
			continue;
		}
		T3DRegion *const pRegion = pData->getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() &&
				( !pData->isInstanceRegionCacheEnabled() || pData->isInstanceRegionResident( location ) ) ) {
			enqueueRegionCellUpdates( pRegion, nMeshId );
		}
	}
}

void T3DInstancer::processPendingCellUpdate( const t3dCellUpdateKey_t &rUpdate ) {
	T3DData *const pData = m_pTerrain->getData();
	T3DRegion *const region = pData->getRegionPtr( rUpdate.m_region );
	if ( !region || region->isDeleted() || pData->getRegionId( rUpdate.m_region ) < 0 ) {
		++m_nSkippedCellUpdatesLastFrame;
		eraseLogicalCellCount( rUpdate.m_region, rUpdate.m_nMeshId, rUpdate.m_cell );
		destroyMmiByCell( rUpdate.m_region, rUpdate.m_nMeshId, rUpdate.m_cell );
		return;
	}
	if ( pData->isInstanceRegionCacheEnabled() && !pData->isInstanceRegionResident( rUpdate.m_region ) ) {
		++m_nSkippedCellUpdatesLastFrame;
		destroyMmiByCell( rUpdate.m_region, rUpdate.m_nMeshId, rUpdate.m_cell );
		return;
	}
	t3dInstanceCell_t *const pCell = region->getInstanceStoreInternal().findCell( rUpdate.m_nMeshId, rUpdate.m_cell );
	const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( rUpdate.m_nMeshId );
	if ( !pCell ) {
		++m_nSkippedCellUpdatesLastFrame;
		eraseLogicalCellCount( rUpdate.m_region, rUpdate.m_nMeshId, rUpdate.m_cell );
		destroyMmiByCell( rUpdate.m_region, rUpdate.m_nMeshId, rUpdate.m_cell );
		return;
	}
	setLogicalCellCount( rUpdate.m_region, rUpdate.m_nMeshId, rUpdate.m_cell, pCell->m_transforms.size() );
	if ( pAsset.is_null() || !pAsset->isEnabled() || pAsset->getLodCount() == 0 ) {
		++m_nSkippedCellUpdatesLastFrame;
		destroyMmiByCell( rUpdate.m_region, rUpdate.m_nMeshId, rUpdate.m_cell );
		return;
	}
	updateMmiCell( region, rUpdate.m_nMeshId, *pCell, pAsset );
}

#ifdef DEBUG_ENABLED
bool T3DInstancer::reportInvariantFailure( const String &rMessage ) {
	UtilityFunctions::push_error( "T3DInstancer: invariant failure: ", rMessage );
	return false;
}

bool T3DInstancer::validateInternalState( void ) const {
	if ( m_iPendingCellUpdateCursor > m_pendingCellUpdates.size() ) {
		return reportInvariantFailure( "pending cell cursor exceeds queue size" );
	}
	if ( m_iRenderCellIndex.size() != m_renderCells.size() ) {
		return reportInvariantFailure( "render index cardinality differs from dense render storage" );
	}
	for ( size_t index = 0; index < m_renderCells.size(); ++index ) {
		const t3dRenderCell_t &render = m_renderCells[index];
		const size_t *const pDenseIndex = m_iRenderCellIndex.find( makeRenderKey( render ) );
		if ( !pDenseIndex || *pDenseIndex != index ) {
			return reportInvariantFailure(
					vformat( "render key does not resolve to dense index %d", ( int64_t )( index ) ) );
		}
		if ( render.m_nRegionListNode == UINT32_MAX || render.m_nRegionListNode >= m_renderRegionNodes.size() ||
				m_renderRegionNodes[( size_t )( render.m_nRegionListNode )].m_iRenderCell != index ) {
			return reportInvariantFailure(
					vformat( "render region node is stale at dense index %d", ( int64_t )( index ) ) );
		}
	}
	for ( size_t region_index = 0; region_index < s_nInstanceRegionTableCapacity; ++region_index ) {
		if ( !m_regionRenderMask.test( region_index ) ) {
			continue;
		}
		const Vector2i regionLocation = regionLocationFromUpdateIndex( ( uint16_t )( region_index ) );
		const t3dRegionList_t &list = m_szRegionRenderLists[region_index];
		if ( list.m_nCount == 0u || list.m_nHead == UINT32_MAX ) {
			return reportInvariantFailure( "active region render slot is empty" );
		}
		uint32_t seen = 0u;
		for ( uint32_t node = list.m_nHead; node != UINT32_MAX;
				node = m_renderRegionNodes[( size_t )( node )].m_nNext ) {
			if ( node >= m_renderRegionNodes.size() ) {
				return reportInvariantFailure( "region render node points outside pool" );
			}
			const size_t render_index = m_renderRegionNodes[( size_t )( node )].m_iRenderCell;
			if ( render_index >= m_renderCells.size() ) {
				return reportInvariantFailure( "region render node points outside dense storage" );
			}
			const t3dRenderCell_t &render = m_renderCells[render_index];
			if ( render.m_region != regionLocation || render.m_nRegionListNode != node ) {
				return reportInvariantFailure( "region render reverse index is inconsistent" );
			}
			++seen;
		}
		if ( seen != list.m_nCount ) {
			return reportInvariantFailure( "region render list count is inconsistent" );
		}
	}

	if ( m_pTerrain && m_pTerrain->getData() ) {
		T3DRegion *const *ppRegionPtrs = m_pTerrain->getData()->getRegionPtrMapInternal();
		for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
			const T3DRegion *region = ppRegionPtrs[region_index];
			if ( !region || region->isDeleted() ) {
				continue;
			}
			if ( m_pTerrain->getData()->isInstanceRegionCacheEnabled() &&
					!m_pTerrain->getData()->isInstanceRegionResident( region->getLocation() ) ) {
				continue;
			}
			for ( const t3dInstanceMesh_t &mesh : region->getInstanceStore().meshes() ) {
				for ( const t3dInstanceCell_t &cell : mesh.m_cells ) {
					if ( cell.usesColors() && cell.m_colors.size() != cell.m_transforms.size() ) {
						return reportInvariantFailure( "instance color sidecar length differs from transform count" );
					}
					if ( cell.usesOwners() && cell.m_owners.size() != cell.m_transforms.size() ) {
						return reportInvariantFailure( "instance owner sidecar length differs from transform count" );
					}
				}
			}
		}
	}

	eastl::array<uint64_t, s_nMaxMeshAssets> calculated_totals{};
	bool logical_valid = true;
	String logical_error;
	m_logicalCellCounts.forEach( [&]( const t3dCellUpdateKey_t &key, const t3dLogicalCellState_t &state ) {
		if ( !logical_valid ) {
			return;
		}
		if ( key.m_nMeshId < 0 || key.m_nMeshId >= s_nMaxMeshAssets ) {
			logical_valid = false;
			logical_error = "logical cell contains an invalid mesh ID";
			return;
		}
		if ( state.m_nRegionListNode == UINT32_MAX || state.m_nRegionListNode >= m_logicalRegionNodes.size() ||
				!( m_logicalRegionNodes[( size_t )( state.m_nRegionListNode )].m_key == key ) ) {
			logical_valid = false;
			logical_error = "logical cell region node is stale";
			return;
		}
		uint64_t &calculated_total = calculated_totals[( size_t )( key.m_nMeshId )];
		calculated_total = UINT64_MAX - calculated_total < ( uint64_t )( state.m_nCount )
				? UINT64_MAX
				: calculated_total + ( uint64_t )( state.m_nCount );
	} );
	if ( !logical_valid ) {
		return reportInvariantFailure( logical_error );
	}
	for ( size_t region_index = 0; region_index < s_nInstanceRegionTableCapacity; ++region_index ) {
		if ( !m_regionLogicalMask.test( region_index ) ) {
			continue;
		}
		const Vector2i regionLocation = regionLocationFromUpdateIndex( ( uint16_t )( region_index ) );
		const t3dRegionList_t &list = m_szRegionLogicalLists[region_index];
		if ( list.m_nCount == 0u || list.m_nHead == UINT32_MAX ) {
			return reportInvariantFailure( "active logical region slot is empty" );
		}
		uint32_t seen = 0u;
		for ( uint32_t node = list.m_nHead; node != UINT32_MAX;
				node = m_logicalRegionNodes[( size_t )( node )].m_nNext ) {
			if ( node >= m_logicalRegionNodes.size() ) {
				return reportInvariantFailure( "logical region node points outside pool" );
			}
			const t3dCellUpdateKey_t &key = m_logicalRegionNodes[( size_t )( node )].m_key;
			const t3dLogicalCellState_t *const pState = m_logicalCellCounts.find( key );
			if ( key.m_region != regionLocation || !pState || pState->m_nRegionListNode != node ) {
				return reportInvariantFailure( "logical region reverse index is inconsistent" );
			}
			++seen;
		}
		if ( seen != list.m_nCount ) {
			return reportInvariantFailure( "logical region list count is inconsistent" );
		}
	}
	for ( int mesh_id = 0; mesh_id < s_nMaxMeshAssets; ++mesh_id ) {
		if ( calculated_totals[( size_t )( mesh_id )] != m_szLogicalMeshInstanceTotals[( size_t )( mesh_id )] ) {
			return reportInvariantFailure( vformat( "logical aggregate differs for mesh %d", mesh_id ) );
		}
		if ( m_pTerrain && m_pTerrain->getAssets().is_valid() ) {
			const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( mesh_id );
			if ( pAsset.is_valid() ) {
				const uint64_t published_total =
						m_pTerrain->getData() && m_pTerrain->getData()->isInstanceRegionCacheEnabled()
						? t3dRepositoryMeshInstanceCount( m_pTerrain->getData(), mesh_id )
						: calculated_totals[( size_t )( mesh_id )];
				const uint32_t expected =
						( uint32_t )( MIN<uint64_t>( published_total, std::numeric_limits<uint32_t>::max() ) );
				if ( pAsset->getInstanceCount() != expected ) {
					return reportInvariantFailure( vformat( "published instance count differs for mesh %d", mesh_id ) );
				}
			}
		}
	}
	return true;
}
#endif

void T3DInstancer::materializePendingUpdates( void ) {
	if ( m_iPendingCellUpdateCursor < m_pendingCellUpdates.size() ) {
		return;
	}
	cancelPendingCellUpdates();
	if ( !m_bRebuildAll && !m_bUpdateAll && !m_globalMeshUpdates.any() && m_nRegionUpdateCount == 0 &&
			m_streamingUpdates.empty() ) {
		return;
	}

	// Snapshot before invoking any downstream operation. Render/count updates can
	// emit synchronous signals whose callbacks schedule more work. Those requests
	// must remain in the live dirty containers for the next batch rather than
	// invalidating this iteration or being cleared at its end.
	const bool bRebuildAll = m_bRebuildAll;
	const bool bUpdateAll = bRebuildAll || m_bUpdateAll;
	const eastl::bitset<s_nMaxMeshAssets> global_mesh_updates = m_globalMeshUpdates;
	snapshotRegionUpdates();
	m_activeStreamingUpdates.clear();
	m_activeStreamingUpdates.swap( m_streamingUpdates );
	m_bRebuildAll = false;
	m_bUpdateAll = false;
	m_globalMeshUpdates.reset();
	m_buildingCellUpdates.clear();
	m_bMaterializingUpdates = true;

	T3DData *const pData = m_pTerrain->getData();
	refreshStreamingCache();
	if ( bRebuildAll ) {
		destroyRenderCells();
		clearLogicalCellCounts();
	} else if ( bUpdateAll ) {
		// Bulk region removal can defer renderer work. Teardown is immediate so
		// removed regions cannot remain visible while creation/upload is budgeted.
		for ( size_t region_index = 0; region_index < s_nInstanceRegionTableCapacity; ++region_index ) {
			if ( !m_regionRenderMask.test( region_index ) ) {
				continue;
			}
			const Vector2i regionLocation = regionLocationFromUpdateIndex( ( uint16_t )( region_index ) );
			T3DRegion *const region = pData->getRegionPtr( regionLocation );
			if ( !region || region->isDeleted() || pData->getRegionId( regionLocation ) < 0 ) {
				destroyMmisByRegion( regionLocation );
			}
		}
		// Erase through the fixed per-region lists rather than mutating the flat hash
		// while iterating its probe buckets. erase() repairs probe clusters in place.
		for ( size_t region_index = 0u; region_index < s_nInstanceRegionTableCapacity; ++region_index ) {
			if ( !m_regionLogicalMask.test( region_index ) ) {
				continue;
			}
			const Vector2i region_location = regionLocationFromUpdateIndex( ( uint16_t )region_index );
			T3DRegion *const pRegion = pData->getRegionPtr( region_location );
			if ( !pRegion || pRegion->isDeleted() || pData->getRegionId( region_location ) < 0 ) {
				eraseLogicalCellsByRegion( region_location );
			}
		}
	}

	if ( bUpdateAll ) {
		enqueueAllRegions( pData, -1 );
	} else {
		for ( int mesh_id = 0; mesh_id < s_nMaxMeshAssets; ++mesh_id ) {
			if ( global_mesh_updates.test( mesh_id ) ) {
				enqueueAllRegions( pData, mesh_id );
			}
		}

		for ( int list_index = 0; list_index < m_nActiveRegionUpdateCount; ++list_index ) {
			const uint16_t region_index = m_szActiveRegionUpdateIndices[list_index];
			const Vector2i regionLocation = regionLocationFromUpdateIndex( region_index );
			const t3dRegionUpdateState_t &state = m_szActiveRegionUpdates[( size_t )( region_index )];
			T3DRegion *const region = pData->getRegionPtr( regionLocation );
			if ( !region || region->isDeleted() ) {
				if ( state.m_bAllMeshes ) {
					eraseLogicalCellsByRegion( regionLocation );
					destroyMmisByRegion( regionLocation );
				} else {
					for ( int mesh_id = 0; mesh_id < s_nMaxMeshAssets; ++mesh_id ) {
						if ( state.m_meshes.test( mesh_id ) ) {
							eraseLogicalCellsByLocation( regionLocation, mesh_id );
							destroyMmiByLocation( regionLocation, mesh_id );
						}
					}
				}
				continue;
			}
			if ( pData->isInstanceRegionCacheEnabled() && !pData->isInstanceRegionResident( regionLocation ) ) {
				destroyMmisByRegion( regionLocation );
				continue;
			}
			if ( state.m_bAllMeshes ) {
				// Global mesh requests already cover those mesh IDs in every region.
				// Reconcile the complete region once, then enqueue only the remaining
				// mesh groups so one logical cell cannot be uploaded twice in a batch.
				reconcileRegionRenderCells( region, -1 );
				for ( const t3dInstanceMesh_t &mesh : region->getInstanceStore().meshes() ) {
					if ( mesh.m_nId >= 0 && mesh.m_nId < s_nMaxMeshAssets && !global_mesh_updates.test( mesh.m_nId ) ) {
						enqueueRegionCellUpdates( region, mesh.m_nId );
					}
				}
			} else {
				for ( int mesh_id = 0; mesh_id < s_nMaxMeshAssets; ++mesh_id ) {
					if ( state.m_meshes.test( mesh_id ) && !global_mesh_updates.test( mesh_id ) ) {
						enqueueRegionCellUpdates( region, mesh_id );
					}
				}
			}
		}
	}

	m_activeStreamingUpdates.forEach( [&]( const t3dCellUpdateKey_t &update ) {
		if ( bUpdateAll || global_mesh_updates.test( update.m_nMeshId ) ) {
			return;
		}
		const t3dRegionUpdateState_t *pRegionUpdate = findActiveRegionUpdate( update.m_region );
		if ( pRegionUpdate && ( pRegionUpdate->m_bAllMeshes || pRegionUpdate->m_meshes.test( update.m_nMeshId ) ) ) {
			return;
		}
		m_buildingCellUpdates.push_back( update );
		markPendingRegionWork( update.m_region );
	} );

	m_bMaterializingUpdates = false;
	clearRegionUpdates( true );
	m_activeStreamingUpdates.clear();
	if ( m_bPendingCancelRequested ) {
		m_buildingCellUpdates.clear();
		m_pendingCellUpdates.clear();
		m_iPendingCellUpdateCursor = 0;
		m_bPendingCancelRequested = false;
		rebuildPendingRegionWorkMask();
		return;
	}
	m_pendingCellUpdates.swap( m_buildingCellUpdates );
	m_iPendingCellUpdateCursor = 0;
	rebuildPendingRegionWorkMask();
}

// Creates MMIs based on native data stored in Terrain3DRegions. Authoritative
// teardown/reconciliation is immediate; expensive cell creation and upload is
// amortized over pre-draw frames according to update_cell_budget.
void T3DInstancer::processUpdates( void ) {
	m_nCellUpdateUsecLastFrame = 0u;
	m_nMaterializeUsecLastFrame = 0u;
	m_nMultimeshUploadBytesLastFrame = 0u;
	m_nInstanceRecordsUploadedLastFrame = 0u;
	m_nQueueBeforeLastFrame = ( uint32_t )( MAX( getPendingStreamingUpdateCount(), 0 ) );
	m_nQueueAfterLastFrame = m_nQueueBeforeLastFrame;
	m_nProcessedCellUpdatesLastFrame = 0u;
	m_nSkippedCellUpdatesLastFrame = 0u;
	m_nMultimeshUploadsLastFrame = 0u;
	m_nNewRenderInstancesLastFrame = 0u;
	const uint64_t update_started_usec = Time::get_singleton()->get_ticks_usec();
	const Callable callback = callable_mp( this, &T3DInstancer::processUpdates );
	if ( !hasPendingUpdates() ) {
		if ( RS->is_connected( "frame_pre_draw", callback ) ) {
			RS->disconnect( "frame_pre_draw", callback );
		}
		return;
	}
	IS_DATA_INIT( VOID );
	if ( !m_pTerrain->is_inside_tree() ) {
		return;
	}

	const uint64_t materialize_started_usec = Time::get_singleton()->get_ticks_usec();
	materializePendingUpdates();
	m_nMaterializeUsecLastFrame = Time::get_singleton()->get_ticks_usec() - materialize_started_usec;
	const size_t remaining = m_pendingCellUpdates.size() - m_iPendingCellUpdateCursor;
	const int effective_update_budget = getEffectiveUpdateCellBudget();
	const size_t budget =
			effective_update_budget == 0 ? remaining : MIN( remaining, ( size_t )( effective_update_budget ) );
	const size_t end = m_iPendingCellUpdateCursor + budget;
	m_bProcessingPendingUpdates = true;
	while ( m_iPendingCellUpdateCursor < end && !m_bPendingCancelRequested ) {
		processPendingCellUpdate( m_pendingCellUpdates[m_iPendingCellUpdateCursor] );
		++m_iPendingCellUpdateCursor;
		++m_nProcessedCellUpdatesLastFrame;
	}
	m_bProcessingPendingUpdates = false;
	rebuildPendingRegionWorkMask();
	flushLogicalMeshCounts();

	if ( m_bPendingCancelRequested ) {
		m_pendingCellUpdates.clear();
		m_buildingCellUpdates.clear();
		m_iPendingCellUpdateCursor = 0;
		m_bPendingCancelRequested = false;
	} else if ( m_iPendingCellUpdateCursor >= m_pendingCellUpdates.size() ) {
		cancelPendingCellUpdates();
		m_pTerrain->getAssets()->loadPendingMeshes();
	}

	if ( !hasPendingUpdates() && RS->is_connected( "frame_pre_draw", callback ) ) {
#ifdef DEBUG_ENABLED
		// A complete cross-index audit is intentionally opt-in. Running it after
		// every editor update on a large world would turn debug instrumentation
		// into a new frame-time regression.
		if ( T3DTerrain::s_debugLevel >= T3DTerrain::EXTREME ) {
			validateInternalState();
		}
#endif
		RS->disconnect( "frame_pre_draw", callback );
	}
	m_nQueueAfterLastFrame = ( uint32_t )( MAX( getPendingStreamingUpdateCount(), 0 ) );
	m_nCellUpdateUsecLastFrame = Time::get_singleton()->get_ticks_usec() - update_started_usec;
}

void T3DInstancer::updateMmiByRegion( T3DRegion *const pRegion, const int nMeshId ) {
	if ( !pRegion || nMeshId < 0 || nMeshId >= s_nMaxMeshAssets ) {
		return;
	}
	const Vector2i regionLocation = pRegion->getLocation();
	t3dInstanceMesh_t *const pNativeMesh = pRegion->getInstanceStoreInternal().findMesh( nMeshId );
	if ( !pNativeMesh ) {
		eraseLogicalCellsByLocation( regionLocation, nMeshId );
		destroyMmiByLocation( regionLocation, nMeshId );
		return;
	}
	const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( nMeshId );
	if ( pAsset.is_null() || !pAsset->isEnabled() || pAsset->getLodCount() == 0 ) {
		for ( const t3dInstanceCell_t &cell : pNativeMesh->m_cells ) {
			setLogicalCellCount( regionLocation, nMeshId, cell.m_location, cell.m_transforms.size() );
		}
		destroyMmiByLocation( regionLocation, nMeshId );
		return;
	}
	for ( t3dInstanceCell_t &cell : pNativeMesh->m_cells ) {
		updateMmiCell( pRegion, nMeshId, cell, pAsset );
	}
}

void T3DInstancer::updateMmiCell( T3DRegion *const pRegion,
		const int nMeshId,
		t3dInstanceCell_t &rCell,
		const Ref<T3DMeshAsset> &rAsset ) {
	const Vector2i regionLocation = pRegion->getLocation();
	const Vector2i cell_location = rCell.m_location;
	const t3dCellUpdateKey_t transition_key{ regionLocation, cell_location, nMeshId };
	if ( rCell.m_transforms.empty() ) {
		eraseLogicalCellCount( regionLocation, nMeshId, cell_location );
		untrackSingleLodCell( transition_key );
		destroyMmiByCell( regionLocation, nMeshId, cell_location );
		return;
	}
	setLogicalCellCount( regionLocation, nMeshId, cell_location, rCell.m_transforms.size() );
	if ( !cellShouldBeResident( regionLocation, cell_location, nMeshId ) ) {
		untrackSingleLodCell( transition_key );
		destroyMmiByCell( regionLocation, nMeshId, cell_location );
		return;
	}

	const Vector3 lod_target = m_bStreamPositionValid ? m_lastStreamPosition : m_pTerrain->getClipmapTargetPosition();
	const int selected_lod = m_bSingleLodInstanceBuffers
			? selectCellLod( regionLocation, cell_location, rAsset, lod_target )
			: INT32_MAX;
	if ( m_bSingleLodInstanceBuffers && selected_lod < 0 ) {
		untrackSingleLodCell( transition_key );
		destroyMmiByCell( regionLocation, nMeshId, cell_location );
		return;
	}
	if ( m_bSingleLodInstanceBuffers ) {
		trackSingleLodCell( transition_key );
	}

	for ( int lod = 0; lod < s_nMeshMaxLodCount; ++lod ) {
		const bool bInvalidLegacyLod = lod > rAsset->getLastLod() ||
				( rAsset->getCastShadows() == s_shadowCastingOnly &&
						( lod > rAsset->getLastShadowLod() || lod < rAsset->getShadowImpostor() ) );
		if ( ( m_bSingleLodInstanceBuffers && lod != selected_lod ) ||
				( !m_bSingleLodInstanceBuffers && bInvalidLegacyLod ) ) {
			destroyMmiByCell( regionLocation, nMeshId, cell_location, lod );
		}
	}
	const bool bShadowDisabled = m_bSingleLodInstanceBuffers || rAsset->getShadowImpostor() == 0 ||
			rAsset->getCastShadows() == s_shadowCastingOff;
	if ( bShadowDisabled ) {
		destroyMmiByCell( regionLocation, nMeshId, cell_location, s_nMeshShadowLodId );
	}

	const Ref<Mesh> pMasterMesh = rAsset->getMesh( getMasterLod( rAsset ) );
	const AABB source_mesh_aabb = pMasterMesh.is_valid() ? pMasterMesh->get_aabb() : AABB();
	const bool bSourceMeshChanged =
			!rCell.m_bCachedAabbValid || !rCell.m_cachedSourceMeshAabb.is_equal_approx( source_mesh_aabb );
	const bool bRebuildAabb =
			!rCell.m_bCachedAabbValid || rCell.m_nCachedAabbRevision != rCell.m_nRevision || bSourceMeshChanged;
	bool bUploadBufferReady = false;
	PackedFloat32Array cooked_fast_start_buffer;
	uint32_t cooked_fast_start_count = 0u;
	const bool bCookedFastStart = !rCell.m_bModified &&
			T3DRuntimeBakeCodec::findMultimeshFastStart( pRegion->getCookedMultimeshFastStartInternal(),
					( uint16_t )nMeshId,
					cell_location,
					cooked_fast_start_buffer,
					cooked_fast_start_count ) &&
			cooked_fast_start_count == rCell.m_transforms.size();
	if ( bCookedFastStart && bRebuildAabb ) {
		bool initialized = false;
		AABB cooked_aabb;
		const float *const values = cooked_fast_start_buffer.ptr();
		for ( uint32_t instance = 0u; instance < cooked_fast_start_count; ++instance ) {
			const float *const record = values + ( size_t )instance * 16u;
			Basis basis;
			basis[0] = Vector3( record[0], record[1], record[2] );
			basis[1] = Vector3( record[4], record[5], record[6] );
			basis[2] = Vector3( record[8], record[9], record[10] );
			const AABB transformed =
					Transform3D( basis, Vector3( record[3], record[7], record[11] ) ).xform( source_mesh_aabb );
			if ( !initialized ) {
				cooked_aabb = transformed;
				initialized = true;
			} else {
				cooked_aabb.merge_with( transformed );
			}
		}
		rCell.m_cachedAabb = initialized ? cooked_aabb : AABB();
		rCell.m_cachedSourceMeshAabb = source_mesh_aabb;
		rCell.m_nCachedAabbRevision = rCell.m_nRevision;
		if ( ++rCell.m_nCachedAabbGeneration == 0 ) {
			rCell.m_nCachedAabbGeneration = 1;
		}
		rCell.m_bCachedAabbValid = true;
	}

	RID shadow_source;
	for ( int lod = rAsset->getLastLod(); lod >= s_nMeshShadowLodId; --lod ) {
		if ( m_bSingleLodInstanceBuffers && lod != selected_lod ) {
			continue;
		}
		if ( lod == s_nMeshShadowLodId && bShadowDisabled ) {
			continue;
		}
		if ( rAsset->getCastShadows() == s_shadowCastingOnly && lod >= 0 &&
				( lod > rAsset->getLastShadowLod() || lod < rAsset->getShadowImpostor() ) ) {
			continue;
		}

		t3dRenderCell_t *const pRenderPtr = getOrCreateRenderCell( regionLocation, nMeshId, lod, cell_location );
		if ( !pRenderPtr ) {
			continue;
		}
		t3dRenderCell_t &pRender = *pRenderPtr;
		const bool bNewMmi = !pRender.m_hMmi.is_valid();
		if ( bNewMmi ) {
			++m_nNewRenderInstancesLastFrame;
			pRender.m_hMmi = RS->instance_create();
			RS->instance_set_scenario( pRender.m_hMmi, m_pTerrain->get_world_3d()->get_scenario() );
			Transform3D region_transform;
			region_transform.origin.x = regionLocation.x * pRegion->getRegionSize() * m_pTerrain->getVertexSpacing();
			region_transform.origin.z = regionLocation.y * pRegion->getRegionSize() * m_pTerrain->getVertexSpacing();
			RS->instance_set_transform( pRender.m_hMmi, region_transform );
		}
		const bool bSettingsChanged = bNewMmi || pRender.m_nSettingsRevision != m_szMeshSettingsRevision[nMeshId];
		if ( bSettingsChanged ) {
			if ( rAsset->isHighlighted() ) {
				RS->instance_geometry_set_material_override( pRender.m_hMmi,
						rAsset->getHighlightMaterial().is_valid() ? rAsset->getHighlightMaterial()->get_rid() : RID() );
				RS->instance_geometry_set_material_overlay( pRender.m_hMmi, RID() );
			} else {
				RS->instance_geometry_set_material_override( pRender.m_hMmi,
						rAsset->getMaterialOverride().is_valid() ? rAsset->getMaterialOverride()->get_rid() : RID() );
				RS->instance_geometry_set_material_overlay( pRender.m_hMmi,
						rAsset->getMaterialOverlay().is_valid() ? rAsset->getMaterialOverlay()->get_rid() : RID() );
			}
			RS->instance_geometry_set_cast_shadows_setting( pRender.m_hMmi, rAsset->getLodCastShadows( lod ) );
			RS->instance_set_layer_mask( pRender.m_hMmi, rAsset->getVisibilityLayers() );
			setMmiLodRanges( pRender.m_hMmi, rAsset, lod );
			pRender.m_nSettingsRevision = m_szMeshSettingsRevision[nMeshId];
		}

		if ( lod == s_nMeshShadowLodId ) {
			pRender.m_hMultimesh = shadow_source;
		} else {
			const bool bUploadUsesColors = bCookedFastStart ? true : rCell.usesColors();
			if ( !pRender.m_hMultimesh.is_valid() || rCell.m_bModified ||
					pRender.m_nUploadedRevision != rCell.m_nRevision || pRender.m_bUsesColors != bUploadUsesColors ) {
				const PackedFloat32Array *pUpload = &cooked_fast_start_buffer;
				if ( !bCookedFastStart ) {
					ensureUploadBuffer( rCell, pMasterMesh, source_mesh_aabb, bRebuildAabb, bUploadBufferReady );
					pUpload = &m_uploadBuffer;
				}
				const t3dMultiMeshUpdateResult_t result =
						updateMultimesh( nMeshId, lod, rCell, pRender, bUploadUsesColors, *pUpload );
				if ( result != t3dMultiMeshUpdateResult_t::UPDATED ) {
					eraseRenderCell( regionLocation, nMeshId, lod, cell_location );
					continue;
				}
				++m_nMultimeshUploadsLastFrame;
				m_nInstanceRecordsUploadedLastFrame += ( uint64_t )( rCell.m_transforms.size() );
				m_nMultimeshUploadBytesLastFrame += ( uint64_t )( pUpload->size() ) * sizeof( float );
				pRender.m_nUploadedRevision = rCell.m_nRevision;
				pRender.m_bUsesColors = bUploadUsesColors;
			} else if ( bSettingsChanged ) {
				const Ref<Mesh> pMesh = rAsset->getMesh( lod );
				if ( pMesh.is_null() ) {
					eraseRenderCell( regionLocation, nMeshId, lod, cell_location );
					continue;
				}
				if ( pRender.m_hBoundMesh != pMesh->get_rid() ) {
					RS->multimesh_set_mesh( pRender.m_hMultimesh, pMesh->get_rid() );
					pRender.m_hBoundMesh = pMesh->get_rid();
				}
			}
		}
		if ( lod == s_nMeshShadowLodId && !pRender.m_hMultimesh.is_valid() ) {
			eraseRenderCell( regionLocation, nMeshId, lod, cell_location );
			continue;
		}
		if ( pRender.m_hMultimesh.is_valid() && pRender.m_hBoundMultimesh != pRender.m_hMultimesh ) {
			RS->instance_set_base( pRender.m_hMmi, pRender.m_hMultimesh );
			pRender.m_hBoundMultimesh = pRender.m_hMultimesh;
		}
		if ( lod == rAsset->getShadowImpostor() ) {
			shadow_source = pRender.m_hMultimesh;
		}
	}
	rCell.m_bModified = false;
	if ( bRebuildAabb && !bUploadBufferReady && !bCookedFastStart ) {
		ensureUploadBuffer( rCell, pMasterMesh, source_mesh_aabb, bRebuildAabb, bUploadBufferReady );
	}

	for ( int lod = s_nMeshShadowLodId; lod <= rAsset->getLastLod(); ++lod ) {
		t3dRenderCell_t *pRender = findRenderCell( regionLocation, nMeshId, lod, cell_location );
		if ( !pRender || !pRender->m_hMmi.is_valid() || pRender->m_nAabbGeneration == rCell.m_nCachedAabbGeneration ) {
			continue;
		}
		if ( lod >= 0 && pRender->m_hMultimesh.is_valid() && lod != getMasterLod( rAsset ) ) {
			RS->multimesh_set_custom_aabb( pRender->m_hMultimesh, rCell.m_cachedAabb );
		}
		RS->instance_set_custom_aabb( pRender->m_hMmi, rCell.m_cachedAabb );
		pRender->m_nAabbGeneration = rCell.m_nCachedAabbGeneration;
	}
}

void T3DInstancer::ensureUploadBuffer( t3dInstanceCell_t &rCell,
		const Ref<Mesh> &rMasterMesh,
		const AABB &aabbSourceMeshAabb,
		const bool bRebuildAabb,
		bool &bReady ) {
	if ( bReady ) {
		return;
	}
	if ( bRebuildAabb && rMasterMesh.is_valid() ) {
		fillMultimeshBuffer( rCell, m_uploadBuffer, &aabbSourceMeshAabb, &rCell.m_cachedAabb );
	} else {
		fillMultimeshBuffer( rCell, m_uploadBuffer );
		if ( bRebuildAabb ) {
			rCell.m_cachedAabb = AABB();
		}
	}
	if ( bRebuildAabb ) {
		rCell.m_cachedSourceMeshAabb = aabbSourceMeshAabb;
		rCell.m_nCachedAabbRevision = rCell.m_nRevision;
		if ( ++rCell.m_nCachedAabbGeneration == 0 ) {
			rCell.m_nCachedAabbGeneration = 1;
		}
		rCell.m_bCachedAabbValid = true;
	}
	bReady = true;
}

void T3DInstancer::setMmiLodRanges( const RID mmi, const Ref<T3DMeshAsset> &rMa, const int nLod ) {
	if ( !mmi || rMa.is_null() ) {
		return;
	}
	if ( m_bSingleLodInstanceBuffers ) {
		RS->instance_geometry_set_visibility_range(
				mmi, 0.0f, 0.0f, 0.0f, 0.0f, RenderingServer::VISIBILITY_RANGE_FADE_DISABLED );
		return;
	}
	int source_lod = nLod;
	real_t fLodBegin = rMa->getLodRangeBegin( source_lod );
	real_t fLodEnd = rMa->getLodRangeEnd( source_lod );
	if ( source_lod == s_nMeshShadowLodId ) {
		source_lod = rMa->getShadowImpostor();
		fLodBegin = 0.f;
		fLodEnd = rMa->getLodRangeBegin( source_lod );
	}
	const real_t fMargin = rMa->getFadeMargin();
	if ( fMargin > 0.f ) {
		fLodBegin = MAX( fLodBegin < 0.001f ? 0.f : fLodBegin - fMargin, 0.f );
		fLodEnd = MAX( fLodEnd < 0.001f ? 0.f : fLodEnd + fMargin, 0.f );
		const real_t fBeginMargin = fLodBegin < 0.001f ? 0.f : fMargin;
		const real_t fEndMargin = fLodEnd < 0.001f ? 0.f : fMargin;
		RS->instance_geometry_set_visibility_range(
				mmi, fLodBegin, fLodEnd, fBeginMargin, fEndMargin, RenderingServer::VISIBILITY_RANGE_FADE_SELF );
	} else {
		RS->instance_geometry_set_visibility_range(
				mmi, fLodBegin, fLodEnd, 0.f, 0.f, RenderingServer::VISIBILITY_RANGE_FADE_DISABLED );
	}
}

void T3DInstancer::updateVertexSpacing( const real_t fVertexSpacing ) {
	IS_DATA_INIT( VOID );
	m_bStreamingCacheDirty = true;
	T3DData *const pData = m_pTerrain->getData();
	const Vector2i *pLocations = pData->getRegionSlotsInternal();
	const int location_count = pData->getRegionSlotCountInternal();
	for ( int location_index = 0; location_index < location_count; ++location_index ) {
		const Vector2i location = pLocations[location_index];
		if ( location == s_v2iMax ) {
			continue;
		}
		T3DRegion *const pRegion = pData->getRegionPtr( location );
		if ( !pRegion ) {
			continue;
		}
		const real_t fOldSpacing = pRegion->getVertexSpacing();
		if ( Math::is_equal_approx( fOldSpacing, fVertexSpacing ) ) {
			continue;
		}
		bool bTemporaryPage = false;
		if ( pRegion->hasInstances() && !t3dBeginInstancePageAccess( this, pData, location, bTemporaryPage ) ) {
			continue;
		}
		if ( pRegion->hasInstances() ) {
			for ( t3dInstanceMesh_t &mesh : pRegion->getInstanceStoreInternal().meshesMut() ) {
				for ( t3dInstanceCell_t &cell : mesh.m_cells ) {
					for ( size_t i = 0; i < cell.m_transforms.size(); ++i ) {
						cell.m_transforms.rescaleOriginXz( i, fOldSpacing, fVertexSpacing );
					}
					cell.touch();
				}
			}
		}
		pRegion->setVertexSpacing( fVertexSpacing );
		pRegion->setModified( true );
		t3dEndInstancePageAccess( pData, location, bTemporaryPage );
	}
	updateMmis( -1, s_v2iMax, true );
}

void T3DInstancer::destroyMmisByRegion( const Vector2i &rRegionLoc ) {
	const int region_index = getRegionTableIndex( rRegionLoc );
	if ( region_index < 0 ) {
		return;
	}
	for ( uint32_t node = m_szRegionLogicalLists[( size_t )( region_index )].m_nHead; node != UINT32_MAX;
			node = m_logicalRegionNodes[( size_t )( node )].m_nNext ) {
		untrackSingleLodCell( m_logicalRegionNodes[( size_t )( node )].m_key );
	}
	while ( m_szRegionRenderLists[( size_t )( region_index )].m_nHead != UINT32_MAX ) {
		const uint32_t node = m_szRegionRenderLists[( size_t )( region_index )].m_nHead;
		const size_t render_index = m_renderRegionNodes[( size_t )( node )].m_iRenderCell;
		if ( render_index >= m_renderCells.size() ) {
			releaseRenderRegionNode( node, ( uint16_t )( region_index ) );
			continue;
		}
		eraseRenderCellAt( render_index );
	}
}

void T3DInstancer::destroyMmiByLocation( const Vector2i &rRegionLoc, const int nMeshId ) {
	const int region_index = getRegionTableIndex( rRegionLoc );
	if ( region_index < 0 ) {
		return;
	}
	for ( uint32_t node = m_szRegionLogicalLists[( size_t )( region_index )].m_nHead; node != UINT32_MAX;
			node = m_logicalRegionNodes[( size_t )( node )].m_nNext ) {
		const t3dCellUpdateKey_t &key = m_logicalRegionNodes[( size_t )( node )].m_key;
		if ( key.m_nMeshId == nMeshId ) {
			untrackSingleLodCell( key );
		}
	}
	uint32_t node = m_szRegionRenderLists[( size_t )( region_index )].m_nHead;
	while ( node != UINT32_MAX ) {
		const uint32_t next = m_renderRegionNodes[( size_t )( node )].m_nNext;
		const size_t render_index = m_renderRegionNodes[( size_t )( node )].m_iRenderCell;
		if ( render_index < m_renderCells.size() && m_renderCells[render_index].m_nMeshId == nMeshId ) {
			eraseRenderCellAt( render_index );
		}
		node = next;
	}
}

void T3DInstancer::eraseMmiLod( const Vector2i &rRegionLoc, const int nMeshId, const Vector2i &vCell, const int nLod ) {
	if ( findRenderCell( rRegionLoc, nMeshId, nLod, vCell ) ) {
		eraseRenderCell( rRegionLoc, nMeshId, nLod, vCell );
	}
}

void T3DInstancer::destroyMmiByCell( const Vector2i &rRegionLoc,
		const int nMeshId,
		const Vector2i vCell,
		const int nLod ) {
	if ( nLod != INT32_MAX ) {
		eraseMmiLod( rRegionLoc, nMeshId, vCell, nLod );
		return;
	}
	untrackSingleLodCell( t3dCellUpdateKey_t{ rRegionLoc, vCell, nMeshId } );
	for ( int lod = s_nMeshShadowLodId; lod < s_nMeshMaxLodCount; ++lod ) {
		eraseMmiLod( rRegionLoc, nMeshId, vCell, lod );
	}
}

void T3DInstancer::backupRegion( const Ref<T3DRegion> &rRegion ) {
	if ( rRegion.is_null() ) {
		return;
	}
	if ( m_pTerrain && m_pTerrain->getEditor() && m_pTerrain->getEditor()->isOperating() ) {
		m_pTerrain->getEditor()->backupRegion( rRegion );
	} else {
		rRegion->setModified( true );
	}
}

void T3DInstancer::fillMultimeshBuffer( const t3dInstanceCell_t &rCell,
		PackedFloat32Array &rBuffer,
		const AABB *pSourceMeshAabb,
		AABB *pCellAabb ) const {
	const int stride = rCell.usesColors() ? 16 : 12;
	const size_t transform_count = rCell.m_transforms.size();
	rBuffer.resize( ( int64_t )( transform_count )*stride );
	float *const pWrite = rBuffer.ptrw();
	const t3dPackedTransform_t *packed = rCell.m_transforms.data();
	const uint32_t *pColors = rCell.m_colors.empty() ? nullptr : rCell.m_colors.data();
	t3dSimdPackedTransformsToMultimesh( packed, pColors, transform_count, stride, pWrite );

	bool bAabbInitialized = false;
	if ( pSourceMeshAabb && pCellAabb ) {
		for ( size_t i = 0u; i < transform_count; ++i ) {
			const float *pRecord = pWrite + i * ( size_t )( stride );
			Basis basis;
			basis[0] = Vector3( pRecord[0], pRecord[1], pRecord[2] );
			basis[1] = Vector3( pRecord[4], pRecord[5], pRecord[6] );
			basis[2] = Vector3( pRecord[8], pRecord[9], pRecord[10] );
			const AABB transformed =
					Transform3D( basis, Vector3( pRecord[3], pRecord[7], pRecord[11] ) ).xform( *pSourceMeshAabb );
			if ( !bAabbInitialized ) {
				*pCellAabb = transformed;
				bAabbInitialized = true;
			} else {
				pCellAabb->merge_with( transformed );
			}
		}
	}
	if ( pCellAabb && !bAabbInitialized ) {
		*pCellAabb = AABB();
	}
}

const PackedFloat32Array &T3DInstancer::buildUploadBuffer( const t3dInstanceCell_t &rCell ) const {
	// RenderingServer upload is main-thread owned. Keep this scratch private so
	// public callers cannot retain it and trigger copy-on-write detachment here.
	fillMultimeshBuffer( rCell, m_uploadBuffer );
	return m_uploadBuffer;
}

PackedFloat32Array T3DInstancer::buildMultimeshBuffer( const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors ) const {
	T3DInstanceStore temporary_store;
	t3dInstanceCell_t &cell = temporary_store.getOrCreateCell( 0, s_v2iZero, ( size_t )rXforms.size(), false, false );
	for ( int i = 0; i < rXforms.size(); ++i ) {
		cell.m_transforms.pushBack( rXforms[i] );
	}
	bool bNonwhite = false;
	for ( int i = 0; i < rColors.size(); ++i ) {
		if ( rColors[i] != s_colorWhite ) {
			bNonwhite = true;
			break;
		}
	}
	if ( bNonwhite ) {
		cell.m_colors.resize( cell.m_transforms.size(), s_nInstanceWhiteRgba8 );
		for ( int i = 0; i < rColors.size() && i < ( int )( cell.m_colors.size() ); ++i ) {
			cell.setColor( ( size_t )( i ), rColors[i] );
		}
	}
	PackedFloat32Array result;
	fillMultimeshBuffer( cell, result );
	return result;
}

t3dMultiMeshUpdateResult_t T3DInstancer::updateMultimesh( const int nMeshId,
		const int nLod,
		const t3dInstanceCell_t &rCell,
		t3dRenderCell_t &rRender,
		const bool bUseColors,
		const PackedFloat32Array &rUploadBuffer ) {
	if ( rCell.m_transforms.empty() ) {
		return t3dMultiMeshUpdateResult_t::EMPTY;
	}
	const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( nMeshId );
	if ( pAsset.is_null() ) {
		return t3dMultiMeshUpdateResult_t::MISSING_ASSET;
	}
	const Ref<Mesh> pMesh = pAsset->getMesh( nLod );
	if ( pMesh.is_null() ) {
		return t3dMultiMeshUpdateResult_t::MISSING_MESH;
	}
	const int instance_count = ( int )( rCell.m_transforms.size() );
	const bool bNewMultimesh = !rRender.m_hMultimesh.is_valid();
	if ( bNewMultimesh ) {
		rRender.m_hMultimesh = RS->multimesh_create();
	}
	// Reallocate only when capacity/layout must change. Transform-only updates
	// with a stable count become one packed buffer upload on the retained RID.
	if ( bNewMultimesh || rRender.m_bUsesColors != bUseColors || rRender.m_nUploadedCount != instance_count ) {
		RS->multimesh_allocate_data( rRender.m_hMultimesh,
				instance_count,
				RenderingServer::MULTIMESH_TRANSFORM_3D,
				bUseColors,
				false,
				false );
		rRender.m_nUploadedCount = instance_count;
	}
	const RID mesh_rid = pMesh->get_rid();
	if ( rRender.m_hBoundMesh != mesh_rid ) {
		RS->multimesh_set_mesh( rRender.m_hMultimesh, mesh_rid );
		rRender.m_hBoundMesh = mesh_rid;
	}
	RS->multimesh_set_buffer( rRender.m_hMultimesh, rUploadBuffer );
	return t3dMultiMeshUpdateResult_t::UPDATED;
}

Vector2i T3DInstancer::getCellForLocalPosition( const Vector3 &rLocalPosition,
		const int nRegionSize,
		const int nCellSize ) const {
	IS_INIT( s_v2iZero );
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	return Vector2i( UtilityFunctions::posmod( UtilityFunctions::floori( rLocalPosition.x / fSpacing ), nRegionSize ) /
					nCellSize,
			UtilityFunctions::posmod( UtilityFunctions::floori( rLocalPosition.z / fSpacing ), nRegionSize ) /
					nCellSize );
}

Vector2i T3DInstancer::getCell( const Vector3 &rGlobalPosition, const int nRegionSize ) const {
	return getCellForLocalPosition( rGlobalPosition, nRegionSize, m_nCellSize );
}

t3dUsableHeight_t T3DInstancer::getUsableHeight( T3DSampler &rSampler,
		const Vector3 &rGlobalPosition,
		const Vector2 &rSlopeRange,
		const bool bOnCollision,
		const real_t fRaycastStart,
		const bool bSampleNormal ) const {
	t3dUsableHeight_t result;
	if ( !m_pTerrain || !m_pTerrain->getData() ) {
		return result;
	}
	const T3DData *pData = m_pTerrain->getData();
	real_t fHeight = rSampler.sampleHeight( rGlobalPosition );
	bool raycast_hit = false;
	real_t fRaycastHeight = -FLT_MAX;
	Vector3 raycast_normal = s_v3Up;
	if ( bOnCollision ) {
		const real_t fBaseHeight = std::isnan( fHeight ) ? rGlobalPosition.y : fHeight;
		const Vector3 start( rGlobalPosition.x, fBaseHeight + fRaycastStart, rGlobalPosition.z );
		const Dictionary hit =
				m_pTerrain->getRaycastResult( start, Vector3( 0, -fRaycastStart - 1, 0 ), 0xFFFFFFFF, true );
		if ( hit.has( "position" ) ) {
			raycast_hit = true;
			fRaycastHeight = Vector3( hit["position"] ).y;
			raycast_normal = hit["normal"];
		}
	}
	if ( std::isnan( fHeight ) ) {
		if ( !raycast_hit ) {
			return result;
		}
		fHeight = fRaycastHeight;
	} else if ( raycast_hit && fRaycastHeight > fHeight ) {
		fHeight = fRaycastHeight;
	}
	Vector3 terrain_normal = s_v3Zero;
	Vector3 slope_normal = raycast_hit ? raycast_normal : s_v3Zero;
	const Vector2 slope_range = CLAMP( rSlopeRange, s_v2Zero, V2( 90.0f ) );
	const bool bNeedsTerrainNormal = !raycast_hit && ( bSampleNormal || slope_range.y - slope_range.x <= 89.99f );
	if ( bNeedsTerrainNormal ) {
		uint32_t control = UINT32_MAX;
		if ( !rSampler.sampleControlAndSlopeNormal( rGlobalPosition, control, terrain_normal ) ) {
			return result;
		}
		slope_normal = terrain_normal;
	}
	if ( !pData->isInSlope( rGlobalPosition, slope_range, slope_normal ) ) {
		return result;
	}
	result.m_bValid = true;
	result.m_fHeight = fHeight;
	result.m_bRaycastHit = raycast_hit;
	result.m_normal = raycast_hit ? raycast_normal : ( bSampleNormal ? terrain_normal : s_v3Up );
	return result;
}

bool T3DInstancer::pendingPlacementRegionCellLess( const t3dPendingPlacement_t &rA, const t3dPendingPlacement_t &rB ) {
	if ( rA.m_pRegion != rB.m_pRegion ) {
		return ( uintptr_t )( rA.m_pRegion ) < ( uintptr_t )( rB.m_pRegion );
	}
	return pendingPlacementCellLess( rA, rB );
}

bool T3DInstancer::pendingPlacementCellLess( const t3dPendingPlacement_t &rA, const t3dPendingPlacement_t &rB ) {
	return rA.m_cell.x != rB.m_cell.x ? rA.m_cell.x < rB.m_cell.x : rA.m_cell.y < rB.m_cell.y;
}

void T3DInstancer::appendGlobalNative( const int nMeshId,
		const eastl::vector<Transform3D> &xforms,
		const eastl::vector<Color> &colors,
		const bool bUpdate,
		const uint32_t owner,
		const bool bBackup ) {
	if ( xforms.empty() ) {
		return;
	}
	T3DData *const pData = m_pTerrain->getData();
	const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( nMeshId );
	if ( pAsset.is_null() ) {
		return;
	}
	const real_t fHeightOffset = pAsset->getHeightOffset();
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	m_placementPending.clear();
	if ( m_placementPending.capacity() < xforms.size() ) {
		m_placementPending.reserve( xforms.size() );
	}

	for ( size_t i = 0; i < xforms.size(); ++i ) {
		Transform3D transform = xforms[i];
		transform.origin += transform.basis.get_column( 1 ) * fHeightOffset;
		const Vector2i regionLocation = pData->getRegionLocation( transform.origin );
		T3DRegion *const region = pData->getRegionPtr( regionLocation );
		if ( !region || region->isDeleted() ) {
			continue;
		}
		const int region_size = region->getRegionSize();
		transform.origin.x -= regionLocation.x * region_size * fSpacing;
		transform.origin.z -= regionLocation.y * region_size * fSpacing;
		m_placementPending.push_back( t3dPendingPlacement_t{
				region,
				getCell( transform.origin, region_size ),
				transform,
				i < colors.size() ? colors[i] : s_colorWhite,
				owner,
		} );
	}

	eastl::sort( m_placementPending.begin(), m_placementPending.end(), pendingPlacementRegionCellLess );

	m_nTouchedRegionCount = 0u;
	const bool bWasMaterializing = m_bMaterializingUpdates;
	m_bMaterializingUpdates = true;
	size_t region_begin = 0;
	while ( region_begin < m_placementPending.size() ) {
		T3DRegion *const region = m_placementPending[region_begin].m_pRegion;
		size_t region_end = region_begin + 1;
		while ( region_end < m_placementPending.size() && m_placementPending[region_end].m_pRegion == region ) {
			++region_end;
		}
		const Vector2i regionLocation = region->getLocation();
		bool bTemporaryPage = false;
		if ( !t3dBeginInstancePageAccess( this, pData, regionLocation, bTemporaryPage ) ) {
			region_begin = region_end;
			continue;
		}
		if ( bBackup ) {
			backupRegion( Ref<T3DRegion>( region ) );
		} else {
			region->setModified( true );
		}
		if ( !bTemporaryPage ) {
			if ( m_nTouchedRegionCount < s_nMaxRegionSlots ) {
				m_pszTouchedRegions[m_nTouchedRegionCount++] = region;
			}
		}

		size_t begin = region_begin;
		while ( begin < region_end ) {
			const Vector2i cell_location = m_placementPending[begin].m_cell;
			size_t end = begin + 1;
			bool bColorData = m_placementPending[begin].m_color != s_colorWhite;
			bool bOwnerData = m_placementPending[begin].m_nOwner != 0u;
			while ( end < region_end && m_placementPending[end].m_cell == cell_location ) {
				bColorData = bColorData || m_placementPending[end].m_color != s_colorWhite;
				bOwnerData = bOwnerData || m_placementPending[end].m_nOwner != 0u;
				++end;
			}

			t3dInstanceCell_t &cell = region->getInstanceStoreMut().getOrCreateCell(
					nMeshId, cell_location, end - begin, bColorData, bOwnerData );
			cell.prepareAppend( end - begin, bColorData, bOwnerData );
			for ( size_t i = begin; i < end; ++i ) {
				cell.m_transforms.pushBack( m_placementPending[i].m_transform );
				if ( !cell.m_colors.empty() ) {
					cell.pushColor( m_placementPending[i].m_color );
				}
				if ( !cell.m_owners.empty() ) {
					cell.m_owners.pushBack( m_placementPending[i].m_nOwner );
				}
			}
			cell.touch();
			setLogicalCellCount( regionLocation, nMeshId, cell_location, cell.m_transforms.size() );
			begin = end;
		}
		t3dEndInstancePageAccess( pData, regionLocation, bTemporaryPage );
		region_begin = region_end;
	}
	m_bMaterializingUpdates = bWasMaterializing;
	if ( !bWasMaterializing && !m_bProcessingPendingUpdates ) {
		flushLogicalMeshCounts();
	}

	if ( bUpdate ) {
		for ( uint16_t i = 0u; i < m_nTouchedRegionCount; ++i ) {
			updateMmis( nMeshId, m_pszTouchedRegions[i]->getLocation() );
		}
	}
}

void T3DInstancer::appendRegionNative( T3DRegion *const pRegion,
		const int nMeshId,
		const eastl::vector<Transform3D> &rXforms,
		const eastl::vector<Color> &rColors,
		const bool bGlobalSpace,
		const bool bUpdate,
		const bool bBackup,
		const uint32_t nOwner ) {
	if ( !pRegion || pRegion->isDeleted() || rXforms.empty() ) {
		return;
	}
	T3DData *const pData = m_pTerrain->getData();
	const Vector2i regionLocation = pRegion->getLocation();
	bool bTemporaryPage = false;
	if ( !t3dBeginInstancePageAccess( this, pData, regionLocation, bTemporaryPage ) ) {
		return;
	}
	if ( bBackup ) {
		backupRegion( Ref<T3DRegion>( pRegion ) );
	} else {
		pRegion->setModified( true );
	}
	const int region_size = pRegion->getRegionSize();
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	m_placementPending.clear();
	if ( m_placementPending.capacity() < rXforms.size() ) {
		m_placementPending.reserve( rXforms.size() );
	}
	for ( size_t i = 0; i < rXforms.size(); ++i ) {
		Transform3D transform = rXforms[i];
		if ( bGlobalSpace ) {
			transform.origin.x -= regionLocation.x * region_size * fSpacing;
			transform.origin.z -= regionLocation.y * region_size * fSpacing;
		}
		m_placementPending.push_back( t3dPendingPlacement_t{
				pRegion,
				getCell( transform.origin, region_size ),
				transform,
				i < rColors.size() ? rColors[i] : s_colorWhite,
				nOwner,
		} );
	}

	eastl::sort( m_placementPending.begin(), m_placementPending.end(), pendingPlacementCellLess );

	const bool bWasMaterializing = m_bMaterializingUpdates;
	m_bMaterializingUpdates = true;
	size_t begin = 0;
	while ( begin < m_placementPending.size() ) {
		const Vector2i cell_location = m_placementPending[begin].m_cell;
		size_t end = begin + 1;
		bool bColorData = m_placementPending[begin].m_color != s_colorWhite;
		bool bOwnerData = m_placementPending[begin].m_nOwner != 0u;
		while ( end < m_placementPending.size() && m_placementPending[end].m_cell == cell_location ) {
			bColorData = bColorData || m_placementPending[end].m_color != s_colorWhite;
			bOwnerData = bOwnerData || m_placementPending[end].m_nOwner != 0u;
			++end;
		}
		t3dInstanceCell_t &cell = pRegion->getInstanceStoreMut().getOrCreateCell(
				nMeshId, cell_location, end - begin, bColorData, bOwnerData );
		cell.prepareAppend( end - begin, bColorData, bOwnerData );
		for ( size_t i = begin; i < end; ++i ) {
			cell.m_transforms.pushBack( m_placementPending[i].m_transform );
			if ( !cell.m_colors.empty() ) {
				cell.pushColor( m_placementPending[i].m_color );
			}
			if ( !cell.m_owners.empty() ) {
				cell.m_owners.pushBack( m_placementPending[i].m_nOwner );
			}
		}
		cell.touch();
		setLogicalCellCount( regionLocation, nMeshId, cell_location, cell.m_transforms.size() );
		begin = end;
	}
	m_bMaterializingUpdates = bWasMaterializing;
	if ( !bWasMaterializing && !m_bProcessingPendingUpdates ) {
		flushLogicalMeshCounts();
	}
	if ( bUpdate && !bTemporaryPage ) {
		updateMmis( nMeshId, regionLocation );
	}
	t3dEndInstancePageAccess( pData, regionLocation, bTemporaryPage );
}

// -----------------------------------------------------------------------------
// Public functions

void T3DInstancer::initialize( T3DTerrain *const pTerrain ) {
	if ( pTerrain ) {
		m_pTerrain = pTerrain;
	}
	IS_DATA_INIT_MESG( "Terrain3D not initialized yet", VOID );
	LOG( INFO, "Initializing Instancer" );
	t3dInstanceLayout_t active_layout;
	if ( !t3dInstanceLayoutForRegion( ( int )( m_pTerrain->getRegionSize() ), m_nCellSize, active_layout ) ) {
		LOG( ERROR, "Instancer cell layout is incompatible with region size" );
		return;
	}
	if ( !repartitionResidentRegions( active_layout ) ) {
		LOG( ERROR, "Could not migrate resident instance page layouts" );
		return;
	}
	size_t estimated_render_cells = 0;
	T3DRegion *const *ppRegionPtrs = m_pTerrain->getData()->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		const T3DRegion *pRegion = ppRegionPtrs[region_index];
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( m_pTerrain->getData()->isInstanceRegionCacheEnabled() &&
				!m_pTerrain->getData()->isInstanceRegionResident( pRegion->getLocation() ) ) {
			continue;
		}
		for ( const t3dInstanceMesh_t &mesh : pRegion->getInstanceStore().meshes() ) {
			const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( mesh.m_nId );
			const int render_lods =
					pAsset.is_valid() ? pAsset->getLodCount() + ( pAsset->getShadowImpostor() > 0 ? 1 : 0 ) : 1;
			estimated_render_cells += mesh.m_cells.size() * ( size_t )( render_lods );
		}
	}
	const size_t render_reserve = MAX<size_t>( 1024, estimated_render_cells );
	const size_t logical_reserve = MAX<size_t>( 256, estimated_render_cells / 2 );
	m_renderCells.reserve( render_reserve );
	m_renderRegionNodes.reserve( render_reserve );
	m_iRenderCellIndex.reserve( render_reserve );
	m_logicalRegionNodes.reserve( logical_reserve );
	m_transitionCellNodes.reserve( logical_reserve );
	m_logicalCellCounts.reserve( logical_reserve );
	m_singleLodTransitionBuckets.reserve( MAX<size_t>( 64, logical_reserve / s_nInstanceTransitionBucketCellSpan ) );
	m_streamingUpdates.reserve( 512 );
	m_activeStreamingUpdates.reserve( 512 );
	m_pendingCellUpdates.reserve( 1024 );
	m_buildingCellUpdates.reserve( 1024 );
	m_bStreamingCacheDirty = true;
	refreshStreamingCache();
	m_placementTransforms.reserve( 256 );
	m_placementColors.reserve( 256 );
	m_placementPending.reserve( 256 );
	rebuildLogicalCountsFromStore();
	if ( m_bCellStreamingEnabled ) {
		m_lastStreamPosition = m_pTerrain->getClipmapTargetPosition();
		m_bStreamPositionValid = true;
	}
	updateMmis();
}

void T3DInstancer::resetRuntimeState( const bool bClearLogicalCounts ) {
	const Callable callback = callable_mp( this, &T3DInstancer::processUpdates );
	if ( RS && RS->is_connected( "frame_pre_draw", callback ) ) {
		RS->disconnect( "frame_pre_draw", callback );
	}
	if ( !m_pTerrain ) {
		return;
	}
	m_bRebuildAll = false;
	m_bUpdateAll = false;
	m_globalMeshUpdates.reset();
	clearRegionUpdates( false );
	m_streamingUpdates.clear();
	clearRegionUpdates( true );
	m_activeStreamingUpdates.clear();
	cancelPendingCellUpdates();
	m_nFiniteStreamingMeshCount = 0;
	m_bStreamingCacheDirty = true;
	m_fMaxStreamingRadius = 0.0f;
	std::fill_n( m_szMeshStreamingRadius, s_nMaxMeshAssets, ( real_t )( 0.0f ) );
	m_bStreamPositionValid = false;
	destroyRenderCells();

	if ( bClearLogicalCounts ) {
		m_dirtyLogicalMeshCounts.reset();
		m_bPublishingLogicalMeshCounts = false;
		clearLogicalCellCounts();
	}
}

void T3DInstancer::destroy( void ) {
	resetRuntimeState( true );
}

void T3DInstancer::suspendRendering( void ) {
	resetRuntimeState( false );
}

void T3DInstancer::clearByMesh( const int nMeshId ) {
	LOG( INFO, "Deleting Multimeshes in all regions with mesh_id: ", nMeshId );
	T3DRegion *const *ppRegionPtrs = m_pTerrain->getData()->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		T3DRegion *const pRegion = ppRegionPtrs[region_index];
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		clearByRegion( Ref<T3DRegion>( pRegion ), nMeshId );
	}
	Ref<T3DMeshAsset> pMa = m_pTerrain->getAssets()->getMeshAsset( nMeshId );
	pMa.is_valid() ? pMa->setInstanceCount( 0 ) : ( void )0; // Reset count for this mesh
}

void T3DInstancer::clearByLocation( const Vector2i &rRegionLoc, const int nMeshId ) {
	LOG( INFO, "Deleting Multimeshes w/ mesh_id: ", nMeshId, " in region: ", rRegionLoc );
	const Ref<T3DRegion> pRegion = m_pTerrain->getData()->getRegion( rRegionLoc );
	clearByRegion( pRegion, nMeshId );
}

void T3DInstancer::clearByRegion( const Ref<T3DRegion> &rRegion, const int nMeshId ) {
	if ( rRegion.is_null() ) {
		return;
	}
	const Vector2i location = rRegion->getLocation();
	if ( rRegion->getInstanceCount( nMeshId ) > 0u ) {
		T3DData *const pData = m_pTerrain->getData();
		bool bTemporaryPage = false;
		if ( !t3dBeginInstancePageAccess( this, pData, location, bTemporaryPage ) ) {
			return;
		}
		if ( rRegion->getInstanceStore().hasMesh( nMeshId ) ) {
			backupRegion( rRegion );
			rRegion->getInstanceStoreMut().eraseMesh( nMeshId );
		}
		t3dEndInstancePageAccess( pData, location, bTemporaryPage );
	}
	eraseLogicalCellsByLocation( location, nMeshId );
	destroyMmiByLocation( location, nMeshId );
}

void T3DInstancer::destroyRegionMmis( const Vector2i &rRegionLoc ) {
	eraseLogicalCellsByRegion( rRegionLoc );
	if ( m_mode != INSTANCER_DISABLED ) {
		destroyMmisByRegion( rRegionLoc );
	}
}

void T3DInstancer::setMode( const t3dInstancerMode_t mode ) {
	LOG( INFO, "Setting instancer mode: ", mode );
	if ( mode != m_mode ) {
		m_mode = mode;
		switch ( m_mode ) {
			case NORMAL:
				updateMmis( -1, s_v2iMax, true );
				break;
			// case PLACEHOLDER:
			//	break;
			default:
				suspendRendering();
				break;
		}
	}
}

void T3DInstancer::addInstances( const Vector3 &rGlobalPosition, const Dictionary &rParams ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", VOID );
	const int mesh_id = rParams.get( "asset_id", 0 );
	if ( mesh_id < 0 || mesh_id >= m_pTerrain->getAssets()->getMeshCount() ) {
		LOG( ERROR, "Mesh ID out of range: ", mesh_id, ", valid: 0 to ", m_pTerrain->getAssets()->getMeshCount() - 1 );
		return;
	}

	const real_t fBrushSize = CLAMP( ( real_t )( rParams.get( "size", 10.f ) ), 0.1f, 4096.f ); // Meters
	const real_t radius = fBrushSize * .5f;
	const real_t fStrength = CLAMP( ( real_t )( rParams.get( "strength", .1f ) ), .01f, 100.f ); // (premul) 1-10k%
	const real_t fFixedScale = CLAMP( ( real_t )( rParams.get( "fixed_scale", 100.f ) ) * .01f, .01f, 100.f ); // 1-10k%
	const real_t fRandomScale =
			CLAMP( ( real_t )( rParams.get( "random_scale", 0.f ) ) * .01f, 0.f, 10.f ); // +/- 1000%
	const Ref<T3DMeshAsset> pMeshAsset = m_pTerrain->getAssets()->getMeshAsset( mesh_id );
	const real_t fDensity = CLAMP(
			.1f * fBrushSize * fStrength * pMeshAsset->getDensity() / MAX( 0.01f, fFixedScale + .5f * fRandomScale ),
			.001f,
			1000.f );
	// Density based on strength, mesh AABB and input scale determines how many to place, even fractional
	const uint32_t count = getDensityCount( fDensity );
	if ( count <= 0 ) {
		return;
	}
	LOG( EXTREME, "Adding ", count, " instances at ", rGlobalPosition );

	const real_t fFixedSpin = CLAMP( ( real_t )( rParams.get( "fixed_spin", 0.f ) ), .0f, 360.f ); // degrees
	const real_t fRandomSpin = CLAMP( ( real_t )( rParams.get( "random_spin", 360.f ) ), 0.f, 360.f ); // degrees
	const real_t fFixedTilt = CLAMP( ( real_t )( rParams.get( "fixed_tilt", 0.f ) ), -180.f, 180.f ); // degrees
	const real_t fRandomTilt = CLAMP( ( real_t )( rParams.get( "random_tilt", 10.f ) ), 0.f, 180.f ); // degrees
	const bool bAlignToNormal = ( bool )( rParams.get( "align_to_normal", false ) );

	const real_t fHeightOffset = CLAMP( ( real_t )( rParams.get( "height_offset", 0.f ) ), -100.0f, 100.f ); // meters
	const real_t fRandomHeight = CLAMP( ( real_t )( rParams.get( "random_height", 0.f ) ), 0.f, 100.f ); // meters

	const Color vertex_color = Color( rParams.get( "vertex_color", s_colorWhite ) );
	const real_t fRandomHue =
			CLAMP( ( real_t )( rParams.get( "random_hue", 0.f ) ) / 360.f, 0.f, 1.f ); // degrees -> 0-1
	const real_t fRandomDarken = CLAMP( ( real_t )( rParams.get( "random_darken", 0.f ) ) * .01f, 0.f, 1.f ); // 0-100%

	Vector2 slope_range = rParams.get( "slope", Vector2( 0.f, 90.f ) ); // 0-90 degrees
	slope_range.x = CLAMP( slope_range.x, 0.f, 90.f );
	slope_range.y = CLAMP( slope_range.y, 0.f, 90.f );
	const bool bOnCollision = ( bool )( rParams.get( "on_collision", false ) );
	const real_t fRaycastHeight = rParams.get( "raycast_height", 10.f );
	T3DData *const pData = m_pTerrain->getData();
	T3DSampler sampler( pData->getSamplingSourceInternal() );

	m_placementTransforms.clear();
	m_placementColors.clear();
	if ( m_placementTransforms.capacity() < count ) {
		m_placementTransforms.reserve( count );
	}
	if ( m_placementColors.capacity() < count ) {
		m_placementColors.reserve( count );
	}
	for ( uint32_t i = 0; i < count; ++i ) {
		Transform3D t;

		// Get random XZ position and height in a circle
		const real_t fRadius = radius * sqrt( UtilityFunctions::randf() );
		const real_t fTheta = UtilityFunctions::randf() * Math_TAU;
		const Vector3 rand_vec = Vector3( fRadius * cos( fTheta ), 0.f, fRadius * sin( fTheta ) );
		Vector3 position = rGlobalPosition + rand_vec;

		// Get height
		const t3dUsableHeight_t height_data =
				getUsableHeight( sampler, position, slope_range, bOnCollision, fRaycastHeight, bAlignToNormal );
		if ( !height_data.m_bValid ) {
			continue;
		}
		position.y = height_data.m_fHeight;
		// Orientation
		Vector3 normal = s_v3Up;
		if ( bAlignToNormal ) {
			// The reusable sampler returns either the collision normal or terrain normal.
			normal = height_data.m_normal;
			if ( !normal.is_finite() ) {
				normal = s_v3Up;
			} else {
				normal = normal.normalized();
				const Vector3 z_axis = Vector3( 0.f, 0.f, 1.f );
				const Vector3 x_axis = -z_axis.cross( normal );
				if ( x_axis.length_squared() > 0.001f ) {
					t.basis = Basis( x_axis, normal, z_axis ).orthonormalized();
				}
			}
		}
		const real_t fSpin = ( fFixedSpin + fRandomSpin * UtilityFunctions::randf() ) * Math_PI / 180.f;
		if ( std::abs( fSpin ) > 0.001f ) {
			t.basis = t.basis.rotated( normal, fSpin );
		}
		const real_t fTilt = ( fFixedTilt + fRandomTilt * ( 2.f * UtilityFunctions::randf() - 1.f ) ) * Math_PI / 180.f;
		if ( std::abs( fTilt ) > 0.001f ) {
			t.basis = t.basis.rotated( t.basis.get_column( 0 ), fTilt ); // Rotate pitch, X-axis
		}

		// Scale
		const real_t fTScale =
				CLAMP( fFixedScale + fRandomScale * ( 2.f * UtilityFunctions::randf() - 1.f ), 0.01f, 10.f );
		t = t.scaled( Vector3( fTScale, fTScale, fTScale ) );

		// Applies the mesh-asset height offset added by addTransforms().
		const real_t fOffset = fHeightOffset + fRandomHeight * ( 2.f * UtilityFunctions::randf() - 1.f );
		position += t.basis.get_column( 1 ) * fOffset; // Offset along UP axis
		t = t.translated( position );

		// Color
		Color col = vertex_color;
		col.set_v( CLAMP( col.get_v() - fRandomDarken * UtilityFunctions::randf(), 0.f, 1.f ) );
		col.set_h( fmod( col.get_h() + fRandomHue * ( 2.f * UtilityFunctions::randf() - 1.f ), 1.f ) );

		m_placementTransforms.push_back( t );
		m_placementColors.push_back( col );
	}
	appendGlobalNative( mesh_id, m_placementTransforms, m_placementColors, true );
}

void T3DInstancer::removeInstances( const Vector3 &rGlobalPosition, const Dictionary &rParams ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", VOID );
	const int selected_mesh = rParams.get( "asset_id", 0 );
	const int mesh_count = m_pTerrain->getAssets()->getMeshCount();
	if ( selected_mesh < 0 || selected_mesh >= mesh_count ) {
		return;
	}
	const bool bAllMeshes = ( bool )( rParams.get( "modifier_shift", false ) );
	const real_t fBrushSize = CLAMP( ( real_t )( rParams.get( "size", 10.f ) ), .1f, 4096.f );
	const real_t fRadius = fBrushSize * .5f;
	const real_t fStrength = CLAMP( ( real_t )( rParams.get( "strength", .1f ) ), .01f, 100.f );
	Vector2 slope = rParams.get( "slope", Vector2( 0, 90 ) );
	slope.x = CLAMP( slope.x, 0.f, 90.f );
	slope.y = CLAMP( slope.y, 0.f, 90.f );
	const bool bOnCollision = ( bool )( rParams.get( "on_collision", false ) );
	const real_t fRaycastHeight = rParams.get( "raycast_height", 10.f );
	const int region_size = m_pTerrain->getRegionSize();
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	T3DData *const pData = m_pTerrain->getData();
	T3DSampler sampler( pData->getSamplingSourceInternal() );
	const bool bWasMaterializing = m_bMaterializingUpdates;
	m_bMaterializingUpdates = true;
	T3DRegion *const *ppRegionPtrs = pData->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		T3DRegion *const pRegionPtr = ppRegionPtrs[region_index];
		if ( !pRegionPtr || pRegionPtr->isDeleted() || !pRegionPtr->hasInstances() ) {
			continue;
		}
		const Vector2i regionLocation = pRegionPtr->getLocation();
		const Vector3 regionOrigin(
				regionLocation.x * region_size * fSpacing, 0, regionLocation.y * region_size * fSpacing );
		const real_t fExtent = region_size * fSpacing;
		if ( rGlobalPosition.x + fRadius < regionOrigin.x || rGlobalPosition.x - fRadius > regionOrigin.x + fExtent ||
				rGlobalPosition.z + fRadius < regionOrigin.z ||
				rGlobalPosition.z - fRadius > regionOrigin.z + fExtent ) {
			continue;
		}
		bool bTemporaryPage = false;
		if ( !t3dBeginInstancePageAccess( this, pData, regionLocation, bTemporaryPage ) ) {
			continue;
		}
		Ref<T3DRegion> region( pRegionPtr );
		bool bBackedUp = false;
		T3DInstanceStore &store = region->getInstanceStoreInternal();
		for ( int mesh_id = bAllMeshes ? 0 : selected_mesh; mesh_id <= ( bAllMeshes ? mesh_count - 1 : selected_mesh );
				++mesh_id ) {
			bool bMeshChanged = false;
			t3dInstanceMesh_t *pMesh = store.findMesh( mesh_id );
			if ( !pMesh ) {
				continue;
			}
			const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( mesh_id );
			const real_t fHeightOffset = pAsset.is_valid() ? pAsset->getHeightOffset() : 0.f;
			for ( int cell_index = ( int )( pMesh->m_cells.size() ) - 1; cell_index >= 0; --cell_index ) {
				t3dInstanceCell_t &cell = pMesh->m_cells[( size_t )( cell_index )];
				size_t write = 0;
				bool bChanged = false;
				for ( size_t i = 0; i < cell.m_transforms.size(); ++i ) {
					const Transform3D transform = cell.m_transforms[i];
					const Vector3 global = transform.origin + regionOrigin;
					const real_t fDx = global.x - rGlobalPosition.x;
					const real_t fDz = global.z - rGlobalPosition.z;
					bool bRemove = fDx * fDx + fDz * fDz < fRadius * fRadius &&
							UtilityFunctions::randf() < CLAMP( .175f * fStrength, .005f, 1.f );
					if ( bRemove ) {
						const Vector3 sample = global - transform.basis.get_column( 1 ) * fHeightOffset;
						bRemove = getUsableHeight( sampler, sample, slope, bOnCollision, fRaycastHeight ).m_bValid;
					}
					if ( bRemove ) {
						if ( !bBackedUp ) {
							backupRegion( region );
							bBackedUp = true;
						}
						bChanged = true;
						bMeshChanged = true;
						continue;
					}
					if ( write != i ) {
						cell.m_transforms[write] = cell.m_transforms[i];
						if ( cell.usesColors() ) {
							cell.m_colors[write] = cell.m_colors[i];
						}
						if ( cell.usesOwners() ) {
							cell.m_owners[write] = cell.m_owners[i];
						}
					}
					++write;
				}
				if ( !bChanged ) {
					continue;
				}
				cell.m_transforms.resize( write );
				if ( cell.usesColors() ) {
					cell.m_colors.resize( write );
				}
				if ( cell.usesOwners() ) {
					cell.m_owners.resize( write );
					cell.compactOwnerSidecar();
				}
				cell.touch();
				if ( write == 0 ) {
					const Vector2i dead_cell = cell.m_location;
					pMesh->m_cells.erase( pMesh->m_cells.begin() + cell_index );
					eraseLogicalCellCount( regionLocation, mesh_id, dead_cell );
					destroyMmiByCell( regionLocation, mesh_id, dead_cell );
				} else {
					setLogicalCellCount( regionLocation, mesh_id, cell.m_location, cell.m_transforms.size() );
				}
			}
			if ( pMesh->m_cells.empty() ) {
				store.eraseMesh( mesh_id );
			}
			if ( bMeshChanged && !bTemporaryPage ) {
				updateMmis( mesh_id, regionLocation );
			}
		}
		t3dEndInstancePageAccess( pData, regionLocation, bTemporaryPage );
	}
	m_bMaterializingUpdates = bWasMaterializing;
	if ( !bWasMaterializing && !m_bProcessingPendingUpdates ) {
		flushLogicalMeshCounts();
	}
}

void T3DInstancer::addMultimesh( const int nMeshId,
		const Ref<MultiMesh> &rMultimesh,
		const Transform3D &rXform,
		const bool bUpdate ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", VOID );
	if ( rMultimesh.is_null() || nMeshId < 0 || nMeshId >= m_pTerrain->getAssets()->getMeshCount() ) {
		return;
	}
	const int instance_count = rMultimesh->get_instance_count();
	if ( instance_count <= 0 ) {
		return;
	}
	LOG( INFO, "Extracting ", instance_count, " transforms from multimesh" );
	const bool bUsesColors = rMultimesh->is_using_colors();
	m_placementTransforms.resize( ( size_t )( instance_count ) );
	m_placementColors.resize( ( size_t )( instance_count ) );
	const bool bCanDecodeBuffer = rMultimesh->get_transform_format() == MultiMesh::TRANSFORM_3D;
	const int stride = 12 + ( bUsesColors ? 4 : 0 ) + ( rMultimesh->is_using_custom_data() ? 4 : 0 );
	const PackedFloat32Array buffer = bCanDecodeBuffer ? rMultimesh->get_buffer() : PackedFloat32Array();
	if ( bCanDecodeBuffer && buffer.size() >= instance_count * stride ) {
		const float *pRead = buffer.ptr();
		for ( int i = 0; i < instance_count; ++i ) {
			const int offset = i * stride;
			Transform3D transform;
			transform.basis[0] = Vector3( pRead[offset + 0], pRead[offset + 1], pRead[offset + 2] );
			transform.basis[1] = Vector3( pRead[offset + 4], pRead[offset + 5], pRead[offset + 6] );
			transform.basis[2] = Vector3( pRead[offset + 8], pRead[offset + 9], pRead[offset + 10] );
			transform.origin = Vector3( pRead[offset + 3], pRead[offset + 7], pRead[offset + 11] );
			m_placementTransforms[( size_t )( i )] = rXform * transform;
			m_placementColors[( size_t )( i )] = bUsesColors
					? Color( pRead[offset + 12], pRead[offset + 13], pRead[offset + 14], pRead[offset + 15] )
					: s_colorWhite;
		}
	} else {
		// Keep compatibility with 2D or backend-specific layouts while avoiding
		// the per-instance API path for the normal 3D terrain use case.
		for ( int i = 0; i < instance_count; ++i ) {
			m_placementTransforms[( size_t )( i )] = rXform * rMultimesh->get_instance_transform( i );
			m_placementColors[( size_t )( i )] = bUsesColors ? rMultimesh->get_instance_color( i ) : s_colorWhite;
		}
	}
	appendGlobalNative( nMeshId, m_placementTransforms, m_placementColors, bUpdate );
}

// Expects transforms in global space
void T3DInstancer::addTransforms( const int nMeshId,
		const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors,
		const bool bUpdate ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", VOID );
	if ( rXforms.is_empty() || nMeshId < 0 || nMeshId >= m_pTerrain->getAssets()->getMeshCount() ) {
		return;
	}
	m_placementTransforms.clear();
	m_placementColors.clear();
	m_placementTransforms.reserve( rXforms.size() );
	m_placementColors.reserve( rXforms.size() );
	for ( int i = 0; i < rXforms.size(); ++i ) {
		m_placementTransforms.push_back( rXforms[i] );
		m_placementColors.push_back( i < rColors.size() ? rColors[i] : s_colorWhite );
	}
	appendGlobalNative( nMeshId, m_placementTransforms, m_placementColors, bUpdate );
}

void T3DInstancer::addOwnedTransforms( const int nMeshId,
		const TypedArray<Transform3D> &rXforms,
		const int64_t nOwnerId,
		const PackedColorArray &rColors,
		const bool bUpdate,
		const bool bBackup ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", VOID );
	if ( rXforms.is_empty() || nOwnerId <= 0 || nOwnerId > ( int64_t )( INT32_MAX ) || nMeshId < 0 ||
			nMeshId >= m_pTerrain->getAssets()->getMeshCount() ) {
		return;
	}
	m_placementTransforms.clear();
	m_placementColors.clear();
	m_placementTransforms.reserve( rXforms.size() );
	m_placementColors.reserve( rXforms.size() );
	for ( int i = 0; i < rXforms.size(); ++i ) {
		m_placementTransforms.push_back( rXforms[i] );
		m_placementColors.push_back( i < rColors.size() ? rColors[i] : s_colorWhite );
	}
	appendGlobalNative( nMeshId, m_placementTransforms, m_placementColors, bUpdate, ( uint32_t )( nOwnerId ), bBackup );
}

int T3DInstancer::removeOwnedInstances( const int64_t nOwnerId, const bool bUpdate, const bool bBackup ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", 0 );
	if ( nOwnerId <= 0 || nOwnerId > ( int64_t )( INT32_MAX ) ) {
		return 0;
	}
	const uint32_t owner_id = ( uint32_t )( nOwnerId );
	T3DData *const pData = m_pTerrain->getData();
	T3DRegion *const *ppRegionPtrs = pData->getRegionPtrMapInternal();
	const bool bWasMaterializing = m_bMaterializingUpdates;
	m_bMaterializingUpdates = true;
	int removed_count = 0;

	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		T3DRegion *const pRegionPtr = ppRegionPtrs[region_index];
		if ( !pRegionPtr || pRegionPtr->isDeleted() || !pRegionPtr->hasInstances() ) {
			continue;
		}
		const Vector2i location = pRegionPtr->getLocation();
		bool bTemporaryPage = false;
		if ( !t3dBeginInstancePageAccess( this, pData, location, bTemporaryPage ) ) {
			continue;
		}
		const Ref<T3DRegion> region( pRegionPtr );
		T3DInstanceStore &store = pRegionPtr->getInstanceStoreInternal();
		bool bBackedUp = false;
		bool bRegionChanged = false;
		for ( int mesh_index = ( int )( store.meshesMut().size() ) - 1; mesh_index >= 0; --mesh_index ) {
			const int mesh_id = store.meshesMut()[( size_t )( mesh_index )].m_nId;
			t3dInstanceMesh_t *pMesh = store.findMesh( mesh_id );
			if ( !pMesh ) {
				continue;
			}
			bool bMeshChanged = false;
			for ( int cell_index = ( int )( pMesh->m_cells.size() ) - 1; cell_index >= 0; --cell_index ) {
				t3dInstanceCell_t &cell = pMesh->m_cells[( size_t )( cell_index )];
				if ( !cell.usesOwners() ) {
					continue;
				}
				size_t write = 0;
				for ( size_t read = 0; read < cell.m_transforms.size(); ++read ) {
					if ( cell.getOwner( read ) == owner_id ) {
						if ( !bBackedUp ) {
							if ( bBackup ) {
								backupRegion( region );
							} else {
								pRegionPtr->setModified( true );
							}
							bBackedUp = true;
						}
						++removed_count;
						bMeshChanged = true;
						bRegionChanged = true;
						continue;
					}
					if ( write != read ) {
						cell.m_transforms[write] = cell.m_transforms[read];
						if ( cell.usesColors() ) {
							cell.m_colors[write] = cell.m_colors[read];
						}
						cell.m_owners[write] = cell.m_owners[read];
					}
					++write;
				}
				if ( write == cell.m_transforms.size() ) {
					continue;
				}
				cell.m_transforms.resize( write );
				if ( cell.usesColors() ) {
					cell.m_colors.resize( write );
				}
				cell.m_owners.resize( write );
				cell.compactOwnerSidecar();
				cell.touch();
				if ( write == 0 ) {
					const Vector2i dead_cell = cell.m_location;
					pMesh->m_cells.erase( pMesh->m_cells.begin() + cell_index );
					eraseLogicalCellCount( location, mesh_id, dead_cell );
					destroyMmiByCell( location, mesh_id, dead_cell );
				} else {
					setLogicalCellCount( location, mesh_id, cell.m_location, write );
				}
			}
			if ( pMesh->m_cells.empty() ) {
				store.eraseMesh( mesh_id );
			}
			if ( bMeshChanged && bUpdate && !bTemporaryPage ) {
				updateMmis( mesh_id, location );
			}
		}
		if ( bRegionChanged ) {
			pRegionPtr->setModified( true );
		}
		t3dEndInstancePageAccess( pData, location, bTemporaryPage );
	}
	m_bMaterializingUpdates = bWasMaterializing;
	if ( !bWasMaterializing && !m_bProcessingPendingUpdates ) {
		flushLogicalMeshCounts();
	}
	return removed_count;
}

void T3DInstancer::appendLocation( const Vector2i &rRegionLoc,
		const int nMeshId,
		const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors,
		const bool bUpdate ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", VOID );
	T3DRegion *const pRegion = m_pTerrain->getData()->getRegionPtr( rRegionLoc );
	if ( !pRegion || pRegion->isDeleted() || rXforms.is_empty() ) {
		return;
	}
	m_placementTransforms.resize( ( size_t )( rXforms.size() ) );
	m_placementColors.resize( ( size_t )( rXforms.size() ) );
	for ( int i = 0; i < rXforms.size(); ++i ) {
		m_placementTransforms[( size_t )( i )] = rXforms[i];
		m_placementColors[( size_t )( i )] = i < rColors.size() ? rColors[i] : s_colorWhite;
	}
	appendRegionNative( pRegion, nMeshId, m_placementTransforms, m_placementColors, true, bUpdate );
}

void T3DInstancer::appendRegion( const Ref<T3DRegion> &rRegion,
		const int nMeshId,
		const TypedArray<Transform3D> &rXforms,
		const PackedColorArray &rColors,
		const bool bUpdate ) {
	if ( rRegion.is_null() || rXforms.is_empty() ) {
		return;
	}
	m_placementTransforms.resize( ( size_t )( rXforms.size() ) );
	m_placementColors.resize( ( size_t )( rXforms.size() ) );
	for ( int i = 0; i < rXforms.size(); ++i ) {
		m_placementTransforms[( size_t )( i )] = rXforms[i];
		m_placementColors[( size_t )( i )] = i < rColors.size() ? rColors[i] : s_colorWhite;
	}
	appendRegionNative( rRegion.ptr(), nMeshId, m_placementTransforms, m_placementColors, false, bUpdate );
}

// Review all transforms in one area and adjust their transforms w/ the current height
void T3DInstancer::updateTransforms( const AABB &rAabb ) {
	IS_DATA_INIT( VOID );
	T3DData *const pData = m_pTerrain->getData();
	T3DSampler sampler( pData->getSamplingSourceInternal() );
	const Dictionary params = m_pTerrain->getEditor() ? m_pTerrain->getEditor()->getBrushData() : Dictionary();
	const bool bOnCollision = params.get( "on_collision", false );
	const real_t fRaycastHeight = params.get( "raycast_height", 10.f );
	const int region_size = m_pTerrain->getRegionSize();
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	T3DRegion *const *ppRegionPtrs = pData->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		T3DRegion *const pRegionPtr = ppRegionPtrs[region_index];
		if ( !pRegionPtr || pRegionPtr->isDeleted() || !pRegionPtr->hasInstances() ) {
			continue;
		}
		const Vector2i regionLocation = pRegionPtr->getLocation();
		const Vector3 regionOrigin(
				regionLocation.x * region_size * fSpacing, 0, regionLocation.y * region_size * fSpacing );
		const real_t fExtent = region_size * fSpacing;
		if ( rAabb.position.x + rAabb.size.x < regionOrigin.x || rAabb.position.x > regionOrigin.x + fExtent ||
				rAabb.position.z + rAabb.size.z < regionOrigin.z || rAabb.position.z > regionOrigin.z + fExtent ) {
			continue;
		}
		bool bTemporaryPage = false;
		if ( !t3dBeginInstancePageAccess( this, pData, regionLocation, bTemporaryPage ) ) {
			continue;
		}
		Ref<T3DRegion> region( pRegionPtr );
		bool bBackedUp = false;
		for ( t3dInstanceMesh_t &mesh : region->getInstanceStoreInternal().meshesMut() ) {
			const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( mesh.m_nId );
			const real_t fOffset = pAsset.is_valid() ? pAsset->getHeightOffset() : 0.f;
			bool bMeshChanged = false;
			for ( t3dInstanceCell_t &cell : mesh.m_cells ) {
				bool bCellChanged = false;
				for ( size_t i = 0; i < cell.m_transforms.size(); ++i ) {
					const Vector3 local_origin = cell.m_transforms.getOrigin( i );
					const Vector3 global = local_origin + regionOrigin;
					if ( !rAabb.has_point( global ) ) {
						continue;
					}
					const Vector3 height_offset = cell.m_transforms.getUpAxis( i ) * fOffset;
					const t3dUsableHeight_t sample = getUsableHeight(
							sampler, global - height_offset, Vector2( 0, 90 ), bOnCollision, fRaycastHeight );
					if ( !sample.m_bValid ) {
						continue;
					}
					if ( !bBackedUp ) {
						backupRegion( region );
						bBackedUp = true;
					}
					cell.m_transforms.setOriginY( i, sample.m_fHeight + height_offset.y );
					bCellChanged = true;
					bMeshChanged = true;
				}
				if ( bCellChanged ) {
					cell.touch();
					setLogicalCellCount( regionLocation, mesh.m_nId, cell.m_location, cell.m_transforms.size() );
				}
			}
			if ( bMeshChanged && !bTemporaryPage ) {
				updateMmis( mesh.m_nId, regionLocation );
			}
		}
		t3dEndInstancePageAccess( pData, regionLocation, bTemporaryPage );
	}
}

bool T3DInstancer::buildRepartitionedStore( const T3DRegion *pRegion,
		const t3dInstanceLayout_t &rLayout,
		T3DInstanceStore &rStore ) const {
	if ( !pRegion || pRegion->isDeleted() || !rLayout.isValid() || !pRegion->isInstanceStoreResident() ) {
		return false;
	}
	const T3DInstanceStore &source = pRegion->getInstanceStore();
	T3DInstanceStore rebuilt;
	rebuilt.setLayout( rLayout );
	const int region_size = pRegion->getRegionSize();
	for ( const t3dInstanceMesh_t &mesh : source.meshes() ) {
		for ( const t3dInstanceCell_t &cell : mesh.m_cells ) {
			if ( cell.usesColors() && cell.m_colors.size() != cell.m_transforms.size() ) {
				return false;
			}
			if ( cell.usesOwners() && cell.m_owners.size() != cell.m_transforms.size() ) {
				return false;
			}
			for ( size_t index = 0; index < cell.m_transforms.size(); ++index ) {
				const Transform3D transform = cell.m_transforms[index];
				const Vector2i destination_cell =
						getCellForLocalPosition( transform.origin, region_size, ( int )( rLayout.m_nCellSize ) );
				if ( !rLayout.contains( destination_cell.x, destination_cell.y ) ) {
					return false;
				}
				t3dInstanceCell_t &destination = rebuilt.getOrCreateCell(
						mesh.m_nId, destination_cell, 1, cell.usesColors(), cell.usesOwners() );
				destination.prepareAppend( 1, cell.usesColors(), cell.usesOwners() );
				destination.m_transforms.pushBack( transform );
				if ( !destination.m_colors.empty() ) {
					destination.m_colors.pushBack( cell.usesColors() ? cell.m_colors[index] : s_nInstanceWhiteRgba8 );
				}
				if ( !destination.m_owners.empty() ) {
					destination.m_owners.pushBack( cell.usesOwners() ? cell.m_owners[index] : 0u );
				}
			}
		}
	}
	for ( t3dInstanceMesh_t &mesh : rebuilt.meshesMut() ) {
		for ( t3dInstanceCell_t &cell : mesh.m_cells ) {
			cell.touch();
		}
	}
	rStore = eastl::move( rebuilt );
	return true;
}

bool T3DInstancer::repartitionRegionStore( T3DRegion *pRegion, const t3dInstanceLayout_t &rLayout ) {
	if ( !pRegion || pRegion->isDeleted() || !rLayout.isValid() ) {
		return false;
	}
	if ( !pRegion->ensureInstanceStoreResident() ) {
		return false;
	}
	const T3DInstanceStore &source = pRegion->getInstanceStore();
	if ( source.getLayout().m_nCellSize == rLayout.m_nCellSize &&
			source.getLayout().m_nCellsPerAxis == rLayout.m_nCellsPerAxis ) {
		return true;
	}
	T3DInstanceStore rebuilt;
	if ( !buildRepartitionedStore( pRegion, rLayout, rebuilt ) ) {
		return false;
	}
	pRegion->getInstanceStoreMut() = eastl::move( rebuilt );
	return true;
}

bool T3DInstancer::repartitionResidentRegions( const t3dInstanceLayout_t &rLayout ) {
	if ( !m_pTerrain || !m_pTerrain->getData() || !rLayout.isValid() ) {
		return false;
	}
	typedef struct t3dResidentMigration_s {
		T3DRegion *m_pRegion = nullptr;
		T3DInstanceStore m_store;
	} t3dResidentMigration_t;
	eastl::vector<t3dResidentMigration_t> migrations;
	T3DRegion *const *ppRegionPtrs = m_pTerrain->getData()->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		T3DRegion *const region = ppRegionPtrs[region_index];
		if ( !region || region->isDeleted() || !region->isInstanceStoreResident() ) {
			continue;
		}
		const T3DInstanceStore &source = region->getInstanceStore();
		if ( source.getLayout().m_nCellSize == rLayout.m_nCellSize &&
				source.getLayout().m_nCellsPerAxis == rLayout.m_nCellsPerAxis ) {
			continue;
		}
		t3dResidentMigration_t migration;
		migration.m_pRegion = region;
		if ( !buildRepartitionedStore( region, rLayout, migration.m_store ) ) {
			return false;
		}
		migrations.push_back( eastl::move( migration ) );
	}
	for ( t3dResidentMigration_t &migration : migrations ) {
		migration.m_pRegion->getInstanceStoreMut() = eastl::move( migration.m_store );
	}
	return true;
}

bool T3DInstancer::ensureRegionLayout( T3DRegion *pRegion ) {
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	t3dInstanceLayout_t layout;
	if ( !t3dInstanceLayoutForRegion( pRegion->getRegionSize(), m_nCellSize, layout ) ) {
		return false;
	}
	return repartitionRegionStore( pRegion, layout );
}

bool T3DInstancer::refreshResidentRegionLayouts( void ) {
	if ( !m_pTerrain ) {
		return false;
	}
	t3dInstanceLayout_t layout;
	if ( !t3dInstanceLayoutForRegion( ( int )( m_pTerrain->getRegionSize() ), m_nCellSize, layout ) ) {
		return false;
	}
	return repartitionResidentRegions( layout );
}

void T3DInstancer::setCellSize( const int nCellSize ) {
	const int cell_size = CLAMP( nCellSize, s_nInstanceMinCellSize, s_nInstanceMaxCellSize );
	if ( cell_size != nCellSize || ( cell_size & ( cell_size - 1 ) ) != 0 ) {
		LOG( ERROR,
				"Instance cell size must be a power of two from ",
				s_nInstanceMinCellSize,
				" through ",
				s_nInstanceMaxCellSize );
		return;
	}
	const int region_size = m_pTerrain ? ( int )( m_pTerrain->getRegionSize() ) : 128;
	t3dInstanceLayout_t layout;
	if ( !t3dInstanceLayoutForRegion( region_size, cell_size, layout ) ) {
		LOG( ERROR, "Instance cell size ", cell_size, " must divide region size ", region_size );
		return;
	}
	if ( m_nCellSize == cell_size ) {
		return;
	}
	if ( m_pTerrain && m_pTerrain->getData() && !repartitionResidentRegions( layout ) ) {
		LOG( ERROR, "Could not repartition resident instance pages for cell size ", cell_size );
		return;
	}
	m_nCellSize = cell_size;
	clearSingleLodTransitionBuckets();
	m_bStreamingCacheDirty = true;
	m_lastStreamPosition = m_pTerrain ? m_pTerrain->getClipmapTargetPosition() : s_v3Zero;
	m_bStreamPositionValid = m_pTerrain != nullptr;
	updateMmis( -1, s_v2iMax, true );
}

int T3DInstancer::getCellsPerRegionAxis( void ) const {
	const int region_size = m_pTerrain ? ( int )( m_pTerrain->getRegionSize() ) : 128;
	return region_size > 0 && region_size % m_nCellSize == 0 ? region_size / m_nCellSize : 0;
}

void T3DInstancer::setCellStreamingEnabled( const bool bEnabled ) {
	if ( m_bCellStreamingEnabled == bEnabled ) {
		return;
	}
	m_bCellStreamingEnabled = bEnabled;
	m_bStreamingCacheDirty = true;
	m_lastStreamPosition = m_pTerrain ? m_pTerrain->getClipmapTargetPosition() : s_v3Zero;
	m_bStreamPositionValid = m_pTerrain != nullptr;
	updateMmis( -1, s_v2iMax, false );
}

void T3DInstancer::setSingleLodInstanceBuffers( const bool bEnabled ) {
	if ( m_bSingleLodInstanceBuffers == bEnabled ) {
		return;
	}
	m_bSingleLodInstanceBuffers = bEnabled;
	clearSingleLodTransitionBuckets();
	m_lastStreamPosition = m_pTerrain ? m_pTerrain->getClipmapTargetPosition() : s_v3Zero;
	m_bStreamPositionValid = m_pTerrain != nullptr;
	updateMmis( -1, s_v2iMax, true );
}

void T3DInstancer::setCellStreamingMargin( const real_t fMargin ) {
	const real_t margin = CLAMP( fMargin, 0.0f, 1024.0f );
	if ( Math::is_equal_approx( m_fCellStreamingMargin, margin ) ) {
		return;
	}
	m_fCellStreamingMargin = margin;
	m_bStreamingCacheDirty = true;
	m_lastStreamPosition = m_pTerrain ? m_pTerrain->getClipmapTargetPosition() : s_v3Zero;
	m_bStreamPositionValid = m_pTerrain != nullptr;
	updateMmis( -1, s_v2iMax, false );
}

int T3DInstancer::floorDivCell( const int nValue, const int nDivisor ) {
	const int quotient = nValue / nDivisor;
	return quotient - ( nValue % nDivisor < 0 ? 1 : 0 );
}

t3dCellInterval_t T3DInstancer::streamingRowInterval( const Vector3 &rPosition,
		const real_t fRadius,
		const int iGlobalZ,
		const real_t fCellWorldSize,
		const int nMinGlobalCell,
		const int nMaxGlobalCell ) const {
	const real_t fCenterZ = ( ( real_t )( iGlobalZ ) + 0.5f ) * fCellWorldSize;
	const real_t fDz = fCenterZ - rPosition.z;
	const real_t fRemaining = fRadius * fRadius - fDz * fDz;
	if ( fRemaining < 0.0f ) {
		return t3dCellInterval_t{};
	}
	const real_t fHalfWidth = Math::sqrt( fRemaining );
	const int first = ( int )( Math::ceil( ( rPosition.x - fHalfWidth ) / fCellWorldSize - 0.5f ) );
	const int last = ( int )( Math::floor( ( rPosition.x + fHalfWidth ) / fCellWorldSize - 0.5f ) );
	return t3dCellInterval_t{ MAX( first, nMinGlobalCell ), MIN( last, nMaxGlobalCell ) };
}

void T3DInstancer::queueStreamingCell( T3DData *const pData,
		const int nMeshId,
		const int iGlobalX,
		const int iGlobalZ,
		const int nCellsPerRegion ) {
	const int region_x = floorDivCell( iGlobalX, nCellsPerRegion );
	const int region_z = floorDivCell( iGlobalZ, nCellsPerRegion );
	const Vector2i regionLocation( region_x, region_z );
	T3DRegion *const pRegion = pData->getRegionPtr( regionLocation );
	if ( !pRegion || pRegion->isDeleted() ||
			( pData->isInstanceRegionCacheEnabled() && !pData->isInstanceRegionResident( regionLocation ) ) ) {
		return;
	}
	const Vector2i localCell( iGlobalX - region_x * nCellsPerRegion, iGlobalZ - region_z * nCellsPerRegion );
	if ( pRegion->getInstanceStore().findCell( nMeshId, localCell ) ) {
		m_streamingUpdates.insert( t3dCellUpdateKey_t{ regionLocation, localCell, nMeshId } );
		markPendingRegionWork( regionLocation );
	}
}

void T3DInstancer::queueStreamingInterval( T3DData *const pData,
		const int nMeshId,
		const int iGlobalZ,
		const int nFirst,
		const int nLast,
		const int nCellsPerRegion ) {
	for ( int global_x = nFirst; global_x <= nLast; ++global_x ) {
		queueStreamingCell( pData, nMeshId, global_x, iGlobalZ, nCellsPerRegion );
	}
}

void T3DInstancer::queueStreamingDifference( T3DData *const pData,
		const int nMeshId,
		const int iGlobalZ,
		const t3dCellInterval_t &rSource,
		const t3dCellInterval_t &rOther,
		const int nCellsPerRegion ) {
	if ( !rSource.valid() ) {
		return;
	}
	if ( !rOther.valid() || rSource.m_nLast < rOther.m_nFirst || rSource.m_nFirst > rOther.m_nLast ) {
		queueStreamingInterval( pData, nMeshId, iGlobalZ, rSource.m_nFirst, rSource.m_nLast, nCellsPerRegion );
		return;
	}
	if ( rSource.m_nFirst < rOther.m_nFirst ) {
		queueStreamingInterval( pData,
				nMeshId,
				iGlobalZ,
				rSource.m_nFirst,
				MIN( rSource.m_nLast, rOther.m_nFirst - 1 ),
				nCellsPerRegion );
	}
	if ( rSource.m_nLast > rOther.m_nLast ) {
		queueStreamingInterval( pData,
				nMeshId,
				iGlobalZ,
				MAX( rSource.m_nFirst, rOther.m_nLast + 1 ),
				rSource.m_nLast,
				nCellsPerRegion );
	}
}

void T3DInstancer::updateStreaming( const Vector3 &rTargetPosition ) {
	m_bStreamingSelectionDeferredLastFrame = false;
	if ( !m_pTerrain || !m_pTerrain->getAssets().is_valid() ||
			( !m_bCellStreamingEnabled && !m_bSingleLodInstanceBuffers ) ) {
		return;
	}
	if ( m_bStreamPositionValid &&
			m_lastStreamPosition.distance_squared_to( rTargetPosition ) <
					m_fCellStreamingStep * m_fCellStreamingStep ) {
		return;
	}

	if ( !m_bStreamPositionValid ) {
		m_lastStreamPosition = rTargetPosition;
		m_bStreamPositionValid = true;
		updateMmis( -1, s_v2iMax, false );
		return;
	}
	const Vector3 previous_position = m_lastStreamPosition;
	m_lastStreamPosition = rTargetPosition;
	queueSingleLodTransitions( previous_position, rTargetPosition );
	if ( !m_bCellStreamingEnabled ) {
		if ( !m_streamingUpdates.empty() ) {
			scheduleUpdateProcessing();
		}
		return;
	}

	refreshStreamingCache();
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	if ( m_fMaxStreamingRadius <= 0.0f ) {
		if ( !m_streamingUpdates.empty() ) {
			scheduleUpdateProcessing();
		}
		return;
	}

	T3DData *const pData = m_pTerrain->getData();
	const int region_size = m_pTerrain->getRegionSize();
	const int cells_per_region = region_size / m_nCellSize;
	if ( cells_per_region <= 0 || region_size % m_nCellSize != 0 ) {
		// Region dimensions are expected to align to instance-cell dimensions.
		// Keep the fallback bounded to the existing update model if a custom fork
		// violates that invariant rather than mapping cells to the wrong region.
		updateMmis( -1, s_v2iMax, false );
		return;
	}
	const real_t fCellWorldSize = m_nCellSize * fSpacing;
	const int region_map_size = pData->getRegionMapSizeInternal();
	const int min_global_cell = -( region_map_size / 2 ) * cells_per_region;
	const int max_global_cell = ( region_map_size / 2 ) * cells_per_region - 1;

	for ( int mesh_index = 0; mesh_index < m_nFiniteStreamingMeshCount; ++mesh_index ) {
		const int mesh_id = m_szFiniteStreamingMeshIds[mesh_index];
		const real_t fRadius = m_szMeshStreamingRadius[mesh_id];
		const int old_first_z = MAX(
				( int )( Math::ceil( ( previous_position.z - fRadius ) / fCellWorldSize - 0.5f ) ), min_global_cell );
		const int old_last_z = MIN(
				( int )( Math::floor( ( previous_position.z + fRadius ) / fCellWorldSize - 0.5f ) ), max_global_cell );
		const int new_first_z = MAX(
				( int )( Math::ceil( ( rTargetPosition.z - fRadius ) / fCellWorldSize - 0.5f ) ), min_global_cell );
		const int new_last_z = MIN(
				( int )( Math::floor( ( rTargetPosition.z + fRadius ) / fCellWorldSize - 0.5f ) ), max_global_cell );
		const int first_z = MIN( old_first_z, new_first_z );
		const int last_z = MAX( old_last_z, new_last_z );
		for ( int global_z = first_z; global_z <= last_z; ++global_z ) {
			const t3dCellInterval_t old_interval = global_z >= old_first_z && global_z <= old_last_z
					? streamingRowInterval(
							  previous_position, fRadius, global_z, fCellWorldSize, min_global_cell, max_global_cell )
					: t3dCellInterval_t{};
			const t3dCellInterval_t new_interval = global_z >= new_first_z && global_z <= new_last_z
					? streamingRowInterval(
							  rTargetPosition, fRadius, global_z, fCellWorldSize, min_global_cell, max_global_cell )
					: t3dCellInterval_t{};
			queueStreamingDifference( pData, mesh_id, global_z, old_interval, new_interval, cells_per_region );
			queueStreamingDifference( pData, mesh_id, global_z, new_interval, old_interval, cells_per_region );
		}
	}
	if ( !m_streamingUpdates.empty() ) {
		scheduleUpdateProcessing();
	}
}

int T3DInstancer::getClosestMeshId( const Vector3 &rGlobalPosition ) const {
	IS_DATA_INIT( -1 );
	T3DData *const pData = m_pTerrain->getData();
	const Vector2i regionLocation = pData->getRegionLocation( rGlobalPosition );
	T3DRegion *const pRegion = pData->getRegionPtr( regionLocation );
	if ( !pRegion || pRegion->isDeleted() || !pRegion->hasInstances() ) {
		return -1;
	}
	bool bTemporaryPage = false;
	if ( !t3dBeginInstancePageAccess( const_cast<T3DInstancer *>( this ), pData, regionLocation, bTemporaryPage ) ) {
		return -1;
	}
	const real_t fSpacing = m_pTerrain->getVertexSpacing();
	const int region_size = pRegion->getRegionSize();
	const Vector3 local = rGlobalPosition -
			Vector3( regionLocation.x * region_size * fSpacing, 0, regionLocation.y * region_size * fSpacing );
	const Vector2i center_cell = getCell( local, region_size );
	real_t fClosest = FLT_MAX;
	int closest_id = -1;
	const T3DInstanceStore &store = pRegion->getInstanceStore();
	for ( const t3dInstanceMesh_t &mesh : store.meshes() ) {
		for ( int y = center_cell.y - 1; y <= center_cell.y + 1; ++y ) {
			for ( int x = center_cell.x - 1; x <= center_cell.x + 1; ++x ) {
				const t3dInstanceCell_t *pCell = store.findCell( mesh.m_nId, Vector2i( x, y ) );
				if ( !pCell ) {
					continue;
				}
				for ( size_t i = 0; i < pCell->m_transforms.size(); ++i ) {
					const Transform3D transform = pCell->m_transforms[i];
					const real_t fDistance = transform.origin.distance_squared_to( local );
					if ( fDistance < fClosest ) {
						fClosest = fDistance;
						closest_id = mesh.m_nId;
					}
				}
			}
		}
	}
	t3dEndInstancePageAccess( pData, regionLocation, bTemporaryPage );
	return closest_id;
}

void T3DInstancer::copyPasteDfr( const T3DRegion *pSrcRegion, const Rect2i &rSrcRect, const T3DRegion *pDstRegion ) {
	if ( !pSrcRegion || !pDstRegion ) {
		LOG( ERROR, "Source (", pSrcRegion, ") or destination (", pDstRegion, ") regions are null" );
		return;
	}
	T3DData *const pData = m_pTerrain->getData();
	const Vector2i src_region_loc = pSrcRegion->getLocation();
	bool bTemporarySource = false;
	if ( !t3dBeginInstancePageAccess( this, pData, src_region_loc, bTemporarySource ) ) {
		return;
	}
	LOG( INFO, "Copying foliage data from src ", src_region_loc, " to dest ", pDstRegion->getLocation() );

	const real_t fVertexSpacing = m_pTerrain->getVertexSpacing();
	const int src_region_size = pSrcRegion->getRegionSize();
	const Vector2i srcOffset( src_region_loc.x * src_region_size, src_region_loc.y * src_region_size );
	const Vector2i dst_region_loc = pDstRegion->getLocation();
	const int dst_region_size = pDstRegion->getRegionSize();
	const Vector2i dst_offset =
			srcOffset - Vector2i( dst_region_loc.x * dst_region_size, dst_region_loc.y * dst_region_size );
	const Vector3 dst_translate = Vector3( dst_offset.x, 0.f, dst_offset.y ) * fVertexSpacing;
	const Vector2i cell_start = V2I_DIVIDE_FLOOR( rSrcRect.get_position(), m_nCellSize );
	const Vector2i cell_end = V2I_DIVIDE_CEIL( rSrcRect.get_end(), m_nCellSize );
	const bool bCellAligned = rSrcRect.position.x % m_nCellSize == 0 && rSrcRect.position.y % m_nCellSize == 0 &&
			rSrcRect.size.x % m_nCellSize == 0 && rSrcRect.size.y % m_nCellSize == 0;

	T3DRegion *const dst_region = const_cast<T3DRegion *>( pDstRegion );
	const T3DInstanceStore &src_store = pSrcRegion->getInstanceStore();
	bool bBackedUp = false;
	for ( const t3dInstanceMesh_t &mesh : src_store.meshes() ) {
		m_placementTransforms.clear();
		m_placementColors.clear();
		for ( int y = cell_start.y; y < cell_end.y; ++y ) {
			for ( int x = cell_start.x; x < cell_end.x; ++x ) {
				const t3dInstanceCell_t *pCell = src_store.findCell( mesh.m_nId, Vector2i( x, y ) );
				if ( !pCell ) {
					continue;
				}
				m_placementTransforms.reserve( m_placementTransforms.size() + pCell->m_transforms.size() );
				m_placementColors.reserve( m_placementColors.size() + pCell->m_transforms.size() );
				for ( size_t i = 0; i < pCell->m_transforms.size(); ++i ) {
					Transform3D transform = pCell->m_transforms[i];
					if ( !bCellAligned ) {
						const Vector2i sourcePosition( Math::floor( transform.origin.x / fVertexSpacing ),
								Math::floor( transform.origin.z / fVertexSpacing ) );
						if ( !rSrcRect.has_point( sourcePosition ) ) {
							continue;
						}
					}
					transform.origin += dst_translate;
					m_placementTransforms.push_back( transform );
					m_placementColors.push_back( pCell->getColor( i ) );
				}
			}
		}
		if ( m_placementTransforms.empty() ) {
			continue;
		}
		if ( !bBackedUp ) {
			backupRegion( Ref<T3DRegion>( dst_region ) );
			bBackedUp = true;
		}
		appendRegionNative( dst_region, mesh.m_nId, m_placementTransforms, m_placementColors, false, false, false );
	}
	t3dEndInstancePageAccess( pData, src_region_loc, bTemporarySource );
	if ( bBackedUp &&
			( !pData->isInstanceRegionCacheEnabled() || pData->isInstanceRegionResident( dst_region_loc ) ) ) {
		updateMmis( -1, dst_region_loc, false );
	}
}

// Changes the ID of a mesh, without changing the mesh on the ground
// Called when the mesh asset id has changed. Updates Multimeshes and MMIs dictionary keys
void T3DInstancer::swapIds( const int nSrcId, const int nDstId ) {
	IS_DATA_INIT_MESG( "Instancer isn't initialized.", VOID );
	if ( nSrcId == nDstId ) {
		return;
	}
	T3DData *const pData = m_pTerrain->getData();
	T3DRegion *const *ppRegionPtrs = pData->getRegionPtrMapInternal();
	for ( int region_index = 0; region_index < s_nMaxRegionSlots; ++region_index ) {
		T3DRegion *const pRegionPtr = ppRegionPtrs[region_index];
		if ( !pRegionPtr || pRegionPtr->isDeleted() ||
				( pRegionPtr->getInstanceCount( nSrcId ) == 0u && pRegionPtr->getInstanceCount( nDstId ) == 0u ) ) {
			continue;
		}
		const Vector2i location = pRegionPtr->getLocation();
		bool bTemporaryPage = false;
		if ( !t3dBeginInstancePageAccess( this, pData, location, bTemporaryPage ) ) {
			continue;
		}
		Ref<T3DRegion> region( pRegionPtr );
		const bool bHasSrc = region->getInstanceStore().hasMesh( nSrcId );
		const bool bHasDst = region->getInstanceStore().hasMesh( nDstId );
		if ( bHasSrc || bHasDst ) {
			backupRegion( region );
			region->getInstanceStoreMut().swapMeshIds( nSrcId, nDstId );
		}
		t3dEndInstancePageAccess( pData, location, bTemporaryPage );
	}
	swapLogicalMeshIds( nSrcId, nDstId );
	updateMmis( -1, s_v2iMax, true );
}

void T3DInstancer::updateMmis( const int nMeshId, const Vector2i &rRegionLoc, const bool bRebuild ) {
	const int mesh_id = CLAMP( nMeshId, -1, s_nMaxMeshAssets - 1 );
	const bool bRendererSuspended = m_mode == INSTANCER_DISABLED || !m_pTerrain || !m_pTerrain->is_visible_in_tree();
	if ( bRendererSuspended ) {
		if ( rRegionLoc == s_v2iMax ) {
			if ( mesh_id < 0 ) {
				rebuildLogicalCountsFromStore();
			} else {
				publishLogicalMeshCount( mesh_id );
			}
		} else {
			T3DRegion *const pRegion = m_pTerrain->getData()->getRegionPtr( rRegionLoc );
			if ( pRegion && !pRegion->isDeleted() ) {
				reconcileLogicalRegion( pRegion, mesh_id );
			} else if ( mesh_id >= 0 ) {
				eraseLogicalCellsByLocation( rRegionLoc, mesh_id );
			} else {
				eraseLogicalCellsByRegion( rRegionLoc );
			}
		}
		return;
	}
	if ( bRebuild ) {
		cancelPendingCellUpdates();
		clearSingleLodTransitionBuckets();
		m_bStreamingCacheDirty = true;
		m_bRebuildAll = true;
		m_bUpdateAll = false;
		m_globalMeshUpdates.reset();
		clearRegionUpdates( false );
		m_streamingUpdates.clear();
		rebuildPendingRegionWorkMask();
		for ( uint32_t &revision : m_szMeshSettingsRevision ) {
			if ( ++revision == 0 ) {
				revision = 1;
			}
		}
	} else if ( rRegionLoc == s_v2iMax && mesh_id < 0 ) {
		if ( !m_bRebuildAll ) {
			cancelPendingCellUpdates();
			m_bUpdateAll = true;
			m_globalMeshUpdates.reset();
			clearRegionUpdates( false );
			m_streamingUpdates.clear();
			rebuildPendingRegionWorkMask();
		}
	} else if ( rRegionLoc == s_v2iMax ) {
		m_bStreamingCacheDirty = true;
		if ( !m_bRebuildAll && !m_bUpdateAll ) {
			m_globalMeshUpdates.set( mesh_id );
			if ( ++m_szMeshSettingsRevision[mesh_id] == 0 ) {
				m_szMeshSettingsRevision[mesh_id] = 1;
			}
		}
	} else if ( !m_bRebuildAll && !m_bUpdateAll ) {
		t3dRegionUpdateState_t *pState = getRegionUpdate( rRegionLoc );
		if ( !pState ) {
			return;
		}
		if ( mesh_id < 0 ) {
			pState->m_bAllMeshes = true;
			pState->m_meshes.reset();
		} else if ( !pState->m_bAllMeshes && !m_globalMeshUpdates.test( mesh_id ) ) {
			pState->m_meshes.set( mesh_id );
		}
	}
	scheduleUpdateProcessing();
}

// -----------------------------------------------------------------------------
// Protected functions

size_t T3DInstancer::getRetainedContainerBytes( void ) const {
	size_t bytes = m_renderCells.capacity() * sizeof( t3dRenderCell_t );
	bytes += m_pendingCellUpdates.capacity() * sizeof( t3dCellUpdateKey_t );
	bytes += m_buildingCellUpdates.capacity() * sizeof( t3dCellUpdateKey_t );
	bytes += m_placementTransforms.capacity() * sizeof( Transform3D );
	bytes += m_placementColors.capacity() * sizeof( Color );
	bytes += m_placementPending.capacity() * sizeof( t3dPendingPlacement_t );
	bytes += m_iRenderCellIndex.reservedBytes();
	bytes += m_logicalCellCounts.reservedBytes();
	bytes += m_singleLodTransitionBuckets.reservedBytes();
	bytes += m_streamingUpdates.reservedBytes();
	bytes += m_activeStreamingUpdates.reservedBytes();
	bytes += m_renderRegionNodes.capacity() * sizeof( t3dRenderRegionNode_t );
	bytes += m_logicalRegionNodes.capacity() * sizeof( t3dLogicalRegionNode_t );
	bytes += m_transitionCellNodes.capacity() * sizeof( t3dTransitionCellNode_t );
	return bytes;
}

size_t T3DInstancer::getActiveMultimeshBytes( void ) const {
	size_t bytes = 0u;
	for ( const t3dRenderCell_t &cell : m_renderCells ) {
		bytes += ( size_t )( MAX( cell.m_nUploadedCount, 0 ) ) * ( size_t )( cell.m_bUsesColors ? 16 : 12 ) *
				sizeof( float );
	}
	return bytes;
}

Dictionary T3DInstancer::debugValidateSingleLodTransitionCandidates( const Vector3 &rPreviousPosition,
		const Vector3 &rTargetPosition ) const {
	Dictionary result;
	uint64_t reference_cells = 0u;
	uint64_t reference_transitions = 0u;
	uint64_t missed_transitions = 0u;
	uint64_t candidate_cells = 0u;
	if ( !m_bSingleLodInstanceBuffers || !m_pTerrain || !m_pTerrain->getAssets().is_valid() ) {
		result["reference_cells"] = ( int64_t )( reference_cells );
		result["reference_transitions"] = ( int64_t )( reference_transitions );
		result["missed_transitions"] = ( int64_t )( missed_transitions );
		result["candidate_cells"] = ( int64_t )( candidate_cells );
		return result;
	}
	m_singleLodTransitionBuckets.forEach(
			[&]( const t3dTransitionBucketKey_t &bucket_key, const t3dTransitionBucketState_t &rState ) {
				const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( bucket_key.m_nMeshId );
				if ( transitionBucketMayCrossBoundary( bucket_key, pAsset, rPreviousPosition, rTargetPosition ) ) {
					candidate_cells += rState.m_nCount;
				}
			} );
	for ( const t3dRenderCell_t &render : m_renderCells ) {
		if ( render.m_nLod < 0 || render.m_nMeshId < 0 || render.m_nMeshId >= s_nMaxMeshAssets ) {
			continue;
		}
		++reference_cells;
		const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( render.m_nMeshId );
		const int previous_lod = selectCellLod( render.m_region, render.m_cell, pAsset, rPreviousPosition );
		const int selected_lod = selectCellLod( render.m_region, render.m_cell, pAsset, rTargetPosition );
		if ( selected_lod == previous_lod ) {
			continue;
		}
		++reference_transitions;
		const t3dCellUpdateKey_t key{ render.m_region, render.m_cell, render.m_nMeshId };
		const t3dTransitionBucketKey_t bucket_key = getTransitionBucketKey( key );
		const t3dTransitionBucketState_t *const pBucket = m_singleLodTransitionBuckets.find( bucket_key );
		if ( !pBucket || !transitionBucketMayCrossBoundary( bucket_key, pAsset, rPreviousPosition, rTargetPosition ) ) {
			++missed_transitions;
		}
	}
	result["reference_cells"] = ( int64_t )( reference_cells );
	result["reference_transitions"] = ( int64_t )( reference_transitions );
	result["missed_transitions"] = ( int64_t )( missed_transitions );
	result["candidate_cells"] = ( int64_t )( candidate_cells );
	return result;
}

Dictionary T3DInstancer::debugValidateSingleLodState( const Vector3 &rTargetPosition ) const {
	Dictionary result;
	uint64_t cells = 0u;
	uint64_t mismatches = 0u;
	if ( !m_bSingleLodInstanceBuffers || !m_pTerrain || !m_pTerrain->getAssets().is_valid() ) {
		result["cells"] = ( int64_t )( cells );
		result["mismatches"] = ( int64_t )( mismatches );
		return result;
	}
	for ( const t3dRenderCell_t &render : m_renderCells ) {
		if ( render.m_nLod < 0 || render.m_nMeshId < 0 || render.m_nMeshId >= s_nMaxMeshAssets ) {
			continue;
		}
		++cells;
		const Ref<T3DMeshAsset> pAsset = m_pTerrain->getAssets()->getMeshAsset( render.m_nMeshId );
		if ( selectCellLod( render.m_region, render.m_cell, pAsset, rTargetPosition ) != render.m_nLod ) {
			++mismatches;
		}
	}
	result["cells"] = ( int64_t )( cells );
	result["mismatches"] = ( int64_t )( mismatches );
	return result;
}

Dictionary T3DInstancer::getStats( void ) const {
	Dictionary stats;
	uint64_t uploaded_instances = 0u;
	uint64_t active_multimesh_bytes = 0u;
	int lod0_cells = 0;
	int lod1_cells = 0;
	int higher_lod_cells = 0;
	for ( const t3dRenderCell_t &cell : m_renderCells ) {
		uploaded_instances += ( uint64_t )( MAX( cell.m_nUploadedCount, 0 ) );
		active_multimesh_bytes += ( uint64_t )( MAX( cell.m_nUploadedCount, 0 ) ) *
				( uint64_t )( cell.m_bUsesColors ? 16 : 12 ) * sizeof( float );
		if ( cell.m_nLod <= 0 ) {
			++lod0_cells;
		} else if ( cell.m_nLod == 1 ) {
			++lod1_cells;
		} else {
			++higher_lod_cells;
		}
	}
	uint64_t logical_instances = 0u;
	for ( int mesh_id = 0; mesh_id < s_nMaxMeshAssets; ++mesh_id ) {
		logical_instances += m_szLogicalMeshInstanceTotals[mesh_id];
	}
	const size_t materialized_pending = m_pendingCellUpdates.size() > m_iPendingCellUpdateCursor
			? m_pendingCellUpdates.size() - m_iPendingCellUpdateCursor
			: 0u;
	stats["enabled"] = isEnabled();
	stats["mode"] = ( int )( m_mode );
	stats["render_cells"] = ( int )( m_renderCells.size() );
	stats["render_cells_lod0"] = lod0_cells;
	stats["render_cells_lod1"] = lod1_cells;
	stats["render_cells_higher_lod"] = higher_lod_cells;
	stats["uploaded_instances"] = ( int64_t )( uploaded_instances );
	stats["active_multimesh_bytes"] = ( int64_t )( active_multimesh_bytes );
	stats["logical_cells"] = ( int64_t )( m_logicalCellCounts.size() );
	stats["logical_instances"] = ( int64_t )( logical_instances );
	stats["region_updates"] = m_nRegionUpdateCount;
	stats["active_region_updates"] = m_nActiveRegionUpdateCount;
	stats["streaming_updates"] = ( int64_t )( m_streamingUpdates.size() );
	stats["active_streaming_updates"] = ( int64_t )( m_activeStreamingUpdates.size() );
	stats["materialized_pending_updates"] = ( int64_t )( materialized_pending );
	stats["pending_updates_total"] = getPendingStreamingUpdateCount();
	stats["queue_before_last_frame"] = ( int64_t )( m_nQueueBeforeLastFrame );
	stats["queue_after_last_frame"] = ( int64_t )( m_nQueueAfterLastFrame );
	stats["processed_cell_updates_last_frame"] = ( int64_t )( m_nProcessedCellUpdatesLastFrame );
	stats["skipped_cell_updates_last_frame"] = ( int64_t )( m_nSkippedCellUpdatesLastFrame );
	stats["multimesh_uploads_last_frame"] = ( int64_t )( m_nMultimeshUploadsLastFrame );
	stats["multimesh_upload_bytes_last_frame"] = ( int64_t )( m_nMultimeshUploadBytesLastFrame );
	stats["instance_records_uploaded_last_frame"] = ( int64_t )( m_nInstanceRecordsUploadedLastFrame );
	stats["new_render_instances_last_frame"] = ( int64_t )( m_nNewRenderInstancesLastFrame );
	stats["materialize_usec_last_frame"] = ( int64_t )( m_nMaterializeUsecLastFrame );
	stats["cell_update_usec_last_frame"] = ( int64_t )( m_nCellUpdateUsecLastFrame );
	const int effective_update_budget = getEffectiveUpdateCellBudget();
	stats["update_budget_saturated_last_frame"] = effective_update_budget > 0 &&
			m_nProcessedCellUpdatesLastFrame >= ( uint32_t )( effective_update_budget ) && m_nQueueAfterLastFrame > 0u;
	stats["materializing_updates"] = m_bMaterializingUpdates;
	stats["processing_pending_updates"] = m_bProcessingPendingUpdates;
	stats["cancel_requested"] = m_bPendingCancelRequested;
	stats["global_mesh_updates"] = ( int64_t )( m_globalMeshUpdates.count() );
	stats["update_all"] = m_bUpdateAll;
	stats["rebuild_all"] = m_bRebuildAll;
	stats["finite_streaming_meshes"] = m_nFiniteStreamingMeshCount;
	stats["maximum_streaming_radius"] = m_fMaxStreamingRadius;
	stats["cell_size"] = m_nCellSize;
	stats["cells_per_region_axis"] = getCellsPerRegionAxis();
	stats["cell_streaming_enabled"] = m_bCellStreamingEnabled;
	stats["single_lod_buffers"] = m_bSingleLodInstanceBuffers;
	stats["streaming_margin"] = m_fCellStreamingMargin;
	stats["streaming_update_distance"] = m_fCellStreamingStep;
	stats["update_cell_budget"] = m_nUpdateCellBudget;
	stats["effective_update_cell_budget"] = effective_update_budget;
	stats["stream_position_valid"] = m_bStreamPositionValid;
	stats["stream_position"] = m_lastStreamPosition;
	stats["streaming_selection_deferred_last_frame"] = m_bStreamingSelectionDeferredLastFrame;
	stats["transition_cells_examined"] = ( int64_t )( m_nTransitionCellsExaminedLastScan );
	stats["transition_lod_evaluations"] = ( int64_t )( m_nTransitionLodEvaluationsLastScan );
	stats["transitions_queued"] = ( int64_t )( m_nTransitionsQueuedLastScan );
	stats["transition_scan_cpu_usec"] = ( int64_t )( m_nTransitionScanUsecLastScan );
	stats["transition_full_rescan_fallback_count"] = ( int64_t )( m_nTransitionFullRescanFallbackCount );
	stats["transition_buckets_examined"] = ( int64_t )( m_nTransitionBucketsExaminedLastScan );
	stats["transition_candidate_buckets"] = ( int64_t )( m_nTransitionCandidateBucketsLastScan );
	stats["transition_bucket_count"] = ( int64_t )( m_singleLodTransitionBuckets.size() );
	return stats;
}

void T3DInstancer::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( NORMAL );
	// BIND_ENUM_CONSTANT(PLACEHOLDER);
	BIND_ENUM_CONSTANT( INSTANCER_DISABLED );

	ClassDB::bind_method( D_METHOD( "clear_by_mesh", "mesh_id" ), &T3DInstancer::clearByMesh );
	ClassDB::bind_method(
			D_METHOD( "clear_by_location", "region_location", "mesh_id" ), &T3DInstancer::clearByLocation );
	ClassDB::bind_method( D_METHOD( "clear_by_region", "region", "mesh_id" ), &T3DInstancer::clearByRegion );
	ClassDB::bind_method( D_METHOD( "set_mode", "mode" ), &T3DInstancer::setMode );
	ClassDB::bind_method( D_METHOD( "get_mode" ), &T3DInstancer::getMode );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &T3DInstancer::isEnabled );
	ClassDB::bind_method( D_METHOD( "add_instances", "global_position", "params" ), &T3DInstancer::addInstances );
	ClassDB::bind_method( D_METHOD( "remove_instances", "global_position", "params" ), &T3DInstancer::removeInstances );
	ClassDB::bind_method( D_METHOD( "add_multimesh", "mesh_id", "multimesh", "transform", "update" ),
			&T3DInstancer::addMultimesh,
			DEFVAL( Transform3D() ),
			DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_transforms", "mesh_id", "transforms", "colors", "update" ),
			&T3DInstancer::addTransforms,
			DEFVAL( PackedColorArray() ),
			DEFVAL( true ) );
	ClassDB::bind_method(
			D_METHOD( "add_owned_transforms", "mesh_id", "transforms", "owner_id", "colors", "update", "backup" ),
			&T3DInstancer::addOwnedTransforms,
			DEFVAL( PackedColorArray() ),
			DEFVAL( true ),
			DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_owned_instances", "owner_id", "update", "backup" ),
			&T3DInstancer::removeOwnedInstances,
			DEFVAL( true ),
			DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "append_location", "region_location", "mesh_id", "transforms", "colors", "update" ),
			&T3DInstancer::appendLocation,
			DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "append_region", "region", "mesh_id", "transforms", "colors", "update" ),
			&T3DInstancer::appendRegion,
			DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "update_transforms", "aabb" ), &T3DInstancer::updateTransforms );
	ClassDB::bind_method( D_METHOD( "get_closest_mesh_id", "global_position" ), &T3DInstancer::getClosestMeshId );
	ClassDB::bind_method( D_METHOD( "update_mmis", "mesh_id", "region_location", "rebuild_all" ),
			&T3DInstancer::updateMmis,
			DEFVAL( -1 ),
			DEFVAL( s_v2iMax ),
			DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "swap_ids", "src_id", "dest_id" ), &T3DInstancer::swapIds );
	ClassDB::bind_method( D_METHOD( "build_multimesh_buffer", "transforms", "colors" ),
			&T3DInstancer::buildMultimeshBuffer,
			DEFVAL( PackedColorArray() ) );
	ClassDB::bind_method( D_METHOD( "set_cell_size", "cell_size" ), &T3DInstancer::setCellSize );
	ClassDB::bind_method( D_METHOD( "get_cell_size" ), &T3DInstancer::getCellSize );
	ClassDB::bind_method( D_METHOD( "get_cells_per_region_axis" ), &T3DInstancer::getCellsPerRegionAxis );
	ClassDB::bind_method( D_METHOD( "set_cell_streaming_enabled", "enabled" ), &T3DInstancer::setCellStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "is_cell_streaming_enabled" ), &T3DInstancer::isCellStreamingEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_single_lod_instance_buffers", "enabled" ), &T3DInstancer::setSingleLodInstanceBuffers );
	ClassDB::bind_method( D_METHOD( "is_single_lod_instance_buffers" ), &T3DInstancer::isSingleLodInstanceBuffers );
	ClassDB::bind_method( D_METHOD( "set_cell_streaming_margin", "margin" ), &T3DInstancer::setCellStreamingMargin );
	ClassDB::bind_method( D_METHOD( "get_cell_streaming_margin" ), &T3DInstancer::getCellStreamingMargin );
	ClassDB::bind_method( D_METHOD( "set_cell_streaming_step", "distance" ), &T3DInstancer::setCellStreamingStep );
	ClassDB::bind_method( D_METHOD( "get_cell_streaming_step" ), &T3DInstancer::getCellStreamingStep );
	ClassDB::bind_method( D_METHOD( "set_update_cell_budget", "cells_per_frame" ), &T3DInstancer::setUpdateCellBudget );
	ClassDB::bind_method( D_METHOD( "get_update_cell_budget" ), &T3DInstancer::getUpdateCellBudget );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &T3DInstancer::getStats );
	ClassDB::bind_method(
			D_METHOD( "debug_validate_single_lod_transition_candidates", "previous_position", "target_position" ),
			&T3DInstancer::debugValidateSingleLodTransitionCandidates );
	ClassDB::bind_method( D_METHOD( "debug_validate_single_lod_state", "target_position" ),
			&T3DInstancer::debugValidateSingleLodState );

	ADD_GROUP( "Streaming Policy", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mode", PROPERTY_HINT_ENUM, "Disabled,Normal" ), "set_mode", "get_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cell_size", PROPERTY_HINT_ENUM, "8,16,32,64,128,suffix: samples" ),
			"set_cell_size",
			"get_cell_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "cell_streaming_enabled" ),
			"set_cell_streaming_enabled",
			"is_cell_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cell_streaming_margin", PROPERTY_HINT_RANGE, "0,1024,1,suffix:m" ),
			"set_cell_streaming_margin",
			"get_cell_streaming_margin" );
	ADD_PROPERTY(
			PropertyInfo( Variant::FLOAT, "cell_streaming_update_distance", PROPERTY_HINT_RANGE, "1,256,1,suffix:m" ),
			"set_cell_streaming_step",
			"get_cell_streaming_step" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "update_cell_budget",
						  PROPERTY_HINT_RANGE,
						  "0,65536,1,or_greater,suffix: cells/frame" ),
			"set_update_cell_budget",
			"get_update_cell_budget" );

	ADD_GROUP( "LOD / Buffer Policy", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "single_lod_instance_buffers" ),
			"set_single_lod_instance_buffers",
			"is_single_lod_instance_buffers" );

	ADD_GROUP( "Runtime Status", "" );
	// Runtime counters remain available through getStats(); status fields are read-only.
}
