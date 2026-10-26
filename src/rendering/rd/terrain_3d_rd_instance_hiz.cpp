// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_hiz_pyramid.h"
#include "rendering/rd/terrain_3d_rd_instance_hiz.h"
#include "rendering/rd/terrain_3d_rd_shader_compile_status.h"

#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <string.h>

using namespace godot;

Terrain3DRdInstanceHiZ::Terrain3DRdInstanceHiZ( Terrain3DRdContext &rContext ) :
        m_context( rContext ), m_pipelineCache( rContext, Terrain3DRdContext::OWNER_INSTANCES ),
        m_descriptorCache( rContext, Terrain3DRdContext::OWNER_INSTANCES ) {
}

Terrain3DRdInstanceHiZ::~Terrain3DRdInstanceHiZ( void ) {
	shutdown();
}

bool Terrain3DRdInstanceHiZ::createPersistentResources( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	if ( !Terrain3DRdHiZPyramid::create( m_context, Terrain3DRdContext::OWNER_INSTANCES, "T3D.Instance.HiZ",
	                    T3D_INSTANCE_HIZ_MAX_DIMENSION, T3D_INSTANCE_HIZ_MAX_DIMENSION, T3D_INSTANCE_HIZ_MAX_MIPS,
	                    m_hSampler, m_hTexture, m_hszMipViews, T3D_INSTANCE_HIZ_MAX_MIPS ) ) {
		return false;
	}
	const Terrain3DRdResourceService resources( m_context, Terrain3DRdContext::OWNER_INSTANCES );
	m_hShader = t3dRdCreateShaderWithStatus( rd, m_sourceShader, "T3D.Instance.HiZ.Reduce" );
	if ( !resources.adopt( rd, m_hShader, "T3D.Instance.HiZ.ReduceShader", Terrain3DRdContext::RESOURCE_SHADER ) ) {
		return false;
	}
	m_hPipeline = m_pipelineCache.getOrCreateCompute( m_hShader, "T3D.Instance.HiZ.ReducePipeline", m_hShader );
	if ( !m_hPipeline.is_valid() ) {
		return false;
	}
	for ( uint32_t iMip = 1u; iMip < T3D_INSTANCE_HIZ_MAX_MIPS; ++iMip ) {
		if ( !createReduceSet( iMip, m_hszMipViews[iMip - 1u], m_hszMipViews[iMip - 1u] ) ) {
			return false;
		}
	}
	m_pushBytes.resize( sizeof( t3dInstanceHiZReducePush_t ) );
	return true;
}

bool Terrain3DRdInstanceHiZ::createReduceSet( const uint32_t nMip, const RID &rSource, const RID &rProxySource ) {
	if ( nMip >= T3D_INSTANCE_HIZ_MAX_MIPS || !rSource.is_valid() || !rProxySource.is_valid() || !m_hszMipViews[nMip].is_valid()
	        || !m_hShader.is_valid() ) {
		return false;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 0, m_hSampler, rSource ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 1, m_hSampler, rProxySource ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::image( 2, m_hszMipViews[nMip] ) );
	m_hszReduceSets[nMip] = m_descriptorCache.getOrCreate( { m_hShader, m_hSampler, rSource, rProxySource, m_hszMipViews[nMip] },
	        String( "T3D.Instance.HiZ.ReduceSet." ) + String::num_uint64( nMip ), uniforms, m_hShader, 0u );
	if ( !m_hszReduceSets[nMip].is_valid() ) {
		return false;
	}
	return true;
}

void Terrain3DRdInstanceHiZ::freeReduceSet( const uint32_t nMip ) {
	if ( nMip < T3D_INSTANCE_HIZ_MAX_MIPS ) {
		m_descriptorCache.erase( m_hszReduceSets[nMip] );
		m_hszReduceSets[nMip] = RID();
	}
}

bool Terrain3DRdInstanceHiZ::initialize( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_hTexture.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() || !m_context.getDevice() ) {
		return false;
	}
	m_sourceShader = rShader;
	if ( !createPersistentResources() ) {
		shutdown();
		return false;
	}
	return true;
}

void Terrain3DRdInstanceHiZ::shutdown( void ) {
	for ( uint32_t iMip = 0u; iMip < T3D_INSTANCE_HIZ_MAX_MIPS; ++iMip ) {
		freeReduceSet( iMip );
	}
	m_descriptorCache.clear();
	m_pipelineCache.clear();
	m_hPipeline = RID();
	m_context.freeResource( m_hShader );
	Terrain3DRdHiZPyramid::release( m_context, m_hSampler, m_hTexture, m_hszMipViews, T3D_INSTANCE_HIZ_MAX_MIPS );
	m_hBorrowedPreviousDepth = RID();
	m_hBorrowedPreviousProxyDepth = RID();
	m_sourceShader.unref();
	m_pushBytes.clear();
	m_nSourceWidth = 0u;
	m_nSourceHeight = 0u;
	m_nProxyWidth = 0u;
	m_nProxyHeight = 0u;
	m_bProxyDepthBound = false;
	m_nActiveWidth = 0u;
	m_nActiveHeight = 0u;
	m_nActiveMipCount = 0u;
}

bool Terrain3DRdInstanceHiZ::bindPreviousDepth( const RID &rDepth, const RID &rProxyDepth ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !rDepth.is_valid() || !rd->texture_is_valid( rDepth ) ) {
		freeReduceSet( 0u );
		m_hBorrowedPreviousDepth = RID();
		m_hBorrowedPreviousProxyDepth = RID();
		m_nSourceWidth = 0u;
		m_nSourceHeight = 0u;
		m_nProxyWidth = 0u;
		m_nProxyHeight = 0u;
		m_bProxyDepthBound = false;
		m_nActiveMipCount = 0u;
		++m_nUnavailableFrames;
		return false;
	}
	const Ref<RDTextureFormat> format = rd->texture_get_format( rDepth );
	if ( format.is_null() || format->get_width() == 0u || format->get_height() == 0u ) {
		freeReduceSet( 0u );
		m_hBorrowedPreviousDepth = RID();
		m_hBorrowedPreviousProxyDepth = RID();
		m_nSourceWidth = 0u;
		m_nSourceHeight = 0u;
		m_nProxyWidth = 0u;
		m_nProxyHeight = 0u;
		m_bProxyDepthBound = false;
		m_nActiveMipCount = 0u;
		++m_nUnavailableFrames;
		return false;
	}

	RID proxy = rDepth;
	uint32_t proxy_width = format->get_width();
	uint32_t proxy_height = format->get_height();
	bool proxy_bound = false;
	if ( rProxyDepth.is_valid() && rd->texture_is_valid( rProxyDepth ) ) {
		const Ref<RDTextureFormat> proxy_format = rd->texture_get_format( rProxyDepth );
		if ( proxy_format.is_valid() && proxy_format->get_width() > 0u && proxy_format->get_height() > 0u ) {
			proxy = rProxyDepth;
			proxy_width = proxy_format->get_width();
			proxy_height = proxy_format->get_height();
			proxy_bound = true;
		}
	}
	if ( rDepth == m_hBorrowedPreviousDepth && proxy == m_hBorrowedPreviousProxyDepth && m_hszReduceSets[0].is_valid() ) {
		m_nSourceWidth = format->get_width();
		m_nSourceHeight = format->get_height();
		m_nProxyWidth = proxy_width;
		m_nProxyHeight = proxy_height;
		m_bProxyDepthBound = proxy_bound;
		return true;
	}
	freeReduceSet( 0u );
	m_hBorrowedPreviousDepth = rDepth;
	m_hBorrowedPreviousProxyDepth = proxy;
	m_nSourceWidth = format->get_width();
	m_nSourceHeight = format->get_height();
	m_nProxyWidth = proxy_width;
	m_nProxyHeight = proxy_height;
	m_bProxyDepthBound = proxy_bound;
	if ( !createReduceSet( 0u, rDepth, proxy ) ) {
		m_hBorrowedPreviousDepth = RID();
		m_hBorrowedPreviousProxyDepth = RID();
		m_nSourceWidth = 0u;
		m_nSourceHeight = 0u;
		m_nProxyWidth = 0u;
		m_nProxyHeight = 0u;
		m_bProxyDepthBound = false;
		return false;
	}
	m_context.recordRenderEvent( Terrain3DRdContext::OWNER_INSTANCES, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
	return true;
}

bool Terrain3DRdInstanceHiZ::build( const Vector2i &rRenderSize ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !m_hPipeline.is_valid() || !m_hszReduceSets[0].is_valid() || m_nSourceWidth == 0u || m_nSourceHeight == 0u ) {
		++m_nUnavailableFrames;
		m_nActiveMipCount = 0u;
		return false;
	}
	const uint32_t render_width = ( uint32_t )( MAX( rRenderSize.x, 1 ) );
	const uint32_t render_height = ( uint32_t )( MAX( rRenderSize.y, 1 ) );
	m_nActiveWidth = MIN( MIN( render_width, m_nSourceWidth ), T3D_INSTANCE_HIZ_MAX_DIMENSION );
	m_nActiveHeight = MIN( MIN( render_height, m_nSourceHeight ), T3D_INSTANCE_HIZ_MAX_DIMENSION );
	uint32_t width = m_nActiveWidth;
	uint32_t height = m_nActiveHeight;
	m_nActiveMipCount = 1u;
	while ( ( width > 1u || height > 1u ) && m_nActiveMipCount < T3D_INSTANCE_HIZ_MAX_MIPS ) {
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
		t3dInstanceHiZReducePush_t push;
		push.m_nSourceWidth = source_width;
		push.m_nSourceHeight = source_height;
		push.m_nProxyWidth = iMip == 0u ? m_nProxyWidth : source_width;
		push.m_nProxyHeight = iMip == 0u ? m_nProxyHeight : source_height;
		push.m_nDestinationWidth = destination_width;
		push.m_nDestinationHeight = destination_height;
		push.m_nCombineProxy = iMip == 0u && m_bProxyDepthBound ? 1u : 0u;
		memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->compute_list_bind_uniform_set( list, m_hszReduceSets[iMip], 0u );
		rd->compute_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
		rd->compute_list_dispatch( list, ( destination_width + 7u ) / 8u, ( destination_height + 7u ) / 8u, 1u );
		++m_nBuildDispatches;
		rd->compute_list_add_barrier( list );
		source_width = destination_width;
		source_height = destination_height;
		destination_width = MAX( 1u, ( destination_width + 1u ) >> 1u );
		destination_height = MAX( 1u, ( destination_height + 1u ) >> 1u );
	}
	rd->compute_list_end();
	m_context.recordRenderEvent( Terrain3DRdContext::OWNER_INSTANCES, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_BUILD );
	return true;
}
