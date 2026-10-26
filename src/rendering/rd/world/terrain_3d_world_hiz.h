// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_HIZ_H
#define TERRAIN3D_WORLD_HIZ_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/world/terrain_3d_world_contract.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_pipeline_cache.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

class Terrain3DWorldHiZ {
private:
	static const uint32_t s_nAbsoluteMaxMipCount = 16u;
	typedef struct t3dWorldHiZReducePush_s {
		uint32_t m_nSourceWidth;
		uint32_t m_nSourceHeight;
		uint32_t m_nDestinationWidth;
		uint32_t m_nDestinationHeight;
	} t3dWorldHiZReducePush_t;
	typedef struct t3dReduceSetKey_s {
		godot::RID shader;
		godot::RID sampler;
		godot::RID source;
		godot::RID destination;
		bool operator==( const t3dReduceSetKey_s &rOther ) const {
			return shader == rOther.shader && sampler == rOther.sampler && source == rOther.source && destination == rOther.destination;
		}
	} t3dReduceSetKey_t;

	Terrain3DRdContext &m_context;
	godot::RID m_hTexture;
	godot::RID m_hSampler;
	godot::RID m_hShader;
	godot::RID m_hPipeline;
	Terrain3DRdPipelineCache<godot::RID, 1u> m_pipelineCache;
	Terrain3DRdDescriptorCache<t3dReduceSetKey_t, s_nAbsoluteMaxMipCount> m_descriptorCache;
	godot::RID m_hszMipViews[s_nAbsoluteMaxMipCount];
	godot::RID m_hszReduceSets[s_nAbsoluteMaxMipCount];
	godot::RID m_hBorrowedPreviousDepth;
	godot::Ref<godot::RDShaderSPIRV> m_sourceShader;
	godot::PackedByteArray m_pushBytes;
	uint32_t m_nMaxWidth = 1u;
	uint32_t m_nMaxHeight = 1u;
	uint32_t m_nMaxMipCount = 1u;
	uint32_t m_nSourceWidth = 0u;
	uint32_t m_nSourceHeight = 0u;
	uint32_t m_nActiveWidth = 0u;
	uint32_t m_nActiveHeight = 0u;
	uint32_t m_nActiveMipCount = 0u;
	uint64_t m_nResourceGeneration = 0u;
	uint64_t m_nBuildDispatches = 0u;
	uint64_t m_nUnavailableFrames = 0u;

	bool createPersistentResourcesCold( void );
	bool createReduceSetCold( uint32_t nMip, const godot::RID &rSource );
	void freeReduceSetCold( uint32_t nMip );

public:
	explicit Terrain3DWorldHiZ( Terrain3DRdContext &rContext );
	~Terrain3DWorldHiZ( void );
	bool initializeCold( const godot::Ref<godot::RDShaderSPIRV> &rShader, const t3dWorldResourceLimits_t &rLimits );
	void shutdownCold( void );
	bool bindPreviousDepthCold( const godot::RID &rDepth );
	bool buildHot( const godot::Vector2i &rRenderSize );
	bool isReady( void ) const;
	const godot::RID &getTexture( void ) const;
	const godot::RID &getSampler( void ) const;
	uint32_t getWidth( void ) const;
	uint32_t getHeight( void ) const;
	uint32_t getMipCount( void ) const;
	uint64_t getResourceGeneration( void ) const;
	uint64_t getBuildCount( void ) const;
	uint64_t getBuildDispatches( void ) const;
	uint64_t getSourceRebinds( void ) const;
	uint64_t getUnavailableFrames( void ) const;
	uint64_t getGpuBytes( void ) const;
};

#endif // TERRAIN3D_WORLD_HIZ_H
