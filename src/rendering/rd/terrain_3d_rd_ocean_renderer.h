// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_OCEAN_RENDERER_H
#define TERRAIN3D_RD_OCEAN_RENDERER_H

#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_topology.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>

typedef struct t3dRdOceanTopologySnapshot_s {
	uint64_t m_nGeneration = 0u;
	t3dClipmapGeometry_t m_szGeometry[MESH_TYPE_COUNT];
} t3dRdOceanTopologySnapshot_t;

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

	typedef struct t3dRdOceanPipelineEntry_s {
		t3dRdOceanPipelineKey_t m_key;
		RID m_hPipeline;
	} t3dRdOceanPipelineEntry_t;

	Terrain3DRdContext &m_context;
	Terrain3DRdTopology m_szTopology[MESH_TYPE_COUNT];
	RID m_hColorShader;
	RID m_hShadowShader;
	RID m_hFrameBuffer;
	RID m_hPatchBuffer;
	RID m_hColorUniformSet;
	RID m_hShadowUniformSet;

	t3dRdOceanPipelineEntry_t m_szPipelines[s_nPipelineCapacity]{};
	uint32_t m_nPipelineCount = 0u;
	uint32_t m_nPipelineEvictCursor = 0u;
	int64_t m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
	uint64_t m_nTopologyGeneration = 0u;
	uint64_t m_nStateGeneration = 0u;
	uint64_t m_nPreparedScenePass = 0u;
	uint64_t m_nPreparedViewportKey = 0u;
	t3dRdForwardPassKind_t m_preparedPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t m_nPreparedPatchCount = 0u;
	bool m_bPreparedHasDraws = false;
	PackedByteArray m_frameBytes;
	PackedByteArray m_patchBytes;
	PackedByteArray m_pushBytes;
	uint64_t m_nFramesPrepared = 0u;
	uint64_t m_nDraws = 0u;
	uint64_t m_nTopologyUploads = 0u;
	uint64_t m_nPipelineBuilds = 0u;
	String m_lastError;

	void setError( const String &rError );
	bool ensureShaders( const Ref<RDShaderSPIRV> &rColor, const Ref<RDShaderSPIRV> &rShadow );
	bool ensureBuffers( void );
	bool ensureUniformSets( void );
	bool ensureTopology( const t3dRdOceanTopologySnapshot_t *pTopology );
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
	        const t3dRdOceanTopologySnapshot_t *pTopology,
	        uint32_t nVisibilityMask );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary getDiagnostics( void ) const;
	String getLastError( void ) const {
		return m_lastError;
	}
};

#endif // TERRAIN3D_RD_OCEAN_RENDERER_H
