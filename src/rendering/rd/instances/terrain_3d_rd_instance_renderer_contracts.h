// Instance publication and draw-packet contracts shared by the renderer domains.
#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_CONTRACTS_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_CONTRACTS_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/containers/terrain_3d_fixed_buffer.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"

#include <godot_cpp/variant/aabb.hpp>
#include <stdint.h>
#include <string.h>

// t3dRdInstanceDirtyRange_s centralized in Terrain3DTypes.h.


// t3dRdInstanceDescriptorSpan_s centralized in Terrain3DTypes.h.


typedef struct t3dRdInstanceSceneSnapshot_s {

	uint64_t m_nGeneration = 0u;
	Terrain3DFixedBuffer<t3dRdInstanceRecord_t> m_instances;
	Terrain3DFixedBuffer<t3dRdInstanceCell_t> m_cells;
	Terrain3DFixedBuffer<t3dRdInstanceDirtyRange_t> m_dirtyRanges;
	Terrain3DFixedBuffer<uint32_t> m_dirtyCells;
	bool m_bInstanceLayoutChanged = true;
	bool m_bTopologyChanged = true;

	EA_FORCE_INLINE bool bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
		return m_instances.bindAllocatorZone( pZone, ePolicy ) && m_cells.bindAllocatorZone( pZone, ePolicy )
		        && m_dirtyRanges.bindAllocatorZone( pZone, ePolicy ) && m_dirtyCells.bindAllocatorZone( pZone, ePolicy );
	}
	EA_FORCE_INLINE bool reserveCold( const size_t nInstanceCapacity, const size_t nCellCapacity ) {
		return m_instances.reserveCold( nInstanceCapacity ) && m_cells.reserveCold( nCellCapacity )
		        && m_dirtyRanges.reserveCold( nCellCapacity ) && m_dirtyCells.reserveCold( nCellCapacity );
	}
	EA_FORCE_INLINE void reset( void ) {
		m_nGeneration = 0u;
		m_instances.reset();
		m_cells.reset();
		m_dirtyRanges.reset();
		m_dirtyCells.reset();
		m_bInstanceLayoutChanged = true;
		m_bTopologyChanged = true;
	}
	EA_FORCE_INLINE void shutdown( void ) {
		m_nGeneration = 0u;
		m_instances.shutdown();
		m_cells.shutdown();
		m_dirtyRanges.shutdown();
		m_dirtyCells.shutdown();
		m_bInstanceLayoutChanged = true;
		m_bTopologyChanged = true;
	}
	bool copyFrom( const t3dRdInstanceSceneSnapshot_s &rOther ) {
		if ( !m_instances.copyFrom( rOther.m_instances.data(), rOther.m_instances.size() )
		        || !m_cells.copyFrom( rOther.m_cells.data(), rOther.m_cells.size() )
		        || !m_dirtyRanges.copyFrom( rOther.m_dirtyRanges.data(), rOther.m_dirtyRanges.size() )
		        || !m_dirtyCells.copyFrom( rOther.m_dirtyCells.data(), rOther.m_dirtyCells.size() ) ) {
			return false;
		}
		m_nGeneration = rOther.m_nGeneration;
		m_bInstanceLayoutChanged = rOther.m_bInstanceLayoutChanged;
		m_bTopologyChanged = rOther.m_bTopologyChanged;
		return true;
	}
	bool copyIncrementalFrom( const t3dRdInstanceSceneSnapshot_s &rOther ) {
		bool layout_compatible = !rOther.m_bInstanceLayoutChanged && m_instances.size() == rOther.m_instances.size()
		        && m_cells.size() == rOther.m_cells.size();
		if ( layout_compatible ) {
			for ( size_t iCell = 0u; iCell < m_cells.size(); ++iCell ) {
				const t3dRdInstanceCell_t &current = m_cells[iCell];
				const t3dRdInstanceCell_t &incoming = rOther.m_cells[iCell];
				if ( current.m_nRegionX != incoming.m_nRegionX || current.m_nRegionZ != incoming.m_nRegionZ
				        || current.m_nCellX != incoming.m_nCellX || current.m_nCellZ != incoming.m_nCellZ
				        || current.m_nMeshAssetId != incoming.m_nMeshAssetId || current.m_nFirstInstance != incoming.m_nFirstInstance
				        || current.m_nInstanceCount != incoming.m_nInstanceCount ) {
					layout_compatible = false;
					break;
				}
			}
		}
		if ( !layout_compatible ) {
			return copyFrom( rOther );
		}
		if ( !m_instances.resizeUninitialized( rOther.m_instances.size() ) ) {
			return false;
		}
		for ( size_t iCell = 0u; iCell < m_cells.size(); ++iCell ) {
			const t3dRdInstanceCell_t &current = m_cells[iCell];
			const t3dRdInstanceCell_t &incoming = rOther.m_cells[iCell];
			if ( current.m_nRevision == incoming.m_nRevision || incoming.m_nInstanceCount == 0u ) {
				continue;
			}
			memcpy( m_instances.data() + incoming.m_nFirstInstance,
			        rOther.m_instances.data() + incoming.m_nFirstInstance,
			        ( size_t )( incoming.m_nInstanceCount ) * sizeof( t3dRdInstanceRecord_t ) );
		}
		if ( !m_cells.copyFrom( rOther.m_cells.data(), rOther.m_cells.size() )
		        || !m_dirtyRanges.copyFrom( rOther.m_dirtyRanges.data(), rOther.m_dirtyRanges.size() )
		        || !m_dirtyCells.copyFrom( rOther.m_dirtyCells.data(), rOther.m_dirtyCells.size() ) ) {
			return false;
		}
		m_nGeneration = rOther.m_nGeneration;
		m_bInstanceLayoutChanged = false;
		m_bTopologyChanged = rOther.m_bTopologyChanged;
		return true;
	}
} t3dRdInstanceSceneSnapshot_t;


// t3dRdInstanceDrawPacket_s centralized in Terrain3DTypes.h.


typedef struct t3dRdPacketKey_s {

	uint32_t m_nSurfaceSlot = UINT32_MAX;
	uint32_t m_nFlags = 0u;
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nUnique = 0u;

	EA_FORCE_INLINE bool operator==( const t3dRdPacketKey_s &rOther ) const {
		return m_nSurfaceSlot == rOther.m_nSurfaceSlot && m_nFlags == rOther.m_nFlags && m_nMaterial == rOther.m_nMaterial
		        && m_nOverlayMaterial == rOther.m_nOverlayMaterial && m_nVisibilityLayers == rOther.m_nVisibilityLayers
		        && m_nUnique == rOther.m_nUnique;
	}
} t3dRdPacketKey_t;


typedef struct t3dRdPacketKeyHash_s {

	EA_FORCE_INLINE size_t operator()( const t3dRdPacketKey_t &rKey ) const {
		uint64_t hash = 1469598103934665603ull;
		const uint32_t values[6] = {
			rKey.m_nSurfaceSlot, rKey.m_nFlags, rKey.m_nMaterial, rKey.m_nOverlayMaterial, rKey.m_nVisibilityLayers, rKey.m_nUnique
		};
		for ( size_t iValue = 0u; iValue < T3D_ARRAY_COUNT( values ); iValue++ ) {
			const uint32_t value = ( values )[iValue];
			hash ^= value;
			hash *= 1099511628211ull;
		}
		return ( size_t )( hash );
	}
} t3dRdPacketKeyHash_t;


typedef struct t3dRdPacketKeyLess_s {

	EA_FORCE_INLINE bool operator()( const t3dRdPacketKey_t &a, const t3dRdPacketKey_t &b ) const {
		if ( a.m_nSurfaceSlot != b.m_nSurfaceSlot ) {
			return a.m_nSurfaceSlot < b.m_nSurfaceSlot;
		}
		if ( a.m_nFlags != b.m_nFlags ) {
			return a.m_nFlags < b.m_nFlags;
		}
		if ( a.m_nMaterial != b.m_nMaterial ) {
			return a.m_nMaterial < b.m_nMaterial;
		}
		if ( a.m_nOverlayMaterial != b.m_nOverlayMaterial ) {
			return a.m_nOverlayMaterial < b.m_nOverlayMaterial;
		}
		if ( a.m_nVisibilityLayers != b.m_nVisibilityLayers ) {
			return a.m_nVisibilityLayers < b.m_nVisibilityLayers;
		}
		return a.m_nUnique < b.m_nUnique;
	}
} t3dRdPacketKeyLess_t;


// t3dRdPacketBuildSlot_s centralized in Terrain3DTypes.h.


// t3dRdPacketBuildEntry_s centralized in Terrain3DTypes.h.


#endif // TERRAIN3D_RD_INSTANCE_RENDERER_CONTRACTS_H
