// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/terrain_3d_rd_instance_hiz.h"

#include <string.h>

using namespace godot;

Terrain3DRdInstanceHiZ::Terrain3DRdInstanceHiZ( Terrain3DRdContext &rContext ) :
        m_context( rContext ), m_reducer( rContext, Terrain3DRdContext::OWNER_INSTANCES ) {
}

Terrain3DRdInstanceHiZ::~Terrain3DRdInstanceHiZ( void ) {
	shutdown();
}

void Terrain3DRdInstanceHiZ::writePush(
        void *, const t3dRdHiZDispatchInfo_t &rInfo, uint8_t *const pDestination, const uint32_t nDestinationBytes ) {
	if ( !pDestination || nDestinationBytes != sizeof( t3dInstanceHiZReducePush_t ) ) {
		return;
	}
	t3dInstanceHiZReducePush_t push;
	push.m_nSourceWidth = rInfo.m_nSourceWidth;
	push.m_nSourceHeight = rInfo.m_nSourceHeight;
	push.m_nProxyWidth = rInfo.m_nAuxiliaryWidth;
	push.m_nProxyHeight = rInfo.m_nAuxiliaryHeight;
	push.m_nDestinationWidth = rInfo.m_nDestinationWidth;
	push.m_nDestinationHeight = rInfo.m_nDestinationHeight;
	push.m_nCombineProxy = rInfo.m_bAuxiliarySourceBound ? 1u : 0u;
	memcpy( pDestination, &push, sizeof( push ) );
}

bool Terrain3DRdInstanceHiZ::initialize( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_reducer.isInitialized() ) {
		return true;
	}
	return m_reducer.initializeCold( rShader,
	        "T3D.Instance.HiZ",
	        T3D_INSTANCE_HIZ_MAX_DIMENSION,
	        T3D_INSTANCE_HIZ_MAX_DIMENSION,
	        T3D_INSTANCE_HIZ_MAX_MIPS,
	        0u,
	        1u,
	        2u,
	        true,
	        sizeof( t3dInstanceHiZReducePush_t ) );
}

void Terrain3DRdInstanceHiZ::shutdown( void ) {
	m_reducer.shutdownCold();
}

bool Terrain3DRdInstanceHiZ::bindPreviousDepth( const RID &rDepth, const RID &rProxyDepth ) {
	bool rebound = false;
	if ( !m_reducer.bindSourceCold( rDepth, rProxyDepth, rebound ) ) {
		++m_nUnavailableFrames;
		return false;
	}
	if ( rebound ) {
		m_context.recordRenderEvent( Terrain3DRdContext::OWNER_INSTANCES, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
	}
	return true;
}

bool Terrain3DRdInstanceHiZ::build( const Vector2i &rRenderSize ) {
	if ( !m_reducer.buildHot( rRenderSize, &Terrain3DRdInstanceHiZ::writePush, this ) ) {
		++m_nUnavailableFrames;
		return false;
	}
	m_context.recordRenderEvent( Terrain3DRdContext::OWNER_INSTANCES, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_BUILD );
	return true;
}
