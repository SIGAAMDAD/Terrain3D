// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_object_bytes.h"
#include "rendering/rd/material/terrain_3d_material_state_builder.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "core/utility/terrain_3d_generation.h"

#include "Terrain3DConstants.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "core/utility/logger.h"
#include "data/model/terrain_3d_data.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "rendering/material/terrain_3d_material.h"
#include "rendering/material/terrain_3d_shader_specializer.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"

#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <math.h>
#include <string.h>

static float t3dRdMaterialFloatParam(
	const Terrain3DMaterial *const pMaterial,
	const char *const			   pszName,
	const float					   fDefault ) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT ? ( float ) ( value ) : fDefault;
}

// The live authoring shader stores mipmap_bias as a derivative multiplier
// (1.0 == neutral), while the RD path consumes an additive log2 LOD offset
// (0.0 == neutral). Convert at this boundary so the two render paths share
// the same authored control without turning the authoring default into +1 mip.
static float t3dRdMaterialMipmapLodBias(
	const Terrain3DMaterial *const pMaterial ) {
	const float fDerivativeScale = MAX( t3dRdMaterialFloatParam( pMaterial, "mipmap_bias", 1.0f ), 0.0001f );
	return log2f( fDerivativeScale );
}

static uint32_t t3dRdMaterialUintParam(
	const Terrain3DMaterial *const pMaterial,
	const char *const			   pszName,
	const uint32_t				   nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::INT ? ( uint32_t ) ( MAX( ( int64_t ) ( value ), int64_t( 0 ) ) ) : nDefault;
}

static bool t3dRdMaterialBoolParam(
	const Terrain3DMaterial *const pMaterial,
	const char *const			   pszName,
	const bool					   bDefault ) {
	if ( !pMaterial ) {
		return bDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::BOOL ? ( bool ) ( value ) : bDefault;
}

static Color t3dRdMaterialColorParam(
	const Terrain3DMaterial *const pMaterial,
	const char *const			   pszName,
	const Color					  &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	if ( value.get_type() == Variant::COLOR ) {
		return ( Color ) ( value );
	}
	if ( value.get_type() == Variant::VECTOR3 ) {
		const Vector3 vector = value;
		return Color( vector.x, vector.y, vector.z, 1.0f );
	}
	return rDefault;
}

static Vector2 t3dRdMaterialVector2Param(
	const Terrain3DMaterial *const pMaterial,
	const char *const			   pszName,
	const Vector2				  &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR2 ? ( Vector2 ) ( value ) : rDefault;
}

static Vector3 t3dRdMaterialVector3Param(
	const Terrain3DMaterial *const pMaterial,
	const char *const			   pszName,
	const Vector3				  &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR3 ? ( Vector3 ) ( value ) : rDefault;
}

static uint32_t t3dRdSamplingMode(
	const t3dMaterialSamplingQuality_t quality ) {
	return quality == MATERIAL_SAMPLING_FAST ? ( uint32_t ) ( TERRAIN_SAMPLING_SINGLE_SAMPLE )
											 : ( uint32_t ) ( TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED );
}

static uint32_t t3dRdNormalMode(
	const t3dTerrainNormalQuality_t quality ) {
	return quality == TERRAIN_NORMAL_QUALITY_GEOMETRY ? ( uint32_t ) ( TERRAIN_NORMAL_GEOMETRY_DERIVATIVE )
													  : ( uint32_t ) ( TERRAIN_NORMAL_DERIVED_TEXTURE );
}

static uint32_t t3dRdColorMode(
	const t3dTerrainColorQuality_t quality ) {
	return quality == TERRAIN_COLOR_QUALITY_FAST ? ( uint32_t ) ( COLOR_SAMPLING_HARDWARE_FILTERED )
												 : ( uint32_t ) ( COLOR_SAMPLING_EXACT_4_TAP );
}

static uint32_t t3dRdDetilingMode(
	const t3dTierDetilingQuality_t quality ) {
	return quality == TIER_DETILING_DISABLED ? ( uint32_t ) ( DETILING_DISABLED )
											 : ( uint32_t ) ( DETILING_DISCRETE_8 );
}

static void t3dRdFillTierConfig(
	t3dRdTierMaterialConfig_t		  &rConfig,
	const t3dMaterialSamplingQuality_t sampling,
	const t3dTerrainNormalQuality_t	   normal,
	const t3dTerrainColorQuality_t	   color,
	const t3dSurfaceResponseQuality_t  surface,
	const t3dTextureSurfaceMode_t	   textureSurface,
	const t3dTierLightingModel_t	   lighting,
	const t3dTierDetilingQuality_t	   detiling,
	const t3dTextureFiltering_t		   filtering,
	const float						   fAuthoredColor,
	const float						   fWetness,
	const float						   fLodBias,
	const float						   fQualityLodOffset ) {
	rConfig.m_nSamplingMode = t3dRdSamplingMode( sampling );
	rConfig.m_nNormalMode = t3dRdNormalMode( normal );
	rConfig.m_nColorMode = t3dRdColorMode( color );
	rConfig.m_nSurfaceResponse = ( uint32_t ) ( surface );
	rConfig.m_nTextureSurfaceMode = ( uint32_t ) ( textureSurface );
	rConfig.m_nLightingModel = ( uint32_t ) ( lighting );
	rConfig.m_nDetilingMode = t3dRdDetilingMode( detiling );
	rConfig.m_nFiltering = ( uint32_t ) ( filtering );
	rConfig.m_fAuthoredColorContribution = CLAMP( fAuthoredColor, 0.0f, 1.0f );
	rConfig.m_fWetnessContribution = CLAMP( fWetness, 0.0f, 1.0f );
	rConfig.m_fMaterialLodBias = fLodBias;
	rConfig.m_fQualityLodOffset = fQualityLodOffset;
}

bool Terrain3DMaterialStateBuilder::ensureWorldNoiseCache(
	const Terrain3DMaterial *const pMaterial ) {
	if ( !pMaterial || pMaterial->getWorldBackground() != NOISE ) {
		return true;
	}
	const t3dWorldNoiseShaderMode_t mode = pMaterial->getWorldNoiseShaderMode();
	const bool bCacheEnabled = t3dRdMaterialBoolParam( pMaterial, "world_noise_cache_enabled", true );
	const bool bNeedsCache = mode == WORLD_NOISE_CACHED_VERTEX_NORMALS || mode == WORLD_NOISE_CACHED_FRAGMENT_NORMALS ||
							 ( mode == WORLD_NOISE_DYNAMIC && bCacheEnabled );
	if ( !bNeedsCache || m_worldNoiseCache.getRid().is_valid() ) {
		return true;
	}
	Ref< FastNoiseLite > noise;
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
	Ref< Image > image = noise->get_seamless_image( 1024, 1024, false, false, 0.1f, true );
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

bool Terrain3DMaterialStateBuilder::ensureMacroNoiseTexture(
	const Terrain3DMaterial *const pMaterial ) {
	if ( !pMaterial || !pMaterial->getMacroVariationEnabled() ) {
		return true;
	}
	const Variant source_value = pMaterial->getShaderParam( StringName( "noise_texture" ) );
	Ref< Image >  image;
	uint64_t	  source_id = 0u;
	if ( source_value.get_type() == Variant::OBJECT ) {
		const Ref< Texture2D > source_texture = source_value;
		if ( source_texture.is_valid() ) {
			source_id = ( uint64_t ) ( source_texture->get_instance_id() );
			if ( source_id == m_nMacroNoiseSourceId && m_macroNoiseTexture.getRid().is_valid() ) {
				return true;
			}
			const Ref< Image > source_image = source_texture->get_image();
			if ( source_image.is_valid() && !source_image->is_empty() ) {
				image = source_image->duplicate();
			}
		}
	}
	if ( image.is_null() || image->is_empty() ) {
		Ref< FastNoiseLite > noise;
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
		uint8_t *const	pPixels = pixels.ptrw();
		for ( int64_t i = 0; i < pixels.size(); ++i ) {
			const float input = ( float ) ( pPixels[i] ) * ( 1.0f / 255.0f );
			const float gradient = input <= 0.2f ? 1.0f : CLAMP( 1.0f - ( input - 0.2f ) * 1.25f, 0.0f, 1.0f );
			pPixels[i] = ( uint8_t ) ( gradient * 255.0f + 0.5f );
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

bool Terrain3DMaterialStateBuilder::ensureEditorBrushTexture(
	const Terrain3DMaterial *const pMaterial ) {
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
	const Ref< Texture2D > source_texture = source_value;
	if ( source_texture.is_null() ) {
		// The editor clears this parameter between tools. Releasing the texture here
		// invalidates immutable frame slots which may still be in flight.
		m_nEditorBrushSourceId = 0u;
		return true;
	}
	const uint64_t source_id = ( uint64_t ) ( source_texture->get_instance_id() );
	if ( source_id == m_nEditorBrushSourceId && m_editorBrushTexture.getRid().is_valid() ) {
		return true;
	}
	Ref< Image > image = source_texture->get_image();
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
	if ( image->get_width() != T3D_EDITOR_BRUSH_TEXTURE_SIZE || image->get_height() != T3D_EDITOR_BRUSH_TEXTURE_SIZE ) {
		image->resize( T3D_EDITOR_BRUSH_TEXTURE_SIZE, T3D_EDITOR_BRUSH_TEXTURE_SIZE, Image::INTERPOLATE_BILINEAR );
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

void Terrain3DMaterialStateBuilder::buildEditorOverlayState(
	const t3dMaterialStateSources_t &rSources,
	t3dRdFrameSnapshot_t			&rSnapshot,
	const Terrain3DMaterial *const	 pMaterial ) {
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
	const Array	  positions = positions_value.get_type() == Variant::ARRAY ? ( Array ) ( positions_value ) : Array();
	const Array	  rotations = rotations_value.get_type() == Variant::ARRAY ? ( Array ) ( rotations_value ) : Array();
	const Array	  sizes = sizes_value.get_type() == Variant::ARRAY ? ( Array ) ( sizes_value ) : Array();
	const Array	  colors = colors_value.get_type() == Variant::ARRAY ? ( Array ) ( colors_value ) : Array();
	const Array	  visible = visible_value.get_type() == Variant::ARRAY ? ( Array ) ( visible_value ) : Array();
	const Array	  parts = parts_value.get_type() == Variant::ARRAY ? ( Array ) ( parts_value ) : Array();
	const bool	  bBrushPart = parts.size() > 0 && ( bool ) ( parts[0] );
	const bool	  bReticlePart = parts.size() > 1 && ( bool ) ( parts[1] );
	for ( int i = 0; i < 3; ++i ) {
		t3dRdEditorDecal_t &decal = overlay.m_szDecals[i];
		if ( i < positions.size() && positions[i].get_type() == Variant::VECTOR2 ) {
			const Vector2 position = positions[i];
			decal.m_fPositionX = ( float ) ( position.x );
			decal.m_fPositionY = ( float ) ( position.y );
		}
		if ( i < rotations.size() &&
			 ( rotations[i].get_type() == Variant::FLOAT || rotations[i].get_type() == Variant::INT ) ) {
			decal.m_fRotation = ( float ) ( rotations[i] );
		}
		// The angle is constant for every pixel in this frame. Publish its
		// rotation basis in the two formerly-unused editor decal state lanes.
		const float fRotationCos = cosf( decal.m_fRotation );
		const float fRotationSin = sinf( decal.m_fRotation );
		memcpy( &decal.m_nRotationCosBits, &fRotationCos, sizeof( float ) );
		memcpy( &decal.m_nRotationSinBits, &fRotationSin, sizeof( float ) );
		if ( i < sizes.size() && ( sizes[i].get_type() == Variant::FLOAT || sizes[i].get_type() == Variant::INT ) ) {
			decal.m_fSize = MAX( ( float ) ( sizes[i] ), 0.0f );
		}
		if ( i < colors.size() && colors[i].get_type() == Variant::COLOR ) {
			const Color color = colors[i];
			decal.m_fColorR = ( float ) ( color.r );
			decal.m_fColorG = ( float ) ( color.g );
			decal.m_fColorB = ( float ) ( color.b );
			decal.m_fColorA = ( float ) ( color.a );
		}
		decal.m_nVisible =
			i < visible.size() && visible[i].get_type() == Variant::BOOL && ( bool ) ( visible[i] ) ? 1u : 0u;
		decal.m_nPartFlags =
			( bReticlePart ? 2u : 0u ) | ( i == 0 && bBrushPart && m_editorBrushTexture.getRid().is_valid() ? 1u : 0u );
	}
	overlay.m_fHeightmapBlackHeight = t3dRdMaterialFloatParam( pMaterial, "heightmap_black_height", -100.0f );
	overlay.m_fHeightmapWhiteHeight = t3dRdMaterialFloatParam( pMaterial, "heightmap_white_height", 300.0f );
	overlay.m_fContourInterval = MAX( t3dRdMaterialFloatParam( pMaterial, "contour_interval", 1.0f ), 0.25f );
	overlay.m_fContourThickness = MAX( t3dRdMaterialFloatParam( pMaterial, "contour_thickness", 1.0f ), 0.0f );
	const Color contour = t3dRdMaterialColorParam( pMaterial, "contour_color", Color( 0.85, 0.85, 0.19, 1.0 ) );
	overlay.m_fContourColorR = ( float ) ( contour.r );
	overlay.m_fContourColorG = ( float ) ( contour.g );
	overlay.m_fContourColorB = ( float ) ( contour.b );
	overlay.m_fContourColorA = ( float ) ( contour.a );
	const Terrain3DInstancer *const pInstancer = rSources.m_pTerrain ? rSources.m_pTerrain->instancerObject() : NULL;
	overlay.m_fInstancerCellSize =
		pInstancer ? ( float ) ( pInstancer->getCellSize() ) : ( float ) ( T3D_INSTANCE_DEFAULT_CELL_SIZE );
}

void Terrain3DMaterialStateBuilder::buildMaterialFlags(
	const t3dMaterialStateSources_t	  &rSources,
	const t3dVTMaterialReadSnapshot_t &rVtSnapshot,
	t3dRdFrameSnapshot_t			  &rSnapshot,
	Terrain3DMaterial *const		   pMaterial,
	Terrain3DData *const			   pData,
	const Ref< Terrain3DAssets >	  &rAssets ) {
	t3dRdMaterialConfig_t &config = rSnapshot.m_materialConfig;
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
	if ( t3dRdMaterialBoolParam( pMaterial, "flat_terrain_normals", false ) ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_FLAT_TERRAIN_NORMALS;
	}
	if ( m_macroNoiseTexture.getRid().is_valid() ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MACRO_NOISE;
	}
	if ( rVtSnapshot.nDatabaseKind == T3D_VT_RUNTIME_DATABASE_PACK_V1 ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MICRO_VT;
		if ( rSources.m_terrainMegaVT.isReady() ) {
			config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MEGAVT;
		}
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
	if ( rSnapshot.m_bDisplacementEnabled &&
		 pMaterial->getTessellationMaterialMode() == TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT &&
		 rSources.m_pTerrain->getComputeDisplacementTextureRid().is_valid() ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DISPLACEMENT;
	}
	if ( pData && pData->getDerivedMapsRid().is_valid() ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_DERIVED_MAPS;
	}
	if ( !Terrain3DRdShaderLibrary::isUnifiedCookedPreviewEnabledCold() &&
         pData && pData->getMiddleCompositeMapsRid().is_valid() ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MIDDLE_COMPOSITE;
	}
	if ( pData && pData->getMiddleHeightMapsRid().is_valid() && pData->getMiddleHeightRegionMapRid().is_valid() ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_MIDDLE_HEIGHT;
	}
	// FAR tint/index/height are V1 page allocations, not standalone atlas RIDs.
	if ( rVtSnapshot.nDatabaseKind == T3D_VT_RUNTIME_DATABASE_PACK_V1 ) {
		config.m_nFeatureFlags |= T3D_RD_MATERIAL_FEATURE_FAR_VT;
	}
	if ( rAssets.is_valid() && rAssets->getFarAlbedoArrayRid().is_valid() ) {
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
}

void Terrain3DMaterialStateBuilder::buildMaterialParameters(
	t3dRdMaterialConfig_t		 &config,
	Terrain3DMaterial *const	  pMaterial,
	const Ref< Terrain3DAssets > &rAssets ) {
	config.m_nBackgroundMode = ( uint32_t ) ( pMaterial->getWorldBackground() );
	config.m_nProjectionMode = ( uint32_t ) ( pMaterial->getProjectionMode() );
	config.m_nTessellationMaterialMode = ( uint32_t ) ( pMaterial->getTessellationMaterialMode() );
	config.m_nWorldNoiseMode = ( uint32_t ) ( pMaterial->getWorldNoiseShaderMode() );
	const uint32_t albedo_layers =
		rAssets.is_valid() ? ( uint32_t ) CLAMP( rAssets->getGeneratedArraySize(), 0, 255 ) : 0u;
	const uint32_t normal_layers =
		rAssets.is_valid() ? ( uint32_t ) CLAMP( rAssets->getNormalArraySize(), 0, 255 ) : 0u;
	const uint32_t surface_layers =
		rAssets.is_valid() ? ( uint32_t ) CLAMP( rAssets->getSurfaceArraySize(), 0, 255 ) : 0u;
	const uint32_t displacement_layers =
		rAssets.is_valid() ? ( uint32_t ) CLAMP( rAssets->getDisplacementArraySize(), 0, 255 ) : 0u;
	config.m_nTextureLayerCounts =
		albedo_layers | ( normal_layers << 8u ) | ( surface_layers << 16u ) | ( displacement_layers << 24u );
	config.m_nFarAlbedoLayerCount =
		rAssets.is_valid() ? ( uint32_t ) CLAMP( rAssets->getFarAlbedoArraySize(), 0, 255 ) : 0u;
	config.m_fDisplacementSharpness = ( float ) ( pMaterial->getDisplacementSharpness() );
	config.m_fBlendSharpness = t3dRdMaterialFloatParam( pMaterial, "blend_sharpness", 0.5f );
	config.m_fDistantNormalScale = t3dRdMaterialFloatParam( pMaterial, "distant_normal_scale", 2.0f );
	config.m_fGroundLevel = t3dRdMaterialFloatParam( pMaterial, "ground_level", 0.0f );
	config.m_fRegionBlend = t3dRdMaterialFloatParam( pMaterial, "region_blend", 0.25f );
	config.m_fBiasDistance = t3dRdMaterialFloatParam( pMaterial, "bias_distance", 512.0f );
	config.m_fMipmapBias = t3dRdMaterialMipmapLodBias( pMaterial );
	// The derivative multiplier is draw-invariant. reserved_2 belongs to the far atlas.
	config.m_fReserved3 = exp2f( config.m_fMipmapBias );
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
	config.m_fMacroVariation1R = ( float ) ( macro1.r );
	config.m_fMacroVariation1G = ( float ) ( macro1.g );
	config.m_fMacroVariation1B = ( float ) ( macro1.b );
	config.m_fMacroVariationSlope = t3dRdMaterialFloatParam( pMaterial, "macro_variation_slope", 0.333f );
	config.m_fMacroVariation2R = ( float ) ( macro2.r );
	config.m_fMacroVariation2G = ( float ) ( macro2.g );
	config.m_fMacroVariation2B = ( float ) ( macro2.b );
	const Vector2 noise_offset = t3dRdMaterialVector2Param( pMaterial, "noise1_offset", Vector2( 0.5, 0.5 ) );
	config.m_fNoise1OffsetX = ( float ) ( noise_offset.x );
	config.m_fNoise1OffsetY = ( float ) ( noise_offset.y );
	config.m_fWorldNoiseCachePeriod = t3dRdMaterialFloatParam( pMaterial, "world_noise_cache_period", 64.0f );
	config.m_fWorldNoiseLodDistance = t3dRdMaterialFloatParam( pMaterial, "world_noise_lod_distance", 7500.0f );
	config.m_fWorldNoiseScale = t3dRdMaterialFloatParam( pMaterial, "world_noise_scale", 5.0f );
	config.m_fWorldNoiseHeight = t3dRdMaterialFloatParam( pMaterial, "world_noise_height", 32.0f );
	config.m_nWorldNoiseMaxOctaves = t3dRdMaterialUintParam( pMaterial, "world_noise_max_octaves", 4u );
	config.m_nWorldNoiseMinOctaves = t3dRdMaterialUintParam( pMaterial, "world_noise_min_octaves", 2u );
	config.m_nWorldNoiseCacheEnabled = t3dRdMaterialBoolParam( pMaterial, "world_noise_cache_enabled", true ) ? 1u : 0u;
	config.m_nWorldNoiseFragmentNormals =
		t3dRdMaterialBoolParam( pMaterial, "world_noise_fragment_normals", false ) ? 1u : 0u;
	const Vector3 world_noise_offset = t3dRdMaterialVector3Param( pMaterial, "world_noise_offset", Vector3() );
	config.m_fWorldNoiseOffsetX = ( float ) ( world_noise_offset.x );
	config.m_fWorldNoiseOffsetY = ( float ) ( world_noise_offset.y );
	config.m_fWorldNoiseOffsetZ = ( float ) ( world_noise_offset.z );
	config.m_fWorldNoiseCacheMaxLod =
		m_worldNoiseCache.getRid().is_valid() ? ( float ) MAX( m_worldNoiseCache.maxMipmapLevel(), 0 ) : 0.0f;
	const Color silhouette = t3dRdMaterialColorParam( pMaterial, "silhouette_color", Color( 0.04, 0.04, 0.04, 1.0 ) );
	config.m_fSilhouetteR = ( float ) ( silhouette.r );
	config.m_fSilhouetteG = ( float ) ( silhouette.g );
	config.m_fSilhouetteB = ( float ) ( silhouette.b );
	config.m_fSilhouetteA = ( float ) ( silhouette.a );
	config.m_fContourStep = t3dRdMaterialFloatParam( pMaterial, "contour_interval", 1.0f );
}

void Terrain3DMaterialStateBuilder::buildMaterialTierConfig(
	t3dRdMaterialConfig_t	&config,
	Terrain3DMaterial *const pMaterial ) {
	t3dRdFillTierConfig( config.m_szTier[0],
						 pMaterial->getNearMaterialSamplingQuality(),
						 pMaterial->getNearTerrainNormalQuality(),
						 pMaterial->getNearTerrainColorQuality(),
						 pMaterial->getNearSurfaceResponseQuality(),
						 pMaterial->getNearTextureSurfaceMode(),
						 pMaterial->getNearLightingModel(),
						 pMaterial->getNearDetilingQuality(),
						 pMaterial->getNearTextureFiltering(),
						 ( float ) ( pMaterial->getNearAuthoredColorContribution() ),
						 ( float ) ( pMaterial->getNearWetnessContribution() ),
						 ( float ) ( pMaterial->getNearMaterialLodBias() ),
						 0.0f );

}

void Terrain3DMaterialStateBuilder::buildMaterialLayers(
	const t3dMaterialStateSources_t	  &rSources,
	const t3dVTMaterialReadSnapshot_t &rVtSnapshot,
	t3dRdFrameSnapshot_t			  &rSnapshot,
	const Ref< Terrain3DAssets >	  &rAssets ) {
	if ( rAssets.is_null() ) {
		rSnapshot.m_nMaterialLayerCount = 1u;
		rSnapshot.m_nTerrainMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1;
		return;
	}
	const PackedColorArray	 colors = rAssets->getTextureColors();
	const PackedFloat32Array normal_depths = rAssets->getTextureNormalDepths();
	const PackedFloat32Array ao_strengths = rAssets->getTextureAoStrengths();
	const PackedFloat32Array ao_affects = rAssets->getTextureAoLightAffects();
	const PackedFloat32Array roughness = rAssets->getTextureRoughnessMods();
	const PackedFloat32Array metallic = rAssets->getTextureMetallics();
	const PackedFloat32Array uv_scales = rAssets->getTextureUvScales();
	const PackedVector2Array detiles = rAssets->getTextureDetiles();
	const PackedVector2Array displacements = rAssets->getTextureDisplacements();
	const uint32_t			 count =
		( uint32_t ) ( CLAMP( rAssets->getTextureCount(), 1, ( int ) ( T3D_RD_MATERIAL_LAYER_COUNT ) ) );
	rSnapshot.m_nMaterialLayerCount = count;
	bool canonical_vt_v1 = ( rSnapshot.m_materialConfig.m_nFeatureFlags & T3D_RD_MATERIAL_FEATURE_MICRO_VT ) != 0u &&
						   rVtSnapshot.nDatabaseKind == T3D_VT_RUNTIME_DATABASE_PACK_V1;
	for ( uint32_t i = 0u; i < T3D_RD_MATERIAL_LAYER_COUNT; ++i ) {
		t3dRdMaterialLayer_t &layer = rSnapshot.m_szMaterials[i];
		const Color			  color =
			  i < ( uint32_t ) ( colors.size() ) ? colors[( int64_t ) ( i )] : Color( 1.0, 1.0, 1.0, 1.0 );
		layer.m_fColorR = ( float ) ( color.r );
		layer.m_fColorG = ( float ) ( color.g );
		layer.m_fColorB = ( float ) ( color.b );
		layer.m_fColorA = ( float ) ( color.a );
		layer.m_fUvScale = i < ( uint32_t ) ( uv_scales.size() ) ? uv_scales[( int64_t ) ( i )] : 0.1f;
		layer.m_fNormalDepth = i < ( uint32_t ) ( normal_depths.size() ) ? normal_depths[( int64_t ) ( i )] : 1.0f;
		layer.m_fRoughness = i < ( uint32_t ) ( roughness.size() ) ? roughness[( int64_t ) ( i )] : 0.0f;
		layer.m_fMetallic = i < ( uint32_t ) ( metallic.size() ) ? metallic[( int64_t ) ( i )] : 0.0f;
		layer.m_fAoStrength = i < ( uint32_t ) ( ao_strengths.size() ) ? ao_strengths[( int64_t ) ( i )] : 1.0f;
		layer.m_fAoLightAffect = i < ( uint32_t ) ( ao_affects.size() ) ? ao_affects[( int64_t ) ( i )] : 0.0f;
		const Vector2 detile = i < ( uint32_t ) ( detiles.size() ) ? detiles[( int64_t ) ( i )] : Vector2();
		layer.m_fDetileRotation = ( float ) ( detile.x );
		layer.m_fDetileShift = ( float ) ( detile.y );
		const Vector2 displacement =
			i < ( uint32_t ) ( displacements.size() ) ? displacements[( int64_t ) ( i )] : Vector2();
		layer.m_fDisplacementOffset = ( float ) ( displacement.x );
		layer.m_fDisplacementScale = ( float ) ( displacement.y );
		const bool legacy_array_resident =
			rAssets->getAlbedoArrayRid().is_valid() && i < ( uint32_t ) ( rAssets->getGeneratedArraySize() );
		layer.m_nPhysicalLayer = i;
		layer.m_nMicroVTAllocation =
			i < rVtSnapshot.nMaterialCount ? rVtSnapshot.nMaterialAllocation[i] : T3D_VT_INVALID_ALLOCATION;
		layer.m_nMicroVTGeneration = ( uint32_t ) ( rAssets->getTexturePublicationGeneration() & 0xffffffffu );
		layer.m_nMicroVTFlags = rVtSnapshot.nDatabaseKind == T3D_VT_RUNTIME_DATABASE_PACK_V1 ? 1u : 0u;
		layer.m_nMicroVTReserved = 0u;
		layer.m_nFlags = 0u;
		if ( rAssets->usesPackedPbr() ) {
			layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_PACKED_SURFACE;
		}
		if ( legacy_array_resident ) {
			layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_RESIDENT;
		}
		if ( layer.m_nMicroVTAllocation != T3D_VT_INVALID_ALLOCATION ) {
			layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_MICRO_VT;
		}
		const bool layer_canonical_v1 = i < count && layer.m_nMicroVTAllocation != T3D_VT_INVALID_ALLOCATION &&
										( layer.m_nMicroVTFlags & 1u ) != 0u;
		if ( layer_canonical_v1 ) {
			layer.m_nFlags |= T3D_RD_MATERIAL_LAYER_CANONICAL_VT_V1;
		}
		if ( i < count ) {
			canonical_vt_v1 = canonical_vt_v1 && layer_canonical_v1;
		}
	}
	// The unified shader uses cooked pages when source generations match and
    // retains the existing detailed evaluator for missing or unsupported pages.
    rSnapshot.m_nTerrainMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1;
}

void Terrain3DMaterialStateBuilder::buildMaterialState(
	const t3dMaterialStateSources_t	  &rSources,
	const t3dVTMaterialReadSnapshot_t &rVtSnapshot,
	t3dRdFrameSnapshot_t			  &rSnapshot ) {
	const Ref< Terrain3DAssets > assets =
		rSources.m_pTerrain ? rSources.m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
	const Ref< Terrain3DMaterial > material =
		rSources.m_pTerrain ? rSources.m_pTerrain->materialResource() : Ref< Terrain3DMaterial >();
	Terrain3DMaterial *const pMaterial = material.ptr();
	if ( pMaterial ) {
		Terrain3DData *const pData = rSources.m_pTerrain ? rSources.m_pTerrain->dataObject() : NULL;
		buildMaterialFlags( rSources, rVtSnapshot, rSnapshot, pMaterial, pData, assets );
		buildMaterialParameters( rSnapshot.m_materialConfig, pMaterial, assets );
		// No separate far-color slot dimension is published by V1.
		rSnapshot.m_materialConfig.m_fReserved2 = 0.0f;
		buildMaterialTierConfig( rSnapshot.m_materialConfig, pMaterial );
	}
	buildMaterialLayers( rSources, rVtSnapshot, rSnapshot, assets );

	rSnapshot.m_nMaterialGeneration = 0u;
}

bool Terrain3DMaterialStateBuilder::publishSnapshotState(
	const t3dMaterialStateSources_t &rSources,
	t3dRdFrameSnapshot_t			&snapshot ) {
	const Ref< Terrain3DMaterial > rd_material = rSources.m_pTerrain->materialResource();
	if ( !ensureWorldNoiseCache( rd_material.ptr() ) ) {
		LOG( ERROR, "Unable to publish Terrain3D direct RD world-noise cache" );
		return false;
	}
	if ( !ensureMacroNoiseTexture( rd_material.ptr() ) ) {
		LOG( ERROR, "Unable to publish Terrain3D direct RD macro-variation noise texture" );
		return false;
	}
	if ( !ensureEditorBrushTexture( rd_material.ptr() ) ) {
		LOG( ERROR, "Unable to publish Terrain3D direct RD editor brush texture" );
		return false;
	}
	const Ref< Terrain3DAssets > rd_assets = rSources.m_pTerrain->assetsResource();
	Terrain3DData *const		 data = rSources.m_pTerrain->dataObject();
	const RID					 derived_maps = data ? data->getDerivedMapsRid() : RID();
	const RID					 middle_composite_maps = data ? data->getMiddleCompositeMapsRid() : RID();
	const RID					 middle_height_maps = data ? data->getMiddleHeightMapsRid() : RID();
	const RID					 middle_height_region_map = data ? data->getMiddleHeightRegionMapRid() : RID();
	const RID					 far_albedo_map = rd_assets.is_valid() ? rd_assets->getFarAlbedoArrayRid() : RID();
	const RID					 displacement_map = rSources.m_pTerrain->getComputeDisplacementTextureRid();
	const RID					 world_noise_map = m_worldNoiseCache.getRid();
	const RID					 macro_noise_map = m_macroNoiseTexture.getRid();
	t3dVTMaterialReadSnapshot_t	 vtSnapshot{};
	if ( !rSources.m_virtualTexture.snapshotMaterialReadStateCold( vtSnapshot ) ) {
		LOG( ERROR, "Unable to snapshot Terrain3D VT material mapping state" );
		return false;
	}
	const uint64_t material_sources[] = {
		rd_material.is_valid() ? rd_material->getRdPublicationGeneration() : 0u,
		rd_assets.is_valid() ? rd_assets->getTexturePublicationGeneration() : 0u,
		data ? data->getActiveStorageGeneration() : 0u,
		data && data->areControlMapsImplicit() ? 1u : 0u,
		data && data->areColorMapsImplicit() ? 1u : 0u,
		rSources.m_pTerrain->isComputeDisplacementActive() ? 1u : 0u,
		( uint64_t ) ( derived_maps.get_id() ),
		( uint64_t ) ( middle_composite_maps.get_id() ),
		( uint64_t ) ( middle_height_maps.get_id() ),
		( uint64_t ) ( middle_height_region_map.get_id() ),
		( uint64_t ) ( far_albedo_map.get_id() ),
		( uint64_t ) ( displacement_map.get_id() ),
		( uint64_t ) ( world_noise_map.get_id() ),
		( uint64_t ) ( macro_noise_map.get_id() ),
		( uint64_t ) ( m_worldNoiseCache.maxMipmapLevel() + 1 ),
		rd_assets.is_valid() ? ( uint64_t ) ( rd_assets->getGeneratedArraySize() ) : 0u,
		rd_assets.is_valid() ? ( uint64_t ) ( rd_assets->getNormalArraySize() ) : 0u,
		rd_assets.is_valid() ? ( uint64_t ) ( rd_assets->getSurfaceArraySize() ) : 0u,
		rd_assets.is_valid() ? ( uint64_t ) ( rd_assets->getDisplacementArraySize() ) : 0u,
		rd_assets.is_valid() ? ( uint64_t ) ( rd_assets->getFarAlbedoArraySize() ) : 0u,
		vtSnapshot.nDatabaseKind == T3D_VT_RUNTIME_DATABASE_PACK_V1 ? 1u : 0u,
		( uint64_t ) vtSnapshot.nDatabaseGeneration,
		vtSnapshot.nPackageContentSignature,
		vtSnapshot.nPackagePublicationGeneration };
	const uint64_t material_source_generation =
		t3dHashFnv1a64( material_sources, sizeof( material_sources ), 1469598103934665603ULL );
	if ( !m_bMaterialStateInitialized || material_source_generation != m_nMaterialSourceGeneration ) {
		buildMaterialState( rSources, vtSnapshot, snapshot );
		memcpy( m_cachedMaterialLayers, snapshot.m_szMaterials, sizeof( m_cachedMaterialLayers ) );
		m_cachedMaterialConfig = snapshot.m_materialConfig;
		m_nCachedMaterialLayerCount = snapshot.m_nMaterialLayerCount;
		m_nMaterialGeneration = t3dNextGeneration( m_nMaterialGeneration );
		m_nMaterialSourceGeneration = material_source_generation;
		++m_nMaterialStateBuildCount;
		m_bMaterialStateInitialized = true;
	} else {
		memcpy( snapshot.m_szMaterials, m_cachedMaterialLayers, sizeof( m_cachedMaterialLayers ) );
		snapshot.m_materialConfig = m_cachedMaterialConfig;
		snapshot.m_nMaterialLayerCount = m_nCachedMaterialLayerCount;
	}
	// Frame slots are reset before every publication. The cached-material path
	// must restore the backend classification just like the rebuilt path.
	snapshot.m_nTerrainMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1;
	snapshot.m_nMaterialGeneration = m_nMaterialGeneration;
	buildEditorOverlayState( rSources, snapshot, rd_material.ptr() );
	return true;
}

void Terrain3DMaterialStateBuilder::resetPublicationState(
	void ) {
	m_nMaterialGeneration = 0u;
	m_nMaterialSourceGeneration = 0u;
	t3dClearTrivialArrayBytes( m_cachedMaterialLayers );
	m_cachedMaterialConfig = t3dRdMaterialConfig_t{};
	m_nCachedMaterialLayerCount = 1u;
	m_bMaterialStateInitialized = false;
	m_nMaterialStateBuildCount = 0u;
}

void Terrain3DMaterialStateBuilder::clear(
	void ) {
	m_worldNoiseCache.clear();
	m_macroNoiseTexture.clear();
	m_editorBrushTexture.clear();
	m_nMacroNoiseSourceId = 0u;
	m_nEditorBrushSourceId = 0u;
	resetPublicationState();
}
