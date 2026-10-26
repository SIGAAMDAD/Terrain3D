// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RUNTIME_FRAME_SCHEDULER_H
#define TERRAIN3D_RUNTIME_FRAME_SCHEDULER_H

#include "streaming/terrain_3d_runtime_profile.h"
#include <godot_cpp/variant/vector3.hpp>

class Terrain3D;

class Terrain3DRuntimeFrameScheduler {
	CLASS_NAME_STATIC( "Terrain3DRuntimeFrameScheduler" );

private:
	void serviceColdRuntimeOwners( Terrain3D &rTerrain, const Vector3 &rTargetPosition );
	void updateRuntimeBoundaryPhasing( Terrain3D &rTerrain, const Vector3 &rTargetPosition, t3dRuntimeProfileSample_t *pProfileSample );
	void updateRuntimeStreamingOwners(
	        Terrain3D &rTerrain, const Vector3 &rTargetPosition, t3dRuntimeProfileSample_t *pProfileSample, uint64_t &rPhaseStartedUsec );
	void updateRuntimeClipmap( Terrain3D &rTerrain, const Vector3 &rTargetPosition );
	void updateRuntimeInstancer( Terrain3D &rTerrain, const Vector3 &rTargetPosition );

public:
	void tick( Terrain3D &rTerrain, double fDelta );
};

#endif // TERRAIN3D_RUNTIME_FRAME_SCHEDULER_H
