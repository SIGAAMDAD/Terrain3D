#ifndef TERRAIN3D_RD_HIZ_PYRAMID_H
#define TERRAIN3D_RD_HIZ_PYRAMID_H

#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>

// Common R32F pyramid lifecycle. Reduction shaders remain domain specific.
// Callers release their descriptor sets before releasing these mip views.
class Terrain3DRdHiZPyramid
{
public:
	static void release( Terrain3DRdContext &rContext,
	        RID &rSampler,
	        RID &rTexture,
	        RID *pMipViews,
	        uint32_t nViewCapacity ) {
		if ( pMipViews ) {
			for ( uint32_t i = 0u; i < nViewCapacity; ++i ) {
				rContext.freeResource( pMipViews[i] );
			}
		}
		rContext.freeResource( rTexture );
		rContext.freeResource( rSampler );
	}

	static bool create( Terrain3DRdContext &rContext,
	        Terrain3DRdContext::ResourceOwner owner,
	        const String &rName,
	        uint32_t nWidth,
	        uint32_t nHeight,
	        uint32_t nMipCount,
	        RID &rSampler,
	        RID &rTexture,
	        RID *pMipViews,
	        uint32_t nViewCapacity ) {
		if ( !rContext.getDevice() || nWidth == 0u || nHeight == 0u || nMipCount == 0u
		        || nMipCount > nViewCapacity || !pMipViews || rSampler.is_valid() || rTexture.is_valid() ) {
			return false;
		}
		const Terrain3DRdResourceService resources( rContext, owner );
		Ref<RDSamplerState> sampler_state;
		sampler_state.instantiate();
		sampler_state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler_state->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler_state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler_state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler_state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler_state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		if ( !resources.create( rSampler, rName + String( ".Sampler" ), Terrain3DRdContext::RESOURCE_SAMPLER,
		                    Terrain3DRdContext::MEMORY_NONE, 0u,
		                    [&]( RenderingDevice *pRd ) { return pRd->sampler_create( sampler_state ); } ) ) {
			return false;
		}
		Ref<RDTextureFormat> format;
		format.instantiate();
		format->set_width( nWidth );
		format->set_height( nHeight );
		format->set_depth( 1u );
		format->set_array_layers( 1u );
		format->set_mipmaps( nMipCount );
		format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
		format->set_format( RenderingDevice::DATA_FORMAT_R32_SFLOAT );
		format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT
		        | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
		Ref<RDTextureView> view;
		view.instantiate();
		const uint64_t texture_bytes = ( uint64_t )nWidth * nHeight * 4u * 4u / 3u;
		if ( !resources.create( rTexture, rName + String( ".Texture" ), Terrain3DRdContext::RESOURCE_TEXTURE,
		                    Terrain3DRdContext::MEMORY_SCRATCH, texture_bytes,
		                    [&]( RenderingDevice *pRd ) { return pRd->texture_create( format, view ); } ) ) {
			release( rContext, rSampler, rTexture, pMipViews, nViewCapacity );
			return false;
		}
		for ( uint32_t iMip = 0u; iMip < nMipCount; ++iMip ) {
			if ( !resources.create( pMipViews[iMip], rName + String( ".Mip." ) + String::num_uint64( iMip ),
			                    Terrain3DRdContext::RESOURCE_TEXTURE, Terrain3DRdContext::MEMORY_NONE, 0u,
			                    [&]( RenderingDevice *pRd ) {
			                    return pRd->texture_create_shared_from_slice( view, rTexture, 0u, iMip, 1u,
			                            RenderingDevice::TEXTURE_SLICE_2D );
			                    } ) ) {
				release( rContext, rSampler, rTexture, pMipViews, nViewCapacity );
				return false;
			}
		}
		return true;
	}
};

#endif // TERRAIN3D_RD_HIZ_PYRAMID_H
