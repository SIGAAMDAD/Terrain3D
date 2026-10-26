// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_PHYSICS_H
#define TERRAIN3D_INSTANCE_PHYSICS_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "instances/terrain_3d_instance_mutation.h"
#include "memory/terrain_3d_memory_zone.h"

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/rid.hpp>

class Terrain3D;
class Terrain3DMeshAsset;
class Terrain3DRegion;
struct t3dInstanceCell_s;
typedef struct t3dInstanceCell_s t3dInstanceCell_t;

typedef struct t3dInstancePhysicsCell_s {
	RID m_hBody;
	Vector2i m_region = Vector2i( INT32_MAX, INT32_MAX );
	Vector2i m_cell = Vector2i( INT32_MAX, INT32_MAX );
	int32_t m_nMeshId = -1;
	uint32_t m_nSourceRevision = 0u;
	uint32_t m_nCollisionGeneration = 0u;
	uint32_t m_nShapeCount = 0u;
	uint32_t m_nMirroredShapeCount = 0u;
	bool m_bActive = false;
	bool m_bDesired = false;
} t3dInstancePhysicsCell_t;

class Terrain3DInstancePhysics {
	CLASS_NAME_STATIC( "Terrain3DInstancePhysics" );

private:
	Terrain3D *m_pTerrain = NULL;
	Terrain3DMemoryZone *m_pRuntimeAllocatorZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eRuntimeAllocatorOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;
	eastl::vector<t3dInstancePhysicsCell_t, Terrain3DEastlAllocator> m_cells;
	Vector3 m_lastTargetPosition;
	Transform3D m_lastTerrainTransform;
	uint32_t m_nLastCollisionLayer = 0u;
	uint32_t m_nLastCollisionMask = 0u;
	uint16_t m_nLastCollisionRadius = 0u;
	int32_t m_nLastCollisionMode = -1;
	real_t m_fLastCollisionPriority = -1.0f;
	bool m_bTargetValid = false;
	bool m_bResidencyDirty = true;
	bool m_bCapacityRebuildPending = false;
	size_t m_nCellCapacity = 0u;
	uint64_t m_nActiveCells = 0u;
	uint64_t m_nActiveShapes = 0u;
	uint64_t m_nCellRebuilds = 0u;
	uint64_t m_nCellsReused = 0u;
	uint64_t m_nCapacityMisses = 0u;
	uint64_t m_nColdCapacityRebuilds = 0u;
	uint64_t m_nMutationEvents = 0u;
	uint64_t m_nMissingCollisionSkips = 0u;
	uint64_t m_nMirroredShapes = 0u;

	bool rebuildCellCapacityCold( void );
	bool cellWithinRange( const Vector2i &rRegion, const Vector2i &rCell, int nCellSize, real_t fRange, const Vector3 &rTarget ) const;
	void configureBody( t3dInstancePhysicsCell_t &rRecord );
	int findCellRecord( const Vector2i &rRegion, const Vector2i &rCell, int nMeshId ) const;
	int findFreeCellRecord( void ) const;
	void deactivateCellRecord( t3dInstancePhysicsCell_t &rRecord );
	bool rebuildCellRecord( t3dInstancePhysicsCell_t &rRecord,
	        Terrain3DRegion *pRegion,
	        const t3dInstanceCell_t &rCell,
	        const Ref<Terrain3DMeshAsset> &rAsset );
	bool reconcileResidency( const Vector3 &rTarget );
	void refreshDiagnostics( void );

public:
	Terrain3DInstancePhysics( void ) = default;
	~Terrain3DInstancePhysics( void ) {
		destroy();
	}
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	void initialize( Terrain3D *pTerrain );
	void destroy( void );
	void serviceColdCapacity( void );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void onMutation( const t3dInstanceMutationEvent_t &rEvent );
	Dictionary getStats( void ) const;
};

#endif // TERRAIN3D_INSTANCE_PHYSICS_H
