// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_HIZ_REDUCER_H
#define TERRAIN3D_RD_HIZ_REDUCER_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

// t3dRdHiZDispatchInfo_t centralized in Terrain3DTypes.h.


typedef void ( *t3dRdHiZPushWriterFn_t )(
        void *pUser, const t3dRdHiZDispatchInfo_t &rInfo, uint8_t *pDestination, uint32_t nDestinationBytes );

class Terrain3DRdHiZReducer {
private:
	
	class t3dReduceSetKey_t {
public:

		godot::RID m_hShader;
		godot::RID m_hSampler;
		godot::RID m_hSource;
		godot::RID m_hAuxiliary;
		godot::RID m_hDestination;
		bool operator==( const t3dReduceSetKey_t &rOther ) const {
			return m_hShader == rOther.m_hShader && m_hSampler == rOther.m_hSampler && m_hSource == rOther.m_hSource
			        && m_hAuxiliary == rOther.m_hAuxiliary && m_hDestination == rOther.m_hDestination;
		}
	};

	Terrain3DRdContext &m_context;
	t3dRdResourceOwner_t m_eOwner;
	Terrain3DRdPipelineCache<godot::RID, 1u> m_pipelineCache;
	Terrain3DRdDescriptorCache<t3dReduceSetKey_t, T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT> m_descriptorCache;
	Terrain3DRdTexture m_texture;
	Terrain3DRdSampler m_hSampler;
	Terrain3DRdShader m_hShader;
	godot::RID m_hPipeline;
	Terrain3DRdTexture m_mipViews[T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT];
	godot::RID m_hszReduceSets[T3D_RD_HIZ_ABSOLUTE_MAX_MIP_COUNT];
	godot::RID m_hBorrowedSource;
	godot::RID m_hBorrowedAuxiliary;
	t3dRdShaderId_t m_shaderId = T3D_RD_SHADER_COUNT;
	godot::PackedByteArray m_pushBytes;
	godot::String m_name;
	uint32_t m_nMaxWidth = 0u;
	uint32_t m_nMaxHeight = 0u;
	uint32_t m_nMaxMipCount = 0u;
	uint32_t m_nSourceBinding = 0u;
	uint32_t m_nAuxiliaryBinding = 0u;
	uint32_t m_nDestinationBinding = 1u;
	uint32_t m_nPushBytes = 0u;
	uint32_t m_nSourceWidth = 0u;
	uint32_t m_nSourceHeight = 0u;
	uint32_t m_nAuxiliaryWidth = 0u;
	uint32_t m_nAuxiliaryHeight = 0u;
	uint32_t m_nActiveWidth = 0u;
	uint32_t m_nActiveHeight = 0u;
	uint32_t m_nActiveMipCount = 0u;
	uint64_t m_nBuildDispatches = 0u;
	uint64_t m_nResourceGeneration = 0u;
	bool m_bUsesAuxiliarySource = false;
	bool m_bAuxiliarySourceBound = false;

	bool createReduceSetCold( uint32_t nMip, const godot::RID &rSource, const godot::RID &rAuxiliary );
	void freeReduceSetCold( uint32_t nMip );
	void clearSourceBindingCold( void );

public:
	Terrain3DRdHiZReducer( Terrain3DRdContext &rContext, t3dRdResourceOwner_t eOwner );
	~Terrain3DRdHiZReducer( void );

	bool initializeCold( t3dRdShaderId_t shaderId,
	        const godot::String &rName,
	        uint32_t nMaxWidth,
	        uint32_t nMaxHeight,
	        uint32_t nMaxMipCount,
	        uint32_t nSourceBinding,
	        uint32_t nAuxiliaryBinding,
	        uint32_t nDestinationBinding,
	        bool bUsesAuxiliarySource,
	        uint32_t nPushBytes );
	void shutdownCold( void );
	bool bindSourceCold( const godot::RID &rSource, const godot::RID &rAuxiliary, bool &rRebound );
	bool buildHot( const godot::Vector2i &rRenderSize, t3dRdHiZPushWriterFn_t pPushWriter, void *pPushUser );

	EA_FORCE_INLINE bool isInitialized( void ) const { return m_texture.isValid() && m_hSampler.isValid(); }
	EA_FORCE_INLINE bool isReady( void ) const {
		return m_texture.isValid() && m_hSampler.isValid() && m_hBorrowedSource.is_valid() && m_nActiveMipCount > 0u;
	}
	EA_FORCE_INLINE const godot::RID &getTexture( void ) const { return m_texture.rid(); }
	EA_FORCE_INLINE const godot::RID &getSampler( void ) const { return m_hSampler.rid(); }
	EA_FORCE_INLINE uint32_t getWidth( void ) const { return m_nActiveWidth; }
	EA_FORCE_INLINE uint32_t getHeight( void ) const { return m_nActiveHeight; }
	EA_FORCE_INLINE uint32_t getMipCount( void ) const { return m_nActiveMipCount; }
	EA_FORCE_INLINE uint64_t getBuildDispatches( void ) const { return m_nBuildDispatches; }
	EA_FORCE_INLINE uint64_t getResourceGeneration( void ) const { return m_nResourceGeneration; }
	EA_FORCE_INLINE bool hasAuxiliarySourceBound( void ) const { return m_bAuxiliarySourceBound; }
	uint64_t getGpuBytes( void ) const;
};

#endif // TERRAIN3D_RD_HIZ_REDUCER_H
