#include "rendering/rd/forward/terrain_3d_rd_forward_variant.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"

bool t3dRdCompileForwardPipelineBaseKey(
	RenderingDevice *const			  pRd,
	const t3dRdForwardPassContext_t	 &rContext,
	const t3dRdForwardShaderVariant_t variant,
	const bool						  bDepthPrepassOnMotion,
	t3dRdForwardPipelineBaseKey_t	 &rKey,
	const bool						  bValidateSampleCount ) {
	if ( !pRd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	rKey.m_nFramebufferFormat = rContext.m_nFramebufferFormat;
	rKey.m_variant = variant;
	rKey.m_passKind = rContext.m_passKind;
	rKey.m_samples = pRd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u );
	if ( bValidateSampleCount && ( 1u << ( uint32_t ) ( rKey.m_samples ) ) != rContext.m_nSampleCount ) {
		return false;
	}
	rKey.m_nViewCount = rContext.m_nViewCount;
	rKey.m_bReverseCull = rContext.m_bReverseCull;
	rKey.m_bDepthPrepassActive = ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
								   ( bDepthPrepassOnMotion && rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) ) &&
								 rContext.m_bDepthPrepassActive;
	return true;
}

bool t3dRdBuildForwardRasterState(
	const t3dRdForwardPipelineStateOptions_t &rOptions,
	Ref< RDPipelineRasterizationState >		 &rRaster ) {
	rRaster.instantiate();
	if ( rRaster.is_null() ) {
		return false;
	}
	rRaster->set_front_face( rOptions.m_frontFace );
	rRaster->set_cull_mode( rOptions.m_cullMode );
	return true;
}

bool t3dRdBuildForwardSharedPipelineStates(
	const t3dRdForwardPipelineBaseKey_t		 &rKey,
	const t3dRdForwardPipelineStateOptions_t &rOptions,
	Ref< RDPipelineMultisampleState >		 &rMultisample,
	Ref< RDPipelineDepthStencilState >		 &rDepth,
	Ref< RDPipelineColorBlendState >		 &rBlend ) {
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
		TypedArray< Ref< RDPipelineColorBlendStateAttachment > > attachments;
		for ( uint32_t i = 0u; i < rOptions.m_nBlendAttachmentCount; ++i ) {
			Ref< RDPipelineColorBlendStateAttachment > attachment;
			attachment.instantiate();
			if ( attachment.is_null() ) {
				return false;
			}
			const bool blend = rOptions.m_bBlendFirstAttachment && i == 0u;
			attachment->set_enable_blend( blend );
			attachment->set_write_r( rOptions.m_bColorWrite );
			attachment->set_write_g( rOptions.m_bColorWrite );
			attachment->set_write_b( rOptions.m_bColorWrite );
			attachment->set_write_a( rOptions.m_bColorWrite );
			if ( blend ) {
				attachment->set_src_color_blend_factor( RenderingDevice::BLEND_FACTOR_SRC_ALPHA );
				attachment->set_dst_color_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
				attachment->set_color_blend_op( RenderingDevice::BLEND_OP_ADD );
				attachment->set_src_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE );
				attachment->set_dst_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
				attachment->set_alpha_blend_op( RenderingDevice::BLEND_OP_ADD );
			}
			attachments.push_back( attachment );
		}
		rBlend->set_attachments( attachments );
	}
	return true;
}

bool t3dRdBuildForwardPipelineStates(
	const t3dRdForwardPipelineBaseKey_t		 &rKey,
	const t3dRdForwardPipelineStateOptions_t &rOptions,
	t3dRdForwardPipelineStates_t			 &rStates ) {
	rStates = t3dRdForwardPipelineStates_t{};
	return t3dRdBuildForwardRasterState( rOptions, rStates.m_raster ) &&
		   t3dRdBuildForwardSharedPipelineStates( rKey,
												  rOptions,
												  rStates.m_multisample,
												  rStates.m_depth,
												  rStates.m_blend );
}

// -------- Consolidated from rendering/rd/forward/terrain_3d_rd_forward_variant.cpp --------
bool t3dRdSelectForwardVariant(
	const t3dRdForwardPassContext_t &rContext,
	const uint32_t					 nCapabilities,
	t3dRdForwardShaderVariant_t		&rVariant ) {
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		if ( ( nCapabilities & T3D_RD_FORWARD_VARIANT_SHADOW_ANY_NATIVE_MODE ) == 0u &&
			 rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_SHADOW ) {
			return false;
		}
		rVariant = T3D_RD_FORWARD_SHADER_DEPTH;
		return true;
	}
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
		switch ( rContext.m_nativePassMode ) {
		case T3D_RD_FORWARD_NATIVE_PASS_DEPTH:
			rVariant = T3D_RD_FORWARD_SHADER_DEPTH;
			return true;
		case T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS:
			rVariant = T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS;
			return true;
		case T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI:
			rVariant = T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI;
			return true;
		default:
			return false;
		}
	}
	const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
	if ( ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION &&
		   !( transparent_pass && ( nCapabilities & T3D_RD_FORWARD_VARIANT_ALLOW_TRANSPARENT ) != 0u ) ) ||
		 rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR ) {
		return false;
	}
	const bool specular = !transparent_pass && ( rContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool motion = !transparent_pass && ( rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ||
											   ( rContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u );
	const bool radiance_array = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( radiance_array ) {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY
									   : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
							: ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY
									   : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = specular
					   ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
					   : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}
