// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_OCCLUSION_H
#define TERRAIN3D_INSTANCE_OCCLUSION_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "instances/contracts/terrain_3d_instance_mutation.h"
#include "memory/arena/terrain_3d_memory_zone.h"

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "data/model/terrain_3d_region.h"
// t3dInstanceOcclusionRecordFlags_e centralized in Terrain3DTypes.h.


// t3dInstanceOcclusionCell_s centralized in Terrain3DTypes.h.


class Terrain3DInstanceOcclusion {
	CLASS_NAME_STATIC( "Terrain3DInstanceOcclusion" );

private:
	Terrain3DOwner *m_pTerrain = NULL;
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
	eastl::vector<t3dInstanceOcclusionCell_t, Terrain3DEastlAllocator> m_cells;
	Vector3 m_lastTargetPosition;
	bool m_bTargetValid = false;
	bool m_bCapacityRebuildPending = false;
	size_t m_nCellCapacity = 0u;
	uint64_t m_nPublicationGeneration = 1u;
	uint64_t m_nProxyCells = 0u;
	uint64_t m_nProxyInstances = 0u;
	uint64_t m_nProxyVertices = 0u;
	uint64_t m_nProxyTriangles = 0u;
	uint64_t m_nCategoryPolicyViolations = 0u;
	uint64_t m_nProxyRebuilds = 0u;
	uint64_t m_nProxyBytes = 0u;
	uint64_t m_nCapacityMisses = 0u;
	mutable uint64_t m_nStaleRevisionErrors = 0u;
	uint64_t m_nRegionUnloadReleases = 0u;
	uint64_t m_nMutationEvents = 0u;
	uint64_t m_nMirroredInstances = 0u;
	uint64_t m_nColdCapacityRebuilds = 0u;

	void advancePublicationGeneration( void );
	bool rebuildCellCapacityCold( void );
	bool hasUsableExplicitProxy( const Ref<Terrain3DMeshAsset> &rAsset ) const;
	bool buildCellRecord( Terrain3DRegion *pRegion,
	        const t3dInstanceCell_s &rCell,
	        const Ref<Terrain3DMeshAsset> &rAsset,
	        t3dInstanceOcclusionCell_t &rRecord ) const;
	int findCellRecord( const Vector2i &rRegion, const Vector2i &rCell, int nMeshId ) const;
	bool appendCellRecord( const t3dInstanceOcclusionCell_t &rRecord );
	bool refreshCellRecord( const Vector2i &rRegion, const Vector2i &rCell, int nMeshId, uint32_t nExpectedRevision = 0u );
	void eraseCellRecordAt( size_t iIndex );
	void eraseRegionRecords( const Vector2i &rRegion );
	void eraseMeshRecords( int nMeshId );
	bool refreshRegionRecords( const Vector2i &rRegion, int nMeshId = -1 );
	bool rebuildResidency( const Vector3 &rTarget );
	void refreshDiagnostics( void );

public:
	Terrain3DInstanceOcclusion( void ) = default;
	~Terrain3DInstanceOcclusion( void ) {
		destroy();
	}
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	void initialize( Terrain3DOwner *pTerrain );
	void destroy( void );
	void serviceColdCapacity( void );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void onMutation( const t3dInstanceMutationEvent_t &rEvent );
	void onAssetPolicyChanged( int nMeshId );
	Dictionary getStats( void ) const;

	EA_FORCE_INLINE uint64_t getPublicationGeneration( void ) const {
		return m_nPublicationGeneration;
	}
	EA_FORCE_INLINE size_t getCellCapacity( void ) const {
		return m_nCellCapacity;
	}
	EA_FORCE_INLINE const eastl::vector<t3dInstanceOcclusionCell_t, Terrain3DEastlAllocator> &getCells( void ) const {
		return m_cells;
	}
	bool getCellRecord( const Vector2i &rRegion, const Vector2i &rCell, int nMeshId, t3dInstanceOcclusionCell_t &rRecord ) const;
};

#endif // TERRAIN3D_INSTANCE_OCCLUSION_H
