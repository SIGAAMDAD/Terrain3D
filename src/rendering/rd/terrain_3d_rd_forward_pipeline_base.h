// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_FORWARD_PIPELINE_BASE_H
#define TERRAIN3D_RD_FORWARD_PIPELINE_BASE_H

#include "rendering/rd/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/typed_array.hpp>

typedef struct t3dRdForwardPipelineBaseKey_s {
	int64_t m_nFramebufferFormat = RenderingDevice::INVALID_ID;
	t3dRdForwardShaderVariant_t m_variant = T3D_RD_FORWARD_SHADER_COLOR;
	t3dRdForwardPassKind_t m_passKind = T3D_RD_FORWARD_PASS_INVALID;
	RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
	uint32_t m_nViewCount = 1u;
	bool m_bReverseCull = false;
	bool m_bDepthPrepassActive = false;

	bool operator==( const t3dRdForwardPipelineBaseKey_s &rOther ) const {
		return m_nFramebufferFormat == rOther.m_nFramebufferFormat && m_variant == rOther.m_variant && m_passKind == rOther.m_passKind
		        && m_samples == rOther.m_samples && m_nViewCount == rOther.m_nViewCount && m_bReverseCull == rOther.m_bReverseCull
		        && m_bDepthPrepassActive == rOther.m_bDepthPrepassActive;
	}
} t3dRdForwardPipelineBaseKey_t;

typedef struct t3dRdForwardPipelineStateOptions_s {
	decltype( RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE ) m_frontFace = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
	RenderingDevice::PolygonCullMode m_cullMode = RenderingDevice::POLYGON_CULL_DISABLED;
	uint32_t m_nBlendAttachmentCount = 0u;
	bool m_bBlendFirstAttachment = false;
	bool m_bEnableDepthTest = true;
	bool m_bEnableDepthWrite = true;
	decltype( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL ) m_depthCompare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
} t3dRdForwardPipelineStateOptions_t;

typedef struct t3dRdForwardPipelineStates_s {
	Ref<RDPipelineRasterizationState> m_raster;
	Ref<RDPipelineMultisampleState> m_multisample;
	Ref<RDPipelineDepthStencilState> m_depth;
	Ref<RDPipelineColorBlendState> m_blend;
} t3dRdForwardPipelineStates_t;

EA_FORCE_INLINE bool t3dRdCompileForwardPipelineBaseKey( RenderingDevice *const pRd,
        const t3dRdForwardPassContext_t &rContext,
        const t3dRdForwardShaderVariant_t variant,
        const bool bDepthPrepassOnMotion,
        t3dRdForwardPipelineBaseKey_t &rKey,
        const bool bValidateSampleCount = true ) {
	if ( !pRd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	rKey.m_nFramebufferFormat = rContext.m_nFramebufferFormat;
	rKey.m_variant = variant;
	rKey.m_passKind = rContext.m_passKind;
	rKey.m_samples = pRd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u );
	if ( bValidateSampleCount && ( 1u << ( uint32_t )( rKey.m_samples ) ) != rContext.m_nSampleCount ) {
		return false;
	}
	rKey.m_nViewCount = rContext.m_nViewCount;
	rKey.m_bReverseCull = rContext.m_bReverseCull;
	rKey.m_bDepthPrepassActive = ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE
	        || ( bDepthPrepassOnMotion && rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) )
	        && rContext.m_bDepthPrepassActive;
	return true;
}

EA_FORCE_INLINE uint32_t t3dRdForwardBlendAttachmentCount(
        const t3dRdForwardShaderVariant_t variant, const bool bTransparentPass ) {
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

EA_FORCE_INLINE bool t3dRdBuildForwardRasterState( const t3dRdForwardPipelineStateOptions_t &rOptions,
        Ref<RDPipelineRasterizationState> &rRaster ) {
	rRaster.instantiate();
	if ( rRaster.is_null() ) {
		return false;
	}
	rRaster->set_front_face( rOptions.m_frontFace );
	rRaster->set_cull_mode( rOptions.m_cullMode );
	return true;
}

EA_FORCE_INLINE bool t3dRdBuildForwardSharedPipelineStates( const t3dRdForwardPipelineBaseKey_t &rKey,
        const t3dRdForwardPipelineStateOptions_t &rOptions,
        Ref<RDPipelineMultisampleState> &rMultisample,
        Ref<RDPipelineDepthStencilState> &rDepth,
        Ref<RDPipelineColorBlendState> &rBlend ) {
	rMultisample.instantiate();
	rDepth.instantiate();
	rBlend.instantiate();
	if ( rMultisample.is_null() || rDepth.is_null() || rBlend.is_null() ) {
		return false;
	}
	rMultisample->set_sample_count( rKey.m_samples );
	rDepth->set_enable_depth_test( rOptions.m_bEnableDepthTest );
	rDepth->set_enable_depth_write( rOptions.m_bEnableDepthWrite );
	rDepth->set_depth_compare_operator( rOptions.m_depthCompare );
	if ( rOptions.m_nBlendAttachmentCount > 0u ) {
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
		for ( uint32_t i = 0u; i < rOptions.m_nBlendAttachmentCount; ++i ) {
			Ref<RDPipelineColorBlendStateAttachment> attachment;
			attachment.instantiate();
			if ( attachment.is_null() ) {
				return false;
			}
			const bool blend = rOptions.m_bBlendFirstAttachment && i == 0u;
			attachment->set_enable_blend( blend );
			if ( blend ) {
				attachment->set_src_color_blend_factor( RenderingDevice::BLEND_FACTOR_SRC_ALPHA );
				attachment->set_dst_color_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
				attachment->set_src_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE );
				attachment->set_dst_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
			}
			attachments.push_back( attachment );
		}
		rBlend->set_attachments( attachments );
	}
	return true;
}

EA_FORCE_INLINE bool t3dRdBuildForwardPipelineStates( const t3dRdForwardPipelineBaseKey_t &rKey,
        const t3dRdForwardPipelineStateOptions_t &rOptions,
        t3dRdForwardPipelineStates_t &rStates ) {
	rStates = t3dRdForwardPipelineStates_t{};
	return t3dRdBuildForwardRasterState( rOptions, rStates.m_raster )
	        && t3dRdBuildForwardSharedPipelineStates( rKey, rOptions, rStates.m_multisample, rStates.m_depth, rStates.m_blend );
}

#endif // TERRAIN3D_RD_FORWARD_PIPELINE_BASE_H
