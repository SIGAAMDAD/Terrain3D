// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/terrain_3d_world_hiz.h"

#include <string.h>

using namespace godot;

Terrain3DWorldHiZ::Terrain3DWorldHiZ( Terrain3DRdContext &rContext ) :
        m_context( rContext ), m_reducer( rContext, Terrain3DRdContext::OWNER_WORLD ) {
}

Terrain3DWorldHiZ::~Terrain3DWorldHiZ( void ) {
	shutdownCold();
}

void Terrain3DWorldHiZ::writePush(
        void *, const t3dRdHiZDispatchInfo_t &rInfo, uint8_t *const pDestination, const uint32_t nDestinationBytes ) {
	if ( !pDestination || nDestinationBytes != sizeof( t3dWorldHiZReducePush_t ) ) {
		return;
	}
	const t3dWorldHiZReducePush_t push = {
		rInfo.m_nSourceWidth, rInfo.m_nSourceHeight, rInfo.m_nDestinationWidth, rInfo.m_nDestinationHeight
	};
	memcpy( pDestination, &push, sizeof( push ) );
}

bool Terrain3DWorldHiZ::initializeCold( const Ref<RDShaderSPIRV> &rShader, const t3dWorldResourceLimits_t &rLimits ) {
	const uint32_t max_width = MAX( rLimits.m_nHiZMaxWidth, 1u );
	const uint32_t max_height = MAX( rLimits.m_nHiZMaxHeight, 1u );
	const uint32_t max_mips = CLAMP( rLimits.m_nHiZMaxMipCount, 1u, s_nAbsoluteMaxMipCount );
	if ( !m_reducer.initializeCold( rShader,
	             "T3D.World.HiZ",
	             max_width,
	             max_height,
	             max_mips,
	             0u,
	             0u,
	             1u,
	             false,
	             sizeof( t3dWorldHiZReducePush_t ) ) ) {
		return false;
	}
	return true;
}

void Terrain3DWorldHiZ::shutdownCold( void ) {
	m_reducer.shutdownCold();
}

bool Terrain3DWorldHiZ::bindPreviousDepthCold( const RID &rDepth ) {
	bool rebound = false;
	if ( !m_reducer.bindSourceCold( rDepth, RID(), rebound ) ) {
		++m_nUnavailableFrames;
		return false;
	}
	if ( rebound ) {
		m_context.recordRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
	}
	return true;
}

bool Terrain3DWorldHiZ::buildHot( const Vector2i &rRenderSize ) {
	if ( !m_reducer.buildHot( rRenderSize, &Terrain3DWorldHiZ::writePush, this ) ) {
		++m_nUnavailableFrames;
		return false;
	}
	m_context.recordRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_BUILD );
	return true;
}

bool Terrain3DWorldHiZ::isReady( void ) const { return m_reducer.isReady(); }
const RID &Terrain3DWorldHiZ::getTexture( void ) const { return m_reducer.getTexture(); }
const RID &Terrain3DWorldHiZ::getSampler( void ) const { return m_reducer.getSampler(); }
uint32_t Terrain3DWorldHiZ::getWidth( void ) const { return m_reducer.getWidth(); }
uint32_t Terrain3DWorldHiZ::getHeight( void ) const { return m_reducer.getHeight(); }
uint32_t Terrain3DWorldHiZ::getMipCount( void ) const { return m_reducer.getMipCount(); }
uint64_t Terrain3DWorldHiZ::getResourceGeneration( void ) const { return m_reducer.getResourceGeneration(); }
uint64_t Terrain3DWorldHiZ::getBuildCount( void ) const {
	return m_context.getRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_BUILD );
}
uint64_t Terrain3DWorldHiZ::getBuildDispatches( void ) const { return m_reducer.getBuildDispatches(); }
uint64_t Terrain3DWorldHiZ::getSourceRebinds( void ) const {
	return m_context.getRenderEvent( Terrain3DRdContext::OWNER_WORLD, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
}
uint64_t Terrain3DWorldHiZ::getUnavailableFrames( void ) const { return m_nUnavailableFrames; }
uint64_t Terrain3DWorldHiZ::getGpuBytes( void ) const { return m_reducer.getGpuBytes(); }
