// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/terrain_3d_world_hiz.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_hiz_pyramid.h"
#include "rendering/rd/terrain_3d_rd_shader_compile_status.h"

#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <string.h>

using namespace godot;



Terrain3DWorldHiZ::Terrain3DWorldHiZ( Terrain3DRdContext &rContext ) :
        m_context( rContext ), m_pipelineCache( rContext, Terrain3DRdContext::OWNER_WORLD ),
        m_descriptorCache( rContext, Terrain3DRdContext::OWNER_WORLD ) {
}

Terrain3DWorldHiZ::~Terrain3DWorldHiZ( void ) {
	shutdownCold();
}

bool Terrain3DWorldHiZ::initializeCold( const Ref<RDShaderSPIRV> &rShader, const t3dWorldResourceLimits_t &rLimits ) {
	const uint32_t max_width = MAX( rLimits.m_nHiZMaxWidth, 1u );
	const uint32_t max_height = MAX( rLimits.m_nHiZMaxHeight, 1u );
	const uint32_t max_mips = CLAMP( rLimits.m_nHiZMaxMipCount, 1u, s_nAbsoluteMaxMipCount );
	if ( m_hTexture.is_valid() && m_sourceShader == rShader && m_nMaxWidth == max_width && m_nMaxHeight == max_height
	        && m_nMaxMipCount == max_mips ) {
		return true;
	}
	shutdownCold();
	if ( rShader.is_null() || !m_context.getDevice() ) {
		return false;
	}
	m_sourceShader = rShader;
	m_nMaxWidth = max_width;
	m_nMaxHeight = max_height;
	m_nMaxMipCount = max_mips;
	if ( !createPersistentResourcesCold() ) {
		shutdownCold();
		return false;
	}
	++m_nResourceGeneration;
	return true;
}

bool Terrain3DWorldHiZ::createPersistentResourcesCold( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	if ( !Terrain3DRdHiZPyramid::create( m_context, Terrain3DRdContext::OWNER_WORLD, "T3D.World.HiZ",
	                    m_nMaxWidth, m_nMaxHeight, m_nMaxMipCount, m_hSampler, m_hTexture, m_hszMipViews,
	                    s_nAbsoluteMaxMipCount ) ) {
		return false;
	}
	const Terrain3DRdResourceService resources( m_context, Terrain3DRdContext::OWNER_WORLD );
	m_hShader = t3dRdCreateShaderWithStatus( rd, m_sourceShader, "T3D.World.HiZ.Reduce" );
	if ( !resources.adopt( rd, m_hShader, "T3D.World.HiZ.ReduceShader", Terrain3DRdContext::RESOURCE_SHADER ) ) {
		return false;
	}
	m_hPipeline = m_pipelineCache.getOrCreateCompute( m_hShader, "T3D.World.HiZ.ReducePipeline", m_hShader );
	if ( !m_hPipeline.is_valid() ) {
		return false;
	}
	for ( uint32_t iMip = 1u; iMip < m_nMaxMipCount; ++iMip ) {
		if ( !createReduceSetCold( iMip, m_hszMipViews[iMip - 1u] ) ) {
			return false;
		}
	}
	m_pushBytes.resize( sizeof( t3dWorldHiZReducePush_t ) );
	return true;
}

bool Terrain3DWorldHiZ::createReduceSetCold( const uint32_t nMip, const RID &rSource ) {
	if ( nMip >= m_nMaxMipCount || !rSource.is_valid() || !m_hszMipViews[nMip].is_valid() || !m_hShader.is_valid() ) {
		return false;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 0, m_hSampler, rSource ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::image( 1, m_hszMipViews[nMip] ) );
	m_hszReduceSets[nMip] = m_descriptorCache.getOrCreate( { m_hShader, m_hSampler, rSource, m_hszMipViews[nMip] },
	        String( "T3D.World.HiZ.ReduceSet." ) + String::num_uint64( nMip ), uniforms, m_hShader, 0u );
	if ( !m_hszReduceSets[nMip].is_valid() ) {
		return false;
	}
	return true;
}

void Terrain3DWorldHiZ::freeReduceSetCold( const uint32_t nMip ) {
	if ( nMip < s_nAbsoluteMaxMipCount ) {
		m_descriptorCache.erase( m_hszReduceSets[nMip] );
		m_hszReduceSets[nMip] = RID();
	}
}

void Terrain3DWorldHiZ::shutdownCold( void ) {
	for ( uint32_t iMip = 0u; iMip < s_nAbsoluteMaxMipCount; ++iMip ) {
		freeReduceSetCold( iMip );
	}
	m_descriptorCache.clear();
	m_pipelineCache.clear();
	m_hPipeline = RID();
	m_context.freeResource( m_hShader );
	Terrain3DRdHiZPyramid::release( m_context, m_hSampler, m_hTexture, m_hszMipViews, s_nAbsoluteMaxMipCount );
	m_hBorrowedPreviousDepth = RID();
	m_sourceShader.unref();
	m_pushBytes.clear();
	m_nSourceWidth = 0u;
	m_nSourceHeight = 0u;
	m_nActiveWidth = 0u;
	m_nActiveHeight = 0u;
	m_nActiveMipCount = 0u;
}

bool Terrain3DWorldHiZ::bindPreviousDepthCold( const RID &rDepth ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !rDepth.is_valid() || !rd->texture_is_valid( rDepth ) ) {
		freeReduceSetCold( 0u );
		m_hBorrowedPreviousDepth = RID();
		m_nActiveMipCount = 0u;
		++m_nUnavailableFrames;
		return false;
	}
	const Ref<RDTextureFormat> format = rd->texture_get_format( rDepth );
	if ( format.is_null() || format->get_width() == 0u || format->get_height() == 0u ) {
		return false;
	}
	m_nSourceWidth = format->get_width();
	m_nSourceHeight = format->get_height();
	if ( rDepth == m_hBorrowedPreviousDepth && m_hszReduceSets[0].is_valid() ) {
		return true;
	}
	freeReduceSetCold( 0u );
	m_hBorrowedPreviousDepth = rDepth;
	if ( !createReduceSetCold( 0u, rDepth ) ) {
		m_hBorrowedPreviousDepth = RID();
		return false;
	}
	m_context.recordRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
	return true;
}

bool Terrain3DWorldHiZ::buildHot( const Vector2i &rRenderSize ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !m_hPipeline.is_valid() || !m_hszReduceSets[0].is_valid() || m_nSourceWidth == 0u || m_nSourceHeight == 0u ) {
		++m_nUnavailableFrames;
		m_nActiveMipCount = 0u;
		return false;
	}
	m_nActiveWidth = MIN( MIN( ( uint32_t )MAX( rRenderSize.x, 1 ), m_nSourceWidth ), m_nMaxWidth );
	m_nActiveHeight = MIN( MIN( ( uint32_t )MAX( rRenderSize.y, 1 ), m_nSourceHeight ), m_nMaxHeight );
	uint32_t width = m_nActiveWidth, height = m_nActiveHeight;
	m_nActiveMipCount = 1u;
	while ( ( width > 1u || height > 1u ) && m_nActiveMipCount < m_nMaxMipCount ) {
		width = MAX( 1u, ( width + 1u ) >> 1u );
		height = MAX( 1u, ( height + 1u ) >> 1u );
		++m_nActiveMipCount;
	}
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_hPipeline );
	uint32_t source_width = m_nSourceWidth, source_height = m_nSourceHeight;
	uint32_t destination_width = m_nActiveWidth, destination_height = m_nActiveHeight;
	for ( uint32_t iMip = 0u; iMip < m_nActiveMipCount; ++iMip ) {
		t3dWorldHiZReducePush_t push = { source_width, source_height, destination_width, destination_height };
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
	m_context.recordRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_BUILD );
	return true;
}

bool Terrain3DWorldHiZ::isReady( void ) const {
	return m_hTexture.is_valid() && m_hSampler.is_valid() && m_nActiveMipCount > 0u;
}
const RID &Terrain3DWorldHiZ::getTexture( void ) const {
	return m_hTexture;
}
const RID &Terrain3DWorldHiZ::getSampler( void ) const {
	return m_hSampler;
}
uint32_t Terrain3DWorldHiZ::getWidth( void ) const {
	return m_nActiveWidth;
}
uint32_t Terrain3DWorldHiZ::getHeight( void ) const {
	return m_nActiveHeight;
}
uint32_t Terrain3DWorldHiZ::getMipCount( void ) const {
	return m_nActiveMipCount;
}
uint64_t Terrain3DWorldHiZ::getResourceGeneration( void ) const {
	return m_nResourceGeneration;
}
uint64_t Terrain3DWorldHiZ::getBuildCount( void ) const {
	return m_context.getRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_BUILD );
}
uint64_t Terrain3DWorldHiZ::getBuildDispatches( void ) const {
	return m_nBuildDispatches;
}
uint64_t Terrain3DWorldHiZ::getSourceRebinds( void ) const {
	return m_context.getRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
}
uint64_t Terrain3DWorldHiZ::getUnavailableFrames( void ) const {
	return m_nUnavailableFrames;
}
uint64_t Terrain3DWorldHiZ::getGpuBytes( void ) const {
	uint64_t nBytes = 0u;
	uint32_t nWidth = m_nMaxWidth;
	uint32_t nHeight = m_nMaxHeight;
	for ( uint32_t iMip = 0u; iMip < m_nMaxMipCount; ++iMip ) {
		nBytes += ( uint64_t )nWidth * ( uint64_t )nHeight * 4u;
		nWidth = MAX( 1u, ( nWidth + 1u ) >> 1u );
		nHeight = MAX( 1u, ( nHeight + 1u ) >> 1u );
	}
	return nBytes;
}
