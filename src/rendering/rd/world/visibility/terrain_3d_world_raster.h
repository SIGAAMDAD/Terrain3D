// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_RASTER_H
#define TERRAIN3D_WORLD_RASTER_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>

class Terrain3DVTGpu;

class Terrain3DWorldRaster {
private:
typedef struct t3dPipelineKey_s : public t3dRdForwardPipelineBaseKey_t {

		uint32_t m_nPrimitiveClass = T3D_PRIMITIVE_TERRAIN_STRIP;
		bool operator==( const t3dPipelineKey_s &rOther ) const {
			return ( const t3dRdForwardPipelineBaseKey_t & )( *this  ) == ( const t3dRdForwardPipelineBaseKey_t & )( rOther  )
			        && m_nPrimitiveClass == rOther.m_nPrimitiveClass;
		}
	} t3dPipelineKey_t;

	typedef Terrain3DRdPipelineCache<t3dPipelineKey_t, T3D_WORLD_PIPELINE_CAPACITY> t3dPipelineCache_t;
	typedef t3dRdPipelineCacheEntry_t<t3dPipelineKey_t> t3dPipelineEntry_t;
	typedef struct t3dSetSignature_s {

		godot::RID shader;
		uint64_t world = 0u;
		uint64_t dependency = 0u;
		bool operator==( const t3dSetSignature_s &rOther ) const {
			return shader == rOther.shader && world == rOther.world && dependency == rOther.dependency;
		}
	} t3dSetSignature_t;


	Terrain3DRdContext &m_context;
	Terrain3DRdForwardLighting m_forwardLighting;
	Terrain3DRdShader m_hShader[T3D_PRIMITIVE_CLASS_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	Terrain3DVTGpu *m_pFeedbackOwner = NULL; // Borrowed shared VT owner.
	bool m_bMeshFeedback[T3D_RD_FORWARD_COLOR_VARIANT_COUNT]{};
	godot::RID m_hCurrentFeedbackSet;
	Terrain3DRdShader m_hShadowShader[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID m_hSet0[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT];
	godot::RID m_hSet1[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT];
	godot::RID m_hCurrentSet0[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID m_hCurrentSet1[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID m_hLightingSet[T3D_PRIMITIVE_CLASS_COUNT];
	t3dPipelineCache_t m_pipelines;
	Terrain3DRdDescriptorCache<t3dSetSignature_t, T3D_PRIMITIVE_CLASS_COUNT * T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT> m_set0Cache;
	Terrain3DRdDescriptorCache<t3dSetSignature_t, T3D_PRIMITIVE_CLASS_COUNT * T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT> m_set1Cache;
	t3dPipelineKey_t m_currentKey[T3D_PRIMITIVE_CLASS_COUNT]{};
	t3dSetSignature_t m_set0Signature[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT]{};
	t3dSetSignature_t m_set1Signature[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT]{};
	uint64_t m_nPrepareCount = 0u;
	uint64_t m_nSubmitCount[T3D_RD_FORWARD_PASS_MAX]{};
	bool m_bCurrentValid = false;
	bool m_bInitialized = false;
	bool m_bPersistentFrameBuffer = false;
	bool m_bPersistentCpuBuffers = false;

	bool ensureShaderCold( t3dPrimitiveClass_t primitive, t3dRdForwardShaderVariant_t variant );
	bool ensureSetsCold( Terrain3DWorldResources &rResources,
	        const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	        const t3dSharedVTBindings_t &rVT,
	        const godot::RID &rShader,
	        t3dPrimitiveClass_t primitiveClass,
	        uint32_t nDescriptorSlot );
	bool ensurePipelineCold(
	        const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant, t3dPrimitiveClass_t primitiveClass );
	const t3dPipelineEntry_t *findPipeline( const t3dPipelineKey_t &rKey ) const;
	void freeSetsCold( void );

public:
	explicit Terrain3DWorldRaster( Terrain3DRdContext &rContext );
	~Terrain3DWorldRaster( void );
	bool initializeCold( bool bPersistentFrameBuffer, bool bPersistentCpuBuffers );
	bool prepareCold( const t3dRdForwardPassContext_t &rContext,
	        Terrain3DWorldResources &rResources,
	        const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	        const t3dSharedVTBindings_t &rVT, Terrain3DVTGpu *pFeedbackOwner = NULL );
	bool submitHot( const t3dRdForwardPassContext_t &rContext, Terrain3DWorldResources &rResources );
	void shutdownCold( void );
	bool isReady( void ) const;
	uint64_t getSubmitCount( t3dRdForwardPassKind_t pass ) const;
	uint64_t getPrepareCount( void ) const;
};

#endif // TERRAIN3D_WORLD_RASTER_H
