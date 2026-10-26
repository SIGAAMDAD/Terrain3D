#include "assets/resources/terrain_3d_resource_helpers.h"
#include "assets/resources/terrain_3d_resource_helpers.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "integration/godot/terrain_3d_binding.h"
#include "assets/resources/terrain_3d_texture_asset.h"

#include "core/utility/logger.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_util.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>

// -----------------------------------------------------------------------------
// Private functions

// Note a null texture is considered a valid format
bool Terrain3DTextureAsset::isValidFormat(
	const Ref< Texture2D > &rTexture ) const {
	if ( rTexture.is_null() ) {
		LOG( DEBUG, "Provided texture is null." );
		return true;
	}
	const Ref< Image > pImg = rTexture->get_image();
	Image::Format	   format = Image::FORMAT_MAX;
	if ( pImg.is_valid() ) {
		format = pImg->get_format();
	}
	if ( format < 0 || format >= Image::FORMAT_MAX ) {
		LOG( ERROR, "Invalid texture format. See documentation for format specification." );
		return false;
	}
	return true;
}

void Terrain3DTextureAsset::disconnectPackedPbrResource(
	void ) {
	const Callable callback = callable_mp( this, &Terrain3DTextureAsset::packedPbrResourceChanged );
	t3dConnectResourceChanged( m_packedPbrResource, callback, false );
}

void Terrain3DTextureAsset::connectPackedPbrResource(
	void ) {
	const Callable callback = callable_mp( this, &Terrain3DTextureAsset::packedPbrResourceChanged );
	t3dConnectResourceChanged( m_packedPbrResource, callback, true );
}

void Terrain3DTextureAsset::packedPbrResourceChanged(
	void ) {
	if ( m_bPackedPbrChangeQueued ) {
		return;
	}
	m_bPackedPbrChangeQueued = true;
	call_deferred( "_emit_packed_pbr_file_changed" );
}

void Terrain3DTextureAsset::emitPackedPbrFileChanged(
	void ) {
	m_bPackedPbrChangeQueued = false;
	emit_signal( "file_changed" );
}

void Terrain3DTextureAsset::setPackedPbrMode(
	const bool bEnabled ) {
	if ( m_bPackedPbrMode == bEnabled ) {
		return;
	}
	m_bPackedPbrMode = bEnabled;
	notify_property_list_changed();
}

// -----------------------------------------------------------------------------
// Public functions

Ref< Terrain3DTextureAsset > Terrain3DTextureAsset::createRuntimeMaterialViewCold(
	void ) const {
	Ref< Terrain3DTextureAsset > result;
	result.instantiate();
	// Copy logical identity and all scalar material/erosion settings, but never
	// share authoring Texture2D, generated PBR, or thumbnail resources.
	result->m_name = m_name;
	result->m_nId = m_nId;
	result->m_bHighlighted = m_bHighlighted;
	result->m_bPlaceholder = m_bPlaceholder;
	result->m_albedoColor = m_albedoColor;
	result->m_fMetallic = m_fMetallic;
	result->m_fNormalDepth = m_fNormalDepth;
	result->m_fAoStrength = m_fAoStrength;
	result->m_fAoLightAffect = m_fAoLightAffect;
	result->m_fRoughness = m_fRoughness;
	result->m_fDisplacementScale = m_fDisplacementScale;
	result->m_fDisplacementOffset = m_fDisplacementOffset;
	result->m_fUvScale = m_fUvScale;
	result->m_fDetilingRotation = m_fDetilingRotation;
	result->m_fDetilingShift = m_fDetilingShift;
	result->m_bErosionEnabled = m_bErosionEnabled;
	result->m_fErosionHardness = m_fErosionHardness;
	result->m_fErosionHydraulicErodibility = m_fErosionHydraulicErodibility;
	result->m_fErosionWindErodibility = m_fErosionWindErodibility;
	result->m_fErosionWaveErodibility = m_fErosionWaveErodibility;
	result->m_fErosionWeatheringSusceptibility = m_fErosionWeatheringSusceptibility;
	result->m_fErosionOxidationSusceptibility = m_fErosionOxidationSusceptibility;
	result->m_fErosionSaltSusceptibility = m_fErosionSaltSusceptibility;
	result->m_fErosionWetDrySusceptibility = m_fErosionWetDrySusceptibility;
	result->m_fErosionInfiltrationMultiplier = m_fErosionInfiltrationMultiplier;
	result->m_fErosionTrafficErodibility = m_fErosionTrafficErodibility;
	result->m_fErosionCohesion = m_fErosionCohesion;
	result->m_fErosionGrainSize = m_fErosionGrainSize;
	result->m_nErosionExposedTextureId = m_nErosionExposedTextureId;
	result->m_nErosionWeatheredTextureId = m_nErosionWeatheredTextureId;
	result->m_nErosionOxidizedTextureId = m_nErosionOxidizedTextureId;
	result->m_nErosionCoarseDepositTextureId = m_nErosionCoarseDepositTextureId;
	result->m_nErosionFineDepositTextureId = m_nErosionFineDepositTextureId;
	result->m_nErosionSaltWeatheredTextureId = m_nErosionSaltWeatheredTextureId;
	result->m_nErosionPavementTextureId = m_nErosionPavementTextureId;
	result->m_nErosionVarnishedTextureId = m_nErosionVarnishedTextureId;
	result->m_highlightColor = m_highlightColor;
	result->m_bPackedPbrMode = m_bPackedPbrMode;
	return result;
}

void Terrain3DTextureAsset::initialize(
	void ) {
	connectPackedPbrResource();
	LOG( INFO, m_nId, ": ", m_name, ": initializing asset" );
}

void Terrain3DTextureAsset::clear(
	void ) {
	LOG( INFO, "Clearing TextureAsset" );
	m_name = "New Texture";
	m_nId = 0;
	m_bHighlighted = false;
	m_highlightColor = Color();
	m_albedoColor = Color( 1.0f, 1.0f, 1.0f, 1.0f );
	m_albedoTexture.unref();
	m_normalTexture.unref();
	disconnectPackedPbrResource();
	m_surfaceTexture.unref();
	m_displacementTexture.unref();
	m_pbrTexture.unref();
	m_packedPbrResource.unref();
	m_thumbnail.unref();

	m_fNormalDepth = 1.0f;
	m_fAoStrength = 1.0f;
	m_fAoLightAffect = 0.35f;
	m_fRoughness = 0.f;
	m_fMetallic = 0.f;
	m_fDisplacementScale = 0.f;
	m_fDisplacementOffset = 0.f;
	m_fUvScale = 0.1f;
	m_fDetilingRotation = 0.0f;
	m_fDetilingShift = 0.0f;

	m_bErosionEnabled = true;
	m_fErosionHardness = -1.0f;
	m_fErosionHydraulicErodibility = 1.0f;
	m_fErosionWindErodibility = 1.0f;
	m_fErosionWaveErodibility = 1.0f;
	m_fErosionWeatheringSusceptibility = 1.0f;
	m_fErosionOxidationSusceptibility = 1.0f;
	m_fErosionSaltSusceptibility = 1.0f;
	m_fErosionWetDrySusceptibility = 1.0f;
	m_fErosionInfiltrationMultiplier = 1.0f;
	m_fErosionTrafficErodibility = 1.0f;
	m_fErosionCohesion = 0.0f;
	m_fErosionGrainSize = 0.5f;
	m_nErosionExposedTextureId = -1;
	m_nErosionWeatheredTextureId = -1;
	m_nErosionOxidizedTextureId = -1;
	m_nErosionCoarseDepositTextureId = -1;
	m_nErosionFineDepositTextureId = -1;
	m_nErosionSaltWeatheredTextureId = -1;
	m_nErosionPavementTextureId = -1;
	m_nErosionVarnishedTextureId = -1;
}

void Terrain3DTextureAsset::setName(
	const String &rName ) {
	if ( rName.length() > 96 ) {
		LOG( WARN, "Name too long, truncating to 96 characters" );
	}
	SET_IF_DIFF( m_name, rName.left( 96 ) );
	LOG( INFO, "Setting name: ", m_name );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setId(
	const int nNewId ) {
	const int old_id = m_nId;
	SET_IF_DIFF( m_nId, CLAMP( nNewId, 0, T3D_MAX_TEXTURE_ASSETS - 1 ) );
	LOG( INFO, "Setting texture id: ", m_nId );
	LOG( DEBUG, "Emitting id_changed, TYPE_TEXTURE, ", old_id, ", ", m_nId );
	emit_signal( "id_changed", TYPE_TEXTURE, old_id, m_nId );
}

void Terrain3DTextureAsset::setHighlighted(
	const bool bHighlighted ) {
	SET_IF_DIFF( m_bHighlighted, bHighlighted );
	LOG( INFO, "Set mesh ID ", m_nId, " highlight: ", bHighlighted );
	const real_t fRandomFloat = ( real_t ) ( rand() ) / ( real_t ) ( RAND_MAX );
	m_highlightColor.set_hsv( fRandomFloat, 1.f, 1.f, 1.f );
	LOG( DEBUG, "Emitting preview_changed" );
	emit_signal( "preview_changed" );
}

void Terrain3DTextureAsset::setAlbedoColor(
	const Color &rColor ) {
	SET_IF_DIFF( m_albedoColor, rColor );
	LOG( INFO, "Setting color: ", rColor );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setAlbedoTexture(
	const Ref< Texture2D > &rTexture ) {
	if ( isValidFormat( rTexture ) ) {
		SET_IF_DIFF( m_albedoTexture, rTexture );
		LOG( INFO, "Setting albedo texture: ", rTexture );
		if ( rTexture.is_valid() ) {
			const String filename = rTexture->get_path().get_file().get_basename();
			if ( m_name == "New Texture" && !rTexture->get_path().contains( "::" ) && !filename.is_empty() ) {
				m_name = filename;
				LOG( INFO, "Setting name based on filename: ", m_name );
			}
			Ref< Image > pImg = rTexture->get_image();
			if ( !pImg->has_mipmaps() ) {
				LOG( WARN, "Albedo texture '", filename, "' has no mipmaps. Change on the Import panel if desired." );
			}
			if ( pImg->get_width() != pImg->get_height() ) {
				LOG( WARN, "Albedo texture '", filename, "' is not square. Mipmaps might have artifacts." );
			}
			if ( !Math::is_power_of_2( pImg->get_width() ) || !Math::is_power_of_2( pImg->get_height() ) ) {
				LOG( WARN, "Albedo texture '", filename, "' size is not power of 2. This is sub-optimal." );
			}
			if ( IS_EDITOR ) {
				pImg = pImg->duplicate();
				pImg->decompress();
				pImg->resize( 256, 256, Image::INTERPOLATE_LANCZOS );
				pImg->convert( Image::FORMAT_RGB8 );
				m_thumbnail = ImageTexture::create_from_image( pImg );
			}
		} else {
			m_thumbnail.unref();
		}
		LOG( DEBUG, "Emitting file_changed" );
		emit_signal( "file_changed" );
	}
}

void Terrain3DTextureAsset::setNormalTexture(
	const Ref< Texture2D > &rTexture ) {
	if ( isValidFormat( rTexture ) ) {
		SET_IF_DIFF( m_normalTexture, rTexture );
		LOG( INFO, "Setting normal texture: ", rTexture );
		if ( rTexture.is_valid() ) {
			const String	   filename = rTexture->get_path().get_file().get_basename();
			const Ref< Image > pImg = rTexture->get_image();
			if ( !pImg->has_mipmaps() ) {
				LOG( WARN, "Normal texture '", filename, "' has no mipmaps. Change on the Import panel if desired." );
			}
			if ( pImg->get_width() != pImg->get_height() ) {
				LOG( WARN,
					 "Normal texture '",
					 filename,
					 "' is not square. Not recommended. Mipmaps might have artifacts." );
			}
			if ( !Math::is_power_of_2( pImg->get_width() ) || !Math::is_power_of_2( pImg->get_height() ) ) {
				LOG( WARN, "Normal texture '", filename, "' dimensions are not power of 2. This is sub-optimal." );
			}
		}
		LOG( DEBUG, "Emitting file_changed" );
		emit_signal( "file_changed" );
	}
}

void Terrain3DTextureAsset::setSurfaceTexture(
	const Ref< Texture2D > &rTexture ) {
	if ( !isValidFormat( rTexture ) ) {
		return;
	}
	if ( rTexture.is_valid() ) {
		const Ref< Image > pImg = rTexture->get_image();
		if ( pImg.is_null() || !t3dIsPackedSurfaceFormat( pImg->get_format() ) ) {
			LOG( ERROR,
				 "Packed PBR textures must use RGBA8, BC3/DXT5, or BC7/BPTC_RGBA "
				 "(R=normal X, G=normal Y, B=roughness, A=AO)." );
			return;
		}
		const String filename = rTexture->get_path().get_file().get_basename();
		if ( !pImg->has_mipmaps() ) {
			LOG( WARN, "Packed PBR texture '", filename, "' has no mipmaps." );
		}
		if ( !Math::is_power_of_2( pImg->get_width() ) || !Math::is_power_of_2( pImg->get_height() ) ) {
			LOG( WARN, "Packed PBR texture '", filename, "' dimensions are not powers of two." );
		}
	}
	SET_IF_DIFF( m_surfaceTexture, rTexture );
	LOG( INFO, "Setting packed PBR texture: ", rTexture );
	emit_signal( "file_changed" );
}

void Terrain3DTextureAsset::setDisplacementTexture(
	const Ref< Texture2D > &rTexture ) {
	if ( !isValidFormat( rTexture ) ) {
		return;
	}
	Ref< Texture2D > pTexture = rTexture;
	if ( pTexture.is_valid() ) {
		Ref< Image > pImage = pTexture->get_image();
		if ( pImage.is_null() || pImage->is_empty() ) {
			LOG( ERROR, "Displacement texture image is missing or empty." );
			return;
		}
		if ( pImage->get_format() == Image::FORMAT_R16 ) {
			const Ref< Image > pMigrated = Terrain3DUtil::packDisplacementR8( pImage, 0, pImage->has_mipmaps() );
			if ( pMigrated.is_null() || pMigrated->is_empty() ) {
				LOG( ERROR, "Could not migrate legacy R16 displacement texture to R8." );
				return;
			}
			pTexture = ImageTexture::create_from_image( pMigrated );
			pImage = pMigrated;
			LOG( WARN,
				 "Migrated legacy R16 displacement texture to R8. Save the texture asset to persist the "
				 "migration." );
		}
		if ( pImage->get_format() != Image::FORMAT_R8 ) {
			LOG( ERROR, "Displacement textures must use Image::FORMAT_R8 (R8_UNORM)." );
			return;
		}
		const String filename = pTexture->get_path().get_file().get_basename();
		if ( !pImage->has_mipmaps() ) {
			LOG( WARN,
				 "Displacement texture '",
				 filename,
				 "' has no mipmaps. Tessellation LOD sampling remains valid but may alias at distance." );
		}
		if ( !Math::is_power_of_2( pImage->get_width() ) || !Math::is_power_of_2( pImage->get_height() ) ) {
			LOG( WARN, "Displacement texture '", filename, "' dimensions are not powers of two." );
		}
	}
	SET_IF_DIFF( m_displacementTexture, pTexture );
	LOG( INFO, "Setting R8 displacement texture: ", pTexture );
	emit_signal( "file_changed" );
}

void Terrain3DTextureAsset::setPbrTexture(
	const Ref< Texture2D > &rTexture ) {
	// Compatibility migration for resources saved by the former RGBA16UI path.
	m_pbrTexture = rTexture;
	if ( rTexture.is_null() ) {
		emit_signal( "file_changed" );
		return;
	}
	const Ref< Image > pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		LOG( ERROR, "Compatibility PBR texture image is invalid or empty." );
		return;
	}
	if ( t3dIsPackedSurfaceFormat( pImage->get_format() ) ) {
		m_surfaceTexture = rTexture;
		m_pbrTexture.unref();
	} else if ( pImage->get_format() == Image::FORMAT_RGBA16I ) {
		const Ref< Image > pSurface = Terrain3DUtil::convertPackedPbrRgba16uiToSurface( pImage, true );
		const Ref< Image > pDisplacement = Terrain3DUtil::convertPackedPbrRgba16uiToDisplacement( pImage, true );
		if ( pSurface.is_null() || pDisplacement.is_null() ) {
			LOG( ERROR, "Could not migrate RGBA16UI packed PBR texture." );
			return;
		}
		m_surfaceTexture = ImageTexture::create_from_image( pSurface );
		m_displacementTexture = ImageTexture::create_from_image( pDisplacement );
		m_pbrTexture.unref();
		LOG( WARN,
			 "Migrated legacy RGBA16UI PBR texture to RGBA8 packed PBR + R8 displacement. Save the texture asset to "
			 "persist the migration." );
	} else {
		LOG( ERROR, "Compatibility pbr_texture accepts old RGBA16UI data or an RGBA8/BC3/BC7 packed PBR texture." );
		return;
	}
	emit_signal( "file_changed" );
}

void Terrain3DTextureAsset::setPackedPbrResource(
	const Ref< Terrain3DPackedPBRTexture > &rResource ) {
	if ( m_packedPbrResource == rResource ) {
		return;
	}
	disconnectPackedPbrResource();
	m_packedPbrResource = rResource;
	connectPackedPbrResource();
	notify_property_list_changed();
	LOG( INFO, "Setting legacy packed-PBR compatibility resource: ", rResource );
	emit_signal( "file_changed" );
}

Ref< Texture2D > Terrain3DTextureAsset::getEffectiveSurfaceTexture(
	void ) {
	// Direct externally authored maps are the active workflow. The hidden
	// compatibility resource is consulted only when a direct map is absent.
	if ( m_surfaceTexture.is_valid() ) {
		return m_surfaceTexture;
	}
	return m_packedPbrResource.is_valid() ? m_packedPbrResource->getSurfaceTexture() : Ref< Texture2D >();
}

Ref< Texture2D > Terrain3DTextureAsset::getEffectiveDisplacementTexture(
	void ) {
	if ( m_displacementTexture.is_valid() ) {
		return m_displacementTexture;
	}
	return m_packedPbrResource.is_valid() ? m_packedPbrResource->getDisplacementTexture() : Ref< Texture2D >();
}

void Terrain3DTextureAsset::setMetallic(
	const real_t fMetallic ) {
	SET_IF_DIFF( m_fMetallic, CLAMP( fMetallic, 0.0f, 1.0f ) );
	LOG( INFO, "Setting metallic: ", m_fMetallic );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setNormalDepth(
	const real_t fNormalDepth ) {
	SET_IF_DIFF( m_fNormalDepth, CLAMP( fNormalDepth, 0.0f, 2.0f ) );
	LOG( INFO, "Setting normal_depth: ", m_fNormalDepth );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setRoughness(
	const real_t fRoughness ) {
	SET_IF_DIFF( m_fRoughness, CLAMP( fRoughness, -1.0f, 1.0f ) );
	LOG( INFO, "Setting roughness modifier: ", m_fRoughness );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setAoStrength(
	const real_t fAoStrength ) {
	SET_IF_DIFF( m_fAoStrength, CLAMP( fAoStrength, 0.0f, 1.0f ) );
	LOG( INFO, "Setting ao_strength: ", m_fAoStrength );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setAoLightAffect(
	const real_t fAoLightAffect ) {
	SET_IF_DIFF( m_fAoLightAffect, CLAMP( fAoLightAffect, 0.0f, 1.0f ) );
	LOG( INFO, "Setting ao_light_affect: ", m_fAoLightAffect );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setDisplacementScale(
	const real_t fDisplacementScale ) {
	SET_IF_DIFF( m_fDisplacementScale, CLAMP( fDisplacementScale, 0.0f, 2.0f ) );
	LOG( INFO, "Setting displacement_scale: ", m_fDisplacementScale );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setDisplacementOffset(
	const real_t fDisplacementOffset ) {
	SET_IF_DIFF( m_fDisplacementOffset, CLAMP( fDisplacementOffset, -1.0f, 1.0f ) );
	LOG( INFO, "Setting displacement_offset: ", m_fDisplacementOffset );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setUvScale(
	const real_t fScale ) {
	SET_IF_DIFF( m_fUvScale, CLAMP( fScale, 0.001f, 100.0f ) );
	LOG( INFO, "Setting uv_scale: ", m_fUvScale );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setDetilingRotation(
	const real_t fDetilingRotation ) {
	SET_IF_DIFF( m_fDetilingRotation, CLAMP( fDetilingRotation, 0.0f, 1.0f ) );
	LOG( INFO, "Setting detiling_rotation: ", m_fDetilingRotation );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

void Terrain3DTextureAsset::setDetilingShift(
	const real_t fDetilingShift ) {
	SET_IF_DIFF( m_fDetilingShift, CLAMP( fDetilingShift, 0.0f, 1.0f ) );
	LOG( INFO, "Setting detiling_shift: ", m_fDetilingShift );
	LOG( DEBUG, "Emitting setting_changed" );
	emit_signal( "setting_changed" );
}

#define SET_EROSION_VALUE( methodName, field_name, min_value, max_value )                                              \
	void Terrain3DTextureAsset::methodName( real_t fValueArg ) {                                                       \
		real_t fValue = CLAMP( fValueArg, ( real_t ) ( min_value ), ( real_t ) ( max_value ) );                        \
		if ( field_name == fValue ) {                                                                                  \
			return;                                                                                                    \
		}                                                                                                              \
		field_name = fValue;                                                                                           \
		emit_signal( "setting_changed" );                                                                              \
	}

#define SET_EROSION_TEXTURE_ID( methodName, field_name )                                                               \
	void Terrain3DTextureAsset::methodName( int32_t valueArg ) {                                                       \
		int32_t value = CLAMP( valueArg, -1, T3D_MAX_TEXTURE_ASSETS - 1 );                                             \
		if ( field_name == value ) {                                                                                   \
			return;                                                                                                    \
		}                                                                                                              \
		field_name = value;                                                                                            \
		emit_signal( "setting_changed" );                                                                              \
	}

void Terrain3DTextureAsset::setErosionEnabled(
	const bool bValue ) {
	if ( m_bErosionEnabled == bValue ) {
		return;
	}
	m_bErosionEnabled = bValue;
	emit_signal( "setting_changed" );
	notify_property_list_changed();
}

SET_EROSION_VALUE(
	setErosionHardness,
	m_fErosionHardness,
	-1.0,
	1.0 )
SET_EROSION_VALUE(
	setErosionHydraulicErodibility,
	m_fErosionHydraulicErodibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionWindErodibility,
	m_fErosionWindErodibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionWaveErodibility,
	m_fErosionWaveErodibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionWeatheringSusceptibility,
	m_fErosionWeatheringSusceptibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionOxidationSusceptibility,
	m_fErosionOxidationSusceptibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionSaltSusceptibility,
	m_fErosionSaltSusceptibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionWetDrySusceptibility,
	m_fErosionWetDrySusceptibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionInfiltrationMultiplier,
	m_fErosionInfiltrationMultiplier,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionTrafficErodibility,
	m_fErosionTrafficErodibility,
	0.0,
	8.0 )
SET_EROSION_VALUE(
	setErosionCohesion,
	m_fErosionCohesion,
	0.0,
	1.0 )
SET_EROSION_VALUE(
	setErosionGrainSize,
	m_fErosionGrainSize,
	0.0,
	1.0 )
SET_EROSION_TEXTURE_ID(
	setErosionExposedTextureId,
	m_nErosionExposedTextureId )
SET_EROSION_TEXTURE_ID(
	setErosionWeatheredTextureId,
	m_nErosionWeatheredTextureId )
SET_EROSION_TEXTURE_ID(
	setErosionOxidizedTextureId,
	m_nErosionOxidizedTextureId )
SET_EROSION_TEXTURE_ID(
	setErosionCoarseDepositTextureId,
	m_nErosionCoarseDepositTextureId )
SET_EROSION_TEXTURE_ID(
	setErosionFineDepositTextureId,
	m_nErosionFineDepositTextureId )
SET_EROSION_TEXTURE_ID(
	setErosionSaltWeatheredTextureId,
	m_nErosionSaltWeatheredTextureId )
SET_EROSION_TEXTURE_ID(
	setErosionPavementTextureId,
	m_nErosionPavementTextureId )
SET_EROSION_TEXTURE_ID(
	setErosionVarnishedTextureId,
	m_nErosionVarnishedTextureId )

#undef SET_EROSION_TEXTURE_ID
#undef SET_EROSION_VALUE

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DTextureAsset::_validate_property(
	PropertyInfo &rProperty ) const {
	t3dApplyPropertyPrefixUsage( rProperty, "erosion_", "erosion_enabled", !m_bErosionEnabled );
	t3dApplyPropertyUsageRules( rProperty,
								{
									{ "pbr_texture", true, PROPERTY_USAGE_STORAGE },
									{ "surface_texture", true, PROPERTY_USAGE_STORAGE },
									{ "packed_pbr_resource", true, PROPERTY_USAGE_STORAGE },
									{ "normal_texture", m_bPackedPbrMode, PROPERTY_USAGE_STORAGE },
									{ "packed_pbr_texture", !m_bPackedPbrMode, PROPERTY_USAGE_STORAGE },
									{ "displacement_texture", !m_bPackedPbrMode, PROPERTY_USAGE_STORAGE },
								} );
}

void Terrain3DTextureAsset::_bind_methods(
	void ) {
	ADD_SIGNAL( MethodInfo( "id_changed" ) );
	ADD_SIGNAL( MethodInfo( "file_changed" ) );
	ADD_SIGNAL( MethodInfo( "setting_changed" ) );
	ADD_SIGNAL( MethodInfo( "preview_changed" ) );

	ClassDB::bind_method( D_METHOD( "clear" ), &Terrain3DTextureAsset::clear );

	ClassDB::bind_method( D_METHOD( "set_highlighted", "enabled" ), &Terrain3DTextureAsset::setHighlighted );
	ClassDB::bind_method( D_METHOD( "is_highlighted" ), &Terrain3DTextureAsset::isHighlighted );
	ClassDB::bind_method( D_METHOD( "get_highlight_color" ), &Terrain3DTextureAsset::getHighlightColor );
	ClassDB::bind_method( D_METHOD( "get_thumbnail" ), &Terrain3DTextureAsset::getThumbnail );

	ClassDB::bind_method( D_METHOD( "get_effective_surface_texture" ),
						  &Terrain3DTextureAsset::getEffectiveSurfaceTexture );
	ClassDB::bind_method( D_METHOD( "get_effective_displacement_texture" ),
						  &Terrain3DTextureAsset::getEffectiveDisplacementTexture );
	ClassDB::bind_method( D_METHOD( "_emit_packed_pbr_file_changed" ),
						  &Terrain3DTextureAsset::emitPackedPbrFileChanged );

	ADD_GROUP( "General", "" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setName,
					   getName,
					   "name",
					   PropertyInfo( Variant::STRING, "name", PROPERTY_HINT_NONE ),
					   "set_asset_name",
					   "get_asset_name" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setId,
					   getId,
					   "id",
					   PropertyInfo( Variant::INT, "id", PROPERTY_HINT_NONE ),
					   "set_id",
					   "get_id" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setAlbedoColor,
					   getAlbedoColor,
					   "color",
					   PropertyInfo( Variant::COLOR, "albedo_color", PROPERTY_HINT_COLOR_NO_ALPHA ),
					   "set_albedo_color",
					   "get_albedo_color" );

	ADD_GROUP( "Runtime Texture Sources", "" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setAlbedoTexture,
					   getAlbedoTexture,
					   "texture",
					   PropertyInfo( Variant::OBJECT,
									 "albedo_texture",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "ImageTexture,CompressedTexture2D" ),
					   "set_albedo_texture",
					   "get_albedo_texture" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setNormalTexture,
					   getNormalTexture,
					   "texture",
					   PropertyInfo( Variant::OBJECT,
									 "normal_texture",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "ImageTexture,CompressedTexture2D" ),
					   "set_normal_texture",
					   "get_normal_texture" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setPackedPbrTexture,
					   getPackedPbrTexture,
					   "texture",
					   PropertyInfo( Variant::OBJECT,
									 "packed_pbr_texture",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "ImageTexture,CompressedTexture2D" ),
					   "set_packed_pbr_texture",
					   "get_packed_pbr_texture" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setDisplacementTexture,
					   getDisplacementTexture,
					   "texture",
					   PropertyInfo( Variant::OBJECT,
									 "displacement_texture",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "ImageTexture,CompressedTexture2D" ),
					   "set_displacement_texture",
					   "get_displacement_texture" );

	ADD_GROUP( "Surface Response", "" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setNormalDepth,
					   getNormalDepth,
					   "normal_depth",
					   PropertyInfo( Variant::FLOAT, "normal_depth", PROPERTY_HINT_RANGE, "0.0, 2.0" ),
					   "set_normal_depth",
					   "get_normal_depth" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setAoStrength,
					   getAoStrength,
					   "ao_strength",
					   PropertyInfo( Variant::FLOAT, "ao_strength", PROPERTY_HINT_RANGE, "0.0, 1.0" ),
					   "set_ao_strength",
					   "get_ao_strength" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setAoLightAffect,
					   getAoLightAffect,
					   "ao_light_affect",
					   PropertyInfo( Variant::FLOAT, "ao_light_affect", PROPERTY_HINT_RANGE, "0.0, 1.0" ),
					   "set_ao_light_affect",
					   "get_ao_light_affect" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setRoughness,
					   getRoughness,
					   "roughness",
					   PropertyInfo( Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "-1.0, 1.0" ),
					   "set_roughness",
					   "get_roughness" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setMetallic,
					   getMetallic,
					   "metallic",
					   PropertyInfo( Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0.0, 1.0, 0.01" ),
					   "set_metallic",
					   "get_metallic" );

	ADD_GROUP( "UV / Scale / Displacement", "" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setUvScale,
					   getUvScale,
					   "scale",
					   PropertyInfo( Variant::FLOAT, "uv_scale", PROPERTY_HINT_RANGE, "0.001, 2.0, or_greater" ),
					   "set_uv_scale",
					   "get_uv_scale" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setDisplacementScale,
					   getDisplacementScale,
					   "displacement_scale",
					   PropertyInfo( Variant::FLOAT, "displacement_scale", PROPERTY_HINT_RANGE, "0.0, 2.0" ),
					   "set_displacement_scale",
					   "get_displacement_scale" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setDisplacementOffset,
					   getDisplacementOffset,
					   "displacement_offset",
					   PropertyInfo( Variant::FLOAT, "displacement_offset", PROPERTY_HINT_RANGE, "-1.0, 1.0" ),
					   "set_displacement_offset",
					   "get_displacement_offset" );

	ADD_GROUP( "Erosion Response", "" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionEnabled,
					   getErosionEnabled,
					   "value",
					   PropertyInfo( Variant::BOOL, "erosion_enabled" ),
					   "set_erosion_enabled",
					   "get_erosion_enabled" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionHardness,
					   getErosionHardness,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_hardness", PROPERTY_HINT_RANGE, "-1,1,0.01" ),
					   "set_erosion_hardness",
					   "get_erosion_hardness" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionHydraulicErodibility,
					   getErosionHydraulicErodibility,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_hydraulic_erodibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
					   "set_erosion_hydraulic_erodibility",
					   "get_erosion_hydraulic_erodibility" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionWindErodibility,
					   getErosionWindErodibility,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_wind_erodibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
					   "set_erosion_wind_erodibility",
					   "get_erosion_wind_erodibility" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionWaveErodibility,
					   getErosionWaveErodibility,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_wave_erodibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
					   "set_erosion_wave_erodibility",
					   "get_erosion_wave_erodibility" );
	T3D_BIND_PROPERTY(
		Terrain3DTextureAsset,
		setErosionWeatheringSusceptibility,
		getErosionWeatheringSusceptibility,
		"value",
		PropertyInfo( Variant::FLOAT, "erosion_weathering_susceptibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
		"set_erosion_weathering_susceptibility",
		"get_erosion_weathering_susceptibility" );
	T3D_BIND_PROPERTY(
		Terrain3DTextureAsset,
		setErosionOxidationSusceptibility,
		getErosionOxidationSusceptibility,
		"value",
		PropertyInfo( Variant::FLOAT, "erosion_oxidation_susceptibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
		"set_erosion_oxidation_susceptibility",
		"get_erosion_oxidation_susceptibility" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionSaltSusceptibility,
					   getErosionSaltSusceptibility,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_salt_susceptibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
					   "set_erosion_salt_susceptibility",
					   "get_erosion_salt_susceptibility" );
	T3D_BIND_PROPERTY(
		Terrain3DTextureAsset,
		setErosionWetDrySusceptibility,
		getErosionWetDrySusceptibility,
		"value",
		PropertyInfo( Variant::FLOAT, "erosion_wet_dry_susceptibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
		"set_erosion_wet_dry_susceptibility",
		"get_erosion_wet_dry_susceptibility" );
	T3D_BIND_PROPERTY(
		Terrain3DTextureAsset,
		setErosionInfiltrationMultiplier,
		getErosionInfiltrationMultiplier,
		"value",
		PropertyInfo( Variant::FLOAT, "erosion_infiltration_multiplier", PROPERTY_HINT_RANGE, "0,8,0.01" ),
		"set_erosion_infiltration_multiplier",
		"get_erosion_infiltration_multiplier" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionTrafficErodibility,
					   getErosionTrafficErodibility,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_traffic_erodibility", PROPERTY_HINT_RANGE, "0,8,0.01" ),
					   "set_erosion_traffic_erodibility",
					   "get_erosion_traffic_erodibility" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionCohesion,
					   getErosionCohesion,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_cohesion", PROPERTY_HINT_RANGE, "0,1,0.01" ),
					   "set_erosion_cohesion",
					   "get_erosion_cohesion" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionGrainSize,
					   getErosionGrainSize,
					   "value",
					   PropertyInfo( Variant::FLOAT, "erosion_grain_size", PROPERTY_HINT_RANGE, "0,1,0.01" ),
					   "set_erosion_grain_size",
					   "get_erosion_grain_size" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionExposedTextureId,
					   getErosionExposedTextureId,
					   "value",
					   PropertyInfo( Variant::INT, "erosion_exposed_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
					   "set_erosion_exposed_texture_id",
					   "get_erosion_exposed_texture_id" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionWeatheredTextureId,
					   getErosionWeatheredTextureId,
					   "value",
					   PropertyInfo( Variant::INT, "erosion_weathered_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
					   "set_erosion_weathered_texture_id",
					   "get_erosion_weathered_texture_id" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionOxidizedTextureId,
					   getErosionOxidizedTextureId,
					   "value",
					   PropertyInfo( Variant::INT, "erosion_oxidized_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
					   "set_erosion_oxidized_texture_id",
					   "get_erosion_oxidized_texture_id" );
	T3D_BIND_PROPERTY(
		Terrain3DTextureAsset,
		setErosionCoarseDepositTextureId,
		getErosionCoarseDepositTextureId,
		"value",
		PropertyInfo( Variant::INT, "erosion_coarse_deposit_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
		"set_erosion_coarse_deposit_texture_id",
		"get_erosion_coarse_deposit_texture_id" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionFineDepositTextureId,
					   getErosionFineDepositTextureId,
					   "value",
					   PropertyInfo( Variant::INT, "erosion_fine_deposit_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
					   "set_erosion_fine_deposit_texture_id",
					   "get_erosion_fine_deposit_texture_id" );
	T3D_BIND_PROPERTY(
		Terrain3DTextureAsset,
		setErosionSaltWeatheredTextureId,
		getErosionSaltWeatheredTextureId,
		"value",
		PropertyInfo( Variant::INT, "erosion_salt_weathered_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
		"set_erosion_salt_weathered_texture_id",
		"get_erosion_salt_weathered_texture_id" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionPavementTextureId,
					   getErosionPavementTextureId,
					   "value",
					   PropertyInfo( Variant::INT, "erosion_pavement_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
					   "set_erosion_pavement_texture_id",
					   "get_erosion_pavement_texture_id" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setErosionVarnishedTextureId,
					   getErosionVarnishedTextureId,
					   "value",
					   PropertyInfo( Variant::INT, "erosion_varnished_texture_id", PROPERTY_HINT_RANGE, "-1,31,1" ),
					   "set_erosion_varnished_texture_id",
					   "get_erosion_varnished_texture_id" );

	ADD_GROUP( "Detiling", "" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setDetilingRotation,
					   getDetilingRotation,
					   "detiling_rotation",
					   PropertyInfo( Variant::FLOAT, "detiling_rotation", PROPERTY_HINT_RANGE, "0.0, 1.0" ),
					   "set_detiling_rotation",
					   "get_detiling_rotation" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setDetilingShift,
					   getDetilingShift,
					   "detiling_shift",
					   PropertyInfo( Variant::FLOAT, "detiling_shift", PROPERTY_HINT_RANGE, "0.0, 1.0" ),
					   "set_detiling_shift",
					   "get_detiling_shift" );

	ADD_GROUP( "Compatibility", "" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setPackedPbrResource,
					   getPackedPbrResource,
					   "resource",
					   PropertyInfo( Variant::OBJECT,
									 "packed_pbr_resource",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "Terrain3DPackedPBRTexture",
									 PROPERTY_USAGE_STORAGE ),
					   "set_packed_pbr_resource",
					   "get_packed_pbr_resource" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setSurfaceTexture,
					   getSurfaceTexture,
					   "texture",
					   PropertyInfo( Variant::OBJECT,
									 "surface_texture",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "ImageTexture,CompressedTexture2D",
									 PROPERTY_USAGE_STORAGE ),
					   "set_surface_texture",
					   "get_surface_texture" );
	T3D_BIND_PROPERTY( Terrain3DTextureAsset,
					   setPbrTexture,
					   getPbrTexture,
					   "texture",
					   PropertyInfo( Variant::OBJECT,
									 "pbr_texture",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "ImageTexture",
									 PROPERTY_USAGE_STORAGE ),
					   "set_pbr_texture",
					   "get_pbr_texture" );
}
