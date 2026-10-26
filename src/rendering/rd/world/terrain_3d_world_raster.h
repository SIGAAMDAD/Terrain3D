// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_RASTER_H
#define TERRAIN3D_WORLD_RASTER_H

#include "rendering/rd/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/world/terrain_3d_world_resources.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>

class Terrain3DWorldRaster {
private:
	typedef enum t3dPrimitiveClass_e : uint32_t {
		T3D_PRIMITIVE_TERRAIN_STRIP = 0u,
		T3D_PRIMITIVE_MESH = 1u,
		T3D_PRIMITIVE_CLASS_COUNT = 2u,
	} t3dPrimitiveClass_t;
	static const uint32_t s_nWorldPipelineCapacity = 32u;
	static const uint32_t s_nWorldDescriptorLayoutCount = T3D_RD_FORWARD_SHADER_VARIANT_MAX + 1u;
	static const uint32_t s_nWorldShadowDescriptorSlot = T3D_RD_FORWARD_SHADER_VARIANT_MAX;
	typedef struct t3dPipelineKey_s {
		int64_t m_nFramebufferFormat = godot::RenderingDevice::INVALID_ID;
		t3dRdForwardShaderVariant_t m_variant = T3D_RD_FORWARD_SHADER_COLOR;
		godot::RenderingDevice::TextureSamples m_samples = godot::RenderingDevice::TEXTURE_SAMPLES_1;
		uint32_t m_nViewCount = 1u;
		uint32_t m_nPrimitiveClass = T3D_PRIMITIVE_TERRAIN_STRIP;
		bool m_bReverseCull = false;
		bool m_bDepthPrepassActive = false;
		bool m_bShadow = false;
		bool operator==( const t3dPipelineKey_s &rOther ) const {
			return m_nFramebufferFormat == rOther.m_nFramebufferFormat && m_variant == rOther.m_variant && m_samples == rOther.m_samples
			        && m_nViewCount == rOther.m_nViewCount && m_nPrimitiveClass == rOther.m_nPrimitiveClass
			        && m_bReverseCull == rOther.m_bReverseCull && m_bDepthPrepassActive == rOther.m_bDepthPrepassActive
			        && m_bShadow == rOther.m_bShadow;
		}
	} t3dPipelineKey_t;
	typedef Terrain3DRdPipelineCache<t3dPipelineKey_t, s_nWorldPipelineCapacity> t3dPipelineCache_t;
	typedef t3dPipelineCache_t::Entry t3dPipelineEntry_t;
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
	godot::Ref<godot::RDShaderSPIRV> m_source[T3D_PRIMITIVE_CLASS_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	godot::Ref<godot::RDShaderSPIRV> m_shadowSource[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID m_hShader[T3D_PRIMITIVE_CLASS_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	godot::RID m_hShadowShader[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID m_hSet0[T3D_PRIMITIVE_CLASS_COUNT][s_nWorldDescriptorLayoutCount];
	godot::RID m_hSet1[T3D_PRIMITIVE_CLASS_COUNT][s_nWorldDescriptorLayoutCount];
	godot::RID m_hCurrentSet0[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID m_hCurrentSet1[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID m_hLightingSet[T3D_PRIMITIVE_CLASS_COUNT];
	t3dPipelineCache_t m_pipelines;
	Terrain3DRdDescriptorCache<t3dSetSignature_t, T3D_PRIMITIVE_CLASS_COUNT * s_nWorldDescriptorLayoutCount> m_set0Cache;
	Terrain3DRdDescriptorCache<t3dSetSignature_t, T3D_PRIMITIVE_CLASS_COUNT * s_nWorldDescriptorLayoutCount> m_set1Cache;
	t3dPipelineKey_t m_currentKey[T3D_PRIMITIVE_CLASS_COUNT]{};
	t3dSetSignature_t m_set0Signature[T3D_PRIMITIVE_CLASS_COUNT][s_nWorldDescriptorLayoutCount]{};
	t3dSetSignature_t m_set1Signature[T3D_PRIMITIVE_CLASS_COUNT][s_nWorldDescriptorLayoutCount]{};
	uint64_t m_nPrepareCount = 0u;
	uint64_t m_nSubmitCount[T3D_RD_FORWARD_PASS_MAX]{};
	bool m_bCurrentValid = false;
	bool m_bInitialized = false;

	bool selectVariant( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t &rVariant ) const;
	bool ensureSetsCold( Terrain3DWorldResources &rResources,
	        const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	        const T3DTerrainStageASharedVTBindings &rVT,
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
	bool initializeCold( const godot::Ref<godot::RDShaderSPIRV> *pTerrainStripVariants,
	        const godot::Ref<godot::RDShaderSPIRV> &rTerrainStripShadow,
	        const godot::Ref<godot::RDShaderSPIRV> *pMeshVariants,
	        const godot::Ref<godot::RDShaderSPIRV> &rMeshShadow,
	        bool bPersistentFrameBuffer,
	        bool bPersistentCpuBuffers );
	bool prepareCold( const t3dRdForwardPassContext_t &rContext,
	        Terrain3DWorldResources &rResources,
	        const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	        const T3DTerrainStageASharedVTBindings &rVT );
	bool submitHot( const t3dRdForwardPassContext_t &rContext, Terrain3DWorldResources &rResources );
	void shutdownCold( void );
	bool isReady( void ) const;
	uint64_t getSubmitCount( t3dRdForwardPassKind_t pass ) const;
	uint64_t getPrepareCount( void ) const;
};

#endif // TERRAIN3D_WORLD_RASTER_H
