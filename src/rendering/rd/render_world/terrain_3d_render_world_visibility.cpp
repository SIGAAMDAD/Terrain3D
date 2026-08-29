// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_visibility.h"

#include <math.h>
#include <string.h>

static float t3d_rw_visibility_length3( float pX, float pY, float pZ ) {
	return sqrtf( pX * pX + pY * pY + pZ * pZ );
}

static void t3d_rw_visibility_transform_point( const float pTransform[12], const float pPoint[3], float rPoint[3] ) {
	rPoint[0] = pTransform[0] * pPoint[0] + pTransform[1] * pPoint[1] + pTransform[2] * pPoint[2] + pTransform[3];
	rPoint[1] = pTransform[4] * pPoint[0] + pTransform[5] * pPoint[1] + pTransform[6] * pPoint[2] + pTransform[7];
	rPoint[2] = pTransform[8] * pPoint[0] + pTransform[9] * pPoint[1] + pTransform[10] * pPoint[2] + pTransform[11];
}

static float t3d_rw_visibility_transform_radius( const float pTransform[12], float pRadius ) {
	const float fFrobenius = sqrtf( pTransform[0] * pTransform[0] + pTransform[1] * pTransform[1] + pTransform[2] * pTransform[2]
	        + pTransform[4] * pTransform[4] + pTransform[5] * pTransform[5] + pTransform[6] * pTransform[6] + pTransform[8] * pTransform[8]
	        + pTransform[9] * pTransform[9] + pTransform[10] * pTransform[10] );
	return pRadius * fFrobenius;
}

static bool t3d_rw_visibility_sphere_in_frustum( const T3DVisibilityFrame &pFrame, const float pCenter[3], float pRadius ) {
	if ( ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_FRUSTUM ) == 0u ) {
		return true;
	}
	for ( uint32_t iPlane = 0u; iPlane < 6u; ++iPlane ) {
		const float *pPlane = pFrame.vFrustumPlanes + iPlane * 4u;
		const float fDistance = pPlane[0] * pCenter[0] + pPlane[1] * pCenter[1] + pPlane[2] * pCenter[2] + pPlane[3];
		const float fNormalLength = t3d_rw_visibility_length3( pPlane[0], pPlane[1], pPlane[2] );
		if ( fNormalLength > 1.0e-20f && fDistance < -pRadius * fNormalLength ) {
			return false;
		}
	}
	return true;
}

static bool t3d_rw_visibility_sphere_in_distance( const T3DVisibilityFrame &pFrame, const float pCenter[3], float pRadius ) {
	if ( ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_DISTANCE ) == 0u ) {
		return true;
	}
	const float fDx = pCenter[0] - pFrame.vCameraPositionAndMaxDistance[0];
	const float fDy = pCenter[1] - pFrame.vCameraPositionAndMaxDistance[1];
	const float fDz = pCenter[2] - pFrame.vCameraPositionAndMaxDistance[2];
	const float fDistance = t3d_rw_visibility_length3( fDx, fDy, fDz );
	return fDistance - pRadius <= pFrame.vCameraPositionAndMaxDistance[3];
}

static float t3d_rw_visibility_instance_distance( const T3DVisibilityFrame &pFrame, const T3DInstanceRecord &pInstance ) {
	const float fDx = pInstance.mTransform3x4[3] - pFrame.vCameraPositionAndMaxDistance[0];
	const float fDy = pInstance.mTransform3x4[7] - pFrame.vCameraPositionAndMaxDistance[1];
	const float fDz = pInstance.mTransform3x4[11] - pFrame.vCameraPositionAndMaxDistance[2];
	return t3d_rw_visibility_length3( fDx, fDy, fDz );
}

static bool t3d_rw_visibility_cluster_cone_visible( const T3DVisibilityFrame &pFrame,
        const T3DInstanceRecord &pInstance,
        const T3DClusterRecord &pCluster,
        const float pWorldCenter[3] ) {
	if ( ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_CONE ) == 0u ) {
		return true;
	}
	if ( ( pInstance.nFlags & T3D_RW_INSTANCE_FLAG_CONE_SAFE ) == 0u ) {
		return true;
	}
	if ( pCluster.fConeCutoff <= -0.9999f ) {
		return true;
	}

	float vAxis[3] = { pInstance.mTransform3x4[0] * pCluster.vConeAxis[0] + pInstance.mTransform3x4[1] * pCluster.vConeAxis[1]
		        + pInstance.mTransform3x4[2] * pCluster.vConeAxis[2],
		pInstance.mTransform3x4[4] * pCluster.vConeAxis[0] + pInstance.mTransform3x4[5] * pCluster.vConeAxis[1]
		        + pInstance.mTransform3x4[6] * pCluster.vConeAxis[2],
		pInstance.mTransform3x4[8] * pCluster.vConeAxis[0] + pInstance.mTransform3x4[9] * pCluster.vConeAxis[1]
		        + pInstance.mTransform3x4[10] * pCluster.vConeAxis[2] };
	const float fAxisLength = t3d_rw_visibility_length3( vAxis[0], vAxis[1], vAxis[2] );
	if ( fAxisLength <= 1.0e-20f ) {
		return true;
	}
	vAxis[0] /= fAxisLength;
	vAxis[1] /= fAxisLength;
	vAxis[2] /= fAxisLength;

	float vToCamera[3] = { pFrame.vCameraPositionAndMaxDistance[0] - pWorldCenter[0],
		pFrame.vCameraPositionAndMaxDistance[1] - pWorldCenter[1],
		pFrame.vCameraPositionAndMaxDistance[2] - pWorldCenter[2] };
	const float fViewLength = t3d_rw_visibility_length3( vToCamera[0], vToCamera[1], vToCamera[2] );
	if ( fViewLength <= 1.0e-20f ) {
		return true;
	}
	vToCamera[0] /= fViewLength;
	vToCamera[1] /= fViewLength;
	vToCamera[2] /= fViewLength;

	const float fDot = vAxis[0] * vToCamera[0] + vAxis[1] * vToCamera[1] + vAxis[2] * vToCamera[2];
	const float fConservativeBackfaceThreshold = -sqrtf( fmaxf( 0.0f, 1.0f - pCluster.fConeCutoff * pCluster.fConeCutoff ) );
	return fDot > fConservativeBackfaceThreshold;
}

static bool t3d_rw_visibility_instance_lod_in_frustum(
        const T3DVisibilityFrame &pFrame, const T3DGeometryDatabase &pGeometry, const T3DInstanceRecord &pInstance, uint32_t pLod ) {
	if ( ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_FRUSTUM ) == 0u ) {
		return true;
	}
	if ( pInstance.nMeshId >= pGeometry.nMeshCount ) {
		return false;
	}
	const T3DMeshRecord &Mesh = pGeometry.Buffers.pMeshRecords[pInstance.nMeshId];
	if ( pLod >= Mesh.nLodCount || Mesh.nLodOffset > pGeometry.nLodCount || Mesh.nLodCount > pGeometry.nLodCount - Mesh.nLodOffset ) {
		return false;
	}
	const T3DMeshLodRecord &Lod = pGeometry.Buffers.pLodRecords[Mesh.nLodOffset + pLod];
	if ( Lod.nClusterOffset > pGeometry.nClusterCount || Lod.nClusterCount > pGeometry.nClusterCount - Lod.nClusterOffset ) {
		return false;
	}
	for ( uint32_t iCluster = 0u; iCluster < Lod.nClusterCount; ++iCluster ) {
		const T3DClusterRecord &Cluster = pGeometry.Buffers.pClusterRecords[Lod.nClusterOffset + iCluster];
		float vWorldCenter[3];
		t3d_rw_visibility_transform_point( pInstance.mTransform3x4, Cluster.vCenter, vWorldCenter );
		const float fWorldRadius = t3d_rw_visibility_transform_radius( pInstance.mTransform3x4, Cluster.fRadius );
		if ( t3d_rw_visibility_sphere_in_frustum( pFrame, vWorldCenter, fWorldRadius ) ) {
			return true;
		}
	}
	return false;
}

static bool t3d_rw_visibility_instance_lod_in_distance(
        const T3DVisibilityFrame &pFrame, const T3DGeometryDatabase &pGeometry, const T3DInstanceRecord &pInstance, uint32_t pLod ) {
	if ( ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_DISTANCE ) == 0u ) {
		return true;
	}
	if ( pInstance.nMeshId >= pGeometry.nMeshCount ) {
		return false;
	}
	const T3DMeshRecord &Mesh = pGeometry.Buffers.pMeshRecords[pInstance.nMeshId];
	if ( pLod >= Mesh.nLodCount || Mesh.nLodOffset > pGeometry.nLodCount || Mesh.nLodCount > pGeometry.nLodCount - Mesh.nLodOffset ) {
		return false;
	}
	const T3DMeshLodRecord &Lod = pGeometry.Buffers.pLodRecords[Mesh.nLodOffset + pLod];
	if ( Lod.nClusterOffset > pGeometry.nClusterCount || Lod.nClusterCount > pGeometry.nClusterCount - Lod.nClusterOffset ) {
		return false;
	}
	for ( uint32_t iCluster = 0u; iCluster < Lod.nClusterCount; ++iCluster ) {
		const T3DClusterRecord &Cluster = pGeometry.Buffers.pClusterRecords[Lod.nClusterOffset + iCluster];
		float vWorldCenter[3];
		t3d_rw_visibility_transform_point( pInstance.mTransform3x4, Cluster.vCenter, vWorldCenter );
		const float fWorldRadius = t3d_rw_visibility_transform_radius( pInstance.mTransform3x4, Cluster.fRadius );
		if ( t3d_rw_visibility_sphere_in_distance( pFrame, vWorldCenter, fWorldRadius ) ) {
			return true;
		}
	}
	return false;
}

uint32_t t3d_render_world_visibility_select_lod_reference_debug( const T3DGeometryDatabase &pGeometry, uint32_t pMeshId, float pDistance ) {
	if ( pMeshId >= pGeometry.nMeshCount ) {
		return T3D_RW_INVALID_ID;
	}
	const T3DMeshRecord &Mesh = pGeometry.Buffers.pMeshRecords[pMeshId];
	if ( Mesh.nLodCount == 0u || Mesh.nLodOffset > pGeometry.nLodCount || Mesh.nLodCount > pGeometry.nLodCount - Mesh.nLodOffset ) {
		return T3D_RW_INVALID_ID;
	}
	for ( uint32_t iLod = 0u; iLod < Mesh.nLodCount; ++iLod ) {
		const T3DMeshLodRecord &Lod = pGeometry.Buffers.pLodRecords[Mesh.nLodOffset + iLod];
		if ( pDistance >= Lod.fMinDistance && pDistance < Lod.fMaxDistance ) {
			return iLod;
		}
	}
	return Mesh.nLodCount - 1u;
}

static bool t3d_rw_visibility_buffers_valid( const T3DVisibilityOutputBuffers &pBuffers, uint32_t pInstanceCount ) {
	if ( pBuffers.pVisibleCells == NULL || pBuffers.nVisibleCellCapacity == 0u ) {
		return false;
	}
	if ( pBuffers.pVisibleInstances == NULL || pBuffers.nVisibleInstanceCapacity == 0u ) {
		return false;
	}
	if ( pBuffers.pSelectedLodByInstance == NULL || pBuffers.nSelectedLodCapacity < pInstanceCount ) {
		return false;
	}
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		if ( pBuffers.pVisibleClusters[iTier] == NULL || pBuffers.nVisibleClusterCapacity[iTier] == 0u ) {
			return false;
		}
	}
	return true;
}

bool t3d_render_world_visibility_build_reference_debug( const T3DVisibilityScene &pScene,
        const T3DVisibilityFrame &pFrame,
        const T3DVisibilityReferenceConfig &pConfig,
        const T3DVisibilityOutputBuffers &pBuffers,
        T3DVisibilityReferenceResult &rResult ) {
	memset( &rResult, 0, sizeof( rResult ) );
	if ( pScene.pCells == NULL || pScene.pInstances == NULL || pScene.pGeometry == NULL ) {
		return false;
	}
	if ( !t3d_rw_visibility_buffers_valid( pBuffers, pScene.nInstanceCount ) ) {
		return false;
	}

	for ( uint32_t iInstance = 0u; iInstance < pScene.nInstanceCount; ++iInstance ) {
		pBuffers.pSelectedLodByInstance[iInstance] = T3D_RW_INVALID_ID;
	}

	for ( uint32_t iCell = 0u; iCell < pScene.nCellCount; ++iCell ) {
		const T3DSceneCell &Cell = pScene.pCells[iCell];
		if ( Cell.nInstanceOffset > pScene.nInstanceCount || Cell.nInstanceCount > pScene.nInstanceCount - Cell.nInstanceOffset ) {
			return false;
		}
		if ( !t3d_rw_visibility_sphere_in_frustum( pFrame, Cell.vCenter, Cell.fRadius )
		        || !t3d_rw_visibility_sphere_in_distance( pFrame, Cell.vCenter, Cell.fRadius ) ) {
			++rResult.Counters.nCellRejectCount;
			continue;
		}
		if ( rResult.Counters.nVisibleCellCount >= pBuffers.nVisibleCellCapacity ) {
			rResult.Counters.nOverflowBits |= T3D_RW_VISIBILITY_OVERFLOW_CELLS;
			continue;
		}
		pBuffers.pVisibleCells[rResult.Counters.nVisibleCellCount++] = iCell;
	}

	for ( uint32_t iVisibleCell = 0u; iVisibleCell < rResult.Counters.nVisibleCellCount; ++iVisibleCell ) {
		const T3DSceneCell &Cell = pScene.pCells[pBuffers.pVisibleCells[iVisibleCell]];
		for ( uint32_t iLocalInstance = 0u; iLocalInstance < Cell.nInstanceCount; ++iLocalInstance ) {
			const uint32_t nInstanceId = Cell.nInstanceOffset + iLocalInstance;
			const T3DInstanceRecord &Instance = pScene.pInstances[nInstanceId];
			if ( Instance.nMeshId >= pScene.pGeometry->nMeshCount ) {
				return false;
			}
			const float fDistance = t3d_rw_visibility_instance_distance( pFrame, Instance );
			const uint32_t nLod = t3d_render_world_visibility_select_lod_reference_debug( *pScene.pGeometry, Instance.nMeshId, fDistance );
			if ( nLod == T3D_RW_INVALID_ID || !t3d_rw_visibility_instance_lod_in_distance( pFrame, *pScene.pGeometry, Instance, nLod )
			        || !t3d_rw_visibility_instance_lod_in_frustum( pFrame, *pScene.pGeometry, Instance, nLod ) ) {
				++rResult.Counters.nInstanceRejectCount;
				continue;
			}
			pBuffers.pSelectedLodByInstance[nInstanceId] = nLod;
			if ( rResult.Counters.nVisibleInstanceCount >= pBuffers.nVisibleInstanceCapacity ) {
				rResult.Counters.nOverflowBits |= T3D_RW_VISIBILITY_OVERFLOW_INSTANCES;
				continue;
			}
			pBuffers.pVisibleInstances[rResult.Counters.nVisibleInstanceCount++] = nInstanceId;
		}
	}

	for ( uint32_t iVisibleInstance = 0u; iVisibleInstance < rResult.Counters.nVisibleInstanceCount; ++iVisibleInstance ) {
		const uint32_t nInstanceId = pBuffers.pVisibleInstances[iVisibleInstance];
		const T3DInstanceRecord &Instance = pScene.pInstances[nInstanceId];
		const T3DMeshRecord &Mesh = pScene.pGeometry->Buffers.pMeshRecords[Instance.nMeshId];
		const uint32_t nLod = pBuffers.pSelectedLodByInstance[nInstanceId];
		if ( nLod >= Mesh.nLodCount ) {
			return false;
		}
		const T3DMeshLodRecord &Lod = pScene.pGeometry->Buffers.pLodRecords[Mesh.nLodOffset + nLod];
		if ( Lod.nClusterOffset > pScene.pGeometry->nClusterCount
		        || Lod.nClusterCount > pScene.pGeometry->nClusterCount - Lod.nClusterOffset ) {
			return false;
		}

		for ( uint32_t iCluster = 0u; iCluster < Lod.nClusterCount; ++iCluster ) {
			const uint32_t nClusterId = Lod.nClusterOffset + iCluster;
			const T3DClusterRecord &Cluster = pScene.pGeometry->Buffers.pClusterRecords[nClusterId];
			++rResult.Counters.nCandidateClusterCount;
			float vWorldCenter[3];
			t3d_rw_visibility_transform_point( Instance.mTransform3x4, Cluster.vCenter, vWorldCenter );
			const float fWorldRadius = t3d_rw_visibility_transform_radius( Instance.mTransform3x4, Cluster.fRadius );
			if ( !t3d_rw_visibility_sphere_in_frustum( pFrame, vWorldCenter, fWorldRadius ) ) {
				++rResult.Counters.nClusterFrustumRejectCount;
				continue;
			}
			if ( !t3d_rw_visibility_cluster_cone_visible( pFrame, Instance, Cluster, vWorldCenter ) ) {
				++rResult.Counters.nClusterConeRejectCount;
				continue;
			}
			const bool bOcclusionEnabled = ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_HIZ ) != 0u
			        && ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_REFERENCE_NO_OCCLUSION ) == 0u
			        && ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_CAMERA_CUT ) == 0u && pConfig.pOcclusionTest != NULL;
			if ( bOcclusionEnabled
			        && pConfig.pOcclusionTest( pConfig.pOcclusionContext, pFrame, vWorldCenter, fWorldRadius, nClusterId, nInstanceId ) ) {
				++rResult.Counters.nClusterHiZRejectCount;
				continue;
			}

			const uint32_t nTier = Cluster.nTier;
			if ( nTier >= T3D_RW_TIER_COUNT ) {
				return false;
			}
			const uint32_t nVisibleCount = rResult.Counters.nVisibleClusterCount[nTier];
			if ( nVisibleCount >= pBuffers.nVisibleClusterCapacity[nTier] ) {
				rResult.Counters.nOverflowBits |= ( T3D_RW_VISIBILITY_OVERFLOW_WORLD << nTier );
				continue;
			}
			pBuffers.pVisibleClusters[nTier][nVisibleCount].nClusterId = nClusterId;
			pBuffers.pVisibleClusters[nTier][nVisibleCount].nInstanceId = nInstanceId;
			rResult.Counters.nVisibleClusterCount[nTier] = nVisibleCount + 1u;
		}
	}

	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		rResult.DrawArgs[iTier].nVertexCount = T3D_RW_CLUSTER_RASTER_VERTEX_COUNT;
		rResult.DrawArgs[iTier].nInstanceCount = rResult.Counters.nVisibleClusterCount[iTier];
		rResult.DrawArgs[iTier].nFirstVertex = 0u;
		rResult.DrawArgs[iTier].nFirstInstance = 0u;
	}
	if ( ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_HIZ ) != 0u && pConfig.pOcclusionTest == NULL ) {
		++rResult.Counters.nFallbackCount;
	}
	return true;
}

bool t3d_render_world_visibility_validate_reference_debug(
        const T3DVisibilityScene &pScene, const T3DVisibilityOutputBuffers &pBuffers, const T3DVisibilityReferenceResult &pResult ) {
	if ( pScene.pGeometry == NULL ) {
		return false;
	}
	if ( pResult.Counters.nVisibleCellCount > pBuffers.nVisibleCellCapacity ) {
		return false;
	}
	if ( pResult.Counters.nVisibleInstanceCount > pBuffers.nVisibleInstanceCapacity ) {
		return false;
	}
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		if ( pResult.Counters.nVisibleClusterCount[iTier] > pBuffers.nVisibleClusterCapacity[iTier] ) {
			return false;
		}
		if ( pResult.DrawArgs[iTier].nVertexCount != T3D_RW_CLUSTER_RASTER_VERTEX_COUNT ) {
			return false;
		}
		if ( pResult.DrawArgs[iTier].nInstanceCount != pResult.Counters.nVisibleClusterCount[iTier] ) {
			return false;
		}
		for ( uint32_t iVisible = 0u; iVisible < pResult.Counters.nVisibleClusterCount[iTier]; ++iVisible ) {
			const T3DVisibleCluster &Visible = pBuffers.pVisibleClusters[iTier][iVisible];
			if ( Visible.nClusterId >= pScene.pGeometry->nClusterCount || Visible.nInstanceId >= pScene.nInstanceCount ) {
				return false;
			}
			if ( pScene.pGeometry->Buffers.pClusterRecords[Visible.nClusterId].nTier != iTier ) {
				return false;
			}
		}
	}
	return true;
}
