// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_OCEAN_RENDERER_H
#define TERRAIN3D_RD_OCEAN_RENDERER_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "rendering/geometry/terrain_3d_mesher.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_lease_table.h"
#include "rendering/geometry/terrain_3d_clipmap_strip_contract.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/environment/terrain_3d_rd_environment_field.h"

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3DRdOceanRenderer
{
private:
	typedef struct t3dRdOceanPipelineKey_s : public t3dRdForwardPipelineBaseKey_t
	{
        bool m_bEnvironmentVariant = false;
        bool operator==( const t3dRdOceanPipelineKey_s &rOther ) const {
            return ( const t3dRdForwardPipelineBaseKey_t & ) ( *this ) ==
                   ( const t3dRdForwardPipelineBaseKey_t & ) ( rOther ) &&
                   m_bEnvironmentVariant == rOther.m_bEnvironmentVariant;
        }
	} t3dRdOceanPipelineKey_t;

	typedef Terrain3DRdPipelineCache< t3dRdOceanPipelineKey_t, T3D_PIPELINE_CAPACITY > t3dRdOceanPipelineCache_t;
	typedef t3dRdPipelineCacheEntry_t< t3dRdOceanPipelineKey_t >					   t3dRdOceanPipelineEntry_t;
	typedef struct t3dRdOceanSetKey_s
	{
		RID	 shader;
		RID	 frame;
		RID	 patches;
		RID	 stripParts;
		bool operator==(
			const t3dRdOceanSetKey_s &rOther ) const {
			return shader == rOther.shader && frame == rOther.frame && patches == rOther.patches &&
				   stripParts == rOther.stripParts;
		}
	} t3dRdOceanSetKey_t;

    typedef struct t3dRdOceanEnvironmentSetKey_s {
        RID shader;
        RID params;
        RID state;
        RID interaction;
        bool operator==( const t3dRdOceanEnvironmentSetKey_s &rOther ) const {
            return shader == rOther.shader && params == rOther.params &&
                   state == rOther.state && interaction == rOther.interaction;
        }
    } t3dRdOceanEnvironmentSetKey_t;
    typedef Terrain3DRdDescriptorLeaseTable< t3dRdOceanEnvironmentSetKey_t, 4u >
        t3dRdOceanEnvironmentDescriptorCache_t;
    typedef Terrain3DRdDescriptorLeaseTable< t3dRdOceanSetKey_t, 4u > t3dRdOceanDescriptorCache_t;

	Terrain3DRdContext	 &m_context;
	Terrain3DRdShader	  m_hColorShader;
    Terrain3DRdShader m_hShadowShader;
    Terrain3DRdShader m_hEnvironmentColorShader;
    Terrain3DRdShader m_hEnvironmentShadowShader;
    bool m_bEnvironmentShadersAvailable = false;
    bool m_bCurrentEnvironmentShader = false;
    RID m_hEnvironmentSet[2][2]{}; // [color/shadow][ping]
    RID m_hCurrentEnvironmentSet;
    uint64_t m_nLastEnvironmentGeneration = 0u;
    uint64_t m_nStaleEnvironmentSetMisses = 0u;
	bool m_bPreparedFragmentVariant = false;
	Terrain3DRdGpuBuffer  m_hFrameBuffer;
	Terrain3DRdGpuBuffer  m_hPatchBuffer;
	Terrain3DRdGpuBuffer  m_hStripPartBuffer;
	Terrain3DRdGpuBuffer *m_pStripIndirectBuffer = NULL;
	t3dRdGpuBufferSlice_t m_stripIndirectSlice;

	t3dRdOceanPipelineCache_t	m_pipelines;
    t3dRdOceanDescriptorCache_t m_descriptors;
    t3dRdOceanEnvironmentDescriptorCache_t m_environmentDescriptors;
	uint64_t					m_nStripGeneration = 0u;
	uint16_t					m_szStripPartCounts[MESH_TYPE_COUNT]{};
	t3dClipmapStripPart_t		m_szStripParts[MESH_TYPE_COUNT][T3D_CLIPMAP_STRIP_PART_CAPACITY]{};
	uint64_t					m_nStateGeneration = 0u;
	uint64_t					m_nPreparedScenePass = 0u;
	uint64_t					m_nPreparedViewportKey = 0u;
	t3dRdForwardPassKind_t		m_preparedPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t					m_nPreparedPatchCount = 0u;
	bool						m_bPreparedHasDraws = false;
	PackedByteArray				m_frameBytes;
	PackedByteArray				m_patchBytes;
	PackedByteArray				m_pushBytes;
	PackedByteArray				m_stripPartBytes;
	PackedByteArray				m_stripIndirectBytes;
	uint64_t					m_nFramesPrepared = 0u;
	uint64_t					m_nDraws = 0u;
	uint64_t					m_nStripDescriptorUploads = 0u;
	String						m_lastError;

	void setError( const String &rError );
	bool ensureShaders( void );
	bool ensureBuffers( void );
    bool ensureUniformSets( void );
    bool ensureEnvironmentSetsCold( const t3dRdEnvironmentBindings_t &rBindings, bool bShadow );
	bool ensureStripParts( const t3dRdOceanStripSnapshot_t *pStrip );
	bool updateStripIndirect( uint32_t nPatchCount );
	bool updateFrame( const t3dRdForwardPassContext_t &rContext, const t3dRdFrameSnapshot_t &rSnapshot );
	bool ensurePipeline( const t3dRdForwardPassContext_t &rContext );
	const t3dRdOceanPipelineEntry_t *findPipeline( const t3dRdOceanPipelineKey_t &rKey ) const;
	void							 freePipelines( void );

public:
	Terrain3DRdOceanRenderer( void );
	~Terrain3DRdOceanRenderer( void );

	bool					 initialize( void );
	void					 shutdown( void );
	bool					 prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
												 const t3dRdFrameSnapshot_t		 &rSnapshot,
												 const t3dRdOceanStripSnapshot_t *pStrip,
												 uint32_t						  nVisibilityMask,
                                                 const t3dRdEnvironmentBindings_t *pEnvironment = NULL );
	bool					 renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary				 getDiagnostics( void ) const;
    uint64_t getEnvironmentStaleSetMisses() const { return m_nStaleEnvironmentSetMisses; }
	EA_FORCE_INLINE uint64_t getDrawsRendered(
		void ) const {
		return m_nDraws;
	}
	String getLastError(
		void ) const {
		return m_lastError;
	}
};

#endif // TERRAIN3D_RD_OCEAN_RENDERER_H
