// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_HIZ_H
#define TERRAIN3D_RENDER_WORLD_HIZ_H

#include "rendering/rd/render_world/terrain_3d_render_world_visibility.h"

#include <stdint.h>

static const uint32_t T3D_RW_HIZ_MAX_MIPS = 16u;

enum T3DHiZInvalidReason : uint32_t {
	T3D_RW_HIZ_VALID = 0,
	T3D_RW_HIZ_INVALID_UNINITIALIZED,
	T3D_RW_HIZ_INVALID_NO_PREVIOUS_DEPTH,
	T3D_RW_HIZ_INVALID_CAMERA_CUT,
	T3D_RW_HIZ_INVALID_EXPLICIT_DISABLE,
	T3D_RW_HIZ_INVALID_DIMENSIONS
};

struct T3DHiZPyramidReference {
	float *pDepth;
	uint32_t nDepthCapacity;
	uint32_t nWidth[T3D_RW_HIZ_MAX_MIPS];
	uint32_t nHeight[T3D_RW_HIZ_MAX_MIPS];
	uint32_t nOffset[T3D_RW_HIZ_MAX_MIPS];
	uint32_t nMipCount;
	uint32_t nDepthCount;
	uint32_t nValid;
};

struct T3DHiZHistoryState {
	float vLastCameraPosition[3];
	float mLastViewProjection[16];
	uint32_t nLastFrameId;
	uint32_t nValid;
	uint32_t nInvalidReason;
	uint32_t nConservativeFramesRemaining;
};

struct T3DHiZOcclusionReferenceContext {
	const T3DHiZPyramidReference *pPyramid;
	uint32_t *pLastVisibleFrameByCluster;
	uint32_t nHistoryCapacity;
	uint32_t nHysteresisFrames;
	float fDepthBias;
};

bool t3d_render_world_hiz_initialize_reference_debug( T3DHiZPyramidReference &rPyramid,
        float *pDepthStorage,
        uint32_t pDepthCapacity,
        uint32_t pWidth,
        uint32_t pHeight,
        uint32_t pMipCount );

bool t3d_render_world_hiz_build_reverse_z_reference_debug(
        T3DHiZPyramidReference &rPyramid, const float *pSourceDepth, uint32_t pSourceWidth, uint32_t pSourceHeight );

bool t3d_render_world_hiz_is_occluded_reference_debug(
        const T3DHiZPyramidReference &pPyramid, const T3DVisibilityFrame &pFrame, const float pCenter[3], float pRadius, float pDepthBias );

bool t3d_render_world_hiz_visibility_occlusion_callback_reference_debug( const void *pContext,
        const T3DVisibilityFrame &pFrame,
        const float pCenter[3],
        float pRadius,
        uint32_t pClusterId,
        uint32_t pInstanceId );

void t3d_render_world_hiz_mark_visible_reference_debug( T3DHiZOcclusionReferenceContext &rContext,
        const T3DVisibilityOutputBuffers &pBuffers,
        const T3DVisibilityReferenceResult &pResult,
        uint32_t pFrameId );

bool t3d_render_world_hiz_update_history_state_reference_debug(
        T3DHiZHistoryState &rState, T3DVisibilityFrame &rFrame, float pCameraCutDistance, float pMatrixCutThreshold );

#endif // TERRAIN3D_RENDER_WORLD_HIZ_H
