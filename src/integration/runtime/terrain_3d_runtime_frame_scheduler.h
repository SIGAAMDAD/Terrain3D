// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RUNTIME_FRAME_SCHEDULER_H
#define TERRAIN3D_RUNTIME_FRAME_SCHEDULER_H

#include "core/utility/terrain_3d_owner.h"
#include "streaming/policy/terrain_3d_runtime_profile.h"
#include <godot_cpp/variant/vector3.hpp>

#include "data/model/terrain_3d_data.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "physics/runtime/terrain_3d_collision.h"
#include "rendering/geometry/terrain_3d_mesher.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "instances/physics/terrain_3d_instance_physics.h"
#include "instances/physics/terrain_3d_instance_occlusion.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "core/utility/target_node_3d.h"
#include "streaming/policy/terrain_3d_boundary_phase_policy.h"

class Terrain3DRuntimeFrameHost
{
protected:
	~Terrain3DRuntimeFrameHost( void ) = default;

public:
	virtual void	applyRuntimeAdaptiveWorkBudgets( void ) = 0;
	virtual bool	effectiveDynamicNodeStreamingEnabled( void ) const = 0;
	virtual bool	effectiveInstancerCellStreamingEnabled( void ) const = 0;
	virtual Vector3 getClipmapTargetPosition( void ) const = 0;
	virtual void	grabCamera( void ) = 0;
	virtual bool	isEditorAuthoringModeInternal( void ) const = 0;
	virtual void	sampleRuntimeProfile( const t3dRuntimeProfileSample_t &rSample ) = 0;
	virtual void	serviceMemoryBudgetPolicy( bool bForce ) = 0;
	virtual void	syncLightShaderParameters( void ) = 0;
	virtual bool	tryUpdateComputeDisplacement( const Vector3 &rTargetPosition ) = 0;
	virtual void updateRegionStreamingPolicy( const Vector3 &rWorldPosition, double fDelta, bool bForce = false ) = 0;
	virtual void updateRuntimeAdaptiveWorkBudgets( void ) = 0;
};
// Borrowed owner-thread control inputs. Subsystems retain their own storage and
// allocation policy; the scheduler invokes their public lifecycle operations.
struct t3dRuntimeFrameServices_t
{
	t3dAtomicBool_t				&m_bComputeDisplacementDirty;
	t3dAtomicBool_t				&m_bComputeDisplacementFallbackRequested;
	bool						&m_bEditorHlodPreview;
	bool						&m_bHlodEnabled;
	bool						&m_bInitialized;
	bool						&m_bInstancerSingleLodBuffers;
	bool						&m_bLightmapStreamingEnabled;
	bool						&m_bOceanEnabled;
	bool						&m_bRuntimeAdaptiveWorkBudgetsEnabled;
	bool						&m_bRuntimeBoundaryInstancerDeferredLastTick;
	bool						&m_bRuntimeBoundaryNodeDeferredLastTick;
	bool						&m_bRuntimeBoundaryWorkPhasingEnabled;
	bool						&m_bRuntimeBoundaryWorldDeferredLastTick;
	bool						&m_bRuntimeProfilingEnabled;
	real_t						&m_fDisplacementUpdateDistance;
	Terrain3DTargetNode3D		&m_hCamera;
	uint64_t					&m_iRuntimePhysicsFrameIndex;
	Vector2						&m_lastBufferPosition;
	Ref< Terrain3DMaterial >	&m_material;
	uint64_t					&m_nComputeDisplacementPageTableGeneration;
	uint64_t					&m_nRuntimeAdaptiveControllerUsecLast;
	uint64_t					&m_nRuntimeAdaptiveLastMeasuredUsec;
	int							&m_nRuntimeBoundaryInstancerDeferralStreak;
	uint64_t					&m_nRuntimeBoundaryInstancerDeferralsTotal;
	int							&m_nRuntimeBoundaryInstancerDelayFrames;
	int							&m_nRuntimeBoundaryNodeDeferralStreak;
	uint64_t					&m_nRuntimeBoundaryNodeDeferralsTotal;
	int							&m_nRuntimeBoundaryNodeDelayFrames;
	uint64_t					&m_nRuntimeBoundaryTransitionFrame;
	int							&m_nRuntimeBoundaryWorldDeferralStreak;
	uint64_t					&m_nRuntimeBoundaryWorldDeferralsTotal;
	int							&m_nRuntimeBoundaryWorldDelayFrames;
	uint64_t					&m_nRuntimeProfileSampleCount;
	int							&m_nTessellationLevel;
	Terrain3DCollision		   *&m_pCollision;
	Terrain3DData			   *&m_pData;
	Terrain3DHLODStreamer	   *&m_pHlodStreamer;
	Terrain3DInstanceOcclusion *&m_pInstanceOcclusion;
	Terrain3DInstancePhysics   *&m_pInstancePhysics;
	Terrain3DInstancer		   *&m_pInstancer;
	Terrain3DLightmapStreamer  *&m_pLightmapStreamer;
	Terrain3DNodeStreamer	   *&m_pNodeStreamer;
	Terrain3DMesher			   *&m_pOceanMesher;
	Terrain3DRenderCoordinator *&m_pRenderCoordinator;
	Terrain3DMesher			   *&m_pTerrainMesher;
	Vector2i					&m_runtimeBoundaryTargetRegion;
	t3dRuntimeProfileSample_t	&m_runtimeProfileLastSample;
	Terrain3DRuntimeFrameHost	&m_host;
};

class Terrain3DRuntimeFrameScheduler
{
	CLASS_NAME_STATIC( "Terrain3DRuntimeFrameScheduler" );

private:
	void serviceColdRuntimeOwners( const t3dRuntimeFrameServices_t &rServices, const Vector3 &rTargetPosition );
	void updateRuntimeBoundaryPhasing( const t3dRuntimeFrameServices_t &rServices,
									   const Vector3				   &rTargetPosition,
									   t3dRuntimeProfileSample_t	   *pProfileSample );
	void updateRuntimeStreamingOwners( const t3dRuntimeFrameServices_t &rServices,
									   const Vector3				   &rTargetPosition,
									   t3dRuntimeProfileSample_t	   *pProfileSample,
									   uint64_t						   &rPhaseStartedUsec );
	void updateRuntimeClipmap( const t3dRuntimeFrameServices_t &rServices, const Vector3 &rTargetPosition );
	void updateRuntimeInstancer( const t3dRuntimeFrameServices_t &rServices, const Vector3 &rTargetPosition );

public:
	void tick( const t3dRuntimeFrameServices_t &rServices, double fDelta );
};

#endif // TERRAIN3D_RUNTIME_FRAME_SCHEDULER_H
