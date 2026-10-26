#include "rendering/rd/infrastructure/visibility/terrain_3d_rd_hiz_pyramid.h"

void Terrain3DRdHiZPyramid::release( Terrain3DRdContext &rContext,
	        Terrain3DRdSampler &rSampler,
	        Terrain3DRdTexture &rTexture,
	        Terrain3DRdTexture *pMipViews,
	        uint32_t nViewCapacity ) {
		if ( pMipViews ) {
			for ( uint32_t i = 0u; i < nViewCapacity; ++i ) {
				pMipViews[i].releaseTracked( rContext );
			}
		}
		rTexture.releaseTracked( rContext );
		rSampler.releaseTracked( rContext );
	}

bool Terrain3DRdHiZPyramid::create( Terrain3DRdContext &rContext,
	        t3dRdResourceOwner_t owner,
	        const String &rName,
	        uint32_t nWidth,
	        uint32_t nHeight,
	        uint32_t nMipCount,
	        Terrain3DRdSampler &rSampler,
	        Terrain3DRdTexture &rTexture,
	        Terrain3DRdTexture *pMipViews,
	        uint32_t nViewCapacity ) {
		if ( !rContext.getDevice() || nWidth == 0u || nHeight == 0u || nMipCount == 0u
		        || nMipCount > nViewCapacity || !pMipViews || rSampler.isValid() || rTexture.isValid() ) {
			return false;
		}
		Ref<RDSamplerState> sampler_state;
		sampler_state.instantiate();
		sampler_state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler_state->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler_state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler_state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler_state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler_state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		if ( !rSampler.createTracked( rContext, owner, rName + String( ".Sampler" ), sampler_state ) ) {
			return false;
		}

		t3dRdTextureSpec_t spec;
		spec.m_name = rName + String( ".Texture" );
		spec.m_owner = owner;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_SCRATCH;
		spec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R32_SFLOAT;
		spec.m_layout.m_type = RenderingDevice::TEXTURE_TYPE_2D;
		spec.m_layout.m_nWidth = nWidth;
		spec.m_layout.m_nHeight = nHeight;
		spec.m_layout.m_nLayers = 1u;
		spec.m_layout.m_nMipmaps = nMipCount;
		spec.m_layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT
		        | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		spec.m_layout.m_nBytes = ( uint64_t )nWidth * nHeight * 4u * 4u / 3u;
		if ( !rTexture.createTracked( rContext, spec ) ) {
			release( rContext, rSampler, rTexture, pMipViews, nViewCapacity );
			return false;
		}
		for ( uint32_t iMip = 0u; iMip < nMipCount; ++iMip ) {
			t3dRdTextureSliceSpec_t slice;
			slice.m_name = rName + String( ".Mip." ) + String::num_uint64( iMip );
			slice.m_owner = owner;
			slice.m_memory = T3D_RD_CONTEXT_MEMORY_NONE;
			slice.m_nMipmap = iMip;
			if ( !pMipViews[iMip].createSharedTracked( rContext, rTexture, slice ) ) {
				release( rContext, rSampler, rTexture, pMipViews, nViewCapacity );
				return false;
			}
		}
		return true;
	}
