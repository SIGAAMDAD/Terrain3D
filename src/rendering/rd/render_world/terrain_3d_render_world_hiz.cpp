// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_hiz.h"

#include <math.h>
#include <string.h>

static uint32_t t3d_rw_hiz_min_u32( uint32_t pA, uint32_t pB ) {
	return pA < pB ? pA : pB;
}
static float t3d_rw_hiz_min_f( float pA, float pB ) {
	return pA < pB ? pA : pB;
}
static float t3d_rw_hiz_max_f( float pA, float pB ) {
	return pA > pB ? pA : pB;
}
static float t3d_rw_hiz_clamp_f( float pV, float pMin, float pMax ) {
	return pV < pMin ? pMin : ( pV > pMax ? pMax : pV );
}

bool t3d_render_world_hiz_initialize_reference_debug( T3DHiZPyramidReference &rPyramid,
        float *pDepthStorage,
        uint32_t pDepthCapacity,
        uint32_t pWidth,
        uint32_t pHeight,
        uint32_t pMipCount ) {
	memset( &rPyramid, 0, sizeof( rPyramid ) );
	if ( pDepthStorage == NULL || pDepthCapacity == 0u || pWidth == 0u || pHeight == 0u || pMipCount == 0u
	        || pMipCount > T3D_RW_HIZ_MAX_MIPS ) {
		return false;
	}
	uint64_t nTotal = 0u;
	uint32_t nWidth = pWidth;
	uint32_t nHeight = pHeight;
	for ( uint32_t iMip = 0u; iMip < pMipCount; ++iMip ) {
		if ( nTotal > UINT32_MAX ) {
			return false;
		}
		rPyramid.nOffset[iMip] = ( uint32_t )nTotal;
		rPyramid.nWidth[iMip] = nWidth;
		rPyramid.nHeight[iMip] = nHeight;
		nTotal += ( uint64_t )nWidth * nHeight;
		if ( nTotal > pDepthCapacity ) {
			return false;
		}
		if ( nWidth > 1u ) {
			nWidth = ( nWidth + 1u ) >> 1u;
		}
		if ( nHeight > 1u ) {
			nHeight = ( nHeight + 1u ) >> 1u;
		}
	}
	rPyramid.pDepth = pDepthStorage;
	rPyramid.nDepthCapacity = pDepthCapacity;
	rPyramid.nMipCount = pMipCount;
	rPyramid.nDepthCount = ( uint32_t )nTotal;
	return true;
}

bool t3d_render_world_hiz_build_reverse_z_reference_debug(
        T3DHiZPyramidReference &rPyramid, const float *pSourceDepth, uint32_t pSourceWidth, uint32_t pSourceHeight ) {
	if ( rPyramid.pDepth == NULL || pSourceDepth == NULL || rPyramid.nMipCount == 0u ) {
		return false;
	}
	if ( pSourceWidth != rPyramid.nWidth[0] || pSourceHeight != rPyramid.nHeight[0] ) {
		return false;
	}
	const uint32_t nMip0Count = pSourceWidth * pSourceHeight;
	memcpy( rPyramid.pDepth + rPyramid.nOffset[0], pSourceDepth, ( size_t )nMip0Count * sizeof( float ) );
	for ( uint32_t iMip = 1u; iMip < rPyramid.nMipCount; ++iMip ) {
		const uint32_t nSourceWidth = rPyramid.nWidth[iMip - 1u];
		const uint32_t nSourceHeight = rPyramid.nHeight[iMip - 1u];
		const uint32_t nDestWidth = rPyramid.nWidth[iMip];
		const uint32_t nDestHeight = rPyramid.nHeight[iMip];
		const float *pSource = rPyramid.pDepth + rPyramid.nOffset[iMip - 1u];
		float *pDest = rPyramid.pDepth + rPyramid.nOffset[iMip];
		for ( uint32_t y = 0u; y < nDestHeight; ++y ) {
			for ( uint32_t x = 0u; x < nDestWidth; ++x ) {
				const uint32_t x0 = t3d_rw_hiz_min_u32( x * 2u, nSourceWidth - 1u );
				const uint32_t x1 = t3d_rw_hiz_min_u32( x * 2u + 1u, nSourceWidth - 1u );
				const uint32_t y0 = t3d_rw_hiz_min_u32( y * 2u, nSourceHeight - 1u );
				const uint32_t y1 = t3d_rw_hiz_min_u32( y * 2u + 1u, nSourceHeight - 1u );
				float fDepth = pSource[y0 * nSourceWidth + x0];
				fDepth = t3d_rw_hiz_min_f( fDepth, pSource[y0 * nSourceWidth + x1] );
				fDepth = t3d_rw_hiz_min_f( fDepth, pSource[y1 * nSourceWidth + x0] );
				fDepth = t3d_rw_hiz_min_f( fDepth, pSource[y1 * nSourceWidth + x1] );
				pDest[y * nDestWidth + x] = fDepth;
			}
		}
	}
	rPyramid.nValid = 1u;
	return true;
}

static bool t3d_rw_hiz_project_point( const float pMatrix[16], float pX, float pY, float pZ, float rNdc[3] ) {
	const float fClipX = pMatrix[0] * pX + pMatrix[4] * pY + pMatrix[8] * pZ + pMatrix[12];
	const float fClipY = pMatrix[1] * pX + pMatrix[5] * pY + pMatrix[9] * pZ + pMatrix[13];
	const float fClipZ = pMatrix[2] * pX + pMatrix[6] * pY + pMatrix[10] * pZ + pMatrix[14];
	const float fClipW = pMatrix[3] * pX + pMatrix[7] * pY + pMatrix[11] * pZ + pMatrix[15];
	if ( !isfinite( fClipW ) || fClipW <= 1.0e-6f ) {
		return false;
	}
	rNdc[0] = fClipX / fClipW;
	rNdc[1] = fClipY / fClipW;
	rNdc[2] = fClipZ / fClipW;
	return isfinite( rNdc[0] ) && isfinite( rNdc[1] ) && isfinite( rNdc[2] );
}

bool t3d_render_world_hiz_is_occluded_reference_debug( const T3DHiZPyramidReference &pPyramid,
        const T3DVisibilityFrame &pFrame,
        const float pCenter[3],
        float pRadius,
        float pDepthBias ) {
	if ( pPyramid.nValid == 0u || pPyramid.pDepth == NULL || pPyramid.nMipCount == 0u || pRadius < 0.0f ) {
		return false;
	}
	if ( ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_HIZ ) == 0u || ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_REFERENCE_NO_OCCLUSION ) != 0u
	        || ( pFrame.nFlags & T3D_RW_VISIBILITY_FLAG_CAMERA_CUT ) != 0u ) {
		return false;
	}
	const float fViewportWidth = pFrame.vViewportAndMotion[0];
	const float fViewportHeight = pFrame.vViewportAndMotion[1];
	if ( fViewportWidth <= 0.0f || fViewportHeight <= 0.0f ) {
		return false;
	}

	float fMinX = 1.0f;
	float fMaxX = -1.0f;
	float fMinY = 1.0f;
	float fMaxY = -1.0f;
	float fNearestDepth = 0.0f;
	for ( uint32_t iCorner = 0u; iCorner < 8u; ++iCorner ) {
		const float fX = pCenter[0] + ( ( iCorner & 1u ) != 0u ? pRadius : -pRadius );
		const float fY = pCenter[1] + ( ( iCorner & 2u ) != 0u ? pRadius : -pRadius );
		const float fZ = pCenter[2] + ( ( iCorner & 4u ) != 0u ? pRadius : -pRadius );
		float vNdc[3];
		if ( !t3d_rw_hiz_project_point( pFrame.mViewProjection, fX, fY, fZ, vNdc ) ) {
			return false;
		}
		if ( vNdc[2] < 0.0f || vNdc[2] > 1.0f ) {
			return false;
		}
		fMinX = t3d_rw_hiz_min_f( fMinX, vNdc[0] );
		fMaxX = t3d_rw_hiz_max_f( fMaxX, vNdc[0] );
		fMinY = t3d_rw_hiz_min_f( fMinY, vNdc[1] );
		fMaxY = t3d_rw_hiz_max_f( fMaxY, vNdc[1] );
		fNearestDepth = t3d_rw_hiz_max_f( fNearestDepth, vNdc[2] );
	}
	if ( fMaxX < -1.0f || fMinX > 1.0f || fMaxY < -1.0f || fMinY > 1.0f ) {
		return false;
	}
	fMinX = t3d_rw_hiz_clamp_f( fMinX, -1.0f, 1.0f );
	fMaxX = t3d_rw_hiz_clamp_f( fMaxX, -1.0f, 1.0f );
	fMinY = t3d_rw_hiz_clamp_f( fMinY, -1.0f, 1.0f );
	fMaxY = t3d_rw_hiz_clamp_f( fMaxY, -1.0f, 1.0f );

	const float fMotionPixels = pFrame.vViewportAndMotion[2] < 0.0f ? 0.0f : pFrame.vViewportAndMotion[2];
	float fPixelMinX = ( fMinX * 0.5f + 0.5f ) * fViewportWidth - fMotionPixels;
	float fPixelMaxX = ( fMaxX * 0.5f + 0.5f ) * fViewportWidth + fMotionPixels;
	float fPixelMinY = ( fMinY * 0.5f + 0.5f ) * fViewportHeight - fMotionPixels;
	float fPixelMaxY = ( fMaxY * 0.5f + 0.5f ) * fViewportHeight + fMotionPixels;
	fPixelMinX = t3d_rw_hiz_clamp_f( fPixelMinX, 0.0f, fViewportWidth - 1.0f );
	fPixelMaxX = t3d_rw_hiz_clamp_f( fPixelMaxX, 0.0f, fViewportWidth - 1.0f );
	fPixelMinY = t3d_rw_hiz_clamp_f( fPixelMinY, 0.0f, fViewportHeight - 1.0f );
	fPixelMaxY = t3d_rw_hiz_clamp_f( fPixelMaxY, 0.0f, fViewportHeight - 1.0f );
	const float fExtent = t3d_rw_hiz_max_f( fPixelMaxX - fPixelMinX + 1.0f, fPixelMaxY - fPixelMinY + 1.0f );
	uint32_t nMip = 0u;
	float fCoverage = 1.0f;
	while ( nMip + 1u < pPyramid.nMipCount && fCoverage < fExtent ) {
		fCoverage *= 2.0f;
		++nMip;
	}
	const uint32_t nScale = 1u << nMip;
	const uint32_t nWidth = pPyramid.nWidth[nMip];
	const uint32_t nHeight = pPyramid.nHeight[nMip];
	const uint32_t x0 = t3d_rw_hiz_min_u32( ( uint32_t )fPixelMinX / nScale, nWidth - 1u );
	const uint32_t x1 = t3d_rw_hiz_min_u32( ( uint32_t )fPixelMaxX / nScale, nWidth - 1u );
	const uint32_t y0 = t3d_rw_hiz_min_u32( ( uint32_t )fPixelMinY / nScale, nHeight - 1u );
	const uint32_t y1 = t3d_rw_hiz_min_u32( ( uint32_t )fPixelMaxY / nScale, nHeight - 1u );
	const float *pMipDepth = pPyramid.pDepth + pPyramid.nOffset[nMip];
	float fOccluder = pMipDepth[y0 * nWidth + x0];
	fOccluder = t3d_rw_hiz_min_f( fOccluder, pMipDepth[y0 * nWidth + x1] );
	fOccluder = t3d_rw_hiz_min_f( fOccluder, pMipDepth[y1 * nWidth + x0] );
	fOccluder = t3d_rw_hiz_min_f( fOccluder, pMipDepth[y1 * nWidth + x1] );
	return fOccluder > fNearestDepth + pDepthBias;
}

bool t3d_render_world_hiz_visibility_occlusion_callback_reference_debug( const void *pContext,
        const T3DVisibilityFrame &pFrame,
        const float pCenter[3],
        float pRadius,
        uint32_t pClusterId,
        uint32_t pInstanceId ) {
	( void )pInstanceId;
	const T3DHiZOcclusionReferenceContext *pHiZ = ( const T3DHiZOcclusionReferenceContext * )pContext;
	if ( pHiZ == NULL || pHiZ->pPyramid == NULL ) {
		return false;
	}
	if ( pHiZ->pLastVisibleFrameByCluster != NULL && pClusterId < pHiZ->nHistoryCapacity ) {
		const uint32_t nLastVisible = pHiZ->pLastVisibleFrameByCluster[pClusterId];
		if ( nLastVisible != 0u && pFrame.nFrameId >= nLastVisible && pFrame.nFrameId - nLastVisible <= pHiZ->nHysteresisFrames ) {
			return false;
		}
	}
	return t3d_render_world_hiz_is_occluded_reference_debug( *pHiZ->pPyramid, pFrame, pCenter, pRadius, pHiZ->fDepthBias );
}

void t3d_render_world_hiz_mark_visible_reference_debug( T3DHiZOcclusionReferenceContext &rContext,
        const T3DVisibilityOutputBuffers &pBuffers,
        const T3DVisibilityReferenceResult &pResult,
        uint32_t pFrameId ) {
	if ( rContext.pLastVisibleFrameByCluster == NULL ) {
		return;
	}
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		for ( uint32_t iVisible = 0u; iVisible < pResult.Counters.nVisibleClusterCount[iTier]; ++iVisible ) {
			const uint32_t nClusterId = pBuffers.pVisibleClusters[iTier][iVisible].nClusterId;
			if ( nClusterId < rContext.nHistoryCapacity ) {
				rContext.pLastVisibleFrameByCluster[nClusterId] = pFrameId;
			}
		}
	}
}

bool t3d_render_world_hiz_update_history_state_reference_debug(
        T3DHiZHistoryState &rState, T3DVisibilityFrame &rFrame, float pCameraCutDistance, float pMatrixCutThreshold ) {
	bool bCut = ( rFrame.nFlags & T3D_RW_VISIBILITY_FLAG_CAMERA_CUT ) != 0u;
	if ( rState.nValid != 0u ) {
		const float fDx = rFrame.vCameraPositionAndMaxDistance[0] - rState.vLastCameraPosition[0];
		const float fDy = rFrame.vCameraPositionAndMaxDistance[1] - rState.vLastCameraPosition[1];
		const float fDz = rFrame.vCameraPositionAndMaxDistance[2] - rState.vLastCameraPosition[2];
		if ( sqrtf( fDx * fDx + fDy * fDy + fDz * fDz ) > pCameraCutDistance ) {
			bCut = true;
		}
		float fMaxMatrixDelta = 0.0f;
		for ( uint32_t i = 0u; i < 16u; ++i ) {
			fMaxMatrixDelta = t3d_rw_hiz_max_f( fMaxMatrixDelta, fabsf( rFrame.mViewProjection[i] - rState.mLastViewProjection[i] ) );
		}
		if ( fMaxMatrixDelta > pMatrixCutThreshold ) {
			bCut = true;
		}
	} else {
		rState.nInvalidReason = T3D_RW_HIZ_INVALID_UNINITIALIZED;
		rState.nConservativeFramesRemaining = 1u;
	}
	if ( bCut ) {
		rFrame.nFlags |= T3D_RW_VISIBILITY_FLAG_CAMERA_CUT;
		rFrame.nHiZInvalidReason = T3D_RW_HIZ_INVALID_CAMERA_CUT;
		rState.nInvalidReason = T3D_RW_HIZ_INVALID_CAMERA_CUT;
		rState.nConservativeFramesRemaining = 0u;
	} else if ( rState.nConservativeFramesRemaining != 0u ) {
		rFrame.nFlags |= T3D_RW_VISIBILITY_FLAG_CAMERA_CUT;
		rFrame.nHiZInvalidReason = rState.nInvalidReason;
		--rState.nConservativeFramesRemaining;
	} else {
		rFrame.nFlags &= ~T3D_RW_VISIBILITY_FLAG_CAMERA_CUT;
		rFrame.nHiZInvalidReason = T3D_RW_HIZ_VALID;
		rState.nInvalidReason = T3D_RW_HIZ_VALID;
	}
	memcpy( rState.vLastCameraPosition, rFrame.vCameraPositionAndMaxDistance, sizeof( rState.vLastCameraPosition ) );
	memcpy( rState.mLastViewProjection, rFrame.mViewProjection, sizeof( rState.mLastViewProjection ) );
	rState.nLastFrameId = rFrame.nFrameId;
	rState.nValid = 1u;
	return !bCut;
}
