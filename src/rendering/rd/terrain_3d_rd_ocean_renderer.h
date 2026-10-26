// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_OCEAN_RENDERER_H
#define TERRAIN3D_RD_OCEAN_RENDERER_H

#include "Terrain3DConstants.h"
#include "rendering/terrain_3d_mesher.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_descriptor_cache.h"
#include "rendering/terrain_3d_clipmap_strip_contract.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>

typedef struct t3dRdOceanStripSnapshot_s {
	uint64_t m_nGeneration = 0u;
	t3dClipmapStripPart_t m_szStripParts[MESH_TYPE_COUNT][T3D_CLIPMAP_STRIP_PART_CAPACITY]{};
	uint16_t m_szStripPartCounts[MESH_TYPE_COUNT]{};
} t3dRdOceanStripSnapshot_t;

class Terrain3DRdOceanRenderer {
private:
	typedef struct alignas( 16 ) t3dRdOceanFrame_s {
		float m_mViewProjection[s_nRdForwardMaxViews][16];
		float m_mView[16];
		float m_szCameraTime[4];
		float m_szLightDirection[4];
		float m_szLightColor[4];
		float m_szMeshParams[4];
		float m_szTargetTime[4];
		float m_szWaterColor[4];
		float m_szDepthParams[4];
		float m_szWaveParams[4];
		float m_szSurfaceParams[4];
		float m_szFoamParams0[4];
		float m_szFoamParams1[4];
		float m_szScatterParams[4];
		float m_szModeParams[4];
	} t3dRdOceanFrame_t;

	typedef struct t3dRdOceanPipelineKey_s {
		int64_t m_nFramebufferFormat = RenderingDevice::INVALID_ID;
		t3dRdForwardPassKind_t m_passKind = T3D_RD_FORWARD_PASS_INVALID;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
		uint32_t m_nViewCount = 1u;
		bool m_bReverseCull = false;
		bool operator==( const t3dRdOceanPipelineKey_s &rOther ) const {
			return m_nFramebufferFormat == rOther.m_nFramebufferFormat && m_passKind == rOther.m_passKind && m_samples == rOther.m_samples
			        && m_nViewCount == rOther.m_nViewCount && m_bReverseCull == rOther.m_bReverseCull;
		}
	} t3dRdOceanPipelineKey_t;

	typedef Terrain3DRdPipelineCache<t3dRdOceanPipelineKey_t, s_nPipelineCapacity> t3dRdOceanPipelineCache_t;
	typedef t3dRdOceanPipelineCache_t::Entry t3dRdOceanPipelineEntry_t;
	typedef struct t3dRdOceanSetKey_s {
		RID shader;
		RID frame;
		RID patches;
		RID stripParts;
		bool operator==( const t3dRdOceanSetKey_s &rOther ) const {
			return shader == rOther.shader && frame == rOther.frame && patches == rOther.patches && stripParts == rOther.stripParts;
		}
	} t3dRdOceanSetKey_t;
	typedef Terrain3DRdDescriptorCache<t3dRdOceanSetKey_t, 2u> t3dRdOceanDescriptorCache_t;

	Terrain3DRdContext &m_context;
	RID m_hColorShader;
	RID m_hShadowShader;
	RID m_hFrameBuffer;
	RID m_hPatchBuffer;
	RID m_hStripPartBuffer;
	RID m_hStripIndirectBuffer;
	uint32_t m_nStripIndirectOffsetBytes = 0u;

	t3dRdOceanPipelineCache_t m_pipelines;
	t3dRdOceanDescriptorCache_t m_descriptors;
	uint64_t m_nStripGeneration = 0u;
	uint16_t m_szStripPartCounts[MESH_TYPE_COUNT]{};
	t3dClipmapStripPart_t m_szStripParts[MESH_TYPE_COUNT][T3D_CLIPMAP_STRIP_PART_CAPACITY]{};
	uint64_t m_nStateGeneration = 0u;
	uint64_t m_nPreparedScenePass = 0u;
	uint64_t m_nPreparedViewportKey = 0u;
	t3dRdForwardPassKind_t m_preparedPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t m_nPreparedPatchCount = 0u;
	bool m_bPreparedHasDraws = false;
	PackedByteArray m_frameBytes;
	PackedByteArray m_patchBytes;
	PackedByteArray m_pushBytes;
	PackedByteArray m_stripPartBytes;
	PackedByteArray m_stripIndirectBytes;
	uint64_t m_nFramesPrepared = 0u;
	uint64_t m_nDraws = 0u;
	uint64_t m_nStripDescriptorUploads = 0u;
	String m_lastError;

	void setError( const String &rError );
	bool ensureShaders( const Ref<RDShaderSPIRV> &rColor, const Ref<RDShaderSPIRV> &rShadow );
	bool ensureBuffers( void );
	bool ensureUniformSets( void );
	bool ensureStripParts( const t3dRdOceanStripSnapshot_t *pStrip );
	bool updateStripIndirect( uint32_t nPatchCount );
	bool updateFrame( const t3dRdForwardPassContext_t &rContext, const t3dRdFrameSnapshot_t &rSnapshot );
	bool ensurePipeline( const t3dRdForwardPassContext_t &rContext );
	const t3dRdOceanPipelineEntry_t *findPipeline( const t3dRdOceanPipelineKey_t &rKey ) const;
	void freePipelines( void );

public:
	Terrain3DRdOceanRenderer( void );
	~Terrain3DRdOceanRenderer( void );

	bool initialize( const Ref<RDShaderSPIRV> &rColor, const Ref<RDShaderSPIRV> &rShadow );
	void shutdown( void );
	bool prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdFrameSnapshot_t &rSnapshot,
	        const t3dRdOceanStripSnapshot_t *pStrip,
	        uint32_t nVisibilityMask );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary getDiagnostics( void ) const;
	EA_FORCE_INLINE uint64_t getDrawsRendered( void ) const {
		return m_nDraws;
	}
	String getLastError( void ) const {
		return m_lastError;
	}
};

#endif // TERRAIN3D_RD_OCEAN_RENDERER_H
