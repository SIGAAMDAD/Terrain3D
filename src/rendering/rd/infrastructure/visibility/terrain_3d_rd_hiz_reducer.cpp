// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "core/utility/terrain_3d_generation.h"
#include "rendering/rd/infrastructure/visibility/terrain_3d_rd_hiz_reducer.h"

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"
#include "rendering/rd/infrastructure/visibility/terrain_3d_rd_hiz_pyramid.h"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

Terrain3DRdHiZReducer::Terrain3DRdHiZReducer( Terrain3DRdContext &rContext, const t3dRdResourceOwner_t eOwner ) :
        m_context( rContext ), m_eOwner( eOwner ), m_pipelineCache( rContext, eOwner ), m_descriptorCache( rContext, eOwner ) {
}

Terrain3DRdHiZReducer::~Terrain3DRdHiZReducer( void ) {
	shutdownCold();
}

bool Terrain3DRdHiZReducer::initializeCold( const t3dRdShaderId_t shaderId,
        const String &rName,
        const uint32_t nMaxWidth,
        const uint32_t nMaxHeight,
        const uint32_t nMaxMipCount,
        const uint32_t nSourceBinding,
        const uint32_t nAuxiliaryBinding,
        const uint32_t nDestinationBinding,
        const bool bUsesAuxiliarySource,
        const uint32_t nPushBytes ) {
	if ( shaderId >= T3D_RD_SHADER_COUNT || !m_context.getDevice() || nMaxWidth == 0u || nMaxHeight == 0u || nMaxMipCount == 0u
	        || nMaxMipCount > T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT || nPushBytes == 0u ) {
		return false;
	}
	if ( m_texture.isValid() && m_shaderId == shaderId && m_nMaxWidth == nMaxWidth && m_nMaxHeight == nMaxHeight
	        && m_nMaxMipCount == nMaxMipCount && m_nSourceBinding == nSourceBinding && m_nAuxiliaryBinding == nAuxiliaryBinding
	        && m_nDestinationBinding == nDestinationBinding && m_bUsesAuxiliarySource == bUsesAuxiliarySource
	        && m_nPushBytes == nPushBytes ) {
		return true;
	}
	shutdownCold();
	m_shaderId = shaderId;
	m_name = rName;
	m_nMaxWidth = nMaxWidth;
	m_nMaxHeight = nMaxHeight;
	m_nMaxMipCount = nMaxMipCount;
	m_nSourceBinding = nSourceBinding;
	m_nAuxiliaryBinding = nAuxiliaryBinding;
	m_nDestinationBinding = nDestinationBinding;
	m_bUsesAuxiliarySource = bUsesAuxiliarySource;
	m_nPushBytes = nPushBytes;

	if ( !Terrain3DRdHiZPyramid::create( m_context, m_eOwner, m_name, m_nMaxWidth, m_nMaxHeight, m_nMaxMipCount,
	                    m_hSampler, m_texture, m_mipViews, T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT ) ) {
		shutdownCold();
		return false;
	}
	if ( !m_hShader.createTracked( m_context, m_eOwner, m_name + String( ".Reduce" ), m_shaderId, m_name + String( ".ReduceShader" ) ) ) {
		shutdownCold();
		return false;
	}
	m_hPipeline = m_pipelineCache.getOrCreateCompute( m_hShader.rid(), m_name + String( ".ReducePipeline" ), m_hShader.rid() );
	if ( !m_hPipeline.is_valid() ) {
		shutdownCold();
		return false;
	}
	for ( uint32_t iMip = 1u; iMip < m_nMaxMipCount; ++iMip ) {
		const RID &source = m_mipViews[iMip - 1u].rid();
		if ( !createReduceSetCold( iMip, source, m_bUsesAuxiliarySource ? source : RID() ) ) {
			shutdownCold();
			return false;
		}
	}
	m_pushBytes.resize( m_nPushBytes );
	m_nResourceGeneration = t3dNextGeneration( m_nResourceGeneration );
	return true;
}

bool Terrain3DRdHiZReducer::createReduceSetCold( const uint32_t nMip, const RID &rSource, const RID &rAuxiliary ) {
	if ( nMip >= m_nMaxMipCount || !rSource.is_valid() || !m_mipViews[nMip].isValid() || !m_hShader.isValid()
	        || ( m_bUsesAuxiliarySource && !rAuxiliary.is_valid() ) ) {
		return false;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( m_nSourceBinding, m_hSampler.rid(), rSource ) );
	if ( m_bUsesAuxiliarySource ) {
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( m_nAuxiliaryBinding, m_hSampler.rid(), rAuxiliary ) );
	}
	uniforms.push_back( Terrain3DRdBindingBuilder::image( m_nDestinationBinding, m_mipViews[nMip].rid() ) );
	m_hszReduceSets[nMip] = m_descriptorCache.getOrCreate(
	        { m_hShader.rid(), m_hSampler.rid(), rSource, rAuxiliary, m_mipViews[nMip].rid() },
	        m_name + String( ".ReduceSet." ) + String::num_uint64( nMip ), uniforms, m_hShader.rid(), 0u );
	return m_hszReduceSets[nMip].is_valid();
}

void Terrain3DRdHiZReducer::freeReduceSetCold( const uint32_t nMip ) {
	if ( nMip < T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT ) {
		m_descriptorCache.erase( m_hszReduceSets[nMip] );
		m_hszReduceSets[nMip] = RID();
	}
}

void Terrain3DRdHiZReducer::clearSourceBindingCold( void ) {
	freeReduceSetCold( 0u );
	m_hBorrowedSource = RID();
	m_hBorrowedAuxiliary = RID();
	m_nSourceWidth = 0u;
	m_nSourceHeight = 0u;
	m_nAuxiliaryWidth = 0u;
	m_nAuxiliaryHeight = 0u;
	m_nActiveWidth = 0u;
	m_nActiveHeight = 0u;
	m_nActiveMipCount = 0u;
	m_bAuxiliarySourceBound = false;
}

bool Terrain3DRdHiZReducer::bindSourceCold( const RID &rSource, const RID &rAuxiliary, bool &rRebound ) {
	rRebound = false;
	RenderingDevice *const rd = m_context.getDevice();
	if ( !Terrain3DRdTexture::isBorrowedValid( rd, rSource ) ) {
		clearSourceBindingCold();
		return false;
	}
	const Ref<RDTextureFormat> source_format = Terrain3DRdTexture::borrowedFormat( rd, rSource );
	if ( source_format.is_null() || source_format->get_width() == 0u || source_format->get_height() == 0u ) {
		clearSourceBindingCold();
		return false;
	}

	RID auxiliary = RID();
	uint32_t auxiliary_width = 0u;
	uint32_t auxiliary_height = 0u;
	bool auxiliary_bound = false;
	if ( m_bUsesAuxiliarySource ) {
		auxiliary = rSource;
		auxiliary_width = source_format->get_width();
		auxiliary_height = source_format->get_height();
		if ( Terrain3DRdTexture::isBorrowedValid( rd, rAuxiliary ) ) {
			const Ref<RDTextureFormat> auxiliary_format = Terrain3DRdTexture::borrowedFormat( rd, rAuxiliary );
			if ( auxiliary_format.is_valid() && auxiliary_format->get_width() > 0u && auxiliary_format->get_height() > 0u ) {
				auxiliary = rAuxiliary;
				auxiliary_width = auxiliary_format->get_width();
				auxiliary_height = auxiliary_format->get_height();
				auxiliary_bound = true;
			}
		}
	}

	m_nSourceWidth = source_format->get_width();
	m_nSourceHeight = source_format->get_height();
	m_nAuxiliaryWidth = auxiliary_width;
	m_nAuxiliaryHeight = auxiliary_height;
	m_bAuxiliarySourceBound = auxiliary_bound;
	if ( rSource == m_hBorrowedSource && auxiliary == m_hBorrowedAuxiliary && m_hszReduceSets[0].is_valid() ) {
		return true;
	}
	freeReduceSetCold( 0u );
	m_hBorrowedSource = rSource;
	m_hBorrowedAuxiliary = auxiliary;
	if ( !createReduceSetCold( 0u, rSource, auxiliary ) ) {
		clearSourceBindingCold();
		return false;
	}
	rRebound = true;
	return true;
}

bool Terrain3DRdHiZReducer::buildHot( const Vector2i &rRenderSize, t3dRdHiZPushWriterFn_t pPushWriter, void *const pPushUser ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !m_hPipeline.is_valid() || !m_hszReduceSets[0].is_valid() || m_nSourceWidth == 0u || m_nSourceHeight == 0u
	        || !pPushWriter || m_pushBytes.size() != ( int64_t )m_nPushBytes ) {
		m_nActiveMipCount = 0u;
		return false;
	}
	m_nActiveWidth = MIN( MIN( ( uint32_t )MAX( rRenderSize.x, 1 ), m_nSourceWidth ), m_nMaxWidth );
	m_nActiveHeight = MIN( MIN( ( uint32_t )MAX( rRenderSize.y, 1 ), m_nSourceHeight ), m_nMaxHeight );
	uint32_t width = m_nActiveWidth;
	uint32_t height = m_nActiveHeight;
	m_nActiveMipCount = 1u;
	while ( ( width > 1u || height > 1u ) && m_nActiveMipCount < m_nMaxMipCount ) {
		width = MAX( 1u, ( width + 1u ) >> 1u );
		height = MAX( 1u, ( height + 1u ) >> 1u );
		++m_nActiveMipCount;
	}

	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		m_nActiveMipCount = 0u;
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_hPipeline );
	uint32_t source_width = m_nSourceWidth;
	uint32_t source_height = m_nSourceHeight;
	uint32_t destination_width = m_nActiveWidth;
	uint32_t destination_height = m_nActiveHeight;
	for ( uint32_t iMip = 0u; iMip < m_nActiveMipCount; ++iMip ) {
		t3dRdHiZDispatchInfo_t info{};
		info.m_nMip = iMip;
		info.m_nSourceWidth = source_width;
		info.m_nSourceHeight = source_height;
		info.m_nAuxiliaryWidth = iMip == 0u ? m_nAuxiliaryWidth : source_width;
		info.m_nAuxiliaryHeight = iMip == 0u ? m_nAuxiliaryHeight : source_height;
		info.m_nDestinationWidth = destination_width;
		info.m_nDestinationHeight = destination_height;
		info.m_bAuxiliarySourceBound = iMip == 0u && m_bAuxiliarySourceBound;
		pPushWriter( pPushUser, info, m_pushBytes.ptrw(), m_nPushBytes );
		rd->compute_list_bind_uniform_set( list, m_hszReduceSets[iMip], 0u );
		rd->compute_list_set_push_constant( list, m_pushBytes, m_nPushBytes );
		rd->compute_list_dispatch( list, ( destination_width + 7u ) / 8u, ( destination_height + 7u ) / 8u, 1u );
		++m_nBuildDispatches;
		rd->compute_list_add_barrier( list );
		source_width = destination_width;
		source_height = destination_height;
		destination_width = MAX( 1u, ( destination_width + 1u ) >> 1u );
		destination_height = MAX( 1u, ( destination_height + 1u ) >> 1u );
	}
	rd->compute_list_end();
	return true;
}

void Terrain3DRdHiZReducer::shutdownCold( void ) {
	for ( uint32_t iMip = 0u; iMip < T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT; ++iMip ) {
		freeReduceSetCold( iMip );
	}
	m_descriptorCache.clear();
	m_pipelineCache.clear();
	m_hPipeline = RID();
	m_hShader.releaseTracked( m_context );
	Terrain3DRdHiZPyramid::release( m_context, m_hSampler, m_texture, m_mipViews, T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT );
	m_shaderId = T3D_RD_SHADER_COUNT;
	m_pushBytes.clear();
	m_name = String();
	m_nMaxWidth = 0u;
	m_nMaxHeight = 0u;
	m_nMaxMipCount = 0u;
	m_nSourceBinding = 0u;
	m_nAuxiliaryBinding = 0u;
	m_nDestinationBinding = 1u;
	m_nPushBytes = 0u;
	m_bUsesAuxiliarySource = false;
	clearSourceBindingCold();
}

uint64_t Terrain3DRdHiZReducer::getGpuBytes( void ) const {
	uint64_t bytes = 0u;
	uint32_t width = m_nMaxWidth;
	uint32_t height = m_nMaxHeight;
	for ( uint32_t iMip = 0u; iMip < m_nMaxMipCount; ++iMip ) {
		bytes += ( uint64_t )width * ( uint64_t )height * 4u;
		width = MAX( 1u, ( width + 1u ) >> 1u );
		height = MAX( 1u, ( height + 1u ) >> 1u );
	}
	return bytes;
}
