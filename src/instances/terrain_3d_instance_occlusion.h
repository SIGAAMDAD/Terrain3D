// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_OCCLUSION_H
#define TERRAIN3D_INSTANCE_OCCLUSION_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "instances/terrain_3d_instance_mutation.h"
#include "memory/terrain_3d_memory_zone.h"

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3D;
class Terrain3DMeshAsset;
class Terrain3DRegion;
struct t3dInstanceCell_s;
typedef struct t3dInstanceCell_s t3dInstanceCell_t;

typedef enum t3dInstanceOcclusionRecordFlags_e : uint8_t {
	T3D_INSTANCE_OCCLUSION_RECORD_NONE = 0u,
	T3D_INSTANCE_OCCLUSION_RECORD_MIRRORED = 1u << 0u,
} t3dInstanceOcclusionRecordFlags_t;

typedef struct t3dInstanceOcclusionCell_s {
	AABB m_worldBounds;
	AABB m_localProxyBounds;
	Vector2i m_region;
	Vector2i m_cell;
	int32_t m_nMeshId = -1;
	uint32_t m_nSourceRevision = 0u;
	uint32_t m_nProxyGeneration = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint16_t m_nMirroredInstanceCount = 0u;
	uint8_t m_nProxyPolicy = 0u;
	uint8_t m_nFlags = T3D_INSTANCE_OCCLUSION_RECORD_NONE;
} t3dInstanceOcclusionCell_t;

class Terrain3DInstanceOcclusion {
	CLASS_NAME_STATIC( "Terrain3DInstanceOcclusion" );

private:
	Terrain3D *m_pTerrain = NULL;
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
	        const t3dInstanceCell_t &rCell,
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
	void initialize( Terrain3D *pTerrain );
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
