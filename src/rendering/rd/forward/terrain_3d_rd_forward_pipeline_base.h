// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_FORWARD_PIPELINE_BASE_H
#define TERRAIN3D_RD_FORWARD_PIPELINE_BASE_H

#include "rendering/rd/core/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/typed_array.hpp>

typedef struct t3dRdForwardPipelineBaseKey_s
{
	int64_t							m_nFramebufferFormat = RenderingDevice::INVALID_ID;
	t3dRdForwardShaderVariant_t		m_variant = T3D_RD_FORWARD_SHADER_COLOR;
	t3dRdForwardPassKind_t			m_passKind = T3D_RD_FORWARD_PASS_INVALID;
	RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
	uint32_t						m_nViewCount = 1u;
	bool							m_bReverseCull = false;
	bool							m_bDepthPrepassActive = false;

	bool operator==(
		const t3dRdForwardPipelineBaseKey_s &rOther ) const {
		return m_nFramebufferFormat == rOther.m_nFramebufferFormat && m_variant == rOther.m_variant &&
			   m_passKind == rOther.m_passKind && m_samples == rOther.m_samples &&
			   m_nViewCount == rOther.m_nViewCount && m_bReverseCull == rOther.m_bReverseCull &&
			   m_bDepthPrepassActive == rOther.m_bDepthPrepassActive;
	}
} t3dRdForwardPipelineBaseKey_t;

typedef struct t3dRdForwardPipelineStateOptions_s
{
	decltype( RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE ) m_frontFace =
		RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
	RenderingDevice::PolygonCullMode						 m_cullMode = RenderingDevice::POLYGON_CULL_DISABLED;
	uint32_t												 m_nBlendAttachmentCount = 0u;
	bool													 m_bBlendFirstAttachment = false;
	bool													 m_bColorWrite = true;
	bool													 m_bEnableDepthTest = true;
	bool													 m_bEnableDepthWrite = true;
	decltype( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL ) m_depthCompare =
		RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
} t3dRdForwardPipelineStateOptions_t;

typedef struct t3dRdForwardPipelineStates_s
{
	Ref< RDPipelineRasterizationState > m_raster;
	Ref< RDPipelineMultisampleState >	m_multisample;
	Ref< RDPipelineDepthStencilState >	m_depth;
	Ref< RDPipelineColorBlendState >	m_blend;
} t3dRdForwardPipelineStates_t;

bool t3dRdCompileForwardPipelineBaseKey( RenderingDevice *const			   pRd,
										 const t3dRdForwardPassContext_t  &rContext,
										 const t3dRdForwardShaderVariant_t variant,
										 const bool						   bDepthPrepassOnMotion,
										 t3dRdForwardPipelineBaseKey_t	  &rKey,
										 const bool						   bValidateSampleCount = true );

EA_FORCE_INLINE uint32_t t3dRdForwardBlendAttachmentCount(
	const t3dRdForwardShaderVariant_t variant,
	const bool						  bTransparentPass ) {
	if ( bTransparentPass || variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
		return 3u;
	}
	if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS ) {
		return 1u;
	}
	if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ) {
		return 2u;
	}
	return 0u;
}

bool t3dRdBuildForwardRasterState( const t3dRdForwardPipelineStateOptions_t &rOptions,
								   Ref< RDPipelineRasterizationState >		&rRaster );

bool t3dRdBuildForwardSharedPipelineStates( const t3dRdForwardPipelineBaseKey_t		 &rKey,
											const t3dRdForwardPipelineStateOptions_t &rOptions,
											Ref< RDPipelineMultisampleState >		 &rMultisample,
											Ref< RDPipelineDepthStencilState >		 &rDepth,
											Ref< RDPipelineColorBlendState >		 &rBlend );

bool t3dRdBuildForwardPipelineStates( const t3dRdForwardPipelineBaseKey_t	   &rKey,
									  const t3dRdForwardPipelineStateOptions_t &rOptions,
									  t3dRdForwardPipelineStates_t			   &rStates );

#endif // TERRAIN3D_RD_FORWARD_PIPELINE_BASE_H
