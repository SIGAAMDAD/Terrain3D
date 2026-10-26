// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_RASTER_H
#define TERRAIN3D_WORLD_RASTER_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_lease_table.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"
#include "rendering/rd/environment/terrain_3d_rd_environment_field.h"
#include "rendering/rd/vt/gpu/terrain_3d_composed_surface_gpu.h"
#include "rendering/rd/superterrain/terrain_3d_st_gpu_resources.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>

class Terrain3DVTGpu;

struct t3dWorldShaderCapabilityKey_t
{
	const Terrain3DVTGpu *m_pFeedbackOwner = NULL;
	uint32_t m_nFeedbackGeneration = 0u;
	bool m_bFeedbackReady = false;
	bool m_bEnvironmentReady = false;
	bool m_bCompactRequested = false;

	constexpr bool operator==( const t3dWorldShaderCapabilityKey_t &rOther ) const {
		return m_pFeedbackOwner == rOther.m_pFeedbackOwner &&
			m_nFeedbackGeneration == rOther.m_nFeedbackGeneration &&
			m_bFeedbackReady == rOther.m_bFeedbackReady &&
			m_bEnvironmentReady == rOther.m_bEnvironmentReady &&
			m_bCompactRequested == rOther.m_bCompactRequested;
	}
};

class Terrain3DWorldRaster
{
private:
	typedef struct t3dPipelineKey_s : public t3dRdForwardPipelineBaseKey_t
	{
		uint32_t m_nPrimitiveClass = T3D_PRIMITIVE_TERRAIN_STRIP;
        bool m_bEnvironmentVariant = false;
		bool	 operator==(
			const t3dPipelineKey_s &rOther ) const {
			return ( const t3dRdForwardPipelineBaseKey_t & ) ( *this ) ==
					   ( const t3dRdForwardPipelineBaseKey_t & ) ( rOther ) &&
				   m_nPrimitiveClass == rOther.m_nPrimitiveClass && m_bEnvironmentVariant == rOther.m_bEnvironmentVariant;
		}
	} t3dPipelineKey_t;

	typedef Terrain3DRdPipelineCache< t3dPipelineKey_t, T3D_WORLD_PIPELINE_CAPACITY > t3dPipelineCache_t;
	typedef t3dRdPipelineCacheEntry_t< t3dPipelineKey_t >							  t3dPipelineEntry_t;
	typedef struct t3dSetSignature_s
	{
		godot::RID shader;
		uint64_t   world = 0u;
		uint64_t   dependency = 0u;
		bool	   operator==(
			  const t3dSetSignature_s &rOther ) const {
			return shader == rOther.shader && world == rOther.world && dependency == rOther.dependency;
		}
	} t3dSetSignature_t;

	Terrain3DRdContext		  &m_context;
    Terrain3DStGpuImage m_stGpuImage; // Immutable six-resource candidate, swapped cold.
	PackedByteArray			   m_hotPushBytes;
	Terrain3DRdForwardLighting m_forwardLighting;
	Terrain3DRdShader		   m_hShader[T3D_PRIMITIVE_CLASS_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	Terrain3DVTGpu			  *m_pFeedbackOwner = NULL; // Borrowed shared VT owner.
	t3dWorldShaderCapabilityKey_t m_shaderCapabilities{};
	bool m_bShaderCapabilitiesValid = false;
	bool					   m_bMeshFeedback[T3D_RD_FORWARD_COLOR_VARIANT_COUNT]{};
	bool m_bCompactShader[T3D_PRIMITIVE_CLASS_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX]{};
	RID m_hCompactSet[T3D_PRIMITIVE_CLASS_COUNT]{};
    static const uint32_t ENV_SHADOW_SLOT = T3D_RD_FORWARD_SHADER_VARIANT_MAX;
    RID m_hEnvironmentSet[T3D_PRIMITIVE_CLASS_COUNT][ENV_SHADOW_SLOT + 1u][2]{};
    RID m_hCurrentEnvironmentSet[T3D_PRIMITIVE_CLASS_COUNT]{};
    bool m_bEnvironmentShader[T3D_PRIMITIVE_CLASS_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX]{};
    bool m_bCurrentEnvironmentShader[T3D_PRIMITIVE_CLASS_COUNT]{};
    bool m_bEnvironmentReady = false;
    uint64_t m_nStaleEnvironmentSetMisses = 0u;
    Terrain3DRdShader m_hEnvironmentMeshShadowShader;
    Terrain3DRdDescriptorLeaseTable< t3dSetSignature_t, T3D_PRIMITIVE_CLASS_COUNT * ( ENV_SHADOW_SLOT + 1u ) * 2u > m_environmentSetCache;
	godot::RID				   m_hCurrentFeedbackSet;
	Terrain3DRdShader		   m_hShadowShader[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID				   m_hSet0[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT];
	godot::RID				   m_hSet1[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT];
	godot::RID				   m_hCurrentSet0[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID				   m_hCurrentSet1[T3D_PRIMITIVE_CLASS_COUNT];
	godot::RID				   m_hLightingSet[T3D_PRIMITIVE_CLASS_COUNT];
	t3dPipelineCache_t		   m_pipelines;
	Terrain3DRdDescriptorLeaseTable< t3dSetSignature_t, T3D_PRIMITIVE_CLASS_COUNT * T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT >
		m_set0Cache;
	Terrain3DRdDescriptorLeaseTable< t3dSetSignature_t, T3D_PRIMITIVE_CLASS_COUNT * T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT >
					  m_set1Cache;
	t3dPipelineKey_t  m_currentKey[T3D_PRIMITIVE_CLASS_COUNT]{};
	t3dSetSignature_t m_set0Signature[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT]{};
	t3dSetSignature_t m_set1Signature[T3D_PRIMITIVE_CLASS_COUNT][T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT]{};
	uint64_t		  m_nPrepareCount = 0u;
	uint64_t		  m_nSubmitCount[T3D_RD_FORWARD_PASS_MAX]{};
	bool			  m_bCurrentValid = false;
	bool			  m_bInitialized = false;
	bool m_bExperimentalComposed = false;
	bool			  m_bPersistentFrameBuffer = false;
	bool			  m_bPersistentCpuBuffers = false;

	bool					  ensureShaderCold( t3dPrimitiveClass_t primitive, t3dRdForwardShaderVariant_t variant );
	bool					  ensureSetsCold( Terrain3DWorldResources				  &rResources,
											  const t3dRdWorldTerrainRasterBindings_t &rTerrain,
											  const t3dSharedVTBindings_t			  &rVT,
											  const godot::RID						  &rShader,
											  t3dPrimitiveClass_t					   primitiveClass,
											  uint32_t								   nDescriptorSlot,
	const t3dComposedSurfaceGpuBindings_t *pComposed );
    bool ensureEnvironmentSetsCold( const RID &rShader, uint32_t nPrimitive, uint32_t nVariant,
                                    const t3dRdEnvironmentBindings_t &rBindings );
	bool					  ensurePipelineCold( const t3dRdForwardPassContext_t &rContext,
												  t3dRdForwardShaderVariant_t	   variant,
												  t3dPrimitiveClass_t			   primitiveClass );
	const t3dPipelineEntry_t *findPipeline( const t3dPipelineKey_t &rKey ) const;
	void					  freeSetsCold( void );
	void                      invalidateMaterialShadersCold( void );

public:
	explicit Terrain3DWorldRaster( Terrain3DRdContext &rContext );
	~Terrain3DWorldRaster( void );
    // Render-device-thread cold boundary. The input must already have passed
    // sidecar, source/recipe signature and revision validation in prepareCold.
    // The region-table producer must publish matching records after success.
    // Render-thread cold rollback: only callable before owner-side commit.
    void discardSuperTerrainCandidateCold( void );
    bool uploadSuperTerrainCandidateCold( const Terrain3DStPublicationImage &rCandidate,
                                           uint32_t nFinePageSlots );
    t3dStGpuBindings_t getSuperTerrainBindingsCold( void ) const { return m_stGpuImage.bindings(); }
	bool configureComposedExperimentalCold( bool bEnable );
	bool isComposedExperimentalEnabled() const { return m_bExperimentalComposed; }
	bool	 initializeCold( bool bPersistentFrameBuffer, bool bPersistentCpuBuffers );
	bool	 prepareCold( const t3dRdForwardPassContext_t		  &rContext,
						  Terrain3DWorldResources				  &rResources,
						  const t3dRdWorldTerrainRasterBindings_t &rTerrain,
						  const t3dSharedVTBindings_t			  &rVT,
						  Terrain3DVTGpu						  *pFeedbackOwner = NULL,
	const t3dComposedSurfaceGpuBindings_t *pComposed = NULL,
    const t3dRdEnvironmentBindings_t *pEnvironment = NULL );
	bool	 submitHot( const t3dRdForwardPassContext_t &rContext, Terrain3DWorldResources &rResources );
	void	 shutdownCold( void );
	bool	 isReady( void ) const;
	uint64_t getSubmitCount( t3dRdForwardPassKind_t pass ) const;
    uint64_t getPrepareCount( void ) const;
    uint64_t getEnvironmentStaleSetMisses() const { return m_nStaleEnvironmentSetMisses; }
};

#endif // TERRAIN3D_WORLD_RASTER_H
