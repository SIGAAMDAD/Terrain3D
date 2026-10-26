// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_INSTANCE_HIZ_H
#define TERRAIN3D_RD_INSTANCE_HIZ_H

#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_pipeline_cache.h"

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
	typedef struct t3dReduceSetKey_s {
		RID shader;
		RID sampler;
		RID source;
		RID proxy;
		RID destination;
		bool operator==( const t3dReduceSetKey_s &rOther ) const {
			return shader == rOther.shader && sampler == rOther.sampler && source == rOther.source && proxy == rOther.proxy
			        && destination == rOther.destination;
		}
	} t3dReduceSetKey_t;

	Terrain3DRdContext &m_context;
	RID m_hTexture;
	RID m_hSampler;
	RID m_hShader;
	RID m_hPipeline;
	Terrain3DRdPipelineCache<RID, 1u> m_pipelineCache;
	Terrain3DRdDescriptorCache<t3dReduceSetKey_t, T3D_INSTANCE_HIZ_MAX_MIPS> m_descriptorCache;
	RID m_hszMipViews[T3D_INSTANCE_HIZ_MAX_MIPS];
	RID m_hszReduceSets[T3D_INSTANCE_HIZ_MAX_MIPS];
	RID m_hBorrowedPreviousDepth;
	RID m_hBorrowedPreviousProxyDepth;
	Ref<RDShaderSPIRV> m_sourceShader;
	PackedByteArray m_pushBytes;
	uint32_t m_nSourceWidth = 0u;
	uint32_t m_nSourceHeight = 0u;
	uint32_t m_nProxyWidth = 0u;
	uint32_t m_nProxyHeight = 0u;
	bool m_bProxyDepthBound = false;
	uint32_t m_nActiveWidth = 0u;
	uint32_t m_nActiveHeight = 0u;
	uint32_t m_nActiveMipCount = 0u;
	uint64_t m_nBuildDispatches = 0u;
	uint64_t m_nUnavailableFrames = 0u;

	bool createPersistentResources( void );
	bool createReduceSet( uint32_t nMip, const RID &rSource, const RID &rProxySource );
	void freeReduceSet( uint32_t nMip );

public:
	explicit Terrain3DRdInstanceHiZ( Terrain3DRdContext &rContext );
	~Terrain3DRdInstanceHiZ( void );
	bool initialize( const Ref<RDShaderSPIRV> &rShader );
	void shutdown( void );
	bool bindPreviousDepth( const RID &rDepth, const RID &rProxyDepth = RID() );
	bool build( const Vector2i &rRenderSize );

	EA_FORCE_INLINE bool isReady( void ) const {
		return m_hTexture.is_valid() && m_hSampler.is_valid() && m_hBorrowedPreviousDepth.is_valid() && m_nActiveMipCount > 0u;
	}
	EA_FORCE_INLINE const RID &getTexture( void ) const {
		return m_hTexture;
	}
	EA_FORCE_INLINE const RID &getSampler( void ) const {
		return m_hSampler;
	}
	EA_FORCE_INLINE uint32_t getWidth( void ) const {
		return m_nActiveWidth;
	}
	EA_FORCE_INLINE uint32_t getHeight( void ) const {
		return m_nActiveHeight;
	}
	EA_FORCE_INLINE uint32_t getMipCount( void ) const {
		return m_nActiveMipCount;
	}
	EA_FORCE_INLINE uint64_t getBuildDispatches( void ) const {
		return m_nBuildDispatches;
	}
	EA_FORCE_INLINE uint64_t getSourceRebinds( void ) const {
		return m_context.getRenderEvent( Terrain3DRdContext::OWNER_INSTANCES, Terrain3DRdContext::RenderTelemetry::EVENT_HIZ_REBIND );
	}
	EA_FORCE_INLINE uint64_t getUnavailableFrames( void ) const {
		return m_nUnavailableFrames;
	}
	EA_FORCE_INLINE bool hasProxyDepthBound( void ) const {
		return m_bProxyDepthBound;
	}
};

#endif // TERRAIN3D_RD_INSTANCE_HIZ_H
