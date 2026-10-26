// Copyright 2026 Terrain3D Contributors.
#ifndef TERRAIN3D_RD_SCREEN_SPACE_PILOT_H
#define TERRAIN3D_RD_SCREEN_SPACE_PILOT_H

#include "rendering/rd/core/terrain_3d_rd_types.h"
#include <stdint.h>

// BR17: this contract is deliberately separate from the borrowed Forward+
// framebuffer ABI. A successful shader compile does not authorize a cutover.
#define T3D_BR23_COVERAGE_COLOR_COUNT 5u
#define T3D_BR23_COVERAGE_FLOAT_CHANNELS 4u
#define T3D_BR23_COVERAGE_BYTES_PER_PIXEL 80u
#define T3D_BR23_COVERAGE_CLEAR_REGION -2.0f
#define T3D_BR23_MAX_PILOT_WIDTH 1920u
#define T3D_BR23_MAX_PILOT_HEIGHT 1080u
#define T3D_BR23_MAX_PILOT_BYTES ( 1920u * 1080u * 96u )

typedef enum t3dBr23RejectBits_e
{
	T3D_BR23_REJECT_NONE = 0u,
	T3D_BR23_REJECT_PASS = 1u << 0u,
	T3D_BR23_REJECT_MULTIVIEW = 1u << 1u,
	T3D_BR23_REJECT_MSAA = 1u << 2u,
	T3D_BR23_REJECT_VIEWPORT = 1u << 3u,
	T3D_BR23_REJECT_MOTION = 1u << 4u,
	T3D_BR23_REJECT_NATIVE_ATTACHMENTS = 1u << 5u,
	T3D_BR23_REJECT_LIGHTING = 1u << 6u,
	T3D_BR23_REJECT_PROOF = 1u << 7u,
} t3dBr23RejectBits_t;

typedef struct t3dBr23CutoverProof_s
{
	bool m_bOwnedFiveColorAttachments = false;
	bool m_bOwnedReverseZDepth = false;
	bool m_bRasterGeometryCoverageExact = false;
	bool m_bRasterGradientsRecorded = false;
	bool m_bNativeAttachmentOrderingVerified = false;
	bool m_bV2FeedbackParityVerified = false;
	bool m_bMotionDnrAndAlphaParityVerified = false;
	bool m_bComposedMaterialRevisionCurrent = false;
	bool m_bTransparentOrderingPreserved = false;
	bool m_bGpuParityVerified = false;
} t3dBr23CutoverProof_t;

// BR17 first stage: no native attachment is borrowed across callback lifetime.
// Single view/sample/zero viewport origin and color pass are initially the
// only legal prototype workloads. The old raster path remains authoritative.
inline uint32_t t3dBr23PilotPreflight(
	const t3dRdForwardPassContext_t &rPass,
	const t3dBr23CutoverProof_t &rProof )
{
	uint32_t result = T3D_BR23_REJECT_NONE;
	if ( rPass.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE || rPass.m_bReflectionProbeRender )
		result |= T3D_BR23_REJECT_PASS;
	if ( rPass.m_nViewCount != 1u ) result |= T3D_BR23_REJECT_MULTIVIEW;
	if ( rPass.m_nSampleCount != 1u ) result |= T3D_BR23_REJECT_MSAA;
	if ( rPass.m_renderSize.x <= 0 || rPass.m_renderSize.y <= 0 ||
		 rPass.m_renderSize.x > int32_t( T3D_BR23_MAX_PILOT_WIDTH ) ||
		 rPass.m_renderSize.y > int32_t( T3D_BR23_MAX_PILOT_HEIGHT ) ||
		 rPass.m_renderRegion.position.x != 0 || rPass.m_renderRegion.position.y != 0 ||
		 rPass.m_renderRegion.size != rPass.m_renderSize ) result |= T3D_BR23_REJECT_VIEWPORT;
	if ( rPass.m_bMotionVectorsEnabled ) result |= T3D_BR23_REJECT_MOTION;
	if ( !rPass.m_hNativeFramebuffer.is_valid() ) result |= T3D_BR23_REJECT_NATIVE_ATTACHMENTS;
	if ( !rPass.m_hDirectionalLightBuffer.is_valid() || !rPass.m_hClusterBuffer.is_valid() )
		result |= T3D_BR23_REJECT_LIGHTING;
	if ( !rProof.m_bOwnedFiveColorAttachments || !rProof.m_bOwnedReverseZDepth ||
		 !rProof.m_bRasterGeometryCoverageExact || !rProof.m_bRasterGradientsRecorded ||
		 !rProof.m_bNativeAttachmentOrderingVerified || !rProof.m_bV2FeedbackParityVerified ||
		 !rProof.m_bMotionDnrAndAlphaParityVerified || !rProof.m_bComposedMaterialRevisionCurrent ||
		 !rProof.m_bTransparentOrderingPreserved || !rProof.m_bGpuParityVerified )
		result |= T3D_BR23_REJECT_PROOF;
	return result;
}

// Storage footprint of the five color targets, excluding depth, alignment, and auxiliary GPU copies.
inline uint64_t t3dScreenSpacePilotColorBytes( const Vector2i &rSize ) {
	if ( rSize.x <= 0 || rSize.y <= 0 ) return 0u;
	return uint64_t( rSize.x ) * uint64_t( rSize.y ) * T3D_BR23_COVERAGE_BYTES_PER_PIXEL;
}

static_assert( T3D_BR23_COVERAGE_COLOR_COUNT * T3D_BR23_COVERAGE_FLOAT_CHANNELS * sizeof( float ) ==
	T3D_BR23_COVERAGE_BYTES_PER_PIXEL, "BR17 screen-space G-buffer contract changed" );

#endif // TERRAIN3D_RD_SCREEN_SPACE_PILOT_H
