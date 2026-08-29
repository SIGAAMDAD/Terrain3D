// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"

#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "instances/terrain_3d_instancer.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "memory/terrain_3d_memory.h"
#include "rendering/terrain_3d_material.h"
#include "rendering/rd/terrain_3d_rd_forward_plus_bridge.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <math.h>
#include <string.h>


static float t3dRdMaterialFloatParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const float fDefault ) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT ? ( float )( value ) : fDefault;
}

static uint32_t t3dRdMaterialUintParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const uint32_t nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::INT ? ( uint32_t )( MAX( ( int64_t )( value ), int64_t( 0 ) ) ) : nDefault;
}

static bool t3dRdMaterialBoolParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const bool bDefault ) {
	if ( !pMaterial ) {
		return bDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::BOOL ? ( bool )( value ) : bDefault;
}

static Color t3dRdMaterialColorParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Color &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	if ( value.get_type() == Variant::COLOR ) {
		return ( Color )( value );
	}
	if ( value.get_type() == Variant::VECTOR3 ) {
		const Vector3 vector = value;
		return Color( vector.x, vector.y, vector.z, 1.0f );
	}
	return rDefault;
}

static Vector2 t3dRdMaterialVector2Param( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Vector2 &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR2 ? ( Vector2 )( value ) : rDefault;
}

static Vector3 t3dRdMaterialVector3Param( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Vector3 &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR3 ? ( Vector3 )( value ) : rDefault;
}


static Variant t3dRdOceanParam( const ShaderMaterial *const pMaterial, const char *const pszName ) {
	return pMaterial ? pMaterial->get_shader_parameter( StringName( pszName ) ) : Variant();
}
static float t3dRdOceanFloatParam( const ShaderMaterial *const pMaterial, const char *const pszName, const float fDefault ) {
	const Variant value = t3dRdOceanParam( pMaterial, pszName );
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT ? ( float )( value ) : fDefault;
}
static Color t3dRdOceanColorParam( const ShaderMaterial *const pMaterial, const char *const pszName, const Color &rDefault ) {
	const Variant value = t3dRdOceanParam( pMaterial, pszName );
	if ( value.get_type() == Variant::COLOR ) {
		return ( Color )( value );
	}
	if ( value.get_type() == Variant::VECTOR3 ) {
		const Vector3 v = value;
		return Color( v.x, v.y, v.z, 1.0f );
	}
	return rDefault;
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

Terrain3DRenderCoordinator::~Terrain3DRenderCoordinator( void ) {
	shutdown();
}

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
	if ( !pMaterial || pMaterial->getWorldBackground() != NOISE ) {
		return true;
	}
	const t3dWorldNoiseShaderMode_t mode = pMaterial->getWorldNoiseShaderMode();
	const bool bCacheEnabled = t3dRdMaterialBoolParam( pMaterial, "world_noise_cache_enabled", true );
	const bool bNeedsCache = mode == WORLD_NOISE_CACHED_VERTEX_NORMALS || mode == WORLD_NOISE_CACHED_FRAGMENT_NORMALS
	        || ( mode == WORLD_NOISE_DYNAMIC && bCacheEnabled );
	if ( !bNeedsCache || m_worldNoiseCache.getRid().is_valid() ) {
		return true;
	}
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
	if ( !m_worldNoiseCache.create( image, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid() ) {
		LOG( ERROR, "Failed to create direct RD world-noise cache texture" );
		return false;
	}
	return true;
}

bool Terrain3DRenderCoordinator::ensureMacroNoiseTexture( const Terrain3DMaterial *const pMaterial ) {
	if ( !pMaterial || !pMaterial->getMacroVariationEnabled() ) {
		return true;
	}
	const Variant source_value = pMaterial->getShaderParam( StringName( "noise_texture" ) );
	Ref<Image> image;
	uint64_t source_id = 0u;
	if ( source_value.get_type() == Variant::OBJECT ) {
		const Ref<Texture2D> source_texture = source_value;
		if ( source_texture.is_valid() ) {
			source_id = ( uint64_t )( source_texture->get_instance_id() );
			if ( source_id == m_nMacroNoiseSourceId && m_macroNoiseTexture.getRid().is_valid() ) {
				return true;
			}
			const Ref<Image> source_image = source_texture->get_image();
			if ( source_image.is_valid() && !source_image->is_empty() ) {
				image = source_image->duplicate();
			}
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
		if ( image->get_format() != Image::FORMAT_L8 ) {
			image->convert( Image::FORMAT_L8 );
		}
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
	if ( image.is_null() || image->is_empty() ) {
		return false;
	}
	if ( !image->has_mipmaps() ) {
		image->generate_mipmaps();
	}
	if ( !m_macroNoiseTexture.create( image, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid() ) {
		LOG( ERROR, "Failed to create direct RD macro-variation noise texture" );
		return false;
	}
	m_nMacroNoiseSourceId = source_id;
	return true;
}

bool Terrain3DRenderCoordinator::ensureEditorBrushTexture( const Terrain3DMaterial *const pMaterial ) {
	if ( !pMaterial ) {
		return true;
	}
	const Variant source_value = pMaterial->getShaderParam( StringName( "_editor_brush_texture" ) );
	if ( source_value.get_type() != Variant::OBJECT ) {
		// Keep the allocation alive while snapshots and their uniform sets can still
		// reference it. Overlay state disables sampling when there is no source.
		m_nEditorBrushSourceId = 0u;
		return true;
	}
	const Ref<Texture2D> source_texture = source_value;
	if ( source_texture.is_null() ) {
		// The editor clears this parameter between tools. Releasing the texture here
		// invalidates immutable frame slots which may still be in flight.
		m_nEditorBrushSourceId = 0u;
		return true;
	}
	const uint64_t source_id = ( uint64_t )( source_texture->get_instance_id() );
	if ( source_id == m_nEditorBrushSourceId && m_editorBrushTexture.getRid().is_valid() ) {
		return true;
	}
	Ref<Image> image = source_texture->get_image();
	if ( image.is_null() || image->is_empty() ) {
		// Imported/streaming Texture2D resources can temporarily have no readable
		// CPU image. Keep the last good brush and retry on the next publication;
		// this optional editor overlay must never suppress the terrain snapshot.
		return true;
	}
	image = image->duplicate();
	if ( image->is_compressed() && image->decompress() != OK ) {
		return true;
	}
	if ( image->get_format() != Image::FORMAT_L8 ) {
		image->convert( Image::FORMAT_L8 );
	}
	if ( image->has_mipmaps() ) {
		image->clear_mipmaps();
	}
	if ( image->get_width() != s_nEditorBrushTextureSize || image->get_height() != s_nEditorBrushTextureSize ) {
		image->resize( s_nEditorBrushTextureSize, s_nEditorBrushTextureSize, Image::INTERPOLATE_BILINEAR );
	}
	// A fixed layout permits in-place uploads. The RID therefore remains stable
	// across brush changes and cannot invalidate descriptor sets borrowed by any
	// of the three atomic frame slots.
	const bool bUploaded = m_editorBrushTexture.getRid().is_valid()
	        ? m_editorBrushTexture.update2d( image )
	        : m_editorBrushTexture.create( image, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid();
	if ( !bUploaded ) {
		LOG( ERROR, "Unable to create Terrain3D direct RD editor brush texture" );
		return false;
	}
	m_nEditorBrushSourceId = source_id;
	return true;
}

void Terrain3DRenderCoordinator::buildEditorOverlayState(
        t3dRdFrameSnapshot_t &rSnapshot, const Terrain3DMaterial *const pMaterial ) const {
	t3dRdEditorOverlayConfig_t &overlay = rSnapshot.m_editorOverlayConfig;
	if ( !pMaterial ) {
		return;
	}
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
		if ( i < rotations.size() && ( rotations[i].get_type() == Variant::FLOAT || rotations[i].get_type() == Variant::INT ) ) {
			decal.m_fRotation = ( float )( rotations[i] );
		}
		if ( i < sizes.size() && ( sizes[i].get_type() == Variant::FLOAT || sizes[i].get_type() == Variant::INT ) ) {
			decal.m_fSize = MAX( ( float )( sizes[i] ), 0.0f );
		}
		if ( i < colors.size() && colors[i].get_type() == Variant::COLOR ) {
			const Color color = colors[i];
			decal.m_fColorR = ( float )( color.r );
			decal.m_fColorG = ( float )( color.g );
			decal.m_fColorB = ( float )( color.b );
			decal.m_fColorA = ( float )( color.a );
		}
		decal.m_nVisible = i < visible.size() && visible[i].get_type() == Variant::BOOL && ( bool )( visible[i] ) ? 1u : 0u;
		decal.m_nPartFlags = ( bReticlePart ? 2u : 0u ) | ( i == 0 && bBrushPart && m_editorBrushTexture.getRid().is_valid() ? 1u : 0u );
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
	const Terrain3DInstancer *const pInstancer = m_pTerrain ? m_pTerrain->getInstancer() : NULL;
	overlay.m_fInstancerCellSize = pInstancer ? ( float )( pInstancer->getCellSize() ) : ( float )( s_nInstanceDefaultCellSize );
}

void Terrain3DRenderCoordinator::buildMaterialState( t3dRdFrameSnapshot_t &rSnapshot ) const {
	const Ref<Terrain3DAssets> assets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	const Ref<Terrain3DMaterial> material = m_pTerrain ? m_pTerrain->getMaterial() : Ref<Terrain3DMaterial>();
	Terrain3DMaterial *const pMaterial = material.ptr();
	t3dRdMaterialConfig_t &config = rSnapshot.m_materialConfig;
	if ( pMaterial ) {
		Terrain3DData *const pData = m_pTerrain ? m_pTerrain->getData() : NULL;
		config.m_nFeatureFlags = 0u;
		if ( pMaterial->getAutoShaderEnabled() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_AUTO_SHADER;
		}
		if ( pMaterial->getDualScalingEnabled() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DUAL_SCALING;
		}
		if ( pMaterial->getMacroVariationEnabled() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MACRO_VARIATION;
		}
		if ( m_macroNoiseTexture.getRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MACRO_NOISE;
		}
		if ( pData && pData->areControlMapsImplicit() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_CONTROL_IMPLICIT;
		}
		if ( pData && pData->areColorMapsImplicit() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_COLOR_IMPLICIT;
		}
		if ( pMaterial->getWorldBackground() == NOISE ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_WORLD_NOISE;
		}
		if ( m_worldNoiseCache.getRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_WORLD_NOISE_CACHE;
		}
		if ( rSnapshot.m_bDisplacementEnabled && m_pTerrain->getComputeDisplacementTextureRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DISPLACEMENT;
		}
		if ( pData && pData->getDerivedMapsRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DERIVED_MAPS;
		}
		if ( pData && pData->getMiddleCompositeMapsRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MIDDLE_COMPOSITE;
		}
		if ( pData && pData->getMiddleHeightMapsRid().is_valid() && pData->getMiddleHeightRegionMapRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MIDDLE_HEIGHT;
		}
		if ( pData && pData->getFarHeightMapRid().is_valid() && pData->getFarColorMapRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_FAR_ATLAS;
		}
		if ( assets.is_valid() && assets->getFarAlbedoArrayRid().is_valid() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_FAR_ALBEDO;
		}

		config.m_nOutputFlags = 0u;
		if ( pMaterial->getOutputAlbedoEnabled() ) {
			config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_ALBEDO;
		}
		if ( pMaterial->getOutputRoughnessEnabled() ) {
			config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_ROUGHNESS;
		}
		if ( pMaterial->getOutputSpecularEnabled() ) {
			config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_SPECULAR;
		}
		if ( pMaterial->getOutputNormalMapEnabled() ) {
			config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_NORMAL;
		}
		if ( pMaterial->getOutputAmbientOcclusionEnabled() ) {
			config.m_nOutputFlags |= T3D_RD_MATERIAL_OUTPUT_AO;
		}

		config.m_nDebugFlags = 0u;
		if ( pMaterial->getShowRegionGrid() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_REGION_GRID;
		}
		if ( pMaterial->getShowInstancerGrid() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_INSTANCER_GRID;
		}
		if ( pMaterial->getShowVertexGrid() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_VERTEX_GRID;
		}
		if ( pMaterial->getShowContours() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_CONTOURS;
		}
		if ( pMaterial->getShowNavigation() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_NAVIGATION;
		}
		if ( pMaterial->getShowCheckered() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_CHECKERED;
		}
		if ( pMaterial->getShowGrey() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_GREY;
		}
		if ( pMaterial->getShowHeightmap() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_HEIGHTMAP;
		}
		if ( pMaterial->getShowJaggedness() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_JAGGEDNESS;
		}
		if ( pMaterial->getShowAutoshader() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_AUTOSHADER;
		}
		if ( pMaterial->getShowControlTexture() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_TEXTURE;
		}
		if ( pMaterial->getShowControlBlend() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_BLEND;
		}
		if ( pMaterial->getShowControlAngle() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_ANGLE;
		}
		if ( pMaterial->getShowControlScale() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_CONTROL_SCALE;
		}
		if ( pMaterial->getShowColormap() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_COLORMAP;
		}
		if ( pMaterial->getShowRoughmap() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_ROUGHMAP;
		}
		if ( pMaterial->getShowDisplacementBuffer() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_DISPLACEMENT;
		}
		if ( pMaterial->getShowTextureAlbedo() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_ALBEDO;
		}
		if ( pMaterial->getShowTextureHeight() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_HEIGHT;
		}
		if ( pMaterial->getShowTextureNormal() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_NORMAL;
		}
		if ( pMaterial->getShowTextureRough() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_ROUGH;
		}
		if ( pMaterial->getShowTextureAo() ) {
			config.m_nDebugFlags |= T3D_RD_DEBUG_TEX_AO;
		}
		config.m_nBackgroundMode = ( uint32_t )( pMaterial->getWorldBackground() );
		config.m_nProjectionMode = ( uint32_t )( pMaterial->getProjectionMode() );
		config.m_nTessellationMaterialMode = ( uint32_t )( pMaterial->getTessellationMaterialMode() );
		config.m_nWorldNoiseMode = ( uint32_t )( pMaterial->getWorldNoiseShaderMode() );
		const uint32_t albedo_layers = assets.is_valid() ? ( uint32_t )CLAMP( assets->getGeneratedArraySize(), 0, 255 ) : 0u;
		const uint32_t normal_layers = assets.is_valid() ? ( uint32_t )CLAMP( assets->getNormalArraySize(), 0, 255 ) : 0u;
		const uint32_t surface_layers = assets.is_valid() ? ( uint32_t )CLAMP( assets->getSurfaceArraySize(), 0, 255 ) : 0u;
		const uint32_t displacement_layers = assets.is_valid() ? ( uint32_t )CLAMP( assets->getDisplacementArraySize(), 0, 255 ) : 0u;
		config.m_nTextureLayerCounts = albedo_layers | ( normal_layers << 8u ) | ( surface_layers << 16u ) | ( displacement_layers << 24u );
		config.m_nFarAlbedoLayerCount = assets.is_valid() ? ( uint32_t )CLAMP( assets->getFarAlbedoArraySize(), 0, 255 ) : 0u;
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
		config.m_fNoise1RotationX = cos( fNoiseAngle );
		config.m_fNoise1RotationY = sin( fNoiseAngle );
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
		config.m_fWorldNoiseCacheMaxLod =
		        m_worldNoiseCache.getRid().is_valid() ? ( float )MAX( m_worldNoiseCache.maxMipmapLevel(), 0 ) : 0.0f;
		const Color silhouette = t3dRdMaterialColorParam( pMaterial, "silhouette_color", Color( 0.04, 0.04, 0.04, 1.0 ) );
		config.m_fSilhouetteR = ( float )( silhouette.r );
		config.m_fSilhouetteG = ( float )( silhouette.g );
		config.m_fSilhouetteB = ( float )( silhouette.b );
		config.m_fSilhouetteA = ( float )( silhouette.a );
		config.m_fContourStep = t3dRdMaterialFloatParam( pMaterial, "contour_interval", 1.0f );
		t3dRdFillTierConfig( config.m_szTier[0],
		        pMaterial->getNearMaterialSamplingQuality(),
		        pMaterial->getNearTerrainNormalQuality(),
		        pMaterial->getNearTerrainColorQuality(),
		        pMaterial->getNearSurfaceResponseQuality(),
		        pMaterial->getNearTextureSurfaceMode(),
		        pMaterial->getNearLightingModel(),
		        pMaterial->getNearDetilingQuality(),
		        pMaterial->getNearTextureFiltering(),
		        ( float )( pMaterial->getNearAuthoredColorContribution() ),
		        ( float )( pMaterial->getNearWetnessContribution() ),
		        ( float )( pMaterial->getNearMaterialLodBias() ),
		        0.0f );
		t3dRdFillTierConfig( config.m_szTier[1],
		        pMaterial->getMiddleMaterialSamplingQuality(),
		        pMaterial->getMiddleTerrainNormalQuality(),
		        pMaterial->getMiddleTerrainColorQuality(),
		        pMaterial->getMiddleSurfaceResponseQuality(),
		        pMaterial->getMiddleTextureSurfaceMode(),
		        pMaterial->getMiddleLightingModel(),
		        pMaterial->getMiddleDetilingQuality(),
		        pMaterial->getMiddleTextureFiltering(),
		        ( float )( pMaterial->getMiddleAuthoredColorContribution() ),
		        ( float )( pMaterial->getMiddleWetnessContribution() ),
		        ( float )( pMaterial->getMiddleMaterialLodBias() ),
		        0.0f );
		t3dRdFillTierConfig( config.m_szTier[2],
		        pMaterial->getFarMaterialSamplingQuality(),
		        pMaterial->getFarTerrainNormalQuality(),
		        pMaterial->getFarTerrainColorQuality(),
		        pMaterial->getFarSurfaceResponseQuality(),
		        pMaterial->getFarTextureSurfaceMode(),
		        pMaterial->getFarLightingModel(),
		        pMaterial->getFarDetilingQuality(),
		        pMaterial->getFarTextureFiltering(),
		        ( float )( pMaterial->getFarAuthoredColorContribution() ),
		        ( float )( pMaterial->getFarWetnessContribution() ),
		        ( float )( pMaterial->getFarMaterialLodBias() ),
		        MAX( 0.0f, ( float )( pMaterial->getFarMaterialLod() ) - 4.0f ) );
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
			if ( assets->usesPackedPbr() ) {
				layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_PACKED_SURFACE;
			}
			if ( physical >= 0 ) {
				layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_RESIDENT;
			}
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

uint32_t Terrain3DRenderCoordinator::selectWritablePublicationBank( const uint32_t nReaderBank, const uint32_t nPendingBank ) const {
	for ( uint32_t bank = 0u; bank < s_nRdFrameSnapshotSlotCount; ++bank ) {
		if ( bank != nReaderBank && bank != nPendingBank ) {
			return bank;
		}
	}
	return s_nRdInvalidFrameSnapshotIndex;
}

bool Terrain3DRenderCoordinator::ensureInstancePublicationStorage( void ) {
	Terrain3DInstancer *const instancer = m_pTerrain ? m_pTerrain->getInstancer() : NULL;
	Terrain3DHLODStreamer *const hlod_streamer = m_pTerrain ? m_pTerrain->getHlodStreamer() : NULL;
	const size_t regular_instances = instancer ? instancer->getRdSnapshotInstanceCapacity() : 0u;
	const size_t regular_cells = instancer ? instancer->getRdSnapshotCellCapacity() : 0u;
	const size_t hlod_entries = hlod_streamer ? hlod_streamer->getRdPublicationCapacity() : 0u;
	const size_t combined_instances = regular_instances + hlod_entries;
	const size_t combined_cells = regular_cells + hlod_entries;

	const bool needs_rebuild = m_cachedRegularInstanceScene.m_instances.capacity() < regular_instances
	        || m_cachedRegularInstanceScene.m_cells.capacity() < regular_cells
	        || m_cachedHlodInstanceScene.m_instances.capacity() < hlod_entries
	        || m_cachedHlodInstanceScene.m_cells.capacity() < hlod_entries
	        || m_cachedInstanceScene.m_instances.capacity() < combined_instances
	        || m_cachedInstanceScene.m_cells.capacity() < combined_cells || m_cachedHlodMeshSources.capacity() < hlod_entries;
	bool bank_needs_rebuild = false;
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		const t3dRdInstanceSceneBank_t &bank = ( m_instanceSceneBanks )[iBank];
		bank_needs_rebuild = bank_needs_rebuild || bank.m_snapshot.m_instances.capacity() < combined_instances
		        || bank.m_snapshot.m_cells.capacity() < combined_cells;
	}
	if ( !needs_rebuild && !bank_needs_rebuild ) {
		return true;
	}

	Terrain3DColdAllocationScope allocation_scope;
	if ( m_cachedRegularInstanceScene.m_instances.capacity() < regular_instances ) {
		if ( !m_cachedRegularInstanceScene.m_instances.reserveCold( regular_instances ) ) {
			return false;
		}
	}
	if ( m_cachedRegularInstanceScene.m_cells.capacity() < regular_cells ) {
		if ( !m_cachedRegularInstanceScene.m_cells.reserveCold( regular_cells ) ) {
			return false;
		}
	}
	if ( m_cachedHlodInstanceScene.m_instances.capacity() < hlod_entries ) {
		if ( !m_cachedHlodInstanceScene.m_instances.reserveCold( hlod_entries ) ) {
			return false;
		}
	}
	if ( m_cachedHlodInstanceScene.m_cells.capacity() < hlod_entries ) {
		if ( !m_cachedHlodInstanceScene.m_cells.reserveCold( hlod_entries ) ) {
			return false;
		}
	}
	if ( m_cachedInstanceScene.m_instances.capacity() < combined_instances ) {
		if ( !m_cachedInstanceScene.m_instances.reserveCold( combined_instances ) ) {
			return false;
		}
	}
	if ( m_cachedInstanceScene.m_cells.capacity() < combined_cells ) {
		if ( !m_cachedInstanceScene.m_cells.reserveCold( combined_cells ) ) {
			return false;
		}
	}
	if ( m_cachedHlodMeshSources.capacity() < hlod_entries ) {
		if ( !t3dContainerReserveCold( m_cachedHlodMeshSources, hlod_entries ) ) {
			return false;
		}
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		t3dRdInstanceSceneBank_t &bank = ( m_instanceSceneBanks )[iBank];
		if ( bank.m_snapshot.m_instances.capacity() < combined_instances ) {
			if ( !bank.m_snapshot.m_instances.reserveCold( combined_instances ) ) {
				return false;
			}
		}
		if ( bank.m_snapshot.m_cells.capacity() < combined_cells ) {
			if ( !bank.m_snapshot.m_cells.reserveCold( combined_cells ) ) {
				return false;
			}
		}
	}
	return true;
}

bool Terrain3DRenderCoordinator::ensureInstanceMeshBankStorage( void ) {
	Terrain3DColdAllocationScope allocation_scope;
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceMeshBanks ); iBank++ ) {
		t3dRdInstanceMeshBank_t &bank = ( m_instanceMeshBanks )[iBank];
		t3dRdMeshSourceSnapshot_t &snapshot = bank.m_snapshot;
		if ( snapshot.m_vertexChunkHashes.capacity() < m_cachedInstanceMeshes.m_vertexChunkHashes.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_vertexChunkHashes, m_cachedInstanceMeshes.m_vertexChunkHashes.size() ) ) {
				return false;
			}
		}
		if ( snapshot.m_index16ChunkHashes.capacity() < m_cachedInstanceMeshes.m_index16ChunkHashes.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_index16ChunkHashes, m_cachedInstanceMeshes.m_index16ChunkHashes.size() ) ) {
				return false;
			}
		}
		if ( snapshot.m_index32ChunkHashes.capacity() < m_cachedInstanceMeshes.m_index32ChunkHashes.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_index32ChunkHashes, m_cachedInstanceMeshes.m_index32ChunkHashes.size() ) ) {
				return false;
			}
		}
		if ( snapshot.m_surfaces.capacity() < m_cachedInstanceMeshes.m_surfaces.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_surfaces, m_cachedInstanceMeshes.m_surfaces.size() ) ) {
				return false;
			}
		}
		if ( snapshot.m_lods.capacity() < m_cachedInstanceMeshes.m_lods.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_lods, m_cachedInstanceMeshes.m_lods.size() ) ) {
				return false;
			}
		}
		if ( snapshot.m_materials.capacity() < m_cachedInstanceMeshes.m_materials.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_materials, m_cachedInstanceMeshes.m_materials.size() ) ) {
				return false;
			}
		}
		if ( snapshot.m_textures.capacity() < m_cachedInstanceMeshes.m_textures.size() ) {
			if ( !t3dContainerReserveCold( snapshot.m_textures, m_cachedInstanceMeshes.m_textures.size() ) ) {
				return false;
			}
		}
	}
	return true;
}

void Terrain3DRenderCoordinator::serviceColdCapacity( void ) {
	if ( !ensureInstancePublicationStorage() || !ensureInstanceMeshBankStorage() ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Failed to establish fixed RD publication capacity";
	}
}

void Terrain3DRenderCoordinator::publishSnapshot( void ) {
	if ( !m_pTerrain || !m_pTerrain->getMesher() || !m_pTerrain->getData() ) {
		return;
	}
	if ( m_nSnapshotWriteIndex >= s_nRdFrameSnapshotSlotCount ) {
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	if ( !ensureInstancePublicationStorage() ) {
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	// The exchange still owns frame slots exactly as before. Large immutable
	// payloads are addressed through fixed generation banks instead of cloned
	// into every reclaimed frame slot.
	t3dRdFrameSnapshotSlot_t &publication = m_snapshotSlots[m_nSnapshotWriteIndex];
	publication = t3dRdFrameSnapshotSlot_t{};
	t3dRdFrameSnapshot_t &snapshot = publication.m_snapshot;
	const uint64_t exchange_token = m_nSnapshotExchangeToken.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint32_t pending_slot = t3dRdFrameSnapshotTokenIsPublished( exchange_token ) ? t3dRdFrameSnapshotTokenIndex( exchange_token )
	                                                                                   : s_nRdInvalidFrameSnapshotIndex;
	const t3dRdFrameSnapshotSlot_t *const pPending = pending_slot < s_nRdFrameSnapshotSlotCount ? &m_snapshotSlots[pending_slot] : NULL;
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
	Camera3D *publication_camera = NULL;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const viewport = editor ? editor->get_editor_viewport_3d( 0 ) : NULL;
		publication_camera = viewport ? viewport->get_camera_3d() : NULL;
	} else if ( m_pTerrain->get_viewport() ) {
		publication_camera = m_pTerrain->get_viewport()->get_camera_3d();
	}
	snapshot.m_nCameraCullMask = publication_camera ? publication_camera->get_cull_mask() : UINT32_MAX;
	snapshot.m_nRenderLayers = m_pTerrain->getRenderLayers();
	snapshot.m_nShadowLodCount = m_pTerrain->getShadowLodCount();
	snapshot.m_nNearShadowLodCount = m_pTerrain->getNearShadowLodCount();
	snapshot.m_nMiddleShadowLodCount = m_pTerrain->getMiddleShadowLodCount();
	snapshot.m_nFarShadowLodCount = m_pTerrain->getFarShadowLodCount();
	snapshot.m_bCastShadows = m_pTerrain->getCastShadows() != GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
	snapshot.m_bNearShadowEnabled = m_pTerrain->getNearShadowEnabled();
	snapshot.m_bMiddleShadowEnabled = m_pTerrain->getMiddleShadowEnabled();
	snapshot.m_bFarShadowEnabled = m_pTerrain->getFarShadowEnabled();
	snapshot.m_nPatchCount = ( uint32_t )( m_pTerrain->getMesher()->fillRdPatches( snapshot.m_szPatches, s_nMaxClipmapLevels ) );
	const float displacement_padding = snapshot.m_bDisplacementEnabled ? abs( snapshot.m_fDisplacementScale ) : 0.0f;
	for ( uint32_t i = 0u; i < snapshot.m_nPatchCount; ++i ) {
		t3dRdPatch_t &patch = snapshot.m_szPatches[i];
		patch.m_fBoundsMinY = snapshot.m_fHeightMin - displacement_padding;
		patch.m_fBoundsMaxY = snapshot.m_fHeightMin + snapshot.m_fHeightRange + displacement_padding;
		patch.m_fBoundsHalfExtent = ( float )( snapshot.m_nMeshSize * 4u + 12u ) * patch.m_fScale + displacement_padding;
		patch.m_fPreviousOriginX = patch.m_fOriginX;
		patch.m_fPreviousOriginZ = patch.m_fOriginZ;
		patch.m_fPreviousScale = patch.m_fScale;
		for ( uint32_t previous = 0u; previous < m_nPreviousPatchCount; ++previous ) {
			const t3dRdPatch_t &previous_patch = m_previousPatches[previous];
			if ( previous_patch.m_nLod != patch.m_nLod ) {
				continue;
			}
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
	for ( uint32_t i = 0u; i < snapshot.m_nPatchCount; ++i ) {
		m_previousPatches[i] = snapshot.m_szPatches[i];
	}
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
		if ( environment->is_fog_enabled() ) {
			snapshot.m_fogColor = environment->get_fog_light_color() * environment->get_fog_light_energy();
			snapshot.m_fFogDensity = environment->get_fog_density();
			snapshot.m_fFogStart = environment->get_fog_depth_begin();
			snapshot.m_fFogEnd = environment->get_fog_depth_end();
			snapshot.m_fFogCurve = environment->get_fog_depth_curve();
			snapshot.m_nFogMode = ( uint32_t )( environment->get_fog_mode() );
		}
	}

	t3dRdOceanState_t &ocean = snapshot.m_ocean;
	ocean.m_nEnabled = m_pTerrain->isOceanEnabled() && m_pTerrain->getOceanMesher() ? 1u : 0u;
	if ( ocean.m_nEnabled != 0u ) {
		ocean.m_nPatchCount = ( uint32_t )( m_pTerrain->getOceanMesher()->fillRdPatches( ocean.m_szPatches, s_nMaxClipmapLevels ) );
		ocean.m_nRenderLayers = m_pTerrain->getOceanRenderLayers();
		ocean.m_nCastShadows = m_pTerrain->getOceanCastShadows() != GeometryInstance3D::SHADOW_CASTING_SETTING_OFF ? 1u : 0u;
		ocean.m_nVertexIterations = ( uint32_t )( MAX( m_pTerrain->getOceanVertexWaveIterations(), 0 ) );
		ocean.m_nFragmentIterations = ( uint32_t )( MAX( m_pTerrain->getOceanFragmentWaveIterations(), 0 ) );
		ocean.m_nRefractionMode = ( uint32_t )( m_pTerrain->getOceanRefractionMode() );
		ocean.m_nFoamMode = ( uint32_t )( m_pTerrain->getOceanFoamMode() );
		ocean.m_nScatteringMode = ( uint32_t )( m_pTerrain->getOceanScatteringMode() );
		ocean.m_nShadingMode = ( uint32_t )( m_pTerrain->getOceanShadingMode() );
		ocean.m_fTimeSeconds = Time::get_singleton() ? ( float )( Time::get_singleton()->get_ticks_msec() * 0.001 ) : 0.0f;
		ocean.m_fMeshSize = ( float )( m_pTerrain->getOceanMeshSize() );
		ocean.m_fVertexSpacing = ( float )( m_pTerrain->getOceanVertexSpacing() );
		ocean.m_fSubdiv = ( float )( 1 << m_pTerrain->getOceanTessellationLevel() );
		const Vector3 target = m_pTerrain->getClipmapTargetPosition();
		ocean.m_fTargetX = ( float )( target.x );
		ocean.m_fTargetY = ( float )( target.y );
		ocean.m_fTargetZ = ( float )( target.z );
		ocean.m_fCullMargin = ( float )( m_pTerrain->getOceanCullMargin() );
		const Ref<Material> ocean_material = m_pTerrain->getOceanMaterial();
		const ShaderMaterial *const shader_material = Object::cast_to<ShaderMaterial>( ocean_material.ptr() );
		ocean.m_fSeaLevel = t3dRdOceanFloatParam( shader_material, "sea_level", 5.0f );
		const Color water = t3dRdOceanColorParam( shader_material, "water_color", Color( 0.1058, 0.1568, 0.2117, 1.0 ) );
		ocean.m_szWaterColor[0] = ( float )( water.r );
		ocean.m_szWaterColor[1] = ( float )( water.g );
		ocean.m_szWaterColor[2] = ( float )( water.b );
		ocean.m_szWaterColor[3] = ( float )( water.a );
		ocean.m_fVisibleDepth = t3dRdOceanFloatParam( shader_material, "visible_depth", 256.0f );
		ocean.m_fDepthCurve = t3dRdOceanFloatParam( shader_material, "depth_curve", 0.1f );
		ocean.m_fShoreBlend = t3dRdOceanFloatParam( shader_material, "shore_blend", 1.0f );
		ocean.m_fRefractionStrength = t3dRdOceanFloatParam( shader_material, "refraction_strength", 0.02f );
		ocean.m_fTimeScale = t3dRdOceanFloatParam( shader_material, "time_scale", 8.0f );
		ocean.m_fHeightScale = t3dRdOceanFloatParam( shader_material, "height_scale", 3.0f );
		ocean.m_fOceanScale = t3dRdOceanFloatParam( shader_material, "ocean_scale", 2.0f );
		ocean.m_fFresnelScale = t3dRdOceanFloatParam( shader_material, "fresnel_scale", 0.5f );
		ocean.m_fFresnelPower = t3dRdOceanFloatParam( shader_material, "fresnel_power", 1.0f );
		ocean.m_fSpecular = t3dRdOceanFloatParam( shader_material, "specular", 1.0f );
		ocean.m_fFoamHeight = t3dRdOceanFloatParam( shader_material, "foam_height", 1.35f );
		ocean.m_fFoamTopIntensity = t3dRdOceanFloatParam( shader_material, "foam_top_intensity", 2.0f );
		ocean.m_fFoamDistance = t3dRdOceanFloatParam( shader_material, "foam_distance", 3072.0f );
		ocean.m_fFoamShoreIntensity = t3dRdOceanFloatParam( shader_material, "foam_shore_intensity", 1.3f );
		ocean.m_fFoamNoiseScale = t3dRdOceanFloatParam( shader_material, "foam_noise_scale", 1.5f );
		ocean.m_fHeightScattering = t3dRdOceanFloatParam( shader_material, "height_scattering", 6.0f );
		ocean.m_fHeightThreshold = t3dRdOceanFloatParam( shader_material, "height_threshold", 0.1f );
		ocean.m_fHeightAngleThreshold = t3dRdOceanFloatParam( shader_material, "height_angle_threshold", 0.21f );
		ocean.m_fFresnelScattering = t3dRdOceanFloatParam( shader_material, "fersnel_scattering", 20.0f );
		ocean.m_fDirectScattering = t3dRdOceanFloatParam( shader_material, "direct_scattering", 10.0f );
		ocean.m_nGeneration = 0u;
		ocean.m_nGeneration = hashBytes( &ocean, sizeof( ocean ) );
	}
	if ( m_bCachedOceanTopologyValid ) {
		if ( m_nCurrentOceanTopologyBank >= s_nRdFrameSnapshotSlotCount
		        || m_oceanTopologyBanks[m_nCurrentOceanTopologyBank].m_nGeneration != m_cachedOceanTopology.m_nGeneration ) {
			const uint32_t pending_bank = pPending ? pPending->m_nOceanTopologyBank : s_nRdInvalidFrameSnapshotIndex;
			const uint32_t bank =
			        selectWritablePublicationBank( m_nOceanTopologyReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ), pending_bank );
			if ( bank >= s_nRdFrameSnapshotSlotCount ) {
				LOG( ERROR, "No writable RD ocean topology publication bank" );
				return;
			}
			m_oceanTopologyBanks[bank].m_snapshot = m_cachedOceanTopology;
			m_oceanTopologyBanks[bank].m_nGeneration = m_cachedOceanTopology.m_nGeneration;
			m_nCurrentOceanTopologyBank = bank;
		}
		publication.m_nOceanTopologyBank = m_nCurrentOceanTopologyBank;
	}

	Terrain3DData *const data = m_pTerrain->getData();
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
	const uint64_t region_topology_generation = data->getRegionTopologyGenerationInternal();
	const uint64_t physical_region_map_generation = data->getPhysicalRegionMapGenerationInternal();
	const int region_map_size = data->getRegionMapSizeInternal();
	const int64_t region_map_count = ( int64_t )( region_map_size )*region_map_size;
	const uint16_t *const pPhysicalRegionMap = data->getPhysicalRegionMapDataInternal();
	const uint16_t *const pLogicalRegionMap = data->getLogicalRegionMapDataInternal();
	if ( region_map_size <= 0 || !pPhysicalRegionMap || !pLogicalRegionMap ) {
		LOG( ERROR, "Physical/logical region map storage unavailable" );
		return;
	}
	const int region_map_half = region_map_size / 2;
	uint64_t region_lighting_signature = 1469598103934665603ull;
	for ( int64_t i = 0; i < region_map_count; ++i ) {
		const int source_index = Terrain3DData::getRegionMapIndex(
		        Vector2i( ( int )( i % region_map_size ) - region_map_half, ( int )( i / region_map_size ) - region_map_half ) );
		if ( source_index < 0 || pLogicalRegionMap[source_index] == 0u ) {
			continue;
		}
		const Vector2i location( ( int )( i % region_map_size ) - region_map_half, ( int )( i / region_map_size ) - region_map_half );
		Terrain3DRegion *const pRegion = data->getRegionPtr( location );
		uint64_t value = ( uint64_t )( ( uint32_t )( location.x ) ) | ( ( uint64_t )( ( uint32_t )( location.y ) ) << 32u );
		if ( pRegion && !pRegion->isDeleted() ) {
			const Ref<Terrain3DRegionLighting> lighting = pRegion->getLighting();
			if ( lighting.is_valid() ) {
				value ^= ( uint64_t )( CLAMP( lighting->getShadowmaskMode(), 0, 2 ) + 1 ) << 8u;
				value ^= ( uint64_t )( lighting->hasTerrainShadowMask() ? 1u : 0u ) << 16u;
				value ^= ( uint64_t )( lighting->getBakeSignature() );
			}
		}
		region_lighting_signature ^= value;
		region_lighting_signature *= 1099511628211ull;
	}
	const bool bRegionLightingChanged = !m_bRegionMapInitialized || region_lighting_signature != m_nRegionLightingSignature;
	if ( bRegionLightingChanged && !data->refreshTerrainShadowMasksInternal() ) {
		LOG( ERROR, "Unable to refresh Terrain3D regional shadowmask residency after lighting change" );
		return;
	}
	const bool region_map_changed = !m_bRegionMapInitialized || physical_region_map_generation != m_nPhysicalRegionMapGeneration
	        || region_topology_generation != m_nRegionTopologyGeneration || bRegionLightingChanged;
	if ( region_map_changed ) {
		const uint32_t pending_bank = pPending ? pPending->m_nRegionMapBank : s_nRdInvalidFrameSnapshotIndex;
		const uint32_t bank = selectWritablePublicationBank( m_nRegionMapReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ), pending_bank );
		if ( bank >= s_nRdFrameSnapshotSlotCount ) {
			LOG( ERROR, "No writable RD region-map publication bank" );
			return;
		}
		const int64_t required_bytes = region_map_count * ( int64_t )( sizeof( uint32_t ) );
		if ( required_bytes > m_regionMapBanks[bank].m_bytes.size() ) {
			LOG( ERROR, "RD region-map publication exceeded fixed bank capacity" );
			return;
		}
		uint8_t *const pPackedRegionMap = m_regionMapBanks[bank].m_bytes.ptrw();
		for ( int64_t i = 0; i < region_map_count; ++i ) {
			const Vector2i map_location(
			        ( int )( i % region_map_size ) - region_map_half, ( int )( i / region_map_size ) - region_map_half );
			const int source_index = Terrain3DData::getRegionMapIndex( map_location );
			const uint32_t nPhysicalLayer = source_index >= 0 ? pPhysicalRegionMap[source_index] : 0u;
			const uint32_t nLogicalPresent = source_index >= 0 && pLogicalRegionMap[source_index] != 0u ? 0x80000000u : 0u;
			uint32_t nShadowmaskMode = 0u;
			if ( nLogicalPresent != 0u ) {
				const Vector2i location(
				        ( int )( i % region_map_size ) - region_map_half, ( int )( i / region_map_size ) - region_map_half );
				Terrain3DRegion *const pRegion = data->getRegionPtr( location );
				if ( pRegion && !pRegion->isDeleted() ) {
					const Ref<Terrain3DRegionLighting> lighting = pRegion->getLighting();
					if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
						nShadowmaskMode = ( uint32_t )( CLAMP( lighting->getShadowmaskMode(), 0, 2 ) );
					}
				}
			}
			const uint32_t nPacked = nPhysicalLayer | ( nShadowmaskMode << 16u ) | nLogicalPresent;
			memcpy( pPackedRegionMap + i * ( int64_t )( sizeof( uint32_t ) ), &nPacked, sizeof( nPacked ) );
		}
		m_nRegionTopologyGeneration = region_topology_generation;
		m_nPhysicalRegionMapGeneration = physical_region_map_generation;
		m_nRegionLightingSignature = region_lighting_signature;
		m_nRegionMapGeneration = advanceGeneration( m_nRegionMapGeneration );
		m_regionMapBanks[bank].m_nGeneration = m_nRegionMapGeneration;
		m_nCurrentRegionMapBank = bank;
		m_bRegionMapInitialized = true;
	}
	publication.m_nRegionMapBank = m_nCurrentRegionMapBank;
	snapshot.m_nRegionMapGeneration = m_nRegionMapGeneration;
	const uint64_t region_cache_sources[2] = { ( uint64_t )( data->getActiveStorageGeneration() ),
		m_pTerrain->getRegionStreamingGeneration() };
	const uint64_t region_cache_source_generation = hashBytes( region_cache_sources, sizeof( region_cache_sources ) );
	const bool bRegionCacheChanged = !m_bRegionCacheInitialized || region_cache_source_generation != m_nLastRegionCacheSourceGeneration;
	if ( bRegionCacheChanged ) {
		m_nRegionCacheGeneration = advanceGeneration( m_nRegionCacheGeneration );
		m_nLastRegionCacheSourceGeneration = region_cache_source_generation;
		m_bRegionCacheInitialized = true;
	}
	snapshot.m_nRegionCacheGeneration = m_nRegionCacheGeneration;

	t3dRdTerrainBindings_t &resources = publication.m_bindings;
	resources.m_hHeightTextureRd = data->getHeightMapsRid();
	resources.m_hDerivedTextureRd = data->getDerivedMapsRid();
	resources.m_hControlTextureRd = data->getControlMapsRid();
	resources.m_hColorTextureRd = data->getColorMapsRid();
	resources.m_hMiddleCompositeTextureRd = data->getMiddleCompositeMapsRid();
	resources.m_hTerrainShadowMaskTextureRd = data->getTerrainShadowMasksRid();
	resources.m_hMiddleHeightTextureRd = data->getMiddleHeightMapsRid();
	resources.m_hMiddleHeightRegionMapTextureRd = data->getMiddleHeightRegionMapRid();
	resources.m_hFarHeightTextureRd = data->getFarHeightMapRid();
	resources.m_hFarColorTextureRd = data->getFarColorMapRid();
	resources.m_hAlbedoTextureRd = assets.is_valid() ? assets->getAlbedoArrayRid() : RID();
	resources.m_hFarAlbedoTextureRd = assets.is_valid() ? assets->getFarAlbedoArrayRid() : RID();
	resources.m_hNormalTextureRd = assets.is_valid() ? assets->getNormalArrayRid() : RID();
	resources.m_hSurfaceTextureRd = assets.is_valid() ? assets->getSurfaceArrayRid() : RID();
	resources.m_hMaterialDisplacementTextureRd = assets.is_valid() ? assets->getDisplacementArrayRid() : RID();
	resources.m_hDisplacementTextureRd = snapshot.m_bDisplacementEnabled ? m_pTerrain->getComputeDisplacementTextureRid() : RID();
	resources.m_hWorldNoiseTextureRd = m_worldNoiseCache.getRid();
	resources.m_hMacroNoiseTextureRd = m_macroNoiseTexture.getRid();
	resources.m_hEditorBrushTextureRd = m_editorBrushTexture.getRid();
	if ( !resources.m_hDisplacementTextureRd.is_valid() ) {
		snapshot.m_bDisplacementEnabled = false;
	}

	const bool bTerrainTexturesChanged = !m_bPublishedResourcesInitialized || bRegionCacheChanged
	        || resources.m_hHeightTextureRd != m_lastPublishedResources.m_hHeightTextureRd
	        || resources.m_hDerivedTextureRd != m_lastPublishedResources.m_hDerivedTextureRd
	        || resources.m_hControlTextureRd != m_lastPublishedResources.m_hControlTextureRd
	        || resources.m_hColorTextureRd != m_lastPublishedResources.m_hColorTextureRd
	        || resources.m_hMiddleCompositeTextureRd != m_lastPublishedResources.m_hMiddleCompositeTextureRd
	        || resources.m_hTerrainShadowMaskTextureRd != m_lastPublishedResources.m_hTerrainShadowMaskTextureRd
	        || resources.m_hMiddleHeightTextureRd != m_lastPublishedResources.m_hMiddleHeightTextureRd
	        || resources.m_hMiddleHeightRegionMapTextureRd != m_lastPublishedResources.m_hMiddleHeightRegionMapTextureRd
	        || resources.m_hFarHeightTextureRd != m_lastPublishedResources.m_hFarHeightTextureRd
	        || resources.m_hFarColorTextureRd != m_lastPublishedResources.m_hFarColorTextureRd;
	if ( bTerrainTexturesChanged ) {
		m_nTerrainTextureGeneration = advanceGeneration( m_nTerrainTextureGeneration );
	}
	const bool bMaterialTexturesChanged = !m_bPublishedResourcesInitialized
	        || resources.m_hAlbedoTextureRd != m_lastPublishedResources.m_hAlbedoTextureRd
	        || resources.m_hFarAlbedoTextureRd != m_lastPublishedResources.m_hFarAlbedoTextureRd
	        || resources.m_hNormalTextureRd != m_lastPublishedResources.m_hNormalTextureRd
	        || resources.m_hSurfaceTextureRd != m_lastPublishedResources.m_hSurfaceTextureRd
	        || resources.m_hMaterialDisplacementTextureRd != m_lastPublishedResources.m_hMaterialDisplacementTextureRd
	        || resources.m_hDisplacementTextureRd != m_lastPublishedResources.m_hDisplacementTextureRd
	        || resources.m_hWorldNoiseTextureRd != m_lastPublishedResources.m_hWorldNoiseTextureRd
	        || resources.m_hMacroNoiseTextureRd != m_lastPublishedResources.m_hMacroNoiseTextureRd
	        || resources.m_hEditorBrushTextureRd != m_lastPublishedResources.m_hEditorBrushTextureRd;
	if ( bMaterialTexturesChanged ) {
		m_nMaterialTextureGeneration = advanceGeneration( m_nMaterialTextureGeneration );
	}
	m_lastPublishedResources = resources;
	m_bPublishedResourcesInitialized = true;
	snapshot.m_nTerrainTextureGeneration = m_nTerrainTextureGeneration;
	snapshot.m_nMaterialTextureGeneration = m_nMaterialTextureGeneration;

	const Ref<Terrain3DAssets> instance_assets = m_pTerrain->getAssets();
	Terrain3DHLODStreamer *const hlod_streamer = m_pTerrain->getHlodStreamer();
	const uint64_t hlod_generation = hlod_streamer ? hlod_streamer->getRdPublicationGeneration() : 0u;
	const bool hlod_changed = !m_bCachedHlodInstanceSceneValid || hlod_generation != m_nHlodSceneGeneration;
	if ( hlod_changed ) {
		m_cachedHlodMeshSources.clear();
		if ( hlod_streamer ) {
			if ( !hlod_streamer->fillRdMeshSources( m_cachedHlodMeshSources )
			        || !hlod_streamer->buildRdInstanceSnapshot( m_cachedHlodInstanceScene ) ) {
				t3dScopedLock_t lock( m_diagnosticsMutex );
				m_initializationError = "Fixed RD HLOD publication capacity was exceeded";
				return;
			}
		} else {
			m_cachedHlodInstanceScene.m_instances.reset();
			m_cachedHlodInstanceScene.m_cells.reset();
			m_cachedHlodInstanceScene.m_nGeneration = 0u;
		}
		m_bCachedHlodInstanceSceneValid = true;
		m_nHlodSceneGeneration = hlod_generation;
	}

	const uint64_t mesh_fingerprint = Terrain3DRdMeshCache::computeSourceFingerprint(
	        instance_assets, m_cachedHlodMeshSources.data(), m_cachedHlodMeshSources.size() );
	if ( !m_bCachedInstanceMeshesValid || mesh_fingerprint != m_nInstanceMeshFingerprint ) {
		Terrain3DColdAllocationScope allocation_scope;
		m_nInstanceMeshGeneration = advanceGeneration( m_nInstanceMeshGeneration );
		String mesh_error;
		const bool built = Terrain3DRdMeshCache::buildSourceSnapshot( instance_assets,
		        m_cachedHlodMeshSources.data(),
		        m_cachedHlodMeshSources.size(),
		        m_cachedInstanceMeshes,
		        mesh_fingerprint,
		        m_nInstanceMeshGeneration,
		        &mesh_error );
		if ( !built ) {
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "Failed to publish RD instance/HLOD meshes: " + mesh_error;
			return;
		}
		if ( !ensureInstanceMeshBankStorage() ) {
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "Fixed RD instance mesh bank capacity allocation failed";
			return;
		}
		m_bCachedInstanceMeshesValid = true;
		m_nInstanceMeshFingerprint = mesh_fingerprint;
	}

	Terrain3DInstancer *const instancer = m_pTerrain->getInstancer();
	const uint64_t instance_generation = instancer ? instancer->getRdPublicationGeneration() : 0u;
	const bool regular_changed = !m_bCachedRegularInstanceSceneValid || instance_generation != m_nInstanceSceneGeneration;
	if ( regular_changed ) {
		if ( instancer ) {
			if ( !instancer->buildRdInstanceSnapshot( m_cachedRegularInstanceScene ) ) {
				t3dScopedLock_t lock( m_diagnosticsMutex );
				m_initializationError = "Fixed RD instance publication capacity was exceeded";
				return;
			}
		} else {
			m_cachedRegularInstanceScene.m_instances.reset();
			m_cachedRegularInstanceScene.m_cells.reset();
			m_cachedRegularInstanceScene.m_nGeneration = 0u;
		}
		m_bCachedRegularInstanceSceneValid = true;
		m_nInstanceSceneGeneration = instance_generation;
	}
	if ( !m_bCachedInstanceSceneValid || regular_changed || hlod_changed ) {
		t3dRdInstanceSceneSnapshot_t &combined = m_cachedInstanceScene;
		combined.m_instances.reset();
		combined.m_cells.reset();
		m_nCombinedInstanceSceneGeneration = advanceGeneration( m_nCombinedInstanceSceneGeneration );
		combined.m_nGeneration = m_nCombinedInstanceSceneGeneration;
		const size_t combined_instance_count =
		        m_cachedRegularInstanceScene.m_instances.size() + m_cachedHlodInstanceScene.m_instances.size();
		const size_t combined_cell_count = m_cachedRegularInstanceScene.m_cells.size() + m_cachedHlodInstanceScene.m_cells.size();
		if ( combined_instance_count > combined.m_instances.capacity() || combined_cell_count > combined.m_cells.capacity() ) {
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "Fixed RD combined instance publication capacity was exceeded";
			return;
		}
		if ( !combined.m_instances.append(
		             m_cachedRegularInstanceScene.m_instances.data(), m_cachedRegularInstanceScene.m_instances.size() ) ) {
			return;
		}
		if ( !combined.m_cells.append( m_cachedRegularInstanceScene.m_cells.data(), m_cachedRegularInstanceScene.m_cells.size() ) ) {
			return;
		}
		{
			const uint32_t instance_offset = ( uint32_t )( combined.m_instances.size() );
			if ( !combined.m_instances.append(
			             m_cachedHlodInstanceScene.m_instances.data(), m_cachedHlodInstanceScene.m_instances.size() ) ) {
				return;
			}
			for ( size_t iCell = 0u; iCell < ( size_t )( ( m_cachedHlodInstanceScene.m_cells ).size() ); iCell++ ) {
				const t3dRdInstanceCell_t &source_cell = ( m_cachedHlodInstanceScene.m_cells )[iCell];
				t3dRdInstanceCell_t cell = source_cell;
				cell.m_nFirstInstance += instance_offset;
				if ( !combined.m_cells.pushBack( cell ) ) {
					return;
				}
			}
		}
		m_bCachedInstanceSceneValid = true;
	}
	if ( m_bCachedInstanceMeshesValid ) {
		if ( m_nCurrentInstanceMeshBank >= s_nRdFrameSnapshotSlotCount
		        || m_instanceMeshBanks[m_nCurrentInstanceMeshBank].m_nGeneration != m_cachedInstanceMeshes.m_nGeneration ) {
			const uint32_t pending_bank = pPending ? pPending->m_nInstanceMeshBank : s_nRdInvalidFrameSnapshotIndex;
			const uint32_t bank = selectWritablePublicationBank( m_nInstanceMeshReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ), pending_bank );
			if ( bank >= s_nRdFrameSnapshotSlotCount ) {
				LOG( ERROR, "No writable RD instance-mesh publication bank" );
				return;
			}
			if ( !m_instanceMeshBanks[bank].m_snapshot.copyFrom( m_cachedInstanceMeshes ) ) {
				return;
			}
			m_instanceMeshBanks[bank].m_nGeneration = m_cachedInstanceMeshes.m_nGeneration;
			m_nCurrentInstanceMeshBank = bank;
		}
		publication.m_nInstanceMeshBank = m_nCurrentInstanceMeshBank;
	}
	if ( m_bCachedInstanceSceneValid ) {
		if ( m_nCurrentInstanceSceneBank >= s_nRdFrameSnapshotSlotCount
		        || m_instanceSceneBanks[m_nCurrentInstanceSceneBank].m_nGeneration != m_cachedInstanceScene.m_nGeneration ) {
			const uint32_t pending_bank = pPending ? pPending->m_nInstanceSceneBank : s_nRdInvalidFrameSnapshotIndex;
			const uint32_t bank =
			        selectWritablePublicationBank( m_nInstanceSceneReaderBank.load( T3D_MEMORY_ORDER_ACQUIRE ), pending_bank );
			if ( bank >= s_nRdFrameSnapshotSlotCount ) {
				LOG( ERROR, "No writable RD instance-scene publication bank" );
				return;
			}
			if ( !m_instanceSceneBanks[bank].m_snapshot.copyFrom( m_cachedInstanceScene ) ) {
				return;
			}
			m_instanceSceneBanks[bank].m_nGeneration = m_cachedInstanceScene.m_nGeneration;
			m_nCurrentInstanceSceneBank = bank;
		}
		publication.m_nInstanceSceneBank = m_nCurrentInstanceSceneBank;
	}

	uint64_t publication_generation = m_nNextSnapshotGeneration;
	if ( publication_generation == 0u || publication_generation > s_nRdFrameSnapshotGenerationMax ) {
		publication_generation = 1u;
	}
	m_nNextSnapshotGeneration = publication_generation == s_nRdFrameSnapshotGenerationMax ? 1u : publication_generation + 1u;
	snapshot.m_nPublicationGeneration = publication_generation;
	const uint64_t published_token = t3dRdMakeFrameSnapshotToken( m_nSnapshotWriteIndex, publication_generation, true );
	const uint64_t reclaimed_token = m_nSnapshotExchangeToken.exchange( published_token, T3D_MEMORY_ORDER_ACQ_REL );
	const uint32_t reclaimed_index = t3dRdFrameSnapshotTokenIndex( reclaimed_token );
	m_nPublishedSnapshotIndex.store( m_nSnapshotWriteIndex, T3D_MEMORY_ORDER_RELEASE );
	m_nPublishedSnapshotGeneration.store( publication_generation, T3D_MEMORY_ORDER_RELEASE );
	m_nSnapshotPublications.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	if ( reclaimed_index >= s_nRdFrameSnapshotSlotCount ) {
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	m_nSnapshotWriteIndex = reclaimed_index;
}

const Terrain3DRenderCoordinator::t3dRdFrameSnapshotSlot_t *Terrain3DRenderCoordinator::acquireSnapshotOnRenderThread( void ) {
	const t3dRdFrameSnapshotSlot_t *const current =
	        m_bSnapshotRenderValid && m_nSnapshotRenderGeneration != 0u && m_nSnapshotRenderIndex < s_nRdFrameSnapshotSlotCount
	        ? &m_snapshotSlots[m_nSnapshotRenderIndex]
	        : NULL;
	for ( ;; ) {
		uint64_t token = m_nSnapshotExchangeToken.load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( !t3dRdFrameSnapshotTokenIsPublished( token ) ) {
			if ( current ) {
				m_nSnapshotReuses.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			}
			return current;
		}
		const uint32_t candidate_index = t3dRdFrameSnapshotTokenIndex( token );
		const uint64_t candidate_generation = t3dRdFrameSnapshotTokenGeneration( token );
		if ( candidate_index >= s_nRdFrameSnapshotSlotCount || candidate_generation == 0u
		        || t3dRdFrameSnapshotGenerationIsStale( candidate_generation, m_nSnapshotRenderGeneration ) ) {
			recordStaleSnapshot( token );
			if ( current ) {
				m_nSnapshotReuses.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			}
			return current;
		}
		const uint64_t released_token = t3dRdMakeFrameSnapshotToken( m_nSnapshotRenderIndex, m_nSnapshotRenderGeneration, false );
		if ( !m_nSnapshotExchangeToken.compare_exchange_weak(
		             token, released_token, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
			continue;
		}
		const uint64_t previous_generation = m_nSnapshotRenderGeneration;
		m_nSnapshotRenderIndex = candidate_index;
		m_nSnapshotRenderGeneration = candidate_generation;
		m_nConsumedSnapshotGeneration.store( candidate_generation, T3D_MEMORY_ORDER_RELEASE );
		m_nSnapshotConsumes.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		const uint64_t generation_distance = t3dRdFrameSnapshotGenerationDistance( candidate_generation, previous_generation );
		if ( previous_generation != 0u && generation_distance > 1u ) {
			m_nSkippedSnapshotGenerations.fetch_add( generation_distance - 1u, T3D_MEMORY_ORDER_RELAXED );
		}
		const t3dRdFrameSnapshotSlot_t &acquired = m_snapshotSlots[candidate_index];
		if ( acquired.m_snapshot.m_nPublicationGeneration != candidate_generation ) {
			m_bSnapshotRenderValid = false;
			recordStaleSnapshot( token );
			return NULL;
		}
		m_nRegionMapReaderBank.store( acquired.m_nRegionMapBank, T3D_MEMORY_ORDER_RELEASE );
		m_nInstanceMeshReaderBank.store( acquired.m_nInstanceMeshBank, T3D_MEMORY_ORDER_RELEASE );
		m_nInstanceSceneReaderBank.store( acquired.m_nInstanceSceneBank, T3D_MEMORY_ORDER_RELEASE );
		m_nOceanTopologyReaderBank.store( acquired.m_nOceanTopologyBank, T3D_MEMORY_ORDER_RELEASE );
		m_bSnapshotRenderValid = true;
		return &acquired;
	}
}

void Terrain3DRenderCoordinator::recordStaleSnapshot( const uint64_t nToken ) {
	if ( m_nLastStaleSnapshotToken.exchange( nToken, T3D_MEMORY_ORDER_ACQ_REL ) != nToken ) {
		m_nStaleSnapshots.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
}

void Terrain3DRenderCoordinator::resetSnapshotPublication( void ) {
	m_nSnapshotExchangeToken.store( t3dRdMakeFrameSnapshotToken( 1u, 0u, false ), T3D_MEMORY_ORDER_RELEASE );
	m_nSnapshotWriteIndex = 2u;
	m_nSnapshotRenderIndex = 0u;
	m_nSnapshotRenderGeneration = 0u;
	m_bSnapshotRenderValid = false;
	m_nPublishedSnapshotIndex.store( s_nRdInvalidFrameSnapshotIndex, T3D_MEMORY_ORDER_RELEASE );
	m_nPublishedSnapshotGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nConsumedSnapshotGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nLastStaleSnapshotToken.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	for ( size_t iSlot = 0u; iSlot < T3D_ARRAY_COUNT( m_snapshotSlots ); iSlot++ ) {
		t3dRdFrameSnapshotSlot_t &slot = ( m_snapshotSlots )[iSlot];
		slot = t3dRdFrameSnapshotSlot_t{};
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_regionMapBanks ); iBank++ ) {
		t3dRdRegionMapBank_t &bank = ( m_regionMapBanks )[iBank];
		bank = t3dRdRegionMapBank_t{};
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceMeshBanks ); iBank++ ) {
		t3dRdInstanceMeshBank_t &bank = ( m_instanceMeshBanks )[iBank];
		bank = t3dRdInstanceMeshBank_t{};
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		t3dRdInstanceSceneBank_t &bank = ( m_instanceSceneBanks )[iBank];
		bank = t3dRdInstanceSceneBank_t{};
	}
	for ( size_t iTopology = 0u; iTopology < T3D_ARRAY_COUNT( m_oceanTopologyBanks ); iTopology++ ) {
		t3dRdOceanTopologyBank_t &bank = ( m_oceanTopologyBanks )[iTopology];
		bank = t3dRdOceanTopologyBank_t{};
	}
	m_nRegionMapReaderBank.store( s_nRdInvalidFrameSnapshotIndex, T3D_MEMORY_ORDER_RELEASE );
	m_nInstanceMeshReaderBank.store( s_nRdInvalidFrameSnapshotIndex, T3D_MEMORY_ORDER_RELEASE );
	m_nInstanceSceneReaderBank.store( s_nRdInvalidFrameSnapshotIndex, T3D_MEMORY_ORDER_RELEASE );
	m_nOceanTopologyReaderBank.store( s_nRdInvalidFrameSnapshotIndex, T3D_MEMORY_ORDER_RELEASE );
	m_nCurrentRegionMapBank = s_nRdInvalidFrameSnapshotIndex;
	m_nCurrentInstanceMeshBank = s_nRdInvalidFrameSnapshotIndex;
	m_nCurrentInstanceSceneBank = s_nRdInvalidFrameSnapshotIndex;
	m_nCurrentOceanTopologyBank = s_nRdInvalidFrameSnapshotIndex;
	m_nRegionTopologyGeneration = 0u;
	m_nPhysicalRegionMapGeneration = 0u;
	m_nRegionLightingSignature = 0u;
	m_nRegionMapGeneration = 0u;
	m_nLastRegionCacheSourceGeneration = 0u;
	m_nRegionCacheGeneration = 0u;
	m_nLastPatchFingerprint = 0u;
	m_nPatchGeneration = 0u;
	m_nPreviousPatchCount = 0u;
	m_nLastMaterialFingerprint = 0u;
	m_nMaterialGeneration = 0u;
	m_nTerrainTextureGeneration = 0u;
	m_nMaterialTextureGeneration = 0u;
	m_lastPublishedResources = t3dRdTerrainBindings_t{};
	m_bRegionMapInitialized = false;
	m_bRegionCacheInitialized = false;
	m_bPatchStateInitialized = false;
	m_bMaterialStateInitialized = false;
	m_bPublishedResourcesInitialized = false;
}

bool Terrain3DRenderCoordinator::attachForwardBridge( void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	m_bForwardBridgeAttached = pBridge && pBridge->attachCoordinator( this );
	return m_bForwardBridgeAttached && pBridge->isBridgeReady();
}

void Terrain3DRenderCoordinator::detachForwardBridge( void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	if ( m_bForwardBridgeAttached && pBridge ) {
		pBridge->detachCoordinator( this );
	}
	m_bForwardBridgeAttached = false;
}

bool Terrain3DRenderCoordinator::isForwardBridgeReady( void ) const {
	const Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	return m_bForwardBridgeAttached && pBridge && pBridge->isBridgeReady();
}

void Terrain3DRenderCoordinator::updateRenderRoute( void ) {
	const bool bBridgeReady = isForwardBridgeReady();
	m_bRenderEnabled.store( bBridgeReady && !m_bFailureLatched, T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( m_bCompositorDebugEnabled && !m_bFailureLatched );
	}
	if ( !bBridgeReady ) {
		Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
		const String bridge_error = pBridge ? pBridge->getLastError() : String( "Terrain3D Forward+ bridge adapter is unavailable" );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		if ( m_initializationError.is_empty() ) {
			m_initializationError = bridge_error;
		}
	}
}


void Terrain3DRenderCoordinator::attachEffectToActiveCamera( void ) {
	if ( !m_pTerrain || m_effect.is_null() || !m_bCompositorDebugEnabled ) {
		return;
	}
	Camera3D *camera = NULL;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const viewport = editor ? editor->get_editor_viewport_3d( 0 ) : NULL;
		camera = viewport ? viewport->get_camera_3d() : NULL;
	} else if ( m_pTerrain->get_viewport() ) {
		camera = m_pTerrain->get_viewport()->get_camera_3d();
	}
	if ( !camera ) {
		return;
	}
	if ( m_nAttachedCameraId == camera->get_instance_id() && m_attachedCompositor.is_valid()
	        && camera->get_compositor() == m_attachedCompositor ) {
		return;
	}
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
		if ( effects[i] == m_effect ) {
			bFound = true;
			break;
		}
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
		if ( effect != m_effect ) {
			remaining.push_back( effect );
		}
	}
	m_attachedCompositor->set_compositor_effects( remaining );
	if ( m_bCreatedCompositor && remaining.is_empty() && m_nAttachedCameraId != 0u ) {
		Object *const object = ObjectDB::get_instance( m_nAttachedCameraId );
		Camera3D *const camera = Object::cast_to<Camera3D>( object );
		if ( camera && camera->get_compositor() == m_attachedCompositor ) {
			camera->set_compositor( Ref<Compositor>() );
		}
	}
	m_attachedCompositor.unref();
	m_nAttachedCameraId = 0u;
	m_bCreatedCompositor = false;
}

bool Terrain3DRenderCoordinator::initializeRendererOnRenderThread( void ) {
	if ( m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return true;
	}
	const bool ready = m_shaderLibrary.isReady()
	        && m_renderer.initialize( m_shaderLibrary.getNear(),
	                m_shaderLibrary.getMiddle(),
	                m_shaderLibrary.getFar(),
	                m_shaderLibrary.getCull(),
	                m_shaderLibrary.getForwardBundle() )
	        && m_instanceRenderer.initialize(
	                m_shaderLibrary.getInstances(), m_shaderLibrary.getHlodCull(), m_shaderLibrary.getForwardBundle() )
	        && m_oceanRenderer.initialize( m_shaderLibrary.getOceanColor(), m_shaderLibrary.getOceanShadow() );
	m_bRendererInitialized.store( ready, T3D_MEMORY_ORDER_RELEASE );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( !ready ) {
		m_renderWorldRuntime.shutdown_cold();
		m_oceanRenderer.shutdown();
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		resetForwardLifecycle();
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
		if ( failures >= 8u ) {
			m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		}
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.isReady()
		        ? ( !m_instanceRenderer.getLastError().is_empty() ? m_instanceRenderer.getLastError() : m_renderer.getLastError() )
		        : m_shaderLibrary.getLastError();
	}
	return ready;
}

void Terrain3DRenderCoordinator::initializeOcean( void ) {
	if ( !m_pTerrain || !m_pTerrain->getOceanMesher() ) {
		m_cachedOceanTopology = t3dRdOceanTopologySnapshot_t{};
		m_bCachedOceanTopologyValid = false;
		return;
	}
	t3dRdOceanTopologySnapshot_t topology;
	m_nOceanTopologyGeneration = advanceGeneration( m_nOceanTopologyGeneration );
	topology.m_nGeneration = m_nOceanTopologyGeneration;
	topology.m_szGeometry[LOD0_MESH] = m_pTerrain->getOceanMesher()->getClipmapGeometry( LOD0_MESH );
	topology.m_szGeometry[OUTER_RING_MESH] = m_pTerrain->getOceanMesher()->getClipmapGeometry( OUTER_RING_MESH );
	m_cachedOceanTopology = topology;
	m_bCachedOceanTopologyValid = true;
}

void Terrain3DRenderCoordinator::initialize( Terrain3D *const pTerrain ) {
	shutdown();
	m_pTerrain = pTerrain;
	if ( !m_pTerrain ) {
		return;
	}
	{
		Terrain3DColdAllocationScope allocation_scope;
		const int64_t region_map_bank_bytes = ( int64_t )( s_nMaxRegionSlots * sizeof( uint32_t ) );
		for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_regionMapBanks ); iBank++ ) {
			t3dRdRegionMapBank_t &bank = ( m_regionMapBanks )[iBank];
			bank.m_bytes.resize( region_map_bank_bytes );
		}
	}
	OS *const os = OS::get_singleton();
	const bool bCompareCulling = os && os->get_environment( "TERRAIN3D_RD_COMPARE_CULL" ) == "1";
	const bool bDirectDebug = os && os->get_environment( "TERRAIN3D_RD_DIRECT_DRAW" ) == "1";
	m_bCompositorDebugEnabled = os && os->get_environment( "TERRAIN3D_RD_DEBUG_COMPOSITOR" ) == "1";
	m_bMotionDebugEnabled = os && os->get_environment( "TERRAIN3D_RD_DEBUG_MOTION" ) == "1";
	m_renderer.setMotionDebugEnabled( m_bMotionDebugEnabled );
	m_instanceRenderer.setMotionDebugEnabled( m_bMotionDebugEnabled );
	m_renderer.setCullingComparisonEnabled( bCompareCulling );
	m_renderer.setDirectDrawDebugEnabled( bDirectDebug );
	const bool bInstanceDirectDebug = os && os->get_environment( "TERRAIN3D_RD_INSTANCE_DIRECT_DRAW" ) == "1";
	m_instanceRenderer.setDirectDrawDebugEnabled( bDirectDebug || bInstanceDirectDebug );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bFailureLatched = false;
	{
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = String();
	}
	m_effect.instantiate();
	m_effect->setCoordinator( this );
	m_effect->set_enabled( false );
	attachForwardBridge();
	if ( !m_shaderLibrary.load() ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
	}
	if ( !m_pTerrain->getMesher()
	        || !m_renderer.prepareTopologies( m_pTerrain->getMesher()->getClipmapGeometry( LOD0_MESH ),
	                m_pTerrain->getMesher()->getClipmapGeometry( OUTER_RING_MESH ) ) ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Failed to prepare packed RenderingDevice clipmap topology";
	}
	initializeOcean();
	publishSnapshot();
	updateRenderRoute();
	if ( m_bCompositorDebugEnabled ) {
		attachEffectToActiveCamera();
	}
}

void Terrain3DRenderCoordinator::shutdown( void ) {
	m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}
	detachForwardBridge();
	resetForwardLifecycle();
	{
		t3dScopedLock_t lock( m_renderWorldPublicationMutex );
		m_renderWorldPendingPublication = t3dRenderWorldPendingPublication_t{};
	}
	// A renderer can own partially-created topology resources before the final
	// initialized flag is published. Always tear it down on the render thread;
	// otherwise a cold mesh rebuild can replace the CPU topology while upload()
	// continues accepting the old GPU-ready vertex/index buffers.
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		shutdownRendererOnRenderThread();
	} else if ( m_effect.is_valid()
	        && Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::shutdownRendererOnRenderThread ) ) ) {
		Terrain3DRdContext::forceSync();
	} else {
		if ( m_bPreviewRendererInitialized ) {
			m_previewRenderer.shutdown();
			m_bPreviewRendererInitialized = false;
		}
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	}
	if ( m_effect.is_valid() ) {
		m_effect->setCoordinator( NULL );
	}
	detachEffect();
	m_effect.unref();
	m_worldNoiseCache.clear();
	m_macroNoiseTexture.clear();
	m_editorBrushTexture.clear();
	m_nMacroNoiseSourceId = 0u;
	m_nEditorBrushSourceId = 0u;
	m_shaderLibrary.clear();
	m_cachedInstanceMeshes = t3dRdMeshSourceSnapshot_t{};
	m_bCachedInstanceMeshesValid = false;
	m_cachedPreviewMeshes = t3dRdMeshSourceSnapshot_t{};
	m_bCachedPreviewMeshesValid = false;
	m_nPreviewMeshFingerprint = 0u;
	m_nPreviewMeshGeneration = 0u;
	{
		t3dScopedLock_t lock( m_previewStateMutex );
		m_pendingPreviewMeshes = t3dRdMeshSourceSnapshot_t{};
		m_bPendingPreviewMeshesValid = false;
		m_pendingPreviewImage.unref();
		m_pendingPreviewError = String();
	}
	m_cachedRegularInstanceScene = t3dRdInstanceSceneSnapshot_t{};
	m_cachedHlodInstanceScene = t3dRdInstanceSceneSnapshot_t{};
	m_cachedInstanceScene = t3dRdInstanceSceneSnapshot_t{};
	m_bCachedRegularInstanceSceneValid = false;
	m_bCachedHlodInstanceSceneValid = false;
	m_bCachedInstanceSceneValid = false;
	m_cachedOceanTopology = t3dRdOceanTopologySnapshot_t{};
	m_bCachedOceanTopologyValid = false;
	m_cachedHlodMeshSources.clear();
	m_nInstanceMeshFingerprint = 0u;
	m_nInstanceMeshGeneration = 0u;
	m_nInstanceSceneGeneration = 0u;
	m_nHlodSceneGeneration = 0u;
	m_nCombinedInstanceSceneGeneration = 0u;
	resetSnapshotPublication();
	m_bCompositorDebugEnabled = false;
	m_bMotionDebugEnabled = false;
	m_pTerrain = NULL;
}

void Terrain3DRenderCoordinator::update( void ) {
	if ( !m_pTerrain ) {
		return;
	}
	publishSnapshot();
	if ( m_bRendererFailed.load( T3D_MEMORY_ORDER_ACQUIRE ) && !m_bFailureLatched ) {
		m_bFailureLatched = true;
		m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
		if ( m_effect.is_valid() ) {
			m_effect->set_enabled( false );
		}
		// A failed RD renderer is intentionally visible. Never resurrect the
		// legacy native terrain surface as a hidden failure fallback.
	}
	updateRenderRoute();
	if ( m_bCompositorDebugEnabled && !m_bFailureLatched ) {
		attachEffectToActiveCamera();
	} else if ( !m_bCompositorDebugEnabled ) {
		detachEffect();
	}
}

void Terrain3DRenderCoordinator::shutdownRendererOnRenderThread( void ) {
	if ( m_bPreviewRendererInitialized ) {
		m_previewRenderer.shutdown();
		m_bPreviewRendererInitialized = false;
	}
	m_renderWorldRuntime.shutdown_cold();
	m_oceanRenderer.shutdown();
	m_instanceRenderer.shutdown();
	m_renderer.shutdown();
	resetForwardLifecycle();
	m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRenderCoordinator::renderPreviewOnRenderThread( void ) {
	t3dScopedLock_t lock( m_previewStateMutex );
	m_pendingPreviewImage.unref();
	m_pendingPreviewError = String();
	if ( !m_bPendingPreviewMeshesValid || !m_shaderLibrary.isReady() ) {
		m_pendingPreviewError = "RD editor preview source/shaders are unavailable";
		return;
	}
	if ( !m_bPreviewRendererInitialized ) {
		m_bPreviewRendererInitialized = m_previewRenderer.initialize(
		        m_shaderLibrary.getInstances(), m_shaderLibrary.getHlodCull(), m_shaderLibrary.getForwardBundle() );
	}
	Ref<Image> image;
	const bool ok = m_bPreviewRendererInitialized
	        && m_previewRenderer.renderPreview( &m_pendingPreviewMeshes, m_nPendingPreviewAssetId, m_pendingPreviewSize, image );
	m_pendingPreviewImage = image;
	if ( !ok ) {
		m_pendingPreviewError = m_previewRenderer.getLastError();
	}
}

Ref<Image> Terrain3DRenderCoordinator::createMeshPreview( const uint16_t nAssetId, const Vector2i &rSize ) {
	t3dScopedLock_t call_lock( m_previewCallMutex );
	if ( !m_pTerrain || rSize.x < 2 || rSize.y < 2 || !m_effect.is_valid() ) {
		return Ref<Image>();
	}
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
	if ( assets.is_null() || nAssetId >= ( uint16_t )( assets->getMeshCount() ) ) {
		return Ref<Image>();
	}
	const uint64_t fingerprint = Terrain3DRdMeshCache::computeSourceFingerprint( assets );
	if ( !m_bCachedPreviewMeshesValid || fingerprint != m_nPreviewMeshFingerprint ) {
		String error;
		m_nPreviewMeshGeneration = advanceGeneration( m_nPreviewMeshGeneration );
		if ( !Terrain3DRdMeshCache::buildSourceSnapshot(
		             assets, m_cachedPreviewMeshes, fingerprint, m_nPreviewMeshGeneration, &error, true ) ) {
			m_bCachedPreviewMeshesValid = false;
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = error;
			return Ref<Image>();
		}
		m_bCachedPreviewMeshesValid = true;
		m_nPreviewMeshFingerprint = fingerprint;
	}
	{
		t3dScopedLock_t lock( m_previewStateMutex );
		if ( !t3dContainerReserveCold( m_pendingPreviewMeshes.m_vertexChunkHashes, m_cachedPreviewMeshes.m_vertexChunkHashes.size() )
		        || !t3dContainerReserveCold(
		                m_pendingPreviewMeshes.m_index16ChunkHashes, m_cachedPreviewMeshes.m_index16ChunkHashes.size() )
		        || !t3dContainerReserveCold(
		                m_pendingPreviewMeshes.m_index32ChunkHashes, m_cachedPreviewMeshes.m_index32ChunkHashes.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_surfaces, m_cachedPreviewMeshes.m_surfaces.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_lods, m_cachedPreviewMeshes.m_lods.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_materials, m_cachedPreviewMeshes.m_materials.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_textures, m_cachedPreviewMeshes.m_textures.size() )
		        || !m_pendingPreviewMeshes.copyFrom( m_cachedPreviewMeshes ) ) {
			return Ref<Image>();
		}
		m_bPendingPreviewMeshesValid = true;
		m_nPendingPreviewAssetId = nAssetId;
		m_pendingPreviewSize = rSize;
		m_pendingPreviewImage.unref();
		m_pendingPreviewError = String();
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderPreviewOnRenderThread();
	} else {
		if ( m_effect.is_null()
		        || !Terrain3DRdContext::enqueueOnRenderThread(
		                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::renderPreviewOnRenderThread ) ) ) {
			return Ref<Image>();
		}
		Terrain3DRdContext::forceSync();
	}
	t3dScopedLock_t lock( m_previewStateMutex );
	if ( m_pendingPreviewImage.is_null() && !m_pendingPreviewError.is_empty() ) {
		t3dScopedLock_t diagnostics_lock( m_diagnosticsMutex );
		m_initializationError = m_pendingPreviewError;
	}
	return m_pendingPreviewImage;
}

void Terrain3DRenderCoordinator::render( RenderData *const pRenderData ) {
	if ( !m_bCompositorDebugEnabled || !pRenderData ) {
		return;
	}
	++m_nCallbackCount;
	if ( !initializeRendererOnRenderThread() ) {
		return;
	}
	const t3dRdFrameSnapshotSlot_t *const publication = acquireSnapshotOnRenderThread();
	if ( !publication ) {
		return;
	}
	const t3dRdFrameSnapshot_t &snapshot = publication->m_snapshot;
	const t3dRdRegionMapBank_t *const pRegionMap =
	        publication->m_nRegionMapBank < s_nRdFrameSnapshotSlotCount ? &m_regionMapBanks[publication->m_nRegionMapBank] : NULL;
	const t3dRdInstanceMeshBank_t *const pMeshes =
	        publication->m_nInstanceMeshBank < s_nRdFrameSnapshotSlotCount ? &m_instanceMeshBanks[publication->m_nInstanceMeshBank] : NULL;
	const t3dRdInstanceSceneBank_t *const pScene = publication->m_nInstanceSceneBank < s_nRdFrameSnapshotSlotCount
	        ? &m_instanceSceneBanks[publication->m_nInstanceSceneBank]
	        : NULL;
	const bool bHasTerrain = snapshot.m_nPatchCount > 0u && pRegionMap && !pRegionMap->m_bytes.is_empty();
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
	const bool instances_ok = m_instanceRenderer.render(
	        pRenderData, pMeshes ? &pMeshes->m_snapshot : NULL, pScene ? &pScene->m_snapshot : NULL, snapshot.m_nCameraCullMask );
	const bool terrain_ok = !bHasTerrain
	        || ( instances_ok
	                && m_renderer.render(
	                        pRenderData, snapshot, pRegionMap->m_bytes, snapshot.m_nRegionMapGeneration, publication->m_bindings, true ) );
	const bool render_ok = instances_ok && terrain_ok;
	if ( render_ok ) {
		m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	} else {
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
		// Resource/render failures are fatal to the current ownership generation.
		// Tear it down on this render thread; a later callback reconstructs from
		// immutable CPU publication without reviving the native renderer.
		m_renderWorldRuntime.shutdown_cold();
		m_oceanRenderer.shutdown();
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		resetForwardLifecycle();
		m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		if ( failures >= 8u ) {
			m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		}
	}
}

bool Terrain3DRenderCoordinator::publishRenderWorldDatabaseCold( const T3DGeometryDatabase &rGeometry,
        const T3DSceneCell *const pCells,
        const uint32_t nCellCount,
        const T3DInstanceRecord *const pInstances,
        const uint32_t nInstanceCount,
        const T3DMaterialRecord *const pMaterials,
        const uint32_t nMaterialCount ) {
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		return m_renderWorldRuntime.upload_scene_database_warm(
		        rGeometry, pCells, nCellCount, pInstances, nInstanceCount, pMaterials, nMaterialCount );
	}
	if ( m_effect.is_null() ) {
		return false;
	}
	{
		t3dScopedLock_t lock( m_renderWorldPublicationMutex );
		if ( m_renderWorldPendingPublication.bPending ) {
			return false;
		}
		m_renderWorldPendingPublication = t3dRenderWorldPendingPublication_t{};
		m_renderWorldPendingPublication.pGeometry = &rGeometry;
		m_renderWorldPendingPublication.pCells = pCells;
		m_renderWorldPendingPublication.pInstances = pInstances;
		m_renderWorldPendingPublication.pMaterials = pMaterials;
		m_renderWorldPendingPublication.nCellCount = nCellCount;
		m_renderWorldPendingPublication.nInstanceCount = nInstanceCount;
		m_renderWorldPendingPublication.nMaterialCount = nMaterialCount;
		m_renderWorldPendingPublication.bPending = true;
	}
	if ( !Terrain3DRdContext::enqueueOnRenderThread(
	             callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::serviceRenderWorldPublicationOnRenderThread ) ) ) {
		t3dScopedLock_t lock( m_renderWorldPublicationMutex );
		m_renderWorldPendingPublication = t3dRenderWorldPendingPublication_t{};
		return false;
	}
	Terrain3DRdContext::forceSync();
	t3dScopedLock_t lock( m_renderWorldPublicationMutex );
	const bool result = !m_renderWorldPendingPublication.bPending && m_renderWorldPendingPublication.bResult;
	m_renderWorldPendingPublication = t3dRenderWorldPendingPublication_t{};
	return result;
}

void Terrain3DRenderCoordinator::releaseRenderWorldDatabaseCold( void ) {
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		m_renderWorldRuntime.release_scene_cold();
		return;
	}
	if ( m_effect.is_null() ) {
		return;
	}
	{
		t3dScopedLock_t lock( m_renderWorldPublicationMutex );
		if ( m_renderWorldPendingPublication.bPending ) {
			return;
		}
		m_renderWorldPendingPublication = t3dRenderWorldPendingPublication_t{};
		m_renderWorldPendingPublication.bRelease = true;
		m_renderWorldPendingPublication.bPending = true;
	}
	if ( !Terrain3DRdContext::enqueueOnRenderThread(
	             callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::serviceRenderWorldPublicationOnRenderThread ) ) ) {
		t3dScopedLock_t lock( m_renderWorldPublicationMutex );
		m_renderWorldPendingPublication = t3dRenderWorldPendingPublication_t{};
		return;
	}
	Terrain3DRdContext::forceSync();
	t3dScopedLock_t lock( m_renderWorldPublicationMutex );
	m_renderWorldPendingPublication = t3dRenderWorldPendingPublication_t{};
}

void Terrain3DRenderCoordinator::serviceRenderWorldPublicationOnRenderThread( void ) {
	t3dScopedLock_t lock( m_renderWorldPublicationMutex );
	if ( !m_renderWorldPendingPublication.bPending ) {
		return;
	}
	bool result = false;
	if ( m_renderWorldPendingPublication.bRelease ) {
		m_renderWorldRuntime.release_scene_cold();
		result = true;
	} else if ( m_renderWorldPendingPublication.pGeometry ) {
		result = m_renderWorldRuntime.upload_scene_database_warm( *m_renderWorldPendingPublication.pGeometry,
		        m_renderWorldPendingPublication.pCells,
		        m_renderWorldPendingPublication.nCellCount,
		        m_renderWorldPendingPublication.pInstances,
		        m_renderWorldPendingPublication.nInstanceCount,
		        m_renderWorldPendingPublication.pMaterials,
		        m_renderWorldPendingPublication.nMaterialCount );
	}
	m_renderWorldPendingPublication.bResult = result;
	m_renderWorldPendingPublication.bPending = false;
}

void Terrain3DRenderCoordinator::resetForwardLifecycle( void ) {
	for ( size_t iViewport = 0u; iViewport < T3D_ARRAY_COUNT( m_szForwardViewportStates ); iViewport++ ) {
		t3dRdForwardViewportState_t &state = ( m_szForwardViewportStates )[iViewport];
		state = t3dRdForwardViewportState_t{};
	}
	m_nForwardPreparedScenePass = 0u;
	m_nForwardPreparedViewportKey = 0u;
	m_bForwardPreparedHasTerrain = false;
	m_bForwardPreparedHasOcean = false;
	m_bForwardPreparedHasRenderWorld = false;
}

bool Terrain3DRenderCoordinator::prepareForwardLifecycle(
        const t3dRdForwardPassContext_t &rContext, bool &rHistoryValid, bool &rViewportReconfigured ) {
	rHistoryValid = false;
	rViewportReconfigured = false;
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		return true;
	}
	if ( rContext.m_nViewportKey == 0u || rContext.m_nViewCount == 0u || rContext.m_nViewCount > s_nRdForwardMaxViews
	        || rContext.m_nSampleCount == 0u || !rContext.m_hNativeFramebuffer.is_valid()
	        || rContext.m_nFramebufferFormat == RenderingDevice::INVALID_ID ) {
		return false;
	}
	t3dRdForwardViewportState_t *slot = NULL;
	t3dRdForwardViewportState_t *oldest = &m_szForwardViewportStates[0];
	for ( size_t iViewport = 0u; iViewport < T3D_ARRAY_COUNT( m_szForwardViewportStates ); iViewport++ ) {
		t3dRdForwardViewportState_t &candidate = ( m_szForwardViewportStates )[iViewport];
		if ( candidate.m_bValid && candidate.m_nViewportKey == rContext.m_nViewportKey ) {
			slot = &candidate;
			break;
		}
		if ( !candidate.m_bValid && !slot ) {
			slot = &candidate;
		}
		if ( candidate.m_nLastRenderFrame < oldest->m_nLastRenderFrame ) {
			oldest = &candidate;
		}
	}
	if ( !slot ) {
		slot = oldest;
		++m_nForwardViewportEvictions;
	}
	const bool same = slot->m_bValid && slot->m_nViewportKey == rContext.m_nViewportKey
	        && slot->m_hFramebuffer == rContext.m_hNativeFramebuffer && slot->m_nFramebufferFormat == rContext.m_nFramebufferFormat
	        && slot->m_renderSize == rContext.m_renderSize && slot->m_renderRegion == rContext.m_renderRegion
	        && slot->m_nViewCount == rContext.m_nViewCount && slot->m_nSampleCount == rContext.m_nSampleCount;
	const bool reconfigured_this_frame = slot->m_bValid && slot->m_nReconfiguredRenderFrame == rContext.m_nRenderFrame;
	rHistoryValid = same && !reconfigured_this_frame && rContext.m_bMotionVectorsEnabled;
	rViewportReconfigured = !same;
	if ( !same ) {
		++m_nForwardViewportReconfigurations;
		slot->m_nReconfiguredRenderFrame = rContext.m_nRenderFrame;
		slot->m_nViewportKey = rContext.m_nViewportKey;
		slot->m_hFramebuffer = rContext.m_hNativeFramebuffer;
		slot->m_nFramebufferFormat = rContext.m_nFramebufferFormat;
		slot->m_renderSize = rContext.m_renderSize;
		slot->m_renderRegion = rContext.m_renderRegion;
		slot->m_nViewCount = rContext.m_nViewCount;
		slot->m_nSampleCount = rContext.m_nSampleCount;
		slot->m_bValid = true;
	}
	slot->m_nLastRenderFrame = rContext.m_nRenderFrame;
	return true;
}

bool Terrain3DRenderCoordinator::renderForwardPass( const t3dRdForwardPassContext_t &rContext ) {
	if ( !m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return false;
	}
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	++m_nForwardPassCallbacks;
	m_nLastForwardScenePass = rContext.m_nScenePass;
	m_nLastForwardDrawList = rContext.m_nDrawList;
	m_nLastForwardPassKind = ( uint32_t )( rContext.m_passKind );
	m_nLastForwardViewCount = rContext.m_nViewCount;
	m_nLastForwardSampleCount = rContext.m_nSampleCount;
	m_nLastForwardFramebufferFormat = rContext.m_nFramebufferFormat;
	switch ( rContext.m_passKind ) {
		case T3D_RD_FORWARD_PASS_DEPTH:
			++m_nDepthPassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_OPAQUE:
			++m_nOpaquePassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_SHADOW:
			++m_nShadowPassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_MOTION:
			++m_nMotionPassCallbacks;
			break;
		case T3D_RD_FORWARD_PASS_TRANSPARENT:
			++m_nTransparentPassCallbacks;
			break;
		default:
			break;
	}
#endif
	if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_DEPTH && rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE
	        && rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION
	        && rContext.m_passKind != T3D_RD_FORWARD_PASS_TRANSPARENT ) {
		return false;
	}

	if ( rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE ) {
		bool history_valid = false;
		bool viewport_reconfigured = false;
		if ( !prepareForwardLifecycle( rContext, history_valid, viewport_reconfigured ) || !initializeRendererOnRenderThread() ) {
			return false;
		}
		const t3dRdFrameSnapshotSlot_t *const publication = acquireSnapshotOnRenderThread();
		if ( !publication ) {
			return false;
		}
		const t3dRdFrameSnapshot_t &snapshot = publication->m_snapshot;
		const t3dRdRegionMapBank_t *const pRegionMap =
		        publication->m_nRegionMapBank < s_nRdFrameSnapshotSlotCount ? &m_regionMapBanks[publication->m_nRegionMapBank] : NULL;
		const t3dRdInstanceMeshBank_t *const pMeshes = publication->m_nInstanceMeshBank < s_nRdFrameSnapshotSlotCount
		        ? &m_instanceMeshBanks[publication->m_nInstanceMeshBank]
		        : NULL;
		const t3dRdInstanceSceneBank_t *const pScene = publication->m_nInstanceSceneBank < s_nRdFrameSnapshotSlotCount
		        ? &m_instanceSceneBanks[publication->m_nInstanceSceneBank]
		        : NULL;
		const t3dRdOceanTopologyBank_t *const pOcean = publication->m_nOceanTopologyBank < s_nRdFrameSnapshotSlotCount
		        ? &m_oceanTopologyBanks[publication->m_nOceanTopologyBank]
		        : NULL;
		const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
		const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
		const uint32_t visibility_mask =
		        shadow_pass ? ( rContext.m_nShadowCasterMask & rContext.m_nShadowLightCullMask ) : rContext.m_nCameraVisibleLayers;
		const bool has_terrain = !transparent_pass && snapshot.m_nPatchCount > 0u && pRegionMap && !pRegionMap->m_bytes.is_empty()
		        && ( !shadow_pass || ( snapshot.m_bCastShadows && ( snapshot.m_nRenderLayers & visibility_mask ) != 0u ) );
		m_nForwardPreparedScenePass = rContext.m_nScenePass;
		m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
		m_bForwardPreparedHasTerrain = has_terrain;
		m_bForwardPreparedHasRenderWorld = false;
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) {
			Terrain3DRdContext &render_world_context = Terrain3DRdContext::getShared();
			bool render_world_ready = false;
			{
				// RenderWorld pipeline/shader/descriptor mutation is an explicit PREPARE-only
				// cold boundary. It is never allowed to occur from SUBMIT/hot rendering.
				Terrain3DColdAllocationScope cold_scope;
				render_world_ready = m_renderWorldRuntime.is_initialized()
				        ? m_renderWorldRuntime.rebuild_raster_cold( rContext.m_nFramebufferFormat )
				        : m_renderWorldRuntime.initialize_cold(
				                  render_world_context, m_shaderLibrary.getRenderWorldBundle(), rContext.m_nFramebufferFormat );
				if ( render_world_ready && m_renderWorldRuntime.has_scene() ) {
					render_world_ready = m_renderWorldRuntime.prepare_forward_cold( rContext );
				}
			}
			const bool render_world_generations_ok = !render_world_ready
			        || m_renderWorldRuntime.publish_terrain_generations_hot( m_nRegionTopologyGeneration,
			                m_nPhysicalRegionMapGeneration,
			                m_nPatchGeneration,
			                m_nMaterialGeneration,
			                m_nRegionLightingSignature );
			// RenderWorld is a companion lane. Failure or an unsupported native pass
			// capability must never suppress authoritative terrain/instance/ocean work.
			m_bForwardPreparedHasRenderWorld = render_world_ready && render_world_generations_ok && m_renderWorldRuntime.has_scene();
		}
		Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
		const bool instances_ok = transparent_pass ? true
		                                           : m_instanceRenderer.prepareForwardPass( rContext,
		                                                     pMeshes ? &pMeshes->m_snapshot : NULL,
		                                                     pScene ? &pScene->m_snapshot : NULL,
		                                                     visibility_mask,
		                                                     history_valid,
		                                                     viewport_reconfigured );
		// Terrain and instance/HLOD resources are independent. Always prepare the
		// terrain pass even when the companion instance lane reports a failure.
		const bool terrain_ok = !has_terrain
		        || m_renderer.prepareForwardPass( rContext,
		                snapshot,
		                pRegionMap->m_bytes,
		                snapshot.m_nRegionMapGeneration,
		                publication->m_bindings,
		                history_valid,
		                viewport_reconfigured );
		const bool ocean_ok =
		        m_oceanRenderer.prepareForwardPass( rContext, snapshot, pOcean ? &pOcean->m_snapshot : NULL, visibility_mask );
		m_bForwardPreparedHasOcean = snapshot.m_ocean.m_nEnabled != 0u && snapshot.m_ocean.m_nPatchCount > 0u
		        && ( transparent_pass || ( shadow_pass && snapshot.m_ocean.m_nCastShadows != 0u ) );
		if ( instances_ok && terrain_ok && ocean_ok ) {
			m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
			return true;
		}
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
		m_renderWorldRuntime.shutdown_cold();
		m_oceanRenderer.shutdown();
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		resetForwardLifecycle();
		m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		if ( failures >= 8u ) {
			m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		}
		return false;
	}

	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || !m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE )
	        || m_nForwardPreparedScenePass != rContext.m_nScenePass || m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		return false;
	}
	const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
	const bool instances_ok = transparent_pass ? true : m_instanceRenderer.renderForwardPass( rContext );
	const bool terrain_ok = !m_bForwardPreparedHasTerrain || m_renderer.renderForwardPass( rContext );
	const bool ocean_ok = !m_bForwardPreparedHasOcean || m_oceanRenderer.renderForwardPass( rContext );
	T3DRenderWorldFrameResult render_world_result = {};
	const bool render_world_ok = !m_bForwardPreparedHasRenderWorld
	        || m_renderWorldRuntime.render_main_camera_hot( rContext, 0u, 0u, 0u, 0u, 0u, render_world_result );
	if ( !render_world_ok ) {
		// Keep the last coherent RenderWorld publication, but isolate the failed
		// companion submission from the native Terrain3D pipeline.
		m_bForwardPreparedHasRenderWorld = false;
	}
	if ( instances_ok && terrain_ok && ocean_ok ) {
		m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		return true;
	}
	// The native draw list is open here. Do not destroy or rebuild RD resources
	// until a later PREPARE callback, where resource mutation is legal.
	const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
	if ( failures >= 8u ) {
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
	return false;
}

static size_t t3dRenderCoordinatorMeshSnapshotBytes( const t3dRdMeshSourceSnapshot_t &rSnapshot ) {
	size_t nBytes = rSnapshot.m_vertexBytes.size() + rSnapshot.m_index16Bytes.size() + rSnapshot.m_index32Bytes.size();
	nBytes += rSnapshot.m_vertexChunkHashes.capacity() * sizeof( uint64_t );
	nBytes += rSnapshot.m_index16ChunkHashes.capacity() * sizeof( uint64_t );
	nBytes += rSnapshot.m_index32ChunkHashes.capacity() * sizeof( uint64_t );
	nBytes += rSnapshot.m_surfaces.capacity() * sizeof( t3dRdMeshSourceSurface_t );
	nBytes += rSnapshot.m_lods.capacity() * sizeof( t3dRdMeshSourceLod_t );
	nBytes += rSnapshot.m_materials.capacity() * sizeof( t3dRdInstanceMaterial_t );
	nBytes += rSnapshot.m_textures.capacity() * sizeof( t3dRdImportedTextureSource_t );
	for ( size_t i = 0u; i < rSnapshot.m_textures.size(); i++ ) {
		nBytes += rSnapshot.m_textures[i].m_bytes.size();
	}
	return nBytes;
}

static size_t t3dRenderCoordinatorSceneSnapshotBytes( const t3dRdInstanceSceneSnapshot_t &rSnapshot ) {
	return rSnapshot.m_instances.capacity() * sizeof( t3dRdInstanceRecord_t )
	        + rSnapshot.m_cells.capacity() * sizeof( t3dRdInstanceCell_t );
}

size_t Terrain3DRenderCoordinator::getRetainedCpuBytes( void ) const {
	size_t bytes = sizeof( m_snapshotSlots ) + sizeof( m_szForwardViewportStates );
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_regionMapBanks ); iBank++ ) {
		const t3dRdRegionMapBank_t &bank = ( m_regionMapBanks )[iBank];
		bytes += bank.m_bytes.size();
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceMeshBanks ); iBank++ ) {
		const t3dRdInstanceMeshBank_t &bank = ( m_instanceMeshBanks )[iBank];
		bytes += t3dRenderCoordinatorMeshSnapshotBytes( bank.m_snapshot );
	}
	for ( size_t iBank = 0u; iBank < T3D_ARRAY_COUNT( m_instanceSceneBanks ); iBank++ ) {
		const t3dRdInstanceSceneBank_t &bank = ( m_instanceSceneBanks )[iBank];
		bytes += t3dRenderCoordinatorSceneSnapshotBytes( bank.m_snapshot );
	}
	for ( size_t iTopology = 0u; iTopology < T3D_ARRAY_COUNT( m_oceanTopologyBanks ); iTopology++ ) {
		const t3dRdOceanTopologyBank_t &bank = ( m_oceanTopologyBanks )[iTopology];
		bytes += sizeof( bank.m_snapshot );
	}
	bytes += t3dRenderCoordinatorMeshSnapshotBytes( m_cachedInstanceMeshes )
	        + t3dRenderCoordinatorMeshSnapshotBytes( m_cachedPreviewMeshes )
	        + t3dRenderCoordinatorMeshSnapshotBytes( m_pendingPreviewMeshes );
	bytes += t3dRenderCoordinatorSceneSnapshotBytes( m_cachedRegularInstanceScene )
	        + t3dRenderCoordinatorSceneSnapshotBytes( m_cachedHlodInstanceScene )
	        + t3dRenderCoordinatorSceneSnapshotBytes( m_cachedInstanceScene );
	bytes += m_cachedHlodMeshSources.capacity() * sizeof( t3dRdExternalMeshSource_t );
	bytes += m_instanceRenderer.getRetainedCpuBytes() + m_previewRenderer.getRetainedCpuBytes();
	bytes += ( size_t )( m_renderWorldRuntime.get_cpu_image_bytes() );
	return bytes;
}

Dictionary Terrain3DRenderCoordinator::getDiagnostics( void ) const {
	Terrain3DRdContext::forceSync();
	Dictionary result;
	const Dictionary renderer = m_renderer.getDiagnostics();
	const Dictionary instances = m_instanceRenderer.getDiagnostics();
	if ( m_pTerrain ) {
		result["configured_mesh_size"] = ( int64_t )( m_pTerrain->getMeshSize() );
		result["configured_mesh_lods"] = ( int64_t )( m_pTerrain->getMeshLods() );
		result["configured_tessellation_level"] = ( int64_t )( m_pTerrain->getTessellationLevel() );
		result["clipmap_target_position"] = m_pTerrain->getClipmapTargetPosition();
		Terrain3DMesher *const pMesher = m_pTerrain->getMesher();
		if ( pMesher ) {
			result["mesher_mesh_size"] = ( int64_t )( pMesher->getMeshSize() );
			result["mesher_tessellation_level"] = ( int64_t )( pMesher->getTessellationLevel() );
			t3dRdPatch_t patches[s_nMaxClipmapLevels]{};
			const int patch_count = pMesher->fillRdPatches( patches, s_nMaxClipmapLevels );
			result["cpu_clipmap_patch_count"] = ( int64_t )( patch_count );
			if ( patch_count > 0 ) {
				result["cpu_clipmap_lod0_origin"] = Vector2( patches[0].m_fOriginX, patches[0].m_fOriginZ );
				result["cpu_clipmap_lod0_scale"] = patches[0].m_fScale;
			}
		}
	}
	result["rd_retained_cpu_bytes"] = ( int64_t )( getRetainedCpuBytes() );
	result["rd_live_gpu_buffer_bytes"] = ( int64_t )( Terrain3DRdContext::getShared().getLiveGpuBufferBytes() );
	result["rd_live_gpu_texture_bytes"] = ( int64_t )( Terrain3DRdContext::getShared().getLiveGpuTextureBytes() );
	result["native_shadow_proxy_active"] = false;
	result["render_enabled"] = m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["forward_plus_bridge_attached"] = m_bForwardBridgeAttached;
	result["forward_plus_bridge_ready"] = isForwardBridgeReady();
	result["forward_pass_callbacks"] = ( int64_t )( m_nForwardPassCallbacks );
	result["depth_pass_callbacks"] = ( int64_t )( m_nDepthPassCallbacks );
	result["opaque_pass_callbacks"] = ( int64_t )( m_nOpaquePassCallbacks );
	result["shadow_pass_callbacks"] = ( int64_t )( m_nShadowPassCallbacks );
	result["motion_pass_callbacks"] = ( int64_t )( m_nMotionPassCallbacks );
	result["transparent_pass_callbacks"] = ( int64_t )( m_nTransparentPassCallbacks );
	result["last_forward_scene_pass"] = ( int64_t )( m_nLastForwardScenePass );
	result["last_forward_draw_list"] = m_nLastForwardDrawList;
	result["last_forward_pass_kind"] = ( int64_t )( m_nLastForwardPassKind );
	result["current_forward_view_count"] = ( int64_t )( m_nLastForwardViewCount );
	result["current_forward_sample_count"] = ( int64_t )( m_nLastForwardSampleCount );
	result["current_forward_framebuffer_format"] = m_nLastForwardFramebufferFormat;
	result["native_depth_prepass_participation"] = m_nDepthPassCallbacks > 0u;
	result["native_opaque_participation"] = m_nOpaquePassCallbacks > 0u;
	result["native_shadow_participation"] = m_nShadowPassCallbacks > 0u;
	result["native_motion_vectors_active"] = m_nMotionPassCallbacks > 0u;
	result["native_transparent_participation"] = m_nTransparentPassCallbacks > 0u;
	result.merge( m_oceanRenderer.getDiagnostics(), true );
	result["forward_viewport_reconfigurations"] = ( int64_t )( m_nForwardViewportReconfigurations );
	result["forward_viewport_evictions"] = ( int64_t )( m_nForwardViewportEvictions );
	result["forward_prepared_viewport_key"] = ( int64_t )( m_nForwardPreparedViewportKey );
	result["motion_debug_enabled"] = m_bMotionDebugEnabled;
	result["compositor_debug_lane"] = m_bCompositorDebugEnabled;
	Terrain3DRdForwardPlusBridgeAdapter *const pForwardBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	result["forward_plus_bridge"] = pForwardBridge ? pForwardBridge->getDiagnostics() : Dictionary();
	result["forward_plus_bridge_abi"] = pForwardBridge ? ( int64_t )( pForwardBridge->getObservedAbiVersion() ) : 0;
	result["forward_plus_callback_registered"] = pForwardBridge && pForwardBridge->isCallbackRegistered();
	result["render_world_initialized"] = m_renderWorldRuntime.is_initialized();
	result["render_world_scene_published"] = m_renderWorldRuntime.has_scene();
	result["render_world_hiz_enabled"] = m_renderWorldRuntime.is_hiz_enabled();
	result["render_world_cpu_image_bytes"] = ( int64_t )( m_renderWorldRuntime.get_cpu_image_bytes() );
	result["render_world_operation_events"] = ( int64_t )( m_renderWorldRuntime.get_operation_event_count() );
	result["renderer_initialized"] = m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["renderer_failure_latched"] = m_bFailureLatched;
	result["callbacks"] = ( int64_t )( m_nCallbackCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["consecutive_render_failures"] = ( int64_t )( m_nConsecutiveRenderFailures.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	const uint64_t snapshot_token = m_nSnapshotExchangeToken.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint32_t published_snapshot_index = m_nPublishedSnapshotIndex.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint64_t published_snapshot_generation = m_nPublishedSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	const uint64_t consumed_snapshot_generation = m_nConsumedSnapshotGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["snapshot_slots"] = ( int64_t )( s_nRdFrameSnapshotSlotCount );
	result["snapshot_exchange_lock_free"] = m_nSnapshotExchangeToken.is_lock_free();
	result["snapshot_exchange_index"] = ( int64_t )( t3dRdFrameSnapshotTokenIndex( snapshot_token ) );
	result["snapshot_exchange_published"] = t3dRdFrameSnapshotTokenIsPublished( snapshot_token );
	result["snapshot_exchange_generation"] = ( int64_t )( t3dRdFrameSnapshotTokenGeneration( snapshot_token ) );
	result["snapshot_published_index"] = ( int64_t )( published_snapshot_index );
	result["snapshot_published_generation"] = ( int64_t )( published_snapshot_generation );
	result["snapshot_consumed_generation"] = ( int64_t )( consumed_snapshot_generation );
	result["snapshot_generation_lag"] = ( int64_t )( consumed_snapshot_generation == 0u
	                ? ( published_snapshot_generation == 0u ? 0u : 1u )
	                : t3dRdFrameSnapshotGenerationDistance( published_snapshot_generation, consumed_snapshot_generation ) );
	result["snapshot_publications"] = ( int64_t )( m_nSnapshotPublications.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["snapshot_consumes"] = ( int64_t )( m_nSnapshotConsumes.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["snapshot_reuses"] = ( int64_t )( m_nSnapshotReuses.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["snapshot_skipped_generations"] = ( int64_t )( m_nSkippedSnapshotGenerations.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["stale_snapshots"] = ( int64_t )( m_nStaleSnapshots.load( T3D_MEMORY_ORDER_ACQUIRE ) );
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
		t3dScopedLock_t lock( m_diagnosticsMutex );
		result["initialization_error"] = m_initializationError;
	}
	result["native_shadow_proxy_available"] = false;
	result["multiview_supported"] = true;
	result["single_view_only"] = false;
	result["native_surface_enabled"] = false;
	result["color_write_enabled"] = true;
	result["hybrid_depth_only"] = false;
	const Dictionary forward_lighting = renderer.get( "forward_lighting", Dictionary() );
	result["clustered_light_path_active"] = forward_lighting.get( "forward_light_resources_ready", false );
	result["borrowed_scene_resource_changes"] = forward_lighting.get( "forward_scene_resource_changes", 0 );
	return result;
}
