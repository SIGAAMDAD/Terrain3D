// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MATERIAL_CLASS_H
#define TERRAIN3D_MATERIAL_CLASS_H

#include "Terrain3DTypes.h"

#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "assets/resources/generated_texture.h"
#include "eastl_config.h"

#include <EASTL/vector.h>
#include <stdint.h>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

// Post-P34 R08 material decomposition. Terrain3DMaterial remains the serialized
// Resource facade; these embedded concrete owners separate authoring truth from
// derived runtime/VT state without introducing independent lifetimes.
class Terrain3DMaterialAuthoringState
{
public:
	bool												   m_bPersistenceDirty = false;
	mutable Dictionary									   m_shaderParams;
	eastl::vector< PropertyInfo, Terrain3DEastlAllocator > m_shaderPropertyCache;
	Dictionary											   m_shaderParamDefaults;
	real_t												   m_fDisplacementScale = 1.0f;
	real_t												   m_fDisplacementSharpness = 0.5f;
	t3dWorldBackground_t								   m_worldBackground = FLAT;
	t3dShaderPerformancePreset_t						   m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	t3dProjectionMode_t									   m_projectionMode = PROJECTION_DISABLED;
	t3dTessellationMaterialMode_t						   m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
	t3dWorldNoiseShaderMode_t							   m_worldNoiseShaderMode = WORLD_NOISE_DYNAMIC;
	bool												   m_bDualScalingEnabled = false;
	bool												   m_bAutoShaderEnabled = false;
	bool												   m_bMacroVariationEnabled = false;
	t3dTierQualityConfig_t								   m_nearQuality = { LINEAR,
																			 MATERIAL_SAMPLING_FULL,
																			 TERRAIN_NORMAL_QUALITY_DETAILED,
																			 TERRAIN_COLOR_QUALITY_EXACT,
																			 SURFACE_RESPONSE_DETAILED,
																			 TEXTURE_SURFACE_TEXTURED,
																			 TIER_LIGHTING_PBR,
																			 TIER_DETILING_ROTATION,
																			 1.0f,
																			 1.0f,
																			 1.0f };
	t3dTierQualityConfig_t								   m_middleQuality = { LINEAR,
																			   MATERIAL_SAMPLING_FULL,
																			   TERRAIN_NORMAL_QUALITY_DETAILED,
																			   TERRAIN_COLOR_QUALITY_FAST,
																			   SURFACE_RESPONSE_SIMPLE,
																			   TEXTURE_SURFACE_TEXTURED,
																			   TIER_LIGHTING_LAMBERT,
																			   TIER_DETILING_ROTATION,
																			   1.0f,
																			   1.0f,
																			   0.0f };
	t3dTierQualityConfig_t								   m_farQuality = { LINEAR,
																			MATERIAL_SAMPLING_FULL,
																			TERRAIN_NORMAL_QUALITY_GEOMETRY,
																			TERRAIN_COLOR_QUALITY_FAST,
																			SURFACE_RESPONSE_SIMPLE,
																			TEXTURE_SURFACE_TEXTURED,
																			TIER_LIGHTING_LAMBERT,
																			TIER_DETILING_DISABLED,
																			1.0f,
																			1.0f,
																			-0.5f };
	bool												   m_bOutputAlbedoEnabled = true;
	bool												   m_bOutputRoughnessEnabled = true;
	bool												   m_bOutputSpecularEnabled = true;
	bool												   m_bOutputNormalMapEnabled = true;
	bool												   m_bOutputAmbientOcclusionEnabled = true;
	bool												   m_bShowRegionGrid = false;
	bool												   m_bShowInstancerGrid = false;
	bool												   m_bShowVertexGrid = false;
	bool												   m_bShowContours = false;
	bool												   m_bShowNavigation = false;
	bool												   m_bDebugViewCheckered = false;
	bool												   m_bDebugViewGrey = false;
	bool												   m_bDebugViewHeightmap = false;
	bool												   m_bDebugViewJaggedness = false;
	bool												   m_bDebugViewAutoshader = false;
	bool												   m_bDebugViewControlTexture = false;
	bool												   m_bDebugViewControlBlend = false;
	bool												   m_bDebugViewControlAngle = false;
	bool												   m_bDebugViewControlScale = false;
	bool												   m_bDebugViewColormap = false;
	bool												   m_bDebugViewRoughmap = false;
	bool												   m_bDebugViewDisplacementBuffer = false;
	bool												   m_bPbrViewTexAlbedo = false;
	bool												   m_bPbrViewTexHeight = false;
	bool												   m_bPbrViewTexNormal = false;
	bool												   m_bPbrViewTexAo = false;
	bool												   m_bPbrViewTexRough = false;
};

class Terrain3DMaterialRuntimeState
{
public:
	mutable TypedArray< StringName > m_activeParams;
	mutable Dictionary				 m_runtimeMaterialParams;
	uint64_t						 m_nRdPublicationGeneration = 1u;
	uint64_t						 m_nEditorOverlayGeneration = 1u;
	uint64_t						 m_nShaderParameterListQueryCount = 0u;
	uint64_t						 m_nShaderParameterDefaultQueryCount = 0u;
	mutable uint64_t				 m_nShaderInspectorRuntimeReadCount = 0u;
	Terrain3DGeneratedTexture		 m_generatedDummy;
	Terrain3DGeneratedTexture		 m_generatedSurfaceDummy;
	Terrain3DGeneratedTexture		 m_generatedDisplacementDummy;
#ifdef DEBUG_ENABLED
	uint64_t m_szTierUniformPublicationCount[SHADER_TIER_COUNT] = {};
#endif
};

class t3dMaterialVTBinding_t
{
public:
	Terrain3DGeneratedTexture m_generatedRegionMap;
	uint64_t				  m_nRegionMapTextureHash = 0u;
	int						  m_nRegionMapTextureSize = 0;
	Terrain3DGeneratedTexture m_generatedLogicalRegionMap;
	uint64_t				  m_nLogicalRegionMapTextureHash = 0u;
	int						  m_nLogicalRegionMapTextureSize = 0;
	uint32_t				  m_nRegionPageTableOwnerGeneration = 0u;
};

class Terrain3DMaterial : public Resource
{
	GDCLASS( Terrain3DMaterial, Resource );
	CLASS_NAME();

public: // Constants
		// Controls the compile-time shader permutation cache. Feature modules
		// are gated by preprocessor defines and resolved through standard shader includes.
		// High-level profiles exposed by the Terrain3D editor sidebar. CUSTOM keeps
		// every individual hotpath control available. The other profiles assign a
		// bounded group of compile-time modes and then compile/cache that exact path.
		// Independently compiled clipmap shader tiers. These are internal runtime
		// materials, not user-facing material resources. Near preserves the selected
		// surface model, middle resolves only control/color/derived-normal data, and
		// far uses the coarse world atlases.
		// Controls terrain control-map and normal interpolation. Static modes remove
		// the distance-dependent bilinear branch from the generated fragment shader.
		// Controls the final terrain geometric normal source. Static modes remove the
		// public flat_terrain_normals uniform branch.
		// Controls regional color/wetness filtering. AUTO uses one hardware-filtered
		// sample inside a region and exact four-corner sampling only at layer edges.
		// Controls steep-surface UV projection. SHARED uses the final fragment terrain
		// normal for all material corners; EXACT evaluates each corner independently.
		// Controls whether tessellation displacement is sampled again in the fragment
		// material path for height blending/AO suppression.
		// Controls whether the material surface array is fetched. AUTO retains the
		// fetch only when an enabled PBR output consumes normal/roughness/AO data.
		// Controls whether terrain texture arrays participate in the surface pass.
		// SILHOUETTE preserves all authored texture/control data while compiling the
		// material texture sampling path out of the terrain shaders.
		// Selects compile-time Godot spatial render modes and lighting frequency.
		// Near, middle, and far rendering share one semantic quality schema.
		// Each tier maps these choices onto a tier-appropriate implementation rather
		// than exposing the legacy Near shader internals directly.
		// Detiling quality/cost. DISCRETE_8 replaces the sine hash plus per-material
		// sin/cos pair with an integer hash and an eight-entry rotation table.
		// Dynamic preserves the public uniforms. The static modes remove both world
		// noise source branches from the compiled shader.
private:
	Terrain3DOwner *m_pTerrain = NULL;

	Terrain3DMaterialAuthoringState m_authoringState;
	Terrain3DMaterialRuntimeState	m_runtimeState;
	t3dMaterialVTBinding_t			m_vtBinding;

	// Native material state / quality helpers. Renderer shader ownership lives under rendering/rd/.
	t3dShaderVariantState_t captureVariantState( void ) const;
	void					applyPerformancePresetToState( t3dShaderVariantState_t			 &rState,
														   const t3dShaderPerformancePreset_t preset ) const;
	void					applyXboxProfileManifestToState( t3dShaderVariantState_t &rState ) const;
	void					applyXboxProfileManifestToTiers( void );
	void					applyVariantState( const t3dShaderVariantState_t &rState );
	void					applyTierQualityToState( t3dShaderVariantState_t	  &rState,
													 const t3dTierQualityConfig_t &rQuality,
													 const t3dShaderTier_t		   tier ) const;
	void					applyPerformancePresetToTiers( const t3dShaderPerformancePreset_t preset );
	void					tierQualityChanged( const t3dShaderTier_t tier, const bool bRecompile = true );
	void					refreshGpuRegionChannelPolicy( void );
	void					updateTierUniforms( const t3dShaderTier_t tier );
	String					getTierRangeContext( const t3dShaderTier_t tier ) const;
	String					getTierShadowContext( const t3dShaderTier_t tier ) const;
	String					getTierShaderStatus( const t3dShaderTier_t tier ) const;
	void					bumpRdPublicationGeneration( void );
	void					setRuntimeMaterialParam( const StringName &rName, const Variant &rValue );
	void					refreshNativeMaterialState( const bool bRefreshSchema, const bool bNotifyPropertyList );
	void					refreshMaterialPropertyCache( void );

	void					   invalidateRegionPageTables( void );
	bool					   updateRegionMapTexture( const PackedInt32Array &rRegionMap, int nRegionMapSize );
	bool					   updateLogicalRegionMapTexture( const PackedInt32Array &rRegionMap, int nRegionMapSize );
	t3dPageTableBuildResult_t  prepareRegionPageTables( void );
	void					   setShaderParameters( const Dictionary &rDict );
	EA_FORCE_INLINE Dictionary getShaderParameters(
		void ) const {
		return m_authoringState.m_shaderParams;
	}

public:
	EA_FORCE_INLINE Terrain3DMaterial(
		void ) {
		refreshMaterialPropertyCache();
	}
	EA_FORCE_INLINE ~Terrain3DMaterial(
		void ) {
		destroy();
	}
	void				 initialize( Terrain3DOwner *pTerrain );
	EA_FORCE_INLINE bool isInitialized(
		void ) const {
		return m_pTerrain != NULL;
	}
	EA_FORCE_INLINE bool isInitializedFor(
		const Terrain3DOwner *pTerrain ) const {
		return m_pTerrain == pTerrain;
	}
	EA_FORCE_INLINE Terrain3DOwner *getTerrainOwner(
		void ) const {
		return m_pTerrain;
	}
	EA_FORCE_INLINE uint64_t getRdPublicationGeneration(
		void ) const {
		return m_runtimeState.m_nRdPublicationGeneration;
	}
	EA_FORCE_INLINE uint64_t getEditorOverlayGeneration(
		void ) const {
		return m_runtimeState.m_nEditorOverlayGeneration;
	}
	void uninitialize( Terrain3DOwner *pTerrain );
	void destroy( void );

	void				 update( const uint32_t nFlags = UNIFORMS_ONLY );
	void				 updateEditorOverlay( void );
	EA_FORCE_INLINE bool isRenderShaderReady(
		void ) const {
		return m_pTerrain != NULL;
	}
	void				 updateRegionPageTable( void );
	EA_FORCE_INLINE bool isRegionPageTableValid(
		void ) const {
		return m_vtBinding.m_generatedRegionMap.getRid().is_valid() &&
			   m_vtBinding.m_generatedLogicalRegionMap.getRid().is_valid() && m_vtBinding.m_nRegionMapTextureSize > 0 &&
			   m_vtBinding.m_nLogicalRegionMapTextureSize == m_vtBinding.m_nRegionMapTextureSize;
	}
	// Material settings
	void				   setDisplacementScale( const real_t fDisplacementScale );
	EA_FORCE_INLINE real_t getDisplacementScale(
		void ) const {
		return m_authoringState.m_fDisplacementScale;
	}
	void				   setDisplacementSharpness( const real_t fDisplacementSharpness );
	EA_FORCE_INLINE real_t getDisplacementSharpness(
		void ) const {
		return m_authoringState.m_fDisplacementSharpness;
	}
	void								 setWorldBackground( const t3dWorldBackground_t background );
	EA_FORCE_INLINE t3dWorldBackground_t getWorldBackground(
		void ) const {
		return m_authoringState.m_worldBackground;
	}
	// Shared 11-row quality-tier schema.
	void								  setNearTextureFiltering( t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getNearTextureFiltering(
		void ) const {
		return m_authoringState.m_nearQuality.m_textureFiltering;
	}
	void setNearMaterialSamplingQuality( t3dMaterialSamplingQuality_t eQuality );
	EA_FORCE_INLINE t3dMaterialSamplingQuality_t getNearMaterialSamplingQuality(
		void ) const {
		return m_authoringState.m_nearQuality.m_materialSamplingQuality;
	}
	void									  setNearTerrainNormalQuality( t3dTerrainNormalQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainNormalQuality_t getNearTerrainNormalQuality(
		void ) const {
		return m_authoringState.m_nearQuality.m_terrainNormalQuality;
	}
	void									 setNearTerrainColorQuality( t3dTerrainColorQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainColorQuality_t getNearTerrainColorQuality(
		void ) const {
		return m_authoringState.m_nearQuality.m_terrainColorQuality;
	}
	void										setNearSurfaceResponseQuality( t3dSurfaceResponseQuality_t eQuality );
	EA_FORCE_INLINE t3dSurfaceResponseQuality_t getNearSurfaceResponseQuality(
		void ) const {
		return m_authoringState.m_nearQuality.m_surfaceResponseQuality;
	}
	void									setNearTextureSurfaceMode( t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getNearTextureSurfaceMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_textureSurfaceMode;
	}
	void								   setNearLightingModel( t3dTierLightingModel_t model );
	EA_FORCE_INLINE t3dTierLightingModel_t getNearLightingModel(
		void ) const {
		return m_authoringState.m_nearQuality.m_lightingModel;
	}
	void									 setNearDetilingQuality( t3dTierDetilingQuality_t eQuality );
	EA_FORCE_INLINE t3dTierDetilingQuality_t getNearDetilingQuality(
		void ) const {
		return m_authoringState.m_nearQuality.m_detilingQuality;
	}
	void				   setNearAuthoredColorContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getNearAuthoredColorContribution(
		void ) const {
		return m_authoringState.m_nearQuality.m_fAuthoredColorContribution;
	}
	void				   setNearWetnessContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getNearWetnessContribution(
		void ) const {
		return m_authoringState.m_nearQuality.m_fWetnessContribution;
	}
	void				   setNearMaterialLodBias( real_t fBias );
	EA_FORCE_INLINE real_t getNearMaterialLodBias(
		void ) const {
		return m_authoringState.m_nearQuality.m_fMaterialLodBias;
	}

	void setMiddleMaterialSamplingQuality( t3dMaterialSamplingQuality_t eQuality );
	EA_FORCE_INLINE t3dMaterialSamplingQuality_t getMiddleMaterialSamplingQuality(
		void ) const {
		return m_authoringState.m_middleQuality.m_materialSamplingQuality;
	}
	EA_FORCE_INLINE void setMiddleSurfaceSource(
		t3dMiddleSurfaceSource_t eSource ) {
		setMiddleMaterialSamplingQuality( eSource == MIDDLE_SURFACE_BAKED_COMPOSITE ? MATERIAL_SAMPLING_FAST
																					: MATERIAL_SAMPLING_FULL );
	}
	EA_FORCE_INLINE t3dMiddleSurfaceSource_t getMiddleSurfaceSource(
		void ) const {
		return m_authoringState.m_middleQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST
				   ? MIDDLE_SURFACE_BAKED_COMPOSITE
				   : MIDDLE_SURFACE_RUNTIME_MATERIAL;
	}
	void									  setMiddleTerrainNormalQuality( t3dTerrainNormalQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainNormalQuality_t getMiddleTerrainNormalQuality(
		void ) const {
		return m_authoringState.m_middleQuality.m_terrainNormalQuality;
	}
	void									 setMiddleTerrainColorQuality( t3dTerrainColorQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainColorQuality_t getMiddleTerrainColorQuality(
		void ) const {
		return m_authoringState.m_middleQuality.m_terrainColorQuality;
	}
	void										setMiddleSurfaceResponseQuality( t3dSurfaceResponseQuality_t eQuality );
	EA_FORCE_INLINE t3dSurfaceResponseQuality_t getMiddleSurfaceResponseQuality(
		void ) const {
		return m_authoringState.m_middleQuality.m_surfaceResponseQuality;
	}
	void								   setMiddleLightingModel( t3dTierLightingModel_t model );
	EA_FORCE_INLINE t3dTierLightingModel_t getMiddleLightingModel(
		void ) const {
		return m_authoringState.m_middleQuality.m_lightingModel;
	}
	void									 setMiddleDetilingQuality( t3dTierDetilingQuality_t eQuality );
	EA_FORCE_INLINE t3dTierDetilingQuality_t getMiddleDetilingQuality(
		void ) const {
		return m_authoringState.m_middleQuality.m_detilingQuality;
	}
	void				   setMiddleAuthoredColorContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getMiddleAuthoredColorContribution(
		void ) const {
		return m_authoringState.m_middleQuality.m_fAuthoredColorContribution;
	}
	void				   setMiddleWetnessContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getMiddleWetnessContribution(
		void ) const {
		return m_authoringState.m_middleQuality.m_fWetnessContribution;
	}

	void										 setFarMaterialSamplingQuality( t3dMaterialSamplingQuality_t eQuality );
	EA_FORCE_INLINE t3dMaterialSamplingQuality_t getFarMaterialSamplingQuality(
		void ) const {
		return m_authoringState.m_farQuality.m_materialSamplingQuality;
	}
	void									  setFarTerrainNormalQuality( t3dTerrainNormalQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainNormalQuality_t getFarTerrainNormalQuality(
		void ) const {
		return m_authoringState.m_farQuality.m_terrainNormalQuality;
	}
	void									 setFarTerrainColorQuality( t3dTerrainColorQuality_t eQuality );
	EA_FORCE_INLINE t3dTerrainColorQuality_t getFarTerrainColorQuality(
		void ) const {
		return m_authoringState.m_farQuality.m_terrainColorQuality;
	}
	void										setFarSurfaceResponseQuality( t3dSurfaceResponseQuality_t eQuality );
	EA_FORCE_INLINE t3dSurfaceResponseQuality_t getFarSurfaceResponseQuality(
		void ) const {
		return m_authoringState.m_farQuality.m_surfaceResponseQuality;
	}
	void								   setFarLightingModel( t3dTierLightingModel_t model );
	EA_FORCE_INLINE t3dTierLightingModel_t getFarLightingModel(
		void ) const {
		return m_authoringState.m_farQuality.m_lightingModel;
	}
	void									 setFarDetilingQuality( t3dTierDetilingQuality_t eQuality );
	EA_FORCE_INLINE t3dTierDetilingQuality_t getFarDetilingQuality(
		void ) const {
		return m_authoringState.m_farQuality.m_detilingQuality;
	}
	void				   setFarAuthoredColorContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getFarAuthoredColorContribution(
		void ) const {
		return m_authoringState.m_farQuality.m_fAuthoredColorContribution;
	}
	void				   setFarWetnessContribution( real_t fContribution );
	EA_FORCE_INLINE real_t getFarWetnessContribution(
		void ) const {
		return m_authoringState.m_farQuality.m_fWetnessContribution;
	}
	void				   setFarMaterialLodBias( real_t fBias );
	EA_FORCE_INLINE real_t getFarMaterialLodBias(
		void ) const {
		return m_authoringState.m_farQuality.m_fMaterialLodBias;
	}

	EA_FORCE_INLINE String getNearRangeContext(
		void ) const {
		return getTierRangeContext( SHADER_TIER_NEAR );
	}
	EA_FORCE_INLINE String getMiddleRangeContext(
		void ) const {
		return getTierRangeContext( SHADER_TIER_MIDDLE );
	}
	EA_FORCE_INLINE String getFarRangeContext(
		void ) const {
		return getTierRangeContext( SHADER_TIER_FAR );
	}
	EA_FORCE_INLINE String getNearShadowContext(
		void ) const {
		return getTierShadowContext( SHADER_TIER_NEAR );
	}
	EA_FORCE_INLINE String getMiddleShadowContext(
		void ) const {
		return getTierShadowContext( SHADER_TIER_MIDDLE );
	}
	EA_FORCE_INLINE String getFarShadowContext(
		void ) const {
		return getTierShadowContext( SHADER_TIER_FAR );
	}
	EA_FORCE_INLINE String getNearShaderStatus(
		void ) const {
		return getTierShaderStatus( SHADER_TIER_NEAR );
	}
	EA_FORCE_INLINE String getMiddleShaderStatus(
		void ) const {
		return getTierShaderStatus( SHADER_TIER_MIDDLE );
	}
	EA_FORCE_INLINE String getFarShaderStatus(
		void ) const {
		return getTierShaderStatus( SHADER_TIER_FAR );
	}

	// Hidden serialized compatibility aliases. Runtime state is owned only by the
	// canonical t3dTierQualityConfig_t rows above.
	void								  setTextureFiltering( const t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getTextureFiltering(
		void ) const {
		return m_authoringState.m_nearQuality.m_textureFiltering;
	}
	void setShaderPerformancePreset( const t3dShaderPerformancePreset_t preset );
	EA_FORCE_INLINE t3dShaderPerformancePreset_t getShaderPerformancePreset(
		void ) const {
		return m_authoringState.m_shaderPerformancePreset;
	}
	// Middle tier quality. Existing unprefixed quality properties remain the Near tier.
	void								  setMiddleTextureFiltering( t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getMiddleTextureFiltering(
		void ) const {
		return m_authoringState.m_middleQuality.m_textureFiltering;
	}
	void									setMiddleTerrainSamplingMode( t3dTerrainSamplingMode_t mode );
	t3dTerrainSamplingMode_t				getMiddleTerrainSamplingMode( void ) const;
	void									setMiddleTerrainNormalMode( t3dTerrainNormalMode_t mode );
	t3dTerrainNormalMode_t					getMiddleTerrainNormalMode( void ) const;
	void									setMiddleColorSamplingMode( t3dColorSamplingMode_t mode );
	t3dColorSamplingMode_t					getMiddleColorSamplingMode( void ) const;
	void									setMiddleSurfaceDataMode( t3dSurfaceDataMode_t mode );
	t3dSurfaceDataMode_t					getMiddleSurfaceDataMode( void ) const;
	void									setMiddleTextureSurfaceMode( t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getMiddleTextureSurfaceMode(
		void ) const {
		return m_authoringState.m_middleQuality.m_textureSurfaceMode;
	}
	void					setMiddleTerrainShadingMode( t3dTerrainShadingMode_t mode );
	t3dTerrainShadingMode_t getMiddleTerrainShadingMode( void ) const;
	void					setMiddleDetilingMode( t3dDetilingMode_t mode );
	t3dDetilingMode_t		getMiddleDetilingMode( void ) const;
	void					setMiddleAutoShaderEnabled( bool bEnabled );
	EA_FORCE_INLINE bool	getMiddleAutoShaderEnabled(
		   void ) const {
		return m_authoringState.m_bAutoShaderEnabled;
	}
	void				 setMiddleAuthoredColorEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getMiddleAuthoredColorEnabled(
		void ) const {
		return m_authoringState.m_middleQuality.m_fAuthoredColorContribution > 0.0f;
	}
	void				 setMiddleWetnessEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getMiddleWetnessEnabled(
		void ) const {
		return m_authoringState.m_middleQuality.m_fWetnessContribution > 0.0f;
	}
	void				   setMiddleMaterialLodBias( real_t fBias );
	EA_FORCE_INLINE real_t getMiddleMaterialLodBias(
		void ) const {
		return m_authoringState.m_middleQuality.m_fMaterialLodBias;
	}

	// Far tier quality. The far shader intentionally remains a dominant-material, one-albedo-sample path.
	void								  setFarTextureFiltering( t3dTextureFiltering_t filtering );
	EA_FORCE_INLINE t3dTextureFiltering_t getFarTextureFiltering(
		void ) const {
		return m_authoringState.m_farQuality.m_textureFiltering;
	}
	void									setFarTextureSurfaceMode( t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getFarTextureSurfaceMode(
		void ) const {
		return m_authoringState.m_farQuality.m_textureSurfaceMode;
	}
	void					setFarTerrainShadingMode( t3dTerrainShadingMode_t mode );
	t3dTerrainShadingMode_t getFarTerrainShadingMode( void ) const;
	void					setFarMaterialLod( real_t fLod );
	EA_FORCE_INLINE real_t	getFarMaterialLod(
		 void ) const {
		return m_authoringState.m_farQuality.m_fMaterialLodBias + 4.0f;
	}
	void				 setFarAuthoredColorEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getFarAuthoredColorEnabled(
		void ) const {
		return m_authoringState.m_farQuality.m_fAuthoredColorContribution > 0.0f;
	}
	void				 setFarWetnessEnabled( bool bEnabled );
	EA_FORCE_INLINE bool getFarWetnessEnabled(
		void ) const {
		return m_authoringState.m_farQuality.m_fWetnessContribution > 0.0f;
	}

	void									 setTerrainSamplingMode( const t3dTerrainSamplingMode_t mode );
	EA_FORCE_INLINE t3dTerrainSamplingMode_t getTerrainSamplingMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST
				   ? TERRAIN_SAMPLING_SINGLE_SAMPLE
				   : TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED;
	}
	void								   setTerrainNormalMode( const t3dTerrainNormalMode_t mode );
	EA_FORCE_INLINE t3dTerrainNormalMode_t getTerrainNormalMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_terrainNormalQuality == TERRAIN_NORMAL_QUALITY_GEOMETRY
				   ? TERRAIN_NORMAL_GEOMETRY_DERIVATIVE
				   : TERRAIN_NORMAL_DERIVED_TEXTURE;
	}
	void								   setColorSamplingMode( const t3dColorSamplingMode_t mode );
	EA_FORCE_INLINE t3dColorSamplingMode_t getColorSamplingMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_terrainColorQuality == TERRAIN_COLOR_QUALITY_FAST
				   ? COLOR_SAMPLING_HARDWARE_FILTERED
				   : COLOR_SAMPLING_EXACT_4_TAP;
	}
	void								setProjectionMode( const t3dProjectionMode_t mode );
	EA_FORCE_INLINE t3dProjectionMode_t getProjectionMode(
		void ) const {
		return m_authoringState.m_projectionMode;
	}
	void setTessellationMaterialMode( const t3dTessellationMaterialMode_t mode );
	EA_FORCE_INLINE t3dTessellationMaterialMode_t getTessellationMaterialMode(
		void ) const {
		return m_authoringState.m_tessellationMaterialMode;
	}
	void								 setSurfaceDataMode( const t3dSurfaceDataMode_t mode );
	EA_FORCE_INLINE t3dSurfaceDataMode_t getSurfaceDataMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_surfaceResponseQuality == SURFACE_RESPONSE_SIMPLE
				   ? SURFACE_DATA_ALBEDO_ONLY
				   : SURFACE_DATA_FULL;
	}
	void									setTextureSurfaceMode( const t3dTextureSurfaceMode_t mode );
	EA_FORCE_INLINE t3dTextureSurfaceMode_t getTextureSurfaceMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_textureSurfaceMode;
	}
	void					setTerrainShadingMode( const t3dTerrainShadingMode_t mode );
	t3dTerrainShadingMode_t getTerrainShadingMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_lightingModel == TIER_LIGHTING_UNSHADED
				   ? SHADING_UNSHADED
				   : ( m_authoringState.m_nearQuality.m_lightingModel == TIER_LIGHTING_BLINN_PHONG
						   ? SHADING_XBOX_360_BLINN_PHONG
						   : ( m_authoringState.m_nearQuality.m_lightingModel == TIER_LIGHTING_PBR
								   ? SHADING_BURLEY_GGX
								   : SHADING_LAMBERT_DIFFUSE_ONLY ) );
	}
	void							  setDetilingMode( const t3dDetilingMode_t mode );
	EA_FORCE_INLINE t3dDetilingMode_t getDetilingMode(
		void ) const {
		return m_authoringState.m_nearQuality.m_detilingQuality == TIER_DETILING_DISABLED ? DETILING_DISABLED
																						  : DETILING_DISCRETE_8;
	}
	void									  setWorldNoiseShaderMode( const t3dWorldNoiseShaderMode_t mode );
	EA_FORCE_INLINE t3dWorldNoiseShaderMode_t getWorldNoiseShaderMode(
		void ) const {
		return m_authoringState.m_worldNoiseShaderMode;
	}
	Dictionary			 getShaderTierDiagnostics( const int nTier ) const;
	void				 setPackedPbrSingleFetch( const bool bEnabled );
	EA_FORCE_INLINE bool getPackedPbrSingleFetch(
		void ) const {
		return false;
	}
	void setAutoShaderEnabled( const bool bEnabled );
	// Editor preview helper. This changes only the active shader state and does
	// not alter the serialized performance preset or dirty the material resource.
	void				 setAutoShaderEnabledTransient( const bool bEnabled );
	EA_FORCE_INLINE bool getAutoShaderEnabled(
		void ) const {
		return m_authoringState.m_bAutoShaderEnabled;
	}
	void				 setDualScalingEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getDualScalingEnabled(
		void ) const {
		return m_authoringState.m_bDualScalingEnabled;
	}
	void				 setMacroVariationEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getMacroVariationEnabled(
		void ) const {
		return m_authoringState.m_bMacroVariationEnabled;
	}
	void				 setProjectionEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getProjectionEnabled(
		void ) const {
		return m_authoringState.m_projectionMode != PROJECTION_DISABLED;
	}

	void	setShaderParam( const StringName &rName, const Variant &rValue );
	Variant getShaderParam( const StringName &rName ) const;

	// PBR outputs
	void				 setOutputAlbedoEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputAlbedoEnabled(
		void ) const {
		return m_authoringState.m_bOutputAlbedoEnabled;
	}
	void				 setOutputRoughnessEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputRoughnessEnabled(
		void ) const {
		return m_authoringState.m_bOutputRoughnessEnabled;
	}
	void				 setOutputSpecularEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputSpecularEnabled(
		void ) const {
		return m_authoringState.m_bOutputSpecularEnabled;
	}
	void				 setOutputNormalMapEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputNormalMapEnabled(
		void ) const {
		return m_authoringState.m_bOutputNormalMapEnabled;
	}
	void				 setOutputAmbientOcclusionEnabled( const bool bEnabled );
	EA_FORCE_INLINE bool getOutputAmbientOcclusionEnabled(
		void ) const {
		return m_authoringState.m_bOutputAmbientOcclusionEnabled;
	}

	// Overlays
	void				 setShowRegionGrid( const bool bEnabled );
	EA_FORCE_INLINE bool getShowRegionGrid(
		void ) const {
		return m_authoringState.m_bShowRegionGrid;
	}
	void				 setShowInstancerGrid( const bool bEnabled );
	EA_FORCE_INLINE bool getShowInstancerGrid(
		void ) const {
		return m_authoringState.m_bShowInstancerGrid;
	}
	void				 setShowVertexGrid( const bool bEnabled );
	EA_FORCE_INLINE bool getShowVertexGrid(
		void ) const {
		return m_authoringState.m_bShowVertexGrid;
	}
	void				 setShowContours( const bool bEnabled );
	EA_FORCE_INLINE bool getShowContours(
		void ) const {
		return m_authoringState.m_bShowContours;
	}
	void				 setShowNavigation( const bool bEnabled );
	EA_FORCE_INLINE bool getShowNavigation(
		void ) const {
		return m_authoringState.m_bShowNavigation;
	}

	// Debug views
	void				 setShowCheckered( const bool bEnabled );
	EA_FORCE_INLINE bool getShowCheckered(
		void ) const {
		return m_authoringState.m_bDebugViewCheckered;
	}
	void				 setShowGrey( const bool bEnabled );
	EA_FORCE_INLINE bool getShowGrey(
		void ) const {
		return m_authoringState.m_bDebugViewGrey;
	}
	void				 setShowHeightmap( const bool bEnabled );
	EA_FORCE_INLINE bool getShowHeightmap(
		void ) const {
		return m_authoringState.m_bDebugViewHeightmap;
	}
	void				 setShowJaggedness( const bool bEnabled );
	EA_FORCE_INLINE bool getShowJaggedness(
		void ) const {
		return m_authoringState.m_bDebugViewJaggedness;
	}
	void				 setShowAutoshader( const bool bEnabled );
	EA_FORCE_INLINE bool getShowAutoshader(
		void ) const {
		return m_authoringState.m_bDebugViewAutoshader;
	}
	void				 setShowControlTexture( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlTexture(
		void ) const {
		return m_authoringState.m_bDebugViewControlTexture;
	}
	void				 setShowControlBlend( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlBlend(
		void ) const {
		return m_authoringState.m_bDebugViewControlBlend;
	}
	void				 setShowControlAngle( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlAngle(
		void ) const {
		return m_authoringState.m_bDebugViewControlAngle;
	}
	void				 setShowControlScale( const bool bEnabled );
	EA_FORCE_INLINE bool getShowControlScale(
		void ) const {
		return m_authoringState.m_bDebugViewControlScale;
	}
	void				 setShowColormap( const bool bEnabled );
	EA_FORCE_INLINE bool getShowColormap(
		void ) const {
		return m_authoringState.m_bDebugViewColormap;
	}
	void				 setShowRoughmap( const bool bEnabled );
	EA_FORCE_INLINE bool getShowRoughmap(
		void ) const {
		return m_authoringState.m_bDebugViewRoughmap;
	}
	void				 setShowDisplacementBuffer( const bool bEnabled );
	EA_FORCE_INLINE bool getShowDisplacementBuffer(
		void ) const {
		return m_authoringState.m_bDebugViewDisplacementBuffer;
	}

	// PBR Views
	void				 setShowTextureAlbedo( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureAlbedo(
		void ) const {
		return m_authoringState.m_bPbrViewTexAlbedo;
	}
	void				 setShowTextureHeight( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureHeight(
		void ) const {
		return m_authoringState.m_bPbrViewTexHeight;
	}
	void				 setShowTextureNormal( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureNormal(
		void ) const {
		return m_authoringState.m_bPbrViewTexNormal;
	}
	void				 setShowTextureRough( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureRough(
		void ) const {
		return m_authoringState.m_bPbrViewTexRough;
	}
	void				 setShowTextureAo( const bool bEnabled );
	EA_FORCE_INLINE bool getShowTextureAo(
		void ) const {
		return m_authoringState.m_bPbrViewTexAo;
	}

	Error save( const String &rPath = "" );

protected:
	void _get_property_list( List< PropertyInfo > *pList ) const;
	bool _property_can_revert( const StringName &rName ) const;
	bool _property_get_revert( const StringName &rName, Variant &rProperty ) const;
	bool _set( const StringName &rName, const Variant &rProperty );
	bool _get( const StringName &rName, Variant &rProperty ) const;

	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dWorldBackground_t );
VARIANT_ENUM_CAST( t3dTextureFiltering_t );
VARIANT_ENUM_CAST( t3dUpdateFlags_t );
VARIANT_ENUM_CAST( t3dShaderPerformancePreset_t );
VARIANT_ENUM_CAST( t3dTerrainSamplingMode_t );
VARIANT_ENUM_CAST( t3dTerrainNormalMode_t );
VARIANT_ENUM_CAST( t3dColorSamplingMode_t );
VARIANT_ENUM_CAST( t3dProjectionMode_t );
VARIANT_ENUM_CAST( t3dTessellationMaterialMode_t );
VARIANT_ENUM_CAST( t3dSurfaceDataMode_t );
VARIANT_ENUM_CAST( t3dTextureSurfaceMode_t );
VARIANT_ENUM_CAST( t3dTerrainShadingMode_t );
VARIANT_ENUM_CAST( t3dDetilingMode_t );
VARIANT_ENUM_CAST( t3dMaterialSamplingQuality_t );
VARIANT_ENUM_CAST( t3dMiddleSurfaceSource_t );
VARIANT_ENUM_CAST( t3dTerrainNormalQuality_t );
VARIANT_ENUM_CAST( t3dTerrainColorQuality_t );
VARIANT_ENUM_CAST( t3dSurfaceResponseQuality_t );
VARIANT_ENUM_CAST( t3dTierLightingModel_t );
VARIANT_ENUM_CAST( t3dTierDetilingQuality_t );
VARIANT_ENUM_CAST( t3dWorldNoiseShaderMode_t );
#endif // TERRAIN3D_MATERIAL_CLASS_H
