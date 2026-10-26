// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_INSTANCE_HIZ_H
#define TERRAIN3D_RD_INSTANCE_HIZ_H

#include "Terrain3DConstants.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_hiz_reducer.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

using namespace godot;

#define T3D_INSTANCE_HIZ_MAX_DIMENSION ( 2048u )
#define T3D_INSTANCE_HIZ_MAX_MIPS ( 12u )

class Terrain3DRdInstanceHiZ {
private:
	typedef struct t3dInstanceHiZReducePush_s {
		uint32_t m_nSourceWidth = 1u;
		uint32_t m_nSourceHeight = 1u;
		uint32_t m_nProxyWidth = 1u;
		uint32_t m_nProxyHeight = 1u;
		uint32_t m_nDestinationWidth = 1u;
		uint32_t m_nDestinationHeight = 1u;
		uint32_t m_nCombineProxy = 0u;
		uint32_t m_nReserved = 0u;
	} t3dInstanceHiZReducePush_t;

	Terrain3DRdContext &m_context;
	Terrain3DRdHiZReducer m_reducer;
	uint64_t m_nUnavailableFrames = 0u;

	static void writePush( void *pUser, const t3dRdHiZDispatchInfo_t &rInfo, uint8_t *pDestination, uint32_t nDestinationBytes );

public:
	explicit Terrain3DRdInstanceHiZ( Terrain3DRdContext &rContext );
	~Terrain3DRdInstanceHiZ( void );
	bool initialize( const Ref<RDShaderSPIRV> &rShader );
	void shutdown( void );
	bool bindPreviousDepth( const RID &rDepth, const RID &rProxyDepth = RID() );
	bool build( const Vector2i &rRenderSize );

	EA_FORCE_INLINE bool isReady( void ) const { return m_reducer.isReady(); }
	EA_FORCE_INLINE const RID &getTexture( void ) const { return m_reducer.getTexture(); }
	EA_FORCE_INLINE const RID &getSampler( void ) const { return m_reducer.getSampler(); }
	EA_FORCE_INLINE uint32_t getWidth( void ) const { return m_reducer.getWidth(); }
	EA_FORCE_INLINE uint32_t getHeight( void ) const { return m_reducer.getHeight(); }
	EA_FORCE_INLINE uint32_t getMipCount( void ) const { return m_reducer.getMipCount(); }
	EA_FORCE_INLINE uint64_t getBuildDispatches( void ) const { return m_reducer.getBuildDispatches(); }
	EA_FORCE_INLINE uint64_t getSourceRebinds( void ) const {
		return m_context.getRenderEvent( Terrain3DRdContext::OWNER_INSTANCES, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
	}
	EA_FORCE_INLINE uint64_t getUnavailableFrames( void ) const { return m_nUnavailableFrames; }
	EA_FORCE_INLINE bool hasProxyDepthBound( void ) const { return m_reducer.hasAuxiliarySourceBound(); }
};

#endif // TERRAIN3D_RD_INSTANCE_HIZ_H
