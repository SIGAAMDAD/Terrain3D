// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_render_coordinator.h"

#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "instances/terrain_3d_instancer.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "rendering/terrain_3d_material.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <cmath>
#include <cstring>


static float t3dRdMaterialFloatParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const float fDefault ) {
	if ( !pMaterial ) return fDefault;
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT ? ( float )( value ) : fDefault;
}

static uint32_t t3dRdMaterialUintParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const uint32_t nDefault ) {
	if ( !pMaterial ) return nDefault;
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::INT ? ( uint32_t )( MAX( ( int64_t )( value ), int64_t( 0 ) ) ) : nDefault;
}

static bool t3dRdMaterialBoolParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const bool bDefault ) {
	if ( !pMaterial ) return bDefault;
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::BOOL ? ( bool )( value ) : bDefault;
}

static Color t3dRdMaterialColorParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Color &rDefault ) {
	if ( !pMaterial ) return rDefault;
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	if ( value.get_type() == Variant::COLOR ) return ( Color )( value );
	if ( value.get_type() == Variant::VECTOR3 ) {
		const Vector3 vector = value;
		return Color( vector.x, vector.y, vector.z, 1.0f );
	}
	return rDefault;
}

static Vector2 t3dRdMaterialVector2Param( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Vector2 &rDefault ) {
	if ( !pMaterial ) return rDefault;
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR2 ? ( Vector2 )( value ) : rDefault;
}

static Vector3 t3dRdMaterialVector3Param( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Vector3 &rDefault ) {
	if ( !pMaterial ) return rDefault;
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR3 ? ( Vector3 )( value ) : rDefault;
}

static uint32_t t3dRdSamplingMode( const t3dMaterialSamplingQuality_t quality ) {
	return quality == MATERIAL_SAMPLING_FAST ? ( uint32_t )( TERRAIN_SAMPLING_SINGLE_SAMPLE )
									 : ( uint32_t )( TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED );
}

static uint32_t t3dRdNormalMode( const t3dTerrainNormalQuality_t quality ) {
	return quality == TERRAIN_NORMAL_QUALITY_GEOMETRY ? ( uint32_t )( TERRAIN_NORMAL_GEOMETRY_DERIVATIVE )
									 : ( uint32_t )( TERRAIN_NORMAL_DERIVED_TEXTURE );
}

static uint32_t t3dRdColorMode( const t3dTerrainColorQuality_t quality ) {
	return quality == TERRAIN_COLOR_QUALITY_FAST ? ( uint32_t )( COLOR_SAMPLING_HARDWARE_FILTERED )
									 : ( uint32_t )( COLOR_SAMPLING_EXACT_4_TAP );
}

static uint32_t t3dRdDetilingMode( const t3dTierDetilingQuality_t quality ) {
	return quality == TIER_DETILING_DISABLED ? ( uint32_t )( DETILING_DISABLED ) : ( uint32_t )( DETILING_DISCRETE_8 );
}

static void t3dRdFillTierConfig( t3dRdTierMaterialConfig_t &rConfig,
		const t3dMaterialSamplingQuality_t sampling,
		const t3dTerrainNormalQuality_t normal,
		const t3dTerrainColorQuality_t color,
		const t3dSurfaceResponseQuality_t surface,
		const t3dTextureSurfaceMode_t textureSurface,
		const t3dTierLightingModel_t lighting,
		const t3dTierDetilingQuality_t detiling,
		const t3dTextureFiltering_t filtering,
		const float fAuthoredColor,
		const float fWetness,
		const float fLodBias,
		const float fQualityLodOffset ) {
	rConfig.m_nSamplingMode = t3dRdSamplingMode( sampling );
	rConfig.m_nNormalMode = t3dRdNormalMode( normal );
	rConfig.m_nColorMode = t3dRdColorMode( color );
	rConfig.m_nSurfaceResponse = ( uint32_t )( surface );
	rConfig.m_nTextureSurfaceMode = ( uint32_t )( textureSurface );
	rConfig.m_nLightingModel = ( uint32_t )( lighting );
	rConfig.m_nDetilingMode = t3dRdDetilingMode( detiling );
	rConfig.m_nFiltering = ( uint32_t )( filtering );
	rConfig.m_fAuthoredColorContribution = CLAMP( fAuthoredColor, 0.0f, 1.0f );
	rConfig.m_fWetnessContribution = CLAMP( fWetness, 0.0f, 1.0f );
	rConfig.m_fMaterialLodBias = fLodBias;
	rConfig.m_fQualityLodOffset = fQualityLodOffset;
}

Terrain3DRenderCoordinator::~Terrain3DRenderCoordinator( void ) { shutdown(); }

uint64_t Terrain3DRenderCoordinator::hashBytes( const void *const pData, const size_t nBytes ) const {
	uint64_t hash = 1469598103934665603ULL;
	const uint8_t *const bytes = ( const uint8_t * )( pData );
	for ( size_t i = 0u; i < nBytes; ++i ) {
		hash ^= ( uint64_t )( bytes[i] );
		hash *= 1099511628211ULL;
	}
	return hash;
}

uint64_t Terrain3DRenderCoordinator::advanceGeneration( const uint64_t nGeneration ) const {
	return nGeneration == UINT64_MAX ? 1u : nGeneration + 1u;
}


bool Terrain3DRenderCoordinator::ensureWorldNoiseCache( const Terrain3DMaterial *const pMaterial ) {
	if ( !pMaterial || pMaterial->getWorldBackground() != NOISE ) return true;
	const t3dWorldNoiseShaderMode_t mode = pMaterial->getWorldNoiseShaderMode();
	const bool bCacheEnabled = t3dRdMaterialBoolParam( pMaterial, "world_noise_cache_enabled", true );
	const bool bNeedsCache = mode == WORLD_NOISE_CACHED_VERTEX_NORMALS || mode == WORLD_NOISE_CACHED_FRAGMENT_NORMALS ||
			( mode == WORLD_NOISE_DYNAMIC && bCacheEnabled );
	if ( !bNeedsCache || m_worldNoiseCache.getRid().is_valid() ) return true;
	Ref<FastNoiseLite> noise;
	noise.instantiate();
	noise->set_noise_type( FastNoiseLite::TYPE_SIMPLEX_SMOOTH );
	noise->set_seed( 1337 );
	noise->set_frequency( 0.0125f );
	noise->set_fractal_type( FastNoiseLite::FRACTAL_FBM );
	noise->set_fractal_octaves( 10 );
	noise->set_fractal_lacunarity( 2.0f );
	noise->set_fractal_gain( 0.5f );
	noise->set_fractal_weighted_strength( 0.35f );
	noise->set_domain_warp_enabled( true );
	noise->set_domain_warp_type( FastNoiseLite::DOMAIN_WARP_SIMPLEX_REDUCED );
	noise->set_domain_warp_amplitude( 24.0f );
	noise->set_domain_warp_fractal_type( FastNoiseLite::DOMAIN_WARP_FRACTAL_PROGRESSIVE );
	noise->set_domain_warp_fractal_octaves( 4 );
	noise->set_domain_warp_fractal_lacunarity( 2.0f );
	noise->set_domain_warp_fractal_gain( 0.5f );
	Ref<Image> image = noise->get_seamless_image( 1024, 1024, false, false, 0.1f, true );
	if ( image.is_null() || image->is_empty() ) {
		LOG( ERROR, "Failed to generate direct RD world-noise cache image" );
		return false;
	}
	image->generate_mipmaps();
	if ( !m_worldNoiseCache.create( image ).is_valid() ) {
		LOG( ERROR, "Failed to create direct RD world-noise cache texture" );
		return false;
	}
	return true;
}

bool Terrain3DRenderCoordinator::ensureMacroNoiseTexture( const Terrain3DMaterial *const pMaterial ) {
	if ( !pMaterial || !pMaterial->getMacroVariationEnabled() ) return true;
	const Variant source_value = pMaterial->getShaderParam( StringName( "noise_texture" ) );
	Ref<Image> image;
	uint64_t source_id = 0u;
	if ( source_value.get_type() == Variant::OBJECT ) {
		const Ref<Texture2D> source_texture = source_value;
		if ( source_texture.is_valid() ) {
			source_id = ( uint64_t )( source_texture->get_instance_id() );
			if ( source_id == m_nMacroNoiseSourceId && m_macroNoiseTexture.getRid().is_valid() ) return true;
			const Ref<Image> source_image = source_texture->get_image();
			if ( source_image.is_valid() && !source_image->is_empty() ) image = source_image->duplicate();
		}
	}
	if ( image.is_null() || image->is_empty() ) {
		Ref<FastNoiseLite> noise;
		noise.instantiate();
		noise->set_noise_type( FastNoiseLite::TYPE_CELLULAR );
		noise->set_frequency( 0.03f );
		noise->set_cellular_jitter( 3.0f );
		noise->set_cellular_return_type( FastNoiseLite::RETURN_CELL_VALUE );
		noise->set_domain_warp_enabled( true );
		noise->set_domain_warp_type( FastNoiseLite::DOMAIN_WARP_SIMPLEX_REDUCED );
		noise->set_domain_warp_amplitude( 50.0f );
		noise->set_domain_warp_fractal_type( FastNoiseLite::DOMAIN_WARP_FRACTAL_INDEPENDENT );
		noise->set_domain_warp_fractal_lacunarity( 1.5f );
		noise->set_domain_warp_fractal_gain( 1.0f );
		image = noise->get_seamless_image( 512, 512, false, false, 0.1f, true );
		if ( image.is_null() || image->is_empty() ) {
			LOG( ERROR, "Failed to generate direct RD macro-variation noise image" );
			return false;
		}
		if ( image->get_format() != Image::FORMAT_L8 ) image->convert( Image::FORMAT_L8 );
		PackedByteArray pixels = image->get_data();
		uint8_t *const pPixels = pixels.ptrw();
		for ( int64_t i = 0; i < pixels.size(); ++i ) {
			const float input = ( float )( pPixels[i] ) * ( 1.0f / 255.0f );
			const float gradient = input <= 0.2f ? 1.0f : CLAMP( 1.0f - ( input - 0.2f ) * 1.25f, 0.0f, 1.0f );
			pPixels[i] = ( uint8_t )( gradient * 255.0f + 0.5f );
		}
		image = Image::create_from_data( image->get_width(), image->get_height(), false, Image::FORMAT_L8, pixels );
		source_id = 0u;
	} else if ( image->get_format() != Image::FORMAT_L8 ) {
		image->convert( Image::FORMAT_L8 );
	}
	if ( image.is_null() || image->is_empty() ) return false;
	if ( !image->has_mipmaps() ) image->generate_mipmaps();
	if ( !m_macroNoiseTexture.create( image ).is_valid() ) {
		LOG( ERROR, "Failed to create direct RD macro-variation noise texture" );
		return false;
	}
	m_nMacroNoiseSourceId = source_id;
	return true;
}

bool Terrain3DRenderCoordinator::ensureEditorBrushTexture( const Terrain3DMaterial *const pMaterial ) {
	if ( !pMaterial ) return true;
	const Variant source_value = pMaterial->getShaderParam( StringName( "_editor_brush_texture" ) );
	if ( source_value.get_type() != Variant::OBJECT ) {
		if ( m_editorBrushTexture.getRid().is_valid() ) m_editorBrushTexture.clear();
		m_nEditorBrushSourceId = 0u;
		return true;
	}
	const Ref<Texture2D> source_texture = source_value;
	if ( source_texture.is_null() ) {
		if ( m_editorBrushTexture.getRid().is_valid() ) m_editorBrushTexture.clear();
		m_nEditorBrushSourceId = 0u;
		return true;
	}
	const uint64_t source_id = ( uint64_t )( source_texture->get_instance_id() );
	if ( source_id == m_nEditorBrushSourceId && m_editorBrushTexture.getRid().is_valid() ) return true;
	Ref<Image> image = source_texture->get_image();
	if ( image.is_null() || image->is_empty() ) {
		LOG( ERROR, "Unable to read Terrain3D editor brush texture for direct RD publication" );
		return false;
	}
	image = image->duplicate();
	if ( image->is_compressed() && image->decompress() != OK ) {
		LOG( ERROR, "Unable to decompress Terrain3D editor brush texture for direct RD publication" );
		return false;
	}
	if ( image->get_format() != Image::FORMAT_L8 ) image->convert( Image::FORMAT_L8 );
	if ( image->has_mipmaps() ) image->clear_mipmaps();
	Terrain3DGeneratedTexture replacement;
	if ( !replacement.create( image ).is_valid() ) {
		LOG( ERROR, "Unable to create Terrain3D direct RD editor brush texture" );
		return false;
	}
	m_editorBrushTexture.swap( replacement );
	m_nEditorBrushSourceId = source_id;
	return true;
}

void Terrain3DRenderCoordinator::buildEditorOverlayState(
		t3dRdFrameSnapshot_t &rSnapshot, const Terrain3DMaterial *const pMaterial ) const {
	t3dRdEditorOverlayConfig_t &overlay = rSnapshot.m_editorOverlayConfig;
	if ( !pMaterial ) return;
	const Variant positions_value = pMaterial->getShaderParam( StringName( "_editor_decal_position" ) );
	const Variant rotations_value = pMaterial->getShaderParam( StringName( "_editor_decal_rotation" ) );
	const Variant sizes_value = pMaterial->getShaderParam( StringName( "_editor_decal_size" ) );
	const Variant colors_value = pMaterial->getShaderParam( StringName( "_editor_decal_color" ) );
	const Variant visible_value = pMaterial->getShaderParam( StringName( "_editor_decal_visible" ) );
	const Variant parts_value = pMaterial->getShaderParam( StringName( "_editor_decal_part" ) );
	const Array positions = positions_value.get_type() == Variant::ARRAY ? ( Array )( positions_value ) : Array();
	const Array rotations = rotations_value.get_type() == Variant::ARRAY ? ( Array )( rotations_value ) : Array();
	const Array sizes = sizes_value.get_type() == Variant::ARRAY ? ( Array )( sizes_value ) : Array();
	const Array colors = colors_value.get_type() == Variant::ARRAY ? ( Array )( colors_value ) : Array();
	const Array visible = visible_value.get_type() == Variant::ARRAY ? ( Array )( visible_value ) : Array();
	const Array parts = parts_value.get_type() == Variant::ARRAY ? ( Array )( parts_value ) : Array();
	const bool bBrushPart = parts.size() > 0 && ( bool )( parts[0] );
	const bool bReticlePart = parts.size() > 1 && ( bool )( parts[1] );
	for ( int i = 0; i < 3; ++i ) {
		t3dRdEditorDecal_t &decal = overlay.m_szDecals[i];
		if ( i < positions.size() && positions[i].get_type() == Variant::VECTOR2 ) {
			const Vector2 position = positions[i];
			decal.m_fPositionX = ( float )( position.x );
			decal.m_fPositionY = ( float )( position.y );
		}
		if ( i < rotations.size() && ( rotations[i].get_type() == Variant::FLOAT || rotations[i].get_type() == Variant::INT ) )
			decal.m_fRotation = ( float )( rotations[i] );
		if ( i < sizes.size() && ( sizes[i].get_type() == Variant::FLOAT || sizes[i].get_type() == Variant::INT ) )
			decal.m_fSize = MAX( ( float )( sizes[i] ), 0.0f );
		if ( i < colors.size() && colors[i].get_type() == Variant::COLOR ) {
			const Color color = colors[i];
			decal.m_fColorR = ( float )( color.r );
			decal.m_fColorG = ( float )( color.g );
			decal.m_fColorB = ( float )( color.b );
			decal.m_fColorA = ( float )( color.a );
		}
		decal.m_nVisible = i < visible.size() && visible[i].get_type() == Variant::BOOL && ( bool )( visible[i] ) ? 1u : 0u;
		decal.m_nPartFlags = ( bReticlePart ? 2u : 0u ) |
				( i == 0 && bBrushPart && m_editorBrushTexture.getRid().is_valid() ? 1u : 0u );
	}
	overlay.m_fHeightmapBlackHeight = t3dRdMaterialFloatParam( pMaterial, "heightmap_black_height", -100.0f );
	overlay.m_fHeightmapWhiteHeight = t3dRdMaterialFloatParam( pMaterial, "heightmap_white_height", 300.0f );
	overlay.m_fContourInterval = MAX( t3dRdMaterialFloatParam( pMaterial, "contour_interval", 1.0f ), 0.25f );
	overlay.m_fContourThickness = MAX( t3dRdMaterialFloatParam( pMaterial, "contour_thickness", 1.0f ), 0.0f );
	const Color contour = t3dRdMaterialColorParam( pMaterial, "contour_color", Color( 0.85, 0.85, 0.19, 1.0 ) );
	overlay.m_fContourColorR = ( float )( contour.r );
	overlay.m_fContourColorG = ( float )( contour.g );
	overlay.m_fContourColorB = ( float )( contour.b );
	overlay.m_fContourColorA = ( float )( contour.a );
	const Terrain3DInstancer *const pInstancer = m_pTerrain ? m_pTerrain->getInstancer() : nullptr;
	overlay.m_fInstancerCellSize = pInstancer ? ( float )( pInstancer->getCellSize() ) : ( float )( s_nInstanceDefaultCellSize );
}

void Terrain3DRenderCoordinator::buildMaterialState( t3dRdFrameSnapshot_t &rSnapshot ) const {
	const Ref<Terrain3DAssets> assets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	const Ref<Terrain3DMaterial> material = m_pTerrain ? m_pTerrain->getMaterial() : Ref<Terrain3DMaterial>();
	Terrain3DMaterial *const pMaterial = material.ptr();
	t3dRdMaterialConfig_t &config = rSnapshot.m_materialConfig;
	if ( pMaterial ) {
		Terrain3DData *const pData = m_pTerrain ? m_pTerrain->getData() : nullptr;
		config.m_nFeatureFlags = 0u;
		if ( pMaterial->getAutoShaderEnabled() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_AUTO_SHADER;
		if ( pMaterial->getDualScalingEnabled() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DUAL_SCALING;
		if ( pMaterial->getMacroVariationEnabled() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MACRO_VARIATION;
		if ( m_macroNoiseTexture.getRid().is_valid() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MACRO_NOISE;
		if ( pData && pData->areControlMapsImplicit() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_CONTROL_IMPLICIT;
		if ( pData && pData->areColorMapsImplicit() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_COLOR_IMPLICIT;
		if ( pMaterial->getWorldBackground() == NOISE ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_WORLD_NOISE;
		if ( m_worldNoiseCache.getRid().is_valid() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_WORLD_NOISE_CACHE;
		if ( rSnapshot.m_bDisplacementEnabled && m_pTerrain->getComputeDisplacementTextureRid().is_valid() )
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DISPLACEMENT;
		if ( pData && pData->getDerivedMapsRid().is_valid() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DERIVED_MAPS;
		if ( pData && pData->getMiddleCompositeMapsRid().is_valid() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MIDDLE_COMPOSITE;
		if ( pData && pData->getMiddleHeightMapsRid().is_valid() && pData->getMiddleHeightRegionMapRid().is_valid() )
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MIDDLE_HEIGHT;
		if ( pData && pData->getFarHeightMapRid().is_valid() && pData->getFarColorMapRid().is_valid() )
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_FAR_ATLAS;
		if ( assets.is_valid() && assets->getFarAlbedoArrayRid().is_valid() ) config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_FAR_ALBEDO;

		config.m_nOutputFlags = 0u;
		if ( pMaterial->getOutputAlbedoEnabled() ) config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_ALBEDO;
		if ( pMaterial->getOutputRoughnessEnabled() ) config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_ROUGHNESS;
		if ( pMaterial->getOutputSpecularEnabled() ) config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_SPECULAR;
		if ( pMaterial->getOutputNormalMapEnabled() ) config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_NORMAL;
		if ( pMaterial->getOutputAmbientOcclusionEnabled() ) config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_AO;

		config.m_nDebugFlags = 0u;
		if ( pMaterial->getShowRegionGrid() ) config.m_nDebugFlags |= T3D_RD_DEBUG_REGION_GRID;
		if ( pMaterial->getShowInstancerGrid() ) config.m_nDebugFlags |= T3D_RD_DEBUG_INSTANCER_GRID;
		if ( pMaterial->getShowVertexGrid() ) config.m_nDebugFlags |= T3D_RD_DEBUG_VERTEX_GRID;
		if ( pMaterial->getShowContours() ) config.m_nDebugFlags |= T3D_RD_DEBUG_CONTOURS;
		if ( pMaterial->getShowNavigation() ) config.m_nDebugFlags |= T3D_RD_DEBUG_NAVIGATION;
		if ( pMaterial->getShowCheckered() ) config.m_nDebugFlags |= T3D_RD_DEBUG_CHECKERED;
		if ( pMaterial->getShowGrey() ) config.m_nDebugFlags |= T3D_RD_DEBUG_GREY;
		if ( pMaterial->getShowHeightmap() ) config.m_nDebugFlags |= T3D_RD_DEBUG_HEIGHTMAP;
		if ( pMaterial->getShowJaggedness() ) config.m_nDebugFlags |= T3D_RD_DEBUG_JAGGEDNESS;
		if ( pMaterial->getShowAutoshader() ) config.m_nDebugFlags |= T3D_RD_DEBUG_AUTOSHADER;
		if ( pMaterial->getShowControlTexture() ) config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_TEXTURE;
		if ( pMaterial->getShowControlBlend() ) config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_BLEND;
		if ( pMaterial->getShowControlAngle() ) config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_ANGLE;
		if ( pMaterial->getShowControlScale() ) config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_SCALE;
		if ( pMaterial->getShowColormap() ) config.m_nDebugFlags |= T3D_RD_DEBUG_COLORMAP;
		if ( pMaterial->getShowRoughmap() ) config.m_nDebugFlags |= T3D_RD_DEBUG_ROUGHMAP;
		if ( pMaterial->getShowDisplacementBuffer() ) config.m_nDebugFlags |= T3D_RD_DEBUG_DISPLACEMENT;
		if ( pMaterial->getShowTextureAlbedo() ) config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_ALBEDO;
		if ( pMaterial->getShowTextureHeight() ) config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_HEIGHT;
		if ( pMaterial->getShowTextureNormal() ) config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_NORMAL;
		if ( pMaterial->getShowTextureRough() ) config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_ROUGH;
		if ( pMaterial->getShowTextureAo() ) config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_AO;
		config.m_nBackgroundMode = ( uint32_t )( pMaterial->getWorldBackground() );
		config.m_nProjectionMode = ( uint32_t )( pMaterial->getProjectionMode() );
		config.m_nTessellationMaterialMode = ( uint32_t )( pMaterial->getTessellationMaterialMode() );
		config.m_nWorldNoiseMode = ( uint32_t )( pMaterial->getWorldNoiseShaderMode() );
		config.m_fDisplacementSharpness = ( float )( pMaterial->getDisplacementSharpness() );
		config.m_fBlendSharpness = t3dRdMaterialFloatParam( pMaterial, "blend_sharpness", 0.5f );
		config.m_fDistantNormalScale = t3dRdMaterialFloatParam( pMaterial, "distant_normal_scale", 2.0f );
		config.m_fGroundLevel = t3dRdMaterialFloatParam( pMaterial, "ground_level", 0.0f );
		config.m_fRegionBlend = t3dRdMaterialFloatParam( pMaterial, "region_blend", 0.25f );
		config.m_fBiasDistance = t3dRdMaterialFloatParam( pMaterial, "bias_distance", 512.0f );
		config.m_fMipmapBias = t3dRdMaterialFloatParam( pMaterial, "mipmap_bias", 1.0f );
		config.m_fDepthBlur = t3dRdMaterialFloatParam( pMaterial, "depth_blur", 0.0f );
		config.m_fAutoSlope = t3dRdMaterialFloatParam( pMaterial, "auto_slope", 1.0f );
		config.m_fAutoHeightReduction = t3dRdMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f );
		config.m_nAutoBaseTexture = t3dRdMaterialUintParam( pMaterial, "auto_base_texture", 0u );
		config.m_nAutoOverlayTexture = t3dRdMaterialUintParam( pMaterial, "auto_overlay_texture", 1u );
		config.m_nDualScaleTexture = t3dRdMaterialUintParam( pMaterial, "dual_scale_texture", 0u );
		config.m_fDualScaleReduction = t3dRdMaterialFloatParam( pMaterial, "dual_scale_reduction", 0.3f );
		config.m_fTriScaleReduction = t3dRdMaterialFloatParam( pMaterial, "tri_scale_reduction", 0.3f );
		config.m_fDualScaleFar = t3dRdMaterialFloatParam( pMaterial, "dual_scale_far", 170.0f );
		config.m_fDualScaleNear = t3dRdMaterialFloatParam( pMaterial, "dual_scale_near", 100.0f );
		config.m_fNoise1Scale = t3dRdMaterialFloatParam( pMaterial, "noise1_scale", 0.04f );
		config.m_fNoise2Scale = t3dRdMaterialFloatParam( pMaterial, "noise2_scale", 0.076f );
		const float fNoiseAngle = t3dRdMaterialFloatParam( pMaterial, "noise1_angle", 0.0f );
		config.m_fNoise1RotationX = std::cos( fNoiseAngle );
		config.m_fNoise1RotationY = std::sin( fNoiseAngle );
		const Color macro1 = t3dRdMaterialColorParam( pMaterial, "macro_variation1", Color( 1.0, 1.0, 1.0, 1.0 ) );
		const Color macro2 = t3dRdMaterialColorParam( pMaterial, "macro_variation2", Color( 1.0, 1.0, 1.0, 1.0 ) );
		config.m_fMacroVariation1R = ( float )( macro1.r );
		config.m_fMacroVariation1G = ( float )( macro1.g );
		config.m_fMacroVariation1B = ( float )( macro1.b );
		config.m_fMacroVariationSlope = t3dRdMaterialFloatParam( pMaterial, "macro_variation_slope", 0.333f );
		config.m_fMacroVariation2R = ( float )( macro2.r );
		config.m_fMacroVariation2G = ( float )( macro2.g );
		config.m_fMacroVariation2B = ( float )( macro2.b );
		const Vector2 noise_offset = t3dRdMaterialVector2Param( pMaterial, "noise1_offset", Vector2( 0.5, 0.5 ) );
		config.m_fNoise1OffsetX = ( float )( noise_offset.x );
		config.m_fNoise1OffsetY = ( float )( noise_offset.y );
		config.m_fWorldNoiseCachePeriod = t3dRdMaterialFloatParam( pMaterial, "world_noise_cache_period", 64.0f );
		config.m_fWorldNoiseLodDistance = t3dRdMaterialFloatParam( pMaterial, "world_noise_lod_distance", 7500.0f );
		config.m_fWorldNoiseScale = t3dRdMaterialFloatParam( pMaterial, "world_noise_scale", 5.0f );
		config.m_fWorldNoiseHeight = t3dRdMaterialFloatParam( pMaterial, "world_noise_height", 32.0f );
		config.m_nWorldNoiseMaxOctaves = t3dRdMaterialUintParam( pMaterial, "world_noise_max_octaves", 4u );
		config.m_nWorldNoiseMinOctaves = t3dRdMaterialUintParam( pMaterial, "world_noise_min_octaves", 2u );
		config.m_nWorldNoiseCacheEnabled = t3dRdMaterialBoolParam( pMaterial, "world_noise_cache_enabled", true ) ? 1u : 0u;
		config.m_nWorldNoiseFragmentNormals = t3dRdMaterialBoolParam( pMaterial, "world_noise_fragment_normals", false ) ? 1u : 0u;
		const Vector3 world_noise_offset = t3dRdMaterialVector3Param( pMaterial, "world_noise_offset", Vector3() );
		config.m_fWorldNoiseOffsetX = ( float )( world_noise_offset.x );
		config.m_fWorldNoiseOffsetY = ( float )( world_noise_offset.y );
		config.m_fWorldNoiseOffsetZ = ( float )( world_noise_offset.z );
		const Color silhouette = t3dRdMaterialColorParam( pMaterial, "silhouette_color", Color( 0.04, 0.04, 0.04, 1.0 ) );
		config.m_fSilhouetteR = ( float )( silhouette.r );
		config.m_fSilhouetteG = ( float )( silhouette.g );
		config.m_fSilhouetteB = ( float )( silhouette.b );
		config.m_fSilhouetteA = ( float )( silhouette.a );
		config.m_fContourStep = t3dRdMaterialFloatParam( pMaterial, "contour_interval", 1.0f );
		t3dRdFillTierConfig( config.m_szTier[0],
				pMaterial->getNearMaterialSamplingQuality(), pMaterial->getNearTerrainNormalQuality(),
				pMaterial->getNearTerrainColorQuality(), pMaterial->getNearSurfaceResponseQuality(),
				pMaterial->getNearTextureSurfaceMode(), pMaterial->getNearLightingModel(),
				pMaterial->getNearDetilingQuality(), pMaterial->getNearTextureFiltering(),
				( float )( pMaterial->getNearAuthoredColorContribution() ), ( float )( pMaterial->getNearWetnessContribution() ),
				( float )( pMaterial->getNearMaterialLodBias() ), 0.0f );
		t3dRdFillTierConfig( config.m_szTier[1],
				pMaterial->getMiddleMaterialSamplingQuality(), pMaterial->getMiddleTerrainNormalQuality(),
				pMaterial->getMiddleTerrainColorQuality(), pMaterial->getMiddleSurfaceResponseQuality(),
				pMaterial->getMiddleTextureSurfaceMode(), pMaterial->getMiddleLightingModel(),
				pMaterial->getMiddleDetilingQuality(), pMaterial->getMiddleTextureFiltering(),
				( float )( pMaterial->getMiddleAuthoredColorContribution() ), ( float )( pMaterial->getMiddleWetnessContribution() ),
				( float )( pMaterial->getMiddleMaterialLodBias() ), 0.0f );
		t3dRdFillTierConfig( config.m_szTier[2],
				pMaterial->getFarMaterialSamplingQuality(), pMaterial->getFarTerrainNormalQuality(),
				pMaterial->getFarTerrainColorQuality(), pMaterial->getFarSurfaceResponseQuality(),
				pMaterial->getFarTextureSurfaceMode(), pMaterial->getFarLightingModel(),
				pMaterial->getFarDetilingQuality(), pMaterial->getFarTextureFiltering(),
				( float )( pMaterial->getFarAuthoredColorContribution() ), ( float )( pMaterial->getFarWetnessContribution() ),
				( float )( pMaterial->getFarMaterialLodBias() ), MAX( 0.0f, ( float )( pMaterial->getFarMaterialLod() ) - 4.0f ) );
	}

	if ( assets.is_valid() ) {
		const PackedColorArray colors = assets->getTextureColors();
		const PackedFloat32Array normal_depths = assets->getTextureNormalDepths();
		const PackedFloat32Array ao_strengths = assets->getTextureAoStrengths();
		const PackedFloat32Array ao_affects = assets->getTextureAoLightAffects();
		const PackedFloat32Array roughness = assets->getTextureRoughnessMods();
		const PackedFloat32Array metallic = assets->getTextureMetallics();
		const PackedFloat32Array uv_scales = assets->getTextureUvScales();
		const PackedVector2Array detiles = assets->getTextureDetiles();
		const PackedVector2Array displacements = assets->getTextureDisplacements();
		const PackedInt32Array layer_map = assets->getTextureLayerMap();
		const uint32_t count = ( uint32_t )( CLAMP( assets->getTextureCount(), 1, ( int )( s_nRdMaterialLayerCount ) ) );
		rSnapshot.m_nMaterialLayerCount = count;
		for ( uint32_t i = 0u; i < s_nRdMaterialLayerCount; ++i ) {
			t3dRdMaterialLayer_t &layer = rSnapshot.m_szMaterials[i];
			const Color color = i < ( uint32_t )( colors.size() ) ? colors[( int64_t )( i )] : Color( 1.0, 1.0, 1.0, 1.0 );
			layer.m_fColorR = ( float )( color.r );
			layer.m_fColorG = ( float )( color.g );
			layer.m_fColorB = ( float )( color.b );
			layer.m_fColorA = ( float )( color.a );
			layer.m_fUvScale = i < ( uint32_t )( uv_scales.size() ) ? uv_scales[( int64_t )( i )] : 0.1f;
			layer.m_fNormalDepth = i < ( uint32_t )( normal_depths.size() ) ? normal_depths[( int64_t )( i )] : 1.0f;
			layer.m_fRoughness = i < ( uint32_t )( roughness.size() ) ? roughness[( int64_t )( i )] : 0.0f;
			layer.m_fMetallic = i < ( uint32_t )( metallic.size() ) ? metallic[( int64_t )( i )] : 0.0f;
			layer.m_fAoStrength = i < ( uint32_t )( ao_strengths.size() ) ? ao_strengths[( int64_t )( i )] : 1.0f;
			layer.m_fAoLightAffect = i < ( uint32_t )( ao_affects.size() ) ? ao_affects[( int64_t )( i )] : 0.0f;
			const Vector2 detile = i < ( uint32_t )( detiles.size() ) ? detiles[( int64_t )( i )] : Vector2();
			layer.m_fDetileRotation = ( float )( detile.x );
			layer.m_fDetileShift = ( float )( detile.y );
			const Vector2 displacement = i < ( uint32_t )( displacements.size() ) ? displacements[( int64_t )( i )] : Vector2();
			layer.m_fDisplacementOffset = ( float )( displacement.x );
			layer.m_fDisplacementScale = ( float )( displacement.y );
			const int physical = i < ( uint32_t )( layer_map.size() ) ? layer_map[( int64_t )( i )] : ( int )( i );
			layer.m_nPhysicalLayer = ( uint32_t )( MAX( physical, 0 ) );
			layer.m_nFlags = 0u;
			if ( assets->usesPackedPbr() ) layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_PACKED_SURFACE;
			if ( physical >= 0 ) layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_RESIDENT;
		}
	} else {
		rSnapshot.m_nMaterialLayerCount = 1u;
	}
	buildEditorOverlayState( rSnapshot, pMaterial );
	uint64_t material_hash = hashBytes( rSnapshot.m_szMaterials, sizeof( rSnapshot.m_szMaterials ) );
	const uint64_t config_hash = hashBytes( &rSnapshot.m_materialConfig, sizeof( rSnapshot.m_materialConfig ) );
	material_hash ^= config_hash + 0x9E3779B97F4A7C15ULL + ( material_hash << 6u ) + ( material_hash >> 2u );
	rSnapshot.m_nMaterialGeneration = material_hash;
}

void Terrain3DRenderCoordinator::publishSnapshot( void ) {
	if ( !m_pTerrain || !m_pTerrain->getMesher() || !m_pTerrain->getData() ) return;
	t3dRdFrameSnapshotSlot_t publication{};
	t3dRdFrameSnapshot_t &snapshot = publication.m_snapshot;
	snapshot.m_nRegionSize = ( uint32_t )( m_pTerrain->getRegionSize() );
	snapshot.m_nRegionMapSize = ( uint32_t )( m_pTerrain->getData()->getRegionMapSizeInternal() );
	snapshot.m_nMeshSize = ( uint32_t )( m_pTerrain->getMeshSize() );
	snapshot.m_fVertexSpacing = ( float )( m_pTerrain->getVertexSpacing() );
	snapshot.m_targetPosition = m_pTerrain->getClipmapTargetPosition();
	snapshot.m_fSubdiv = ( float )( 1 << m_pTerrain->getTessellationLevel() );
	snapshot.m_fHeightMin = s_fRegionHeightMin;
	snapshot.m_fHeightRange = s_fRegionHeightRange;
	snapshot.m_fDisplacementScale = ( float )( m_pTerrain->getDisplacementScale() );
	snapshot.m_bDisplacementEnabled = m_pTerrain->getTessellationLevel() > 0;
	Camera3D *publication_camera = nullptr;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const viewport = editor ? editor->get_editor_viewport_3d( 0 ) : nullptr;
		publication_camera = viewport ? viewport->get_camera_3d() : nullptr;
	} else if ( m_pTerrain->get_viewport() ) {
		publication_camera = m_pTerrain->get_viewport()->get_camera_3d();
	}
	snapshot.m_nCameraCullMask = publication_camera ? publication_camera->get_cull_mask() : UINT32_MAX;
	snapshot.m_nPatchCount = ( uint32_t )( m_pTerrain->getMesher()->fillRdPatches( snapshot.m_szPatches, s_nMaxClipmapLevels ) );
	const float displacement_padding = snapshot.m_bDisplacementEnabled ? std::abs( snapshot.m_fDisplacementScale ) : 0.0f;
	for ( uint32_t i = 0u; i < snapshot.m_nPatchCount; ++i ) {
		t3dRdPatch_t &patch = snapshot.m_szPatches[i];
		patch.m_fBoundsMinY = snapshot.m_fHeightMin - displacement_padding;
		patch.m_fBoundsMaxY = snapshot.m_fHeightMin + snapshot.m_fHeightRange + displacement_padding;
		patch.m_fBoundsHalfExtent =
				( float )( snapshot.m_nMeshSize * 4u + 12u ) * patch.m_fScale + displacement_padding;
		patch.m_fPreviousOriginX = patch.m_fOriginX;
		patch.m_fPreviousOriginZ = patch.m_fOriginZ;
		patch.m_fPreviousScale = patch.m_fScale;
		for ( uint32_t previous = 0u; previous < m_nPreviousPatchCount; ++previous ) {
			const t3dRdPatch_t &previous_patch = m_previousPatches[previous];
			if ( previous_patch.m_nLod != patch.m_nLod ) continue;
			patch.m_fPreviousOriginX = previous_patch.m_fOriginX;
			patch.m_fPreviousOriginZ = previous_patch.m_fOriginZ;
			patch.m_fPreviousScale = previous_patch.m_fScale;
			break;
		}
	}
	const uint64_t patch_fingerprint = hashBytes( snapshot.m_szPatches, sizeof( snapshot.m_szPatches ) );
	if ( !m_bPatchStateInitialized || patch_fingerprint != m_nLastPatchFingerprint ) {
		m_nPatchGeneration = advanceGeneration( m_nPatchGeneration );
		m_nLastPatchFingerprint = patch_fingerprint;
		m_bPatchStateInitialized = true;
	}
	snapshot.m_nGeneration = m_nPatchGeneration;
	for ( uint32_t i = 0u; i < snapshot.m_nPatchCount; ++i ) m_previousPatches[i] = snapshot.m_szPatches[i];
	m_nPreviousPatchCount = snapshot.m_nPatchCount;
	const Ref<Terrain3DMaterial> rd_material = m_pTerrain->getMaterial();
	if ( !ensureWorldNoiseCache( rd_material.ptr() ) ) {
		LOG( ERROR, "Unable to publish Terrain3D direct RD world-noise cache" );
		return;
	}
	if ( !ensureMacroNoiseTexture( rd_material.ptr() ) ) {
		LOG( ERROR, "Unable to publish Terrain3D direct RD macro-variation noise texture" );
		return;
	}
	if ( !ensureEditorBrushTexture( rd_material.ptr() ) ) {
		LOG( ERROR, "Unable to publish Terrain3D direct RD editor brush texture" );
		return;
	}
	buildMaterialState( snapshot );
	const uint64_t material_fingerprint = snapshot.m_nMaterialGeneration;
	if ( !m_bMaterialStateInitialized || material_fingerprint != m_nLastMaterialFingerprint ) {
		m_nMaterialGeneration = advanceGeneration( m_nMaterialGeneration );
		m_nLastMaterialFingerprint = material_fingerprint;
		m_bMaterialStateInitialized = true;
	}
	snapshot.m_nMaterialGeneration = m_nMaterialGeneration;
	DirectionalLight3D *const light = Object::cast_to<DirectionalLight3D>( m_pTerrain->getLightTarget() );
	if ( light ) {
		snapshot.m_lightColor = light->get_color() * light->get_param( DirectionalLight3D::PARAM_ENERGY );
		snapshot.m_lightDirection = light->get_global_basis().get_column( 2 ).normalized();
	}
	const Ref<World3D> world = m_pTerrain->get_world_3d();
	const Ref<Environment> environment = world.is_valid() ? world->get_environment() : Ref<Environment>();
	if ( environment.is_valid() ) {
		const Color ambient = environment->get_ambient_light_color();
		snapshot.m_fAmbient = ( float )( environment->get_ambient_light_energy() * MAX( ambient.get_luminance(), 0.0f ) );
		if ( environment->is_fog_enabled() ) {
			snapshot.m_fogColor = environment->get_fog_light_color() * environment->get_fog_light_energy();
			snapshot.m_fFogDensity = environment->get_fog_density();
			snapshot.m_fFogStart = environment->get_fog_depth_begin();
			snapshot.m_fFogEnd = environment->get_fog_depth_end();
			snapshot.m_fFogCurve = environment->get_fog_depth_curve();
			snapshot.m_nFogMode = ( uint32_t )( environment->get_fog_mode() );
		}
	}

	Terrain3DData *const data = m_pTerrain->getData();
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
	const uint64_t region_topology_generation = data->getRegionTopologyGenerationInternal();
	PackedByteArray region_bytes;
	if ( !m_bRegionMapInitialized || region_topology_generation != m_nRegionTopologyGeneration ) {
		const PackedInt32Array region_map = data->getRegionMap();
		const PackedInt32Array logical_region_map = data->getLogicalRegionMap();
		if ( region_map.size() != logical_region_map.size() ) {
			LOG( ERROR, "Physical/logical region map sizes diverged" );
			return;
		}
		region_bytes.resize( region_map.size() * ( int64_t )( sizeof( uint32_t ) ) );
		uint8_t *const pPackedRegionMap = region_bytes.ptrw();
		for ( int64_t i = 0; i < region_map.size(); ++i ) {
			const uint32_t nPhysicalLayer = ( uint32_t )( CLAMP( region_map[i], 0, 0xFFFF ) );
			const uint32_t nLogicalPresent = logical_region_map[i] != 0 ? 0x80000000u : 0u;
			const uint32_t nPacked = nPhysicalLayer | nLogicalPresent;
			std::memcpy( pPackedRegionMap + i * ( int64_t )( sizeof( uint32_t ) ), &nPacked, sizeof( nPacked ) );
		}
		m_regionMapBytes = region_bytes;
		m_nRegionTopologyGeneration = region_topology_generation;
		m_nRegionMapGeneration = advanceGeneration( m_nRegionMapGeneration );
		m_bRegionMapInitialized = true;
	}
	publication.m_regionMapBytes = m_regionMapBytes;
	snapshot.m_nRegionMapGeneration = m_nRegionMapGeneration;
	const uint64_t region_cache_sources[2] = {
		( uint64_t )( data->getActiveStorageGeneration() ), m_pTerrain->getRegionStreamingGeneration()
	};
	const uint64_t region_cache_source_generation = hashBytes( region_cache_sources, sizeof( region_cache_sources ) );
	const bool bRegionCacheChanged =
			!m_bRegionCacheInitialized || region_cache_source_generation != m_nLastRegionCacheSourceGeneration;
	if ( bRegionCacheChanged ) {
		m_nRegionCacheGeneration = advanceGeneration( m_nRegionCacheGeneration );
		m_nLastRegionCacheSourceGeneration = region_cache_source_generation;
		m_bRegionCacheInitialized = true;
	}
	snapshot.m_nRegionCacheGeneration = m_nRegionCacheGeneration;

	t3dRdPublishedResources_t &resources = publication.m_resources;
	resources.m_hHeightTextureRd = data->getHeightMapsRid();
	resources.m_hDerivedTextureRd = data->getDerivedMapsRid();
	resources.m_hControlTextureRd = data->getControlMapsRid();
	resources.m_hColorTextureRd = data->getColorMapsRid();
	resources.m_hMiddleCompositeTextureRd = data->getMiddleCompositeMapsRid();
	resources.m_hMiddleHeightTextureRd = data->getMiddleHeightMapsRid();
	resources.m_hMiddleHeightRegionMapTextureRd = data->getMiddleHeightRegionMapRid();
	resources.m_hFarHeightTextureRd = data->getFarHeightMapRid();
	resources.m_hFarColorTextureRd = data->getFarColorMapRid();
	resources.m_hAlbedoTextureRd = assets.is_valid() ? assets->getAlbedoArrayRid() : RID();
	resources.m_hFarAlbedoTextureRd = assets.is_valid() ? assets->getFarAlbedoArrayRid() : RID();
	resources.m_hNormalTextureRd = assets.is_valid() ? assets->getNormalArrayRid() : RID();
	resources.m_hSurfaceTextureRd = assets.is_valid() ? assets->getSurfaceArrayRid() : RID();
	resources.m_hDisplacementTextureRd =
			snapshot.m_bDisplacementEnabled ? m_pTerrain->getComputeDisplacementTextureRid() : RID();
	resources.m_hWorldNoiseTextureRd = m_worldNoiseCache.getRid();
	resources.m_hMacroNoiseTextureRd = m_macroNoiseTexture.getRid();
	resources.m_hEditorBrushTextureRd = m_editorBrushTexture.getRid();
	if ( !resources.m_hDisplacementTextureRd.is_valid() ) snapshot.m_bDisplacementEnabled = false;

	const bool bTerrainTexturesChanged = !m_bPublishedResourcesInitialized || bRegionCacheChanged ||
			resources.m_hHeightTextureRd != m_lastPublishedResources.m_hHeightTextureRd ||
			resources.m_hDerivedTextureRd != m_lastPublishedResources.m_hDerivedTextureRd ||
			resources.m_hControlTextureRd != m_lastPublishedResources.m_hControlTextureRd ||
			resources.m_hColorTextureRd != m_lastPublishedResources.m_hColorTextureRd ||
			resources.m_hMiddleCompositeTextureRd != m_lastPublishedResources.m_hMiddleCompositeTextureRd ||
			resources.m_hMiddleHeightTextureRd != m_lastPublishedResources.m_hMiddleHeightTextureRd ||
			resources.m_hMiddleHeightRegionMapTextureRd != m_lastPublishedResources.m_hMiddleHeightRegionMapTextureRd ||
			resources.m_hFarHeightTextureRd != m_lastPublishedResources.m_hFarHeightTextureRd ||
			resources.m_hFarColorTextureRd != m_lastPublishedResources.m_hFarColorTextureRd;
	if ( bTerrainTexturesChanged ) m_nTerrainTextureGeneration = advanceGeneration( m_nTerrainTextureGeneration );
	const bool bMaterialTexturesChanged = !m_bPublishedResourcesInitialized ||
			resources.m_hAlbedoTextureRd != m_lastPublishedResources.m_hAlbedoTextureRd ||
			resources.m_hFarAlbedoTextureRd != m_lastPublishedResources.m_hFarAlbedoTextureRd ||
			resources.m_hNormalTextureRd != m_lastPublishedResources.m_hNormalTextureRd ||
			resources.m_hSurfaceTextureRd != m_lastPublishedResources.m_hSurfaceTextureRd ||
			resources.m_hDisplacementTextureRd != m_lastPublishedResources.m_hDisplacementTextureRd ||
			resources.m_hWorldNoiseTextureRd != m_lastPublishedResources.m_hWorldNoiseTextureRd ||
			resources.m_hMacroNoiseTextureRd != m_lastPublishedResources.m_hMacroNoiseTextureRd ||
			resources.m_hEditorBrushTextureRd != m_lastPublishedResources.m_hEditorBrushTextureRd;
	if ( bMaterialTexturesChanged ) m_nMaterialTextureGeneration = advanceGeneration( m_nMaterialTextureGeneration );
	m_lastPublishedResources = resources;
	m_bPublishedResourcesInitialized = true;
	snapshot.m_nTerrainTextureGeneration = m_nTerrainTextureGeneration;
	snapshot.m_nMaterialTextureGeneration = m_nMaterialTextureGeneration;

	const Ref<Terrain3DAssets> instance_assets = m_pTerrain->getAssets();
	Terrain3DHLODStreamer *const hlod_streamer = m_pTerrain->getHlodStreamer();
	const uint64_t hlod_generation = hlod_streamer ? hlod_streamer->getRdPublicationGeneration() : 0u;
	const bool hlod_changed = !m_cachedHlodInstanceScene || hlod_generation != m_nHlodSceneGeneration;
	if ( hlod_changed ) {
		m_cachedHlodMeshSources.clear();
		if ( hlod_streamer ) {
			hlod_streamer->fillRdMeshSources( m_cachedHlodMeshSources );
			m_cachedHlodInstanceScene = hlod_streamer->buildRdInstanceSnapshot();
		} else {
			m_cachedHlodInstanceScene = std::make_shared<t3dRdInstanceSceneSnapshot_t>();
		}
		m_nHlodSceneGeneration = hlod_generation;
	}

	const uint64_t mesh_fingerprint = Terrain3DRdMeshCache::computeSourceFingerprint(
			instance_assets, m_cachedHlodMeshSources );
	if ( !m_cachedInstanceMeshes || mesh_fingerprint != m_nInstanceMeshFingerprint ) {
		m_nInstanceMeshGeneration = advanceGeneration( m_nInstanceMeshGeneration );
		String mesh_error;
		m_cachedInstanceMeshes = Terrain3DRdMeshCache::buildSourceSnapshot(
				instance_assets,
				m_cachedHlodMeshSources,
				mesh_fingerprint,
				m_nInstanceMeshGeneration,
				&mesh_error );
		if ( !m_cachedInstanceMeshes ) {
			std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
			m_initializationError = "Failed to publish RD instance/HLOD meshes: " + mesh_error;
			return;
		}
		m_nInstanceMeshFingerprint = mesh_fingerprint;
	}

	Terrain3DInstancer *const instancer = m_pTerrain->getInstancer();
	const uint64_t instance_generation = instancer ? instancer->getRdPublicationGeneration() : 0u;
	const bool regular_changed = !m_cachedRegularInstanceScene || instance_generation != m_nInstanceSceneGeneration;
	if ( regular_changed ) {
		m_cachedRegularInstanceScene = instancer ? instancer->buildRdInstanceSnapshot()
				: std::make_shared<t3dRdInstanceSceneSnapshot_t>();
		m_nInstanceSceneGeneration = instance_generation;
	}
	if ( !m_cachedInstanceScene || regular_changed || hlod_changed ) {
		std::shared_ptr<t3dRdInstanceSceneSnapshot_t> combined = std::make_shared<t3dRdInstanceSceneSnapshot_t>();
		m_nCombinedInstanceSceneGeneration = advanceGeneration( m_nCombinedInstanceSceneGeneration );
		combined->m_nGeneration = m_nCombinedInstanceSceneGeneration;
		const size_t regular_instance_count = m_cachedRegularInstanceScene ? m_cachedRegularInstanceScene->m_instances.size() : 0u;
		const size_t hlod_instance_count = m_cachedHlodInstanceScene ? m_cachedHlodInstanceScene->m_instances.size() : 0u;
		const size_t regular_cell_count = m_cachedRegularInstanceScene ? m_cachedRegularInstanceScene->m_cells.size() : 0u;
		const size_t hlod_cell_count = m_cachedHlodInstanceScene ? m_cachedHlodInstanceScene->m_cells.size() : 0u;
		combined->m_instances.reserve( regular_instance_count + hlod_instance_count );
		combined->m_cells.reserve( regular_cell_count + hlod_cell_count );
		if ( m_cachedRegularInstanceScene ) {
			combined->m_instances.insert( combined->m_instances.end(),
					m_cachedRegularInstanceScene->m_instances.begin(), m_cachedRegularInstanceScene->m_instances.end() );
			combined->m_cells.insert( combined->m_cells.end(),
					m_cachedRegularInstanceScene->m_cells.begin(), m_cachedRegularInstanceScene->m_cells.end() );
		}
		if ( m_cachedHlodInstanceScene ) {
			const uint32_t instance_offset = ( uint32_t )( combined->m_instances.size() );
			combined->m_instances.insert( combined->m_instances.end(),
					m_cachedHlodInstanceScene->m_instances.begin(), m_cachedHlodInstanceScene->m_instances.end() );
			for ( const t3dRdInstanceCell_t &source_cell : m_cachedHlodInstanceScene->m_cells ) {
				t3dRdInstanceCell_t cell = source_cell;
				cell.m_nFirstInstance += instance_offset;
				combined->m_cells.push_back( cell );
			}
		}
		m_cachedInstanceScene = combined;
	}
	publication.m_instanceMeshes = m_cachedInstanceMeshes;
	publication.m_instanceScene = m_cachedInstanceScene;

	uint64_t publication_generation = m_nNextSnapshotGeneration;
	if ( publication_generation == 0u || publication_generation > s_nRdFrameSnapshotGenerationMax ) publication_generation = 1u;
	m_nNextSnapshotGeneration = publication_generation == s_nRdFrameSnapshotGenerationMax ? 1u : publication_generation + 1u;
	snapshot.m_nPublicationGeneration = publication_generation;
	if ( m_nSnapshotWriteIndex >= s_nRdFrameSnapshotSlotCount ) {
		m_bRendererFailed.store( true, std::memory_order_release );
		return;
	}
	m_snapshotSlots[m_nSnapshotWriteIndex] = publication;
	const uint64_t published_token = t3dRdMakeFrameSnapshotToken( m_nSnapshotWriteIndex, publication_generation, true );
	const uint64_t reclaimed_token = m_nSnapshotExchangeToken.exchange( published_token, std::memory_order_acq_rel );
	const uint32_t reclaimed_index = t3dRdFrameSnapshotTokenIndex( reclaimed_token );
	m_nPublishedSnapshotIndex.store( m_nSnapshotWriteIndex, std::memory_order_release );
	m_nPublishedSnapshotGeneration.store( publication_generation, std::memory_order_release );
	m_nSnapshotPublications.fetch_add( 1u, std::memory_order_relaxed );
	if ( reclaimed_index >= s_nRdFrameSnapshotSlotCount ) {
		m_bRendererFailed.store( true, std::memory_order_release );
		return;
	}
	m_nSnapshotWriteIndex = reclaimed_index;
}

const Terrain3DRenderCoordinator::t3dRdFrameSnapshotSlot_t *
Terrain3DRenderCoordinator::acquireSnapshotOnRenderThread( void ) {
	const t3dRdFrameSnapshotSlot_t *const current = m_bSnapshotRenderValid && m_nSnapshotRenderGeneration != 0u &&
			m_nSnapshotRenderIndex < s_nRdFrameSnapshotSlotCount ?
			&m_snapshotSlots[m_nSnapshotRenderIndex] :
			nullptr;
	for ( ;; ) {
		uint64_t token = m_nSnapshotExchangeToken.load( std::memory_order_acquire );
		if ( !t3dRdFrameSnapshotTokenIsPublished( token ) ) {
			if ( current ) m_nSnapshotReuses.fetch_add( 1u, std::memory_order_relaxed );
			return current;
		}
		const uint32_t candidate_index = t3dRdFrameSnapshotTokenIndex( token );
		const uint64_t candidate_generation = t3dRdFrameSnapshotTokenGeneration( token );
		if ( candidate_index >= s_nRdFrameSnapshotSlotCount || candidate_generation == 0u ||
				t3dRdFrameSnapshotGenerationIsStale( candidate_generation, m_nSnapshotRenderGeneration ) ) {
			recordStaleSnapshot( token );
			if ( current ) m_nSnapshotReuses.fetch_add( 1u, std::memory_order_relaxed );
			return current;
		}
		const uint64_t released_token =
				t3dRdMakeFrameSnapshotToken( m_nSnapshotRenderIndex, m_nSnapshotRenderGeneration, false );
		if ( !m_nSnapshotExchangeToken.compare_exchange_weak(
					token, released_token, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
			continue;
		}
		const uint64_t previous_generation = m_nSnapshotRenderGeneration;
		m_nSnapshotRenderIndex = candidate_index;
		m_nSnapshotRenderGeneration = candidate_generation;
		m_nConsumedSnapshotGeneration.store( candidate_generation, std::memory_order_release );
		m_nSnapshotConsumes.fetch_add( 1u, std::memory_order_relaxed );
		const uint64_t generation_distance =
				t3dRdFrameSnapshotGenerationDistance( candidate_generation, previous_generation );
		if ( previous_generation != 0u && generation_distance > 1u )
			m_nSkippedSnapshotGenerations.fetch_add( generation_distance - 1u, std::memory_order_relaxed );
		const t3dRdFrameSnapshotSlot_t &acquired = m_snapshotSlots[candidate_index];
		if ( acquired.m_snapshot.m_nPublicationGeneration != candidate_generation ) {
			m_bSnapshotRenderValid = false;
			recordStaleSnapshot( token );
			return nullptr;
		}
		m_bSnapshotRenderValid = true;
		return &acquired;
	}
}

void Terrain3DRenderCoordinator::recordStaleSnapshot( const uint64_t nToken ) {
	if ( m_nLastStaleSnapshotToken.exchange( nToken, std::memory_order_acq_rel ) != nToken ) {
		m_nStaleSnapshots.fetch_add( 1u, std::memory_order_relaxed );
	}
}

void Terrain3DRenderCoordinator::resetSnapshotPublication( void ) {
	m_nSnapshotExchangeToken.store( t3dRdMakeFrameSnapshotToken( 1u, 0u, false ), std::memory_order_release );
	m_nSnapshotWriteIndex = 2u;
	m_nSnapshotRenderIndex = 0u;
	m_nSnapshotRenderGeneration = 0u;
	m_bSnapshotRenderValid = false;
	m_nPublishedSnapshotIndex.store( s_nRdInvalidFrameSnapshotIndex, std::memory_order_release );
	m_nPublishedSnapshotGeneration.store( 0u, std::memory_order_release );
	m_nConsumedSnapshotGeneration.store( 0u, std::memory_order_release );
	m_nLastStaleSnapshotToken.store( 0u, std::memory_order_release );
	for ( t3dRdFrameSnapshotSlot_t &slot : m_snapshotSlots ) slot = t3dRdFrameSnapshotSlot_t{};
	m_regionMapBytes.clear();
	m_nRegionTopologyGeneration = 0u;
	m_nRegionMapGeneration = 0u;
	m_nLastRegionCacheSourceGeneration = 0u;
	m_nRegionCacheGeneration = 0u;
	m_nLastPatchFingerprint = 0u;
	m_nPatchGeneration = 0u;
	m_previousPatches.fill( t3dRdPatch_t{} );
	m_nPreviousPatchCount = 0u;
	m_nLastMaterialFingerprint = 0u;
	m_nMaterialGeneration = 0u;
	m_nTerrainTextureGeneration = 0u;
	m_nMaterialTextureGeneration = 0u;
	m_lastPublishedResources = t3dRdPublishedResources_t{};
	m_bRegionMapInitialized = false;
	m_bRegionCacheInitialized = false;
	m_bPatchStateInitialized = false;
	m_bMaterialStateInitialized = false;
	m_bPublishedResourcesInitialized = false;
}

void Terrain3DRenderCoordinator::updateNativeProxy( void ) {
	if ( !m_pTerrain || !m_pTerrain->getMesher() ) return;
	const t3dRenderBackend_t backend = m_backend.load( std::memory_order_acquire );
	const t3dRdNativeProxyMode_t proxy_mode = m_proxyMode.load( std::memory_order_acquire );
	const bool bRd = backend != T3D_RENDER_BACKEND_RENDERING_SERVER;
	const bool bFullNative = !bRd || backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID || proxy_mode == T3D_RD_NATIVE_PROXY_FULL;
	const bool bShadowProxy = bRd && !bFullNative && proxy_mode == T3D_RD_NATIVE_PROXY_SHADOWS_ONLY;
	m_pTerrain->getMesher()->setRdPresentation( bFullNative, bShadowProxy );
}

void Terrain3DRenderCoordinator::attachEffectToActiveCamera( void ) {
	if ( !m_pTerrain || m_effect.is_null() ||
		m_backend.load( std::memory_order_acquire ) == T3D_RENDER_BACKEND_RENDERING_SERVER ) return;
	Camera3D *camera = nullptr;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const viewport = editor ? editor->get_editor_viewport_3d( 0 ) : nullptr;
		camera = viewport ? viewport->get_camera_3d() : nullptr;
	} else if ( m_pTerrain->get_viewport() ) {
		camera = m_pTerrain->get_viewport()->get_camera_3d();
	}
	if ( !camera ) return;
	if ( m_nAttachedCameraId == camera->get_instance_id() && m_attachedCompositor.is_valid() && camera->get_compositor() == m_attachedCompositor ) return;
	detachEffect();
	Ref<Compositor> compositor = camera->get_compositor();
	m_bCreatedCompositor = compositor.is_null();
	if ( compositor.is_null() ) {
		compositor.instantiate();
		camera->set_compositor( compositor );
	}
	TypedArray<Ref<CompositorEffect>> effects = compositor->get_compositor_effects();
	bool bFound = false;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		if ( effects[i] == m_effect ) { bFound = true; break; }
	}
	if ( !bFound ) {
		effects.push_back( m_effect );
		compositor->set_compositor_effects( effects );
	}
	m_attachedCompositor = compositor;
	m_nAttachedCameraId = camera->get_instance_id();
}

void Terrain3DRenderCoordinator::detachEffect( void ) {
	if ( m_effect.is_null() || m_attachedCompositor.is_null() ) {
		m_attachedCompositor.unref();
		m_nAttachedCameraId = 0u;
		m_bCreatedCompositor = false;
		return;
	}
	TypedArray<Ref<CompositorEffect>> effects = m_attachedCompositor->get_compositor_effects();
	TypedArray<Ref<CompositorEffect>> remaining;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		Ref<CompositorEffect> effect = effects[i];
		if ( effect != m_effect ) remaining.push_back( effect );
	}
	m_attachedCompositor->set_compositor_effects( remaining );
	if ( m_bCreatedCompositor && remaining.is_empty() && m_nAttachedCameraId != 0u ) {
		Object *const object = ObjectDB::get_instance( m_nAttachedCameraId );
		Camera3D *const camera = Object::cast_to<Camera3D>( object );
		if ( camera && camera->get_compositor() == m_attachedCompositor ) camera->set_compositor( Ref<Compositor>() );
	}
	m_attachedCompositor.unref();
	m_nAttachedCameraId = 0u;
	m_bCreatedCompositor = false;
}

bool Terrain3DRenderCoordinator::initializeRendererOnRenderThread( void ) {
	if ( m_bRendererInitialized.load( std::memory_order_acquire ) ) return true;
	const bool ready = m_shaderLibrary.isReady() && m_renderer.initialize( m_shaderLibrary.getNear(),
			m_shaderLibrary.getMiddle(),
			m_shaderLibrary.getFar(),
			m_shaderLibrary.getCull() ) && m_instanceRenderer.initialize( m_shaderLibrary.getInstances(), m_shaderLibrary.getHlodCull() );
	m_bRendererInitialized.store( ready, std::memory_order_release );
	m_bRendererFailed.store( false, std::memory_order_release );
	if ( !ready ) {
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
		if ( failures >= 8u ) m_bRendererFailed.store( true, std::memory_order_release );
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.isReady() ? ( !m_instanceRenderer.getLastError().is_empty() ? m_instanceRenderer.getLastError() : m_renderer.getLastError() ) : m_shaderLibrary.getLastError();
	}
	return ready;
}

void Terrain3DRenderCoordinator::initialize( Terrain3D *const pTerrain ) {
	shutdown();
	m_pTerrain = pTerrain;
	if ( !m_pTerrain ) return;
	OS *const os = OS::get_singleton();
	const bool bCompareCulling = os && os->get_environment( "TERRAIN3D_RD_COMPARE_CULL" ) == "1";
	const bool bDirectDebug = os && os->get_environment( "TERRAIN3D_RD_DIRECT_DRAW" ) == "1";
	m_renderer.setCullingComparisonEnabled( bCompareCulling );
	m_renderer.setDirectDrawDebugEnabled( bDirectDebug );
	const bool bInstanceDirectDebug = os && os->get_environment( "TERRAIN3D_RD_INSTANCE_DIRECT_DRAW" ) == "1";
	m_instanceRenderer.setDirectDrawDebugEnabled( bDirectDebug || bInstanceDirectDebug );
	m_backend.store( bCompareCulling || bDirectDebug ? T3D_RENDER_BACKEND_RENDERING_DEVICE_DIRECT :
			m_pTerrain->getRenderBackend(),
			std::memory_order_release );
	m_proxyMode.store( m_pTerrain->getRdNativeProxyMode(), std::memory_order_release );
	m_bRendererFailed.store( false, std::memory_order_release );
	m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
	m_bFallbackActive = false;
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = String();
	}
	m_effect.instantiate();
	m_effect->setCoordinator( this );
	if ( !m_shaderLibrary.load() ) {
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
	}
	if ( !m_pTerrain->getMesher() || !m_renderer.prepareTopologies( m_pTerrain->getMesher()->getClipmapGeometry( LOD0_MESH ),
			m_pTerrain->getMesher()->getClipmapGeometry( OUTER_RING_MESH ) ) ) {
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		m_initializationError = "Failed to prepare packed RenderingDevice clipmap topology";
	}
	publishSnapshot();
	updateNativeProxy();
	m_bRenderEnabled.store( m_backend.load( std::memory_order_acquire ) != T3D_RENDER_BACKEND_RENDERING_SERVER,
			std::memory_order_release );
	attachEffectToActiveCamera();
}

void Terrain3DRenderCoordinator::shutdown( void ) {
	m_bRenderEnabled.store( false, std::memory_order_release );
	if ( m_effect.is_valid() ) m_effect->set_enabled( false );
	RenderingServer *const server = RenderingServer::get_singleton();
	if ( m_bRendererInitialized.load( std::memory_order_acquire ) ) {
		if ( server && server->is_on_render_thread() ) shutdownRendererOnRenderThread();
		else if ( server && m_effect.is_valid() ) {
			server->call_on_render_thread( callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::shutdownRendererOnRenderThread ) );
			server->force_sync();
		} else {
			m_instanceRenderer.shutdown();
			m_renderer.shutdown();
			m_bRendererInitialized.store( false, std::memory_order_release );
		}
	} else if ( server && m_effect.is_valid() && !server->is_on_render_thread() ) {
		server->force_sync();
	}
	if ( m_effect.is_valid() ) m_effect->setCoordinator( nullptr );
	detachEffect();
	m_effect.unref();
	if ( m_pTerrain && m_pTerrain->getMesher() ) m_pTerrain->getMesher()->setRdPresentation( true, false );
	m_worldNoiseCache.clear();
	m_macroNoiseTexture.clear();
	m_editorBrushTexture.clear();
	m_nMacroNoiseSourceId = 0u;
	m_nEditorBrushSourceId = 0u;
	m_shaderLibrary.clear();
	m_cachedInstanceMeshes.reset();
	m_cachedRegularInstanceScene.reset();
	m_cachedHlodInstanceScene.reset();
	m_cachedInstanceScene.reset();
	m_cachedHlodMeshSources.clear();
	m_nInstanceMeshFingerprint = 0u;
	m_nInstanceMeshGeneration = 0u;
	m_nInstanceSceneGeneration = 0u;
	m_nHlodSceneGeneration = 0u;
	m_nCombinedInstanceSceneGeneration = 0u;
	resetSnapshotPublication();
	m_pTerrain = nullptr;
}

void Terrain3DRenderCoordinator::update( void ) {
	if ( !m_pTerrain ) return;
	publishSnapshot();
	if ( m_bRendererFailed.load( std::memory_order_acquire ) && !m_bFallbackActive ) {
		m_bFallbackActive = true;
		m_bRenderEnabled.store( false, std::memory_order_release );
		if ( m_effect.is_valid() ) m_effect->set_enabled( false );
		// A failed RD renderer is intentionally visible. Never resurrect the
		// legacy native terrain surface as a hidden failure fallback.
		if ( m_pTerrain->getMesher() ) m_pTerrain->getMesher()->setRdPresentation( false, false );
	}
	if ( m_backend.load( std::memory_order_acquire ) != T3D_RENDER_BACKEND_RENDERING_SERVER && !m_bFallbackActive )
		attachEffectToActiveCamera();
}

void Terrain3DRenderCoordinator::setBackend( const t3dRenderBackend_t backend ) {
	if ( m_backend.load( std::memory_order_acquire ) == backend ) return;
	m_backend.store( backend, std::memory_order_release );
	if ( backend != T3D_RENDER_BACKEND_RENDERING_SERVER ) {
		m_bRendererFailed.store( false, std::memory_order_release );
		m_bFallbackActive = false;
	}
	m_bRenderEnabled.store( backend != T3D_RENDER_BACKEND_RENDERING_SERVER, std::memory_order_release );
	if ( m_effect.is_valid() ) m_effect->set_enabled( backend != T3D_RENDER_BACKEND_RENDERING_SERVER );
	updateNativeProxy();
	if ( backend == T3D_RENDER_BACKEND_RENDERING_SERVER ) detachEffect();
	else attachEffectToActiveCamera();
}

void Terrain3DRenderCoordinator::setProxyMode( const t3dRdNativeProxyMode_t mode ) {
	if ( m_proxyMode.load( std::memory_order_acquire ) == mode ) return;
	m_proxyMode.store( mode, std::memory_order_release );
	updateNativeProxy();
}

void Terrain3DRenderCoordinator::shutdownRendererOnRenderThread( void ) {
	m_instanceRenderer.shutdown();
	m_renderer.shutdown();
	m_bRendererInitialized.store( false, std::memory_order_release );
}

void Terrain3DRenderCoordinator::render( RenderData *const pRenderData ) {
	if ( !m_bRenderEnabled.load( std::memory_order_acquire ) || !pRenderData ) return;
	++m_nCallbackCount;
	if ( !initializeRendererOnRenderThread() ) return;
	const t3dRdFrameSnapshotSlot_t *const publication = acquireSnapshotOnRenderThread();
	if ( !publication ) return;
	const t3dRdFrameSnapshot_t &snapshot = publication->m_snapshot;
	const bool bHasTerrain = snapshot.m_nPatchCount > 0u && !publication->m_regionMapBytes.is_empty();
	const t3dRenderBackend_t backend = m_backend.load( std::memory_order_acquire );
	const t3dRdNativeProxyMode_t proxy_mode = m_proxyMode.load( std::memory_order_acquire );
	const bool bNativeSurface = backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID || proxy_mode == T3D_RD_NATIVE_PROXY_FULL;
	const bool bColorWrite = !bNativeSurface;
	const bool instances_ok = m_instanceRenderer.render( pRenderData,
			publication->m_instanceMeshes,
			publication->m_instanceScene,
			snapshot.m_nCameraCullMask );
	const bool terrain_ok = !bHasTerrain || ( instances_ok && m_renderer.render( pRenderData,
			 snapshot,
			 publication->m_regionMapBytes,
			 snapshot.m_nRegionMapGeneration,
			 publication->m_resources.m_hHeightTextureRd,
			 publication->m_resources.m_hDerivedTextureRd,
			 publication->m_resources.m_hControlTextureRd,
			 publication->m_resources.m_hColorTextureRd,
			 publication->m_resources.m_hMiddleCompositeTextureRd,
			 publication->m_resources.m_hMiddleHeightTextureRd,
			 publication->m_resources.m_hMiddleHeightRegionMapTextureRd,
			 publication->m_resources.m_hFarHeightTextureRd,
			 publication->m_resources.m_hFarColorTextureRd,
			 publication->m_resources.m_hAlbedoTextureRd,
			 publication->m_resources.m_hFarAlbedoTextureRd,
			 publication->m_resources.m_hNormalTextureRd,
			 publication->m_resources.m_hSurfaceTextureRd,
			 publication->m_resources.m_hDisplacementTextureRd,
			 publication->m_resources.m_hWorldNoiseTextureRd,
			 publication->m_resources.m_hMacroNoiseTextureRd,
			 publication->m_resources.m_hEditorBrushTextureRd,
			 bColorWrite ) );
	const bool render_ok = instances_ok && terrain_ok;
	if ( render_ok ) {
		m_nConsecutiveRenderFailures.store( 0u, std::memory_order_release );
	} else {
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
		// Resource/render failures are fatal to the current ownership generation.
		// Tear it down on this render thread; a later callback reconstructs from
		// immutable CPU publication without reviving the native renderer.
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		m_bRendererInitialized.store( false, std::memory_order_release );
		if ( failures >= 8u ) m_bRendererFailed.store( true, std::memory_order_release );
	}
}

Dictionary Terrain3DRenderCoordinator::getDiagnostics( void ) const {
	Dictionary result;
	const Dictionary renderer = m_renderer.getDiagnostics();
	const Dictionary instances = m_instanceRenderer.getDiagnostics();
	const t3dRenderBackend_t backend = m_backend.load( std::memory_order_acquire );
	const t3dRdNativeProxyMode_t proxy_mode = m_proxyMode.load( std::memory_order_acquire );
	result["backend"] = ( int )( backend );
	result["proxy_mode"] = ( int )( proxy_mode );
	result["render_enabled"] = m_bRenderEnabled.load( std::memory_order_acquire );
	result["renderer_initialized"] = m_bRendererInitialized.load( std::memory_order_acquire );
	result["fallback_active"] = m_bFallbackActive;
	result["callbacks"] = ( int64_t )( m_nCallbackCount.load( std::memory_order_acquire ) );
	result["consecutive_render_failures"] = ( int64_t )( m_nConsecutiveRenderFailures.load( std::memory_order_acquire ) );
	const uint64_t snapshot_token = m_nSnapshotExchangeToken.load( std::memory_order_acquire );
	const uint32_t published_snapshot_index = m_nPublishedSnapshotIndex.load( std::memory_order_acquire );
	const uint64_t published_snapshot_generation = m_nPublishedSnapshotGeneration.load( std::memory_order_acquire );
	const uint64_t consumed_snapshot_generation = m_nConsumedSnapshotGeneration.load( std::memory_order_acquire );
	result["snapshot_slots"] = ( int64_t )( s_nRdFrameSnapshotSlotCount );
	result["snapshot_exchange_lock_free"] = m_nSnapshotExchangeToken.is_lock_free();
	result["snapshot_exchange_index"] = ( int64_t )( t3dRdFrameSnapshotTokenIndex( snapshot_token ) );
	result["snapshot_exchange_published"] = t3dRdFrameSnapshotTokenIsPublished( snapshot_token );
	result["snapshot_exchange_generation"] = ( int64_t )( t3dRdFrameSnapshotTokenGeneration( snapshot_token ) );
	result["snapshot_published_index"] = ( int64_t )( published_snapshot_index );
	result["snapshot_published_generation"] = ( int64_t )( published_snapshot_generation );
	result["snapshot_consumed_generation"] = ( int64_t )( consumed_snapshot_generation );
	result["snapshot_generation_lag"] = ( int64_t )( consumed_snapshot_generation == 0u ?
			( published_snapshot_generation == 0u ? 0u : 1u ) :
			t3dRdFrameSnapshotGenerationDistance( published_snapshot_generation, consumed_snapshot_generation ) );
	result["snapshot_publications"] = ( int64_t )( m_nSnapshotPublications.load( std::memory_order_acquire ) );
	result["snapshot_consumes"] = ( int64_t )( m_nSnapshotConsumes.load( std::memory_order_acquire ) );
	result["snapshot_reuses"] = ( int64_t )( m_nSnapshotReuses.load( std::memory_order_acquire ) );
	result["snapshot_skipped_generations"] =
			( int64_t )( m_nSkippedSnapshotGenerations.load( std::memory_order_acquire ) );
	result["stale_snapshots"] = ( int64_t )( m_nStaleSnapshots.load( std::memory_order_acquire ) );
	result["renderer"] = renderer;
	result["instances"] = instances;
	result["shaders"] = m_shaderLibrary.getDiagnostics();
	result["frames_rendered"] = renderer.get( "frames_rendered", 0 );
	result["draws_rendered"] = ( int64_t )( renderer.get( "draws_rendered", 0 ) ) + ( int64_t )( instances.get( "instance_draws", 0 ) );
	result["static_instances"] = instances.get( "static_instances", 0 );
	result["visible_instances"] = instances.get( "visible_instances", 0 );
	result["instance_draw_packets"] = instances.get( "instance_draw_packets", 0 );
	result["instance_depth_packets"] = instances.get( "depth_packets", 0 );
	result["instance_shadow_packets"] = instances.get( "shadow_packets", 0 );
	result["indirect_draws"] = renderer.get( "indirect_draws", 0 );
	result["cull_dispatches"] = renderer.get( "cull_dispatches", 0 );
	result["patch_uploads"] = renderer.get( "patch_uploads", 0 );
	result["topology_bytes"] = renderer.get( "topology_bytes", 0 );
	result["lod0_index_bits"] = renderer.get( "lod0_index_bits", 0 );
	result["outer_index_bits"] = renderer.get( "outer_index_bits", 0 );
	result["gpu_culling"] = renderer.get( "gpu_culling", false );
	result["resource_creates"] = renderer.get( "resource_creates", 0 );
	result["resource_frees"] = renderer.get( "resource_frees", 0 );
	result["live_buffer_bytes"] = renderer.get( "live_buffer_bytes", 0 );
	result["peak_buffer_bytes"] = renderer.get( "peak_buffer_bytes", 0 );
	result["live_texture_bytes"] = renderer.get( "live_texture_bytes", 0 );
	result["peak_texture_bytes"] = renderer.get( "peak_texture_bytes", 0 );
	result["buffer_updates"] = renderer.get( "buffer_updates", 0 );
	result["bytes_uploaded"] = renderer.get( "bytes_uploaded", 0 );
	result["last_render_error"] = renderer.get( "last_error", "" );
	{
		std::lock_guard<std::mutex> lock( m_diagnosticsMutex );
		result["initialization_error"] = m_initializationError;
	}
	result["native_depth_prepass_participation"] = false;
	result["native_shadow_proxy_available"] = true;
	result["multiview_supported"] = true;
	result["single_view_only"] = false;
	const bool bNativeSurface = backend == T3D_RENDER_BACKEND_RENDERING_SERVER ||
			backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID || proxy_mode == T3D_RD_NATIVE_PROXY_FULL;
	result["native_surface_enabled"] = bNativeSurface;
	result["color_write_enabled"] = backend != T3D_RENDER_BACKEND_RENDERING_SERVER && !bNativeSurface;
	result["hybrid_depth_only"] = backend == T3D_RENDER_BACKEND_RENDERING_DEVICE_HYBRID;
	return result;
}
