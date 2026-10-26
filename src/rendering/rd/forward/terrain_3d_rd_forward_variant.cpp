#include "rendering/rd/forward/terrain_3d_rd_forward_variant.h"

bool t3dRdSelectForwardVariant( const t3dRdForwardPassContext_t &rContext,
        const uint32_t nCapabilities,
        t3dRdForwardShaderVariant_t &rVariant ) {
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		if ( ( nCapabilities & T3D_RD_FORWARD_VARIANT_SHADOW_ANY_NATIVE_MODE ) == 0u
		        && rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_SHADOW ) {
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
	if ( ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION
	             && !( transparent_pass && ( nCapabilities & T3D_RD_FORWARD_VARIANT_ALLOW_TRANSPARENT ) != 0u ) )
	        || rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR ) {
		return false;
	}
	const bool specular = !transparent_pass && ( rContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool motion = !transparent_pass
	        && ( rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || ( rContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u );
	const bool radiance_array = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( radiance_array ) {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY
		                               : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}
