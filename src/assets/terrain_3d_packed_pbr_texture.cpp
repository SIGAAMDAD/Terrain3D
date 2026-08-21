// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "assets/terrain_3d_packed_pbr_texture.h"

#include "core/logger.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_util.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/class_db.hpp>

EA_FORCE_INLINE static bool t3dPackedPbrTextureIsPackedSurfaceFormat( const Image::Format format ) {
	return format == Image::FORMAT_RGBA8 || format == Image::FORMAT_DXT5 || format == Image::FORMAT_BPTC_RGBA;
}

void T3DPackedPBRTexture::disconnectSource( const Ref<Texture2D> &rSource ) {
	if ( rSource.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &T3DPackedPBRTexture::sourceChanged );
	if ( rSource->is_connected( "changed", callback ) ) {
		rSource->disconnect( "changed", callback );
	}
}

void T3DPackedPBRTexture::connectSource( const Ref<Texture2D> &rSource ) {
	if ( rSource.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &T3DPackedPBRTexture::sourceChanged );
	if ( !rSource->is_connected( "changed", callback ) ) {
		rSource->connect( "changed", callback );
	}
}

void T3DPackedPBRTexture::setSource( Ref<Texture2D> &iSlot, const Ref<Texture2D> &rSource ) {
	if ( iSlot == rSource ) {
		return;
	}
	disconnectSource( iSlot );
	iSlot = rSource;
	connectSource( iSlot );
	invalidate();
}

void T3DPackedPBRTexture::sourceChanged( void ) {
	invalidate();
}

void T3DPackedPBRTexture::invalidate( void ) {
	m_bDirty = true;
	m_lastError = String();
	emit_changed();
	if ( m_bAutoRebuild && hasRequiredSources() ) {
		queueRebuild();
	}
}

void T3DPackedPBRTexture::queueRebuild( void ) {
	if ( m_bRebuildQueued ) {
		return;
	}
	m_bRebuildQueued = true;
	call_deferred( "_deferred_rebuild" );
}

void T3DPackedPBRTexture::deferredRebuild( void ) {
	m_bRebuildQueued = false;
	if ( m_bAutoRebuild && m_bDirty && hasRequiredSources() ) {
		rebuild();
	}
}

Ref<Image> T3DPackedPBRTexture::prepareSource( const Ref<Texture2D> &rSource,
		const Vector2i &rTargetSize,
		const String &rLabel ) const {
	if ( rSource.is_null() ) {
		return Ref<Image>();
	}
	Ref<Image> pImage = rSource->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		LOG( ERROR, "Surface/height ", rLabel, " source image is missing or empty." );
		return Ref<Image>();
	}
	pImage = pImage->duplicate();
	if ( pImage->is_compressed() && pImage->decompress() != OK ) {
		LOG( ERROR, "Surface/height ", rLabel, " source could not be decompressed." );
		return Ref<Image>();
	}
	if ( pImage->has_mipmaps() ) {
		pImage->clear_mipmaps();
	}
	if ( pImage->get_size() != rTargetSize ) {
		pImage->resize( rTargetSize.x, rTargetSize.y, Image::INTERPOLATE_LANCZOS );
	}
	return pImage;
}

void T3DPackedPBRTexture::setGeneratedSurfaceTexture( const Ref<ImageTexture> &rTexture ) {
	m_generatedSurfaceTexture = rTexture;
	m_bDirty = !( isValidSurfaceTexture() && isValidDisplacementTexture() );
}

void T3DPackedPBRTexture::setGeneratedDisplacementTexture( const Ref<ImageTexture> &rTexture ) {
	m_generatedDisplacementTexture = rTexture;
	if ( m_generatedDisplacementTexture.is_valid() ) {
		const Ref<Image> pImage = m_generatedDisplacementTexture->get_image();
		if ( pImage.is_valid() && pImage->get_format() == Image::FORMAT_R16 ) {
			const Ref<Image> pMigrated = T3DUtil::packDisplacementR8( pImage, 0, pImage->has_mipmaps() );
			if ( pMigrated.is_valid() && !pMigrated->is_empty() ) {
				m_generatedDisplacementTexture = ImageTexture::create_from_image( pMigrated );
				LOG( WARN, "Migrated serialized R16 displacement data to R8." );
			}
		}
	}
	m_bDirty = !( isValidSurfaceTexture() && isValidDisplacementTexture() );
}

void T3DPackedPBRTexture::setGeneratedTexture( const Ref<ImageTexture> &rTexture ) {
	if ( rTexture.is_null() ) {
		return;
	}
	const Ref<Image> pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		return;
	}
	if ( pImage->get_format() == Image::FORMAT_RGBA8 ) {
		m_generatedSurfaceTexture = rTexture;
		m_bDirty = !( isValidSurfaceTexture() && isValidDisplacementTexture() );
		return;
	}
	if ( pImage->get_format() == Image::FORMAT_RGBA16I ) {
		const Ref<Image> pSurface = T3DUtil::convertPackedPbrRgba16uiToSurface( pImage, m_bGenerateMipmaps );
		const Ref<Image> pDisplacement = T3DUtil::convertPackedPbrRgba16uiToDisplacement( pImage, m_bGenerateMipmaps );
		if ( pSurface.is_valid() && pDisplacement.is_valid() ) {
			m_generatedSurfaceTexture = ImageTexture::create_from_image( pSurface );
			m_generatedDisplacementTexture = ImageTexture::create_from_image( pDisplacement );
			m_bDirty = false;
			LOG( WARN, "Migrated serialized RGBA16UI packed PBR data to RGBA8 packed PBR + R8 displacement." );
		}
	}
}

void T3DPackedPBRTexture::setPackedTexture( const Ref<Texture2D> &rTexture ) {
	if ( rTexture.is_valid() ) {
		const Ref<Image> pImage = rTexture->get_image();
		if ( pImage.is_null() || pImage->is_empty() ||
				!t3dPackedPbrTextureIsPackedSurfaceFormat( pImage->get_format() ) ) {
			m_lastError = "Packed PBR texture must be RGBA8, BC3/DXT5, or BC7/BPTC_RGBA: "
						  "R=normal X, G=normal Y, B=roughness, A=AO.";
			LOG( ERROR, m_lastError );
			return;
		}
	}
	if ( m_packedTexture == rTexture ) {
		return;
	}
	m_packedTexture = rTexture;
	m_bDirty = !( isValidSurfaceTexture() && isValidDisplacementTexture() );
	m_lastError = String();
	emit_changed();
}

void T3DPackedPBRTexture::setDisplacementTexture( const Ref<Texture2D> &rTexture ) {
	Ref<Texture2D> pTexture = rTexture;
	if ( pTexture.is_valid() ) {
		Ref<Image> pImage = pTexture->get_image();
		if ( pImage.is_null() || pImage->is_empty() ) {
			m_lastError = "Displacement texture image is missing or empty.";
			LOG( ERROR, m_lastError );
			return;
		}
		if ( pImage->get_format() == Image::FORMAT_R16 ) {
			const Ref<Image> pMigrated = T3DUtil::packDisplacementR8( pImage, 0, pImage->has_mipmaps() );
			if ( pMigrated.is_null() || pMigrated->is_empty() ) {
				m_lastError = "Could not migrate legacy R16 displacement texture to R8.";
				LOG( ERROR, m_lastError );
				return;
			}
			pTexture = ImageTexture::create_from_image( pMigrated );
			pImage = pMigrated;
			LOG( WARN, "Migrated legacy R16 displacement texture to R8." );
		}
		if ( pImage->get_format() != Image::FORMAT_R8 ) {
			m_lastError = "Displacement texture must be R8_UNORM.";
			LOG( ERROR, m_lastError );
			return;
		}
	}
	if ( m_displacementTexture == pTexture ) {
		return;
	}
	m_displacementTexture = pTexture;
	m_bDirty = !( isValidSurfaceTexture() && isValidDisplacementTexture() );
	m_lastError = String();
	emit_changed();
}

void T3DPackedPBRTexture::setNormalSource( const Ref<Texture2D> &rTexture ) {
	setSource( m_normalSource, rTexture );
}

void T3DPackedPBRTexture::setRoughnessSource( const Ref<Texture2D> &rTexture ) {
	setSource( m_roughnessSource, rTexture );
}

void T3DPackedPBRTexture::setDisplacementSource( const Ref<Texture2D> &rTexture ) {
	setSource( m_displacementSource, rTexture );
}

void T3DPackedPBRTexture::setAoSource( const Ref<Texture2D> &rTexture ) {
	setSource( m_aoSource, rTexture );
}

void T3DPackedPBRTexture::setMetallicSource( const Ref<Texture2D> &rTexture ) {
	// Kept only for old resource compatibility. Runtime metallic is now the
	// T3DTextureAsset metallic scalar.
	setSource( m_metallicSource, rTexture );
}

void T3DPackedPBRTexture::setOutputSize( const Vector2i &rSize ) {
	const Vector2i size( MAX( rSize.x, 1 ), MAX( rSize.y, 1 ) );
	if ( m_outputSize == size ) {
		return;
	}
	m_outputSize = size;
	invalidate();
}

void T3DPackedPBRTexture::setInvertNormalGreen( const bool bEnabled ) {
	if ( m_bInvertNormalGreen == bEnabled ) {
		return;
	}
	m_bInvertNormalGreen = bEnabled;
	invalidate();
}

EA_FORCE_INLINE static int32_t t3dSanitizeSurfaceChannel( int32_t nChannel ) {
	return CLAMP( nChannel, ( int32_t )( CHANNEL_RED ), ( int32_t )( CHANNEL_ALPHA ) );
}

void T3DPackedPBRTexture::setRoughnessChannel( const int32_t nChannel ) {
	const int32_t channel = t3dSanitizeSurfaceChannel( nChannel );
	if ( m_nRoughnessChannel == channel ) {
		return;
	}
	m_nRoughnessChannel = channel;
	invalidate();
}

void T3DPackedPBRTexture::setDisplacementChannel( const int32_t nChannel ) {
	const int32_t channel = t3dSanitizeSurfaceChannel( nChannel );
	if ( m_nDisplacementChannel == channel ) {
		return;
	}
	m_nDisplacementChannel = channel;
	invalidate();
}

void T3DPackedPBRTexture::setAoChannel( const int32_t nChannel ) {
	const int32_t channel = t3dSanitizeSurfaceChannel( nChannel );
	if ( m_nAoChannel == channel ) {
		return;
	}
	m_nAoChannel = channel;
	invalidate();
}

void T3DPackedPBRTexture::setMetallicChannel( const int32_t nChannel ) {
	const int32_t channel = t3dSanitizeSurfaceChannel( nChannel );
	if ( m_nMetallicChannel == channel ) {
		return;
	}
	m_nMetallicChannel = channel;
	// Stored for backward compatibility only; no rebuild is necessary.
	emit_changed();
}

void T3DPackedPBRTexture::setGenerateMipmaps( const bool bEnabled ) {
	if ( m_bGenerateMipmaps == bEnabled ) {
		return;
	}
	m_bGenerateMipmaps = bEnabled;
	invalidate();
}

void T3DPackedPBRTexture::setAutoRebuild( const bool bEnabled ) {
	if ( m_bAutoRebuild == bEnabled ) {
		return;
	}
	m_bAutoRebuild = bEnabled;
	if ( m_bAutoRebuild && m_bDirty && hasRequiredSources() ) {
		queueRebuild();
	}
	emit_changed();
}

bool T3DPackedPBRTexture::hasRequiredSources( void ) const {
	return ( m_packedTexture.is_valid() && m_displacementTexture.is_valid() ) ||
			( m_normalSource.is_valid() && m_roughnessSource.is_valid() );
}

bool T3DPackedPBRTexture::isValidSurfaceTexture( void ) const {
	const Ref<Texture2D> pTexture =
			m_packedTexture.is_valid() ? m_packedTexture : Ref<Texture2D>( m_generatedSurfaceTexture );
	if ( pTexture.is_null() ) {
		return false;
	}
	Ref<Image> pImage = pTexture->get_image();
	return pImage.is_valid() && !pImage->is_empty() && t3dPackedPbrTextureIsPackedSurfaceFormat( pImage->get_format() );
}

bool T3DPackedPBRTexture::isValidDisplacementTexture( void ) const {
	const Ref<Texture2D> pTexture =
			m_displacementTexture.is_valid() ? m_displacementTexture : Ref<Texture2D>( m_generatedDisplacementTexture );
	if ( pTexture.is_null() ) {
		return false;
	}
	Ref<Image> pImage = pTexture->get_image();
	return pImage.is_valid() && !pImage->is_empty() && pImage->get_format() == Image::FORMAT_R8;
}

bool T3DPackedPBRTexture::rebuild( void ) {
	m_bRebuildQueued = false;
	m_lastError = String();
	if ( m_packedTexture.is_valid() || m_displacementTexture.is_valid() ) {
		m_bDirty = !( isValidSurfaceTexture() && isValidDisplacementTexture() );
		if ( m_bDirty ) {
			m_lastError =
					"Packed PBR resources require one RGBA8/BC3/BC7 packed texture and one R8 displacement texture.";
		}
		return !m_bDirty;
	}
	if ( !hasRequiredSources() ) {
		m_lastError = "Normal and roughness sources are required.";
		LOG( ERROR, m_lastError );
		return false;
	}
	const Vector2i size( MAX( m_outputSize.x, 1 ), MAX( m_outputSize.y, 1 ) );
	const Ref<Image> pNormal = prepareSource( m_normalSource, size, "normal" );
	const Ref<Image> pRoughness = prepareSource( m_roughnessSource, size, "roughness" );
	const Ref<Image> pDisplacement = prepareSource( m_displacementSource, size, "displacement" );
	const Ref<Image> pAo = prepareSource( m_aoSource, size, "AO" );
	if ( pNormal.is_null() || pRoughness.is_null() ) {
		m_lastError = "A required source image could not be prepared.";
		return false;
	}

	const Ref<Image> pSurface = T3DUtil::packSurfaceRgba8(
			pNormal, pRoughness, pAo, m_bInvertNormalGreen, m_nRoughnessChannel, m_nAoChannel, m_bGenerateMipmaps );
	const Ref<Image> pHeight = pDisplacement.is_valid()
			? T3DUtil::packDisplacementR8( pDisplacement, m_nDisplacementChannel, m_bGenerateMipmaps )
			: T3DUtil::getDefaultDisplacementImage( size, m_bGenerateMipmaps );
	if ( pSurface.is_null() || pSurface->is_empty() || pHeight.is_null() || pHeight->is_empty() ) {
		m_lastError = "T3DUtil failed to generate the surface/height images.";
		return false;
	}

	m_generatedSurfaceTexture = ImageTexture::create_from_image( pSurface );
	m_generatedDisplacementTexture = ImageTexture::create_from_image( pHeight );
	m_bDirty = false;
	emit_changed();
	return true;
}

Ref<Texture2D> T3DPackedPBRTexture::getSurfaceTexture( void ) {
	if ( m_bDirty && hasRequiredSources() && !rebuild() ) {
		return Ref<Texture2D>();
	}
	Ref<Texture2D> pTexture =
			m_packedTexture.is_valid() ? m_packedTexture : Ref<Texture2D>( m_generatedSurfaceTexture );
	return isValidSurfaceTexture() ? pTexture : Ref<Texture2D>();
}

Ref<Texture2D> T3DPackedPBRTexture::getDisplacementTexture( void ) {
	if ( m_bDirty && hasRequiredSources() && !rebuild() ) {
		return Ref<Texture2D>();
	}
	Ref<Texture2D> pTexture =
			m_displacementTexture.is_valid() ? m_displacementTexture : Ref<Texture2D>( m_generatedDisplacementTexture );
	return isValidDisplacementTexture() ? pTexture : Ref<Texture2D>();
}

Error T3DPackedPBRTexture::saveDisplacementDds( const String &rPath ) {
	const Ref<Texture2D> pTexture = getDisplacementTexture();
	if ( pTexture.is_null() ) {
		m_lastError = "No valid R8 displacement texture is available.";
		return ERR_INVALID_DATA;
	}
	const Ref<Image> pImage = pTexture->get_image();
	if ( pImage.is_null() || pImage->get_format() != Image::FORMAT_R8 ) {
		m_lastError = "Displacement image is not R8_UNORM.";
		return ERR_INVALID_DATA;
	}
	const Error error = pImage->save_dds( rPath );
	if ( error != OK ) {
		m_lastError = "Failed to save R8 displacement DDS: " + rPath;
	}
	return error;
}

String T3DPackedPBRTexture::getChannelLayout( void ) const {
	return "Surface RGBA8: R=Normal X, G=Normal Y, B=Roughness, A=AO; Displacement R8_UNORM: R=Height; "
		   "Metallic=T3DTextureAsset scalar";
}

void T3DPackedPBRTexture::_validate_property( PropertyInfo &rProperty ) const {
	if ( rProperty.name == StringName( "metallic_source" ) || rProperty.name == StringName( "metallic_channel" ) ||
			rProperty.name == StringName( "generated_texture" ) ) {
		rProperty.usage = PROPERTY_USAGE_STORAGE;
	}
}

void T3DPackedPBRTexture::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_packed_texture", "texture" ), &T3DPackedPBRTexture::setPackedTexture );
	ClassDB::bind_method( D_METHOD( "get_packed_texture" ), &T3DPackedPBRTexture::getPackedTexture );
	ClassDB::bind_method(
			D_METHOD( "set_displacement_texture", "texture" ), &T3DPackedPBRTexture::setDisplacementTexture );
	ClassDB::bind_method(
			D_METHOD( "get_direct_displacement_texture" ), &T3DPackedPBRTexture::getDirectDisplacementTexture );
	ClassDB::bind_method( D_METHOD( "set_normal_source", "texture" ), &T3DPackedPBRTexture::setNormalSource );
	ClassDB::bind_method( D_METHOD( "get_normal_source" ), &T3DPackedPBRTexture::getNormalSource );
	ClassDB::bind_method( D_METHOD( "set_roughness_source", "texture" ), &T3DPackedPBRTexture::setRoughnessSource );
	ClassDB::bind_method( D_METHOD( "get_roughness_source" ), &T3DPackedPBRTexture::getRoughnessSource );
	ClassDB::bind_method(
			D_METHOD( "set_displacement_source", "texture" ), &T3DPackedPBRTexture::setDisplacementSource );
	ClassDB::bind_method( D_METHOD( "get_displacement_source" ), &T3DPackedPBRTexture::getDisplacementSource );
	ClassDB::bind_method( D_METHOD( "set_ao_source", "texture" ), &T3DPackedPBRTexture::setAoSource );
	ClassDB::bind_method( D_METHOD( "get_ao_source" ), &T3DPackedPBRTexture::getAoSource );
	ClassDB::bind_method( D_METHOD( "set_metallic_source", "texture" ), &T3DPackedPBRTexture::setMetallicSource );
	ClassDB::bind_method( D_METHOD( "get_metallic_source" ), &T3DPackedPBRTexture::getMetallicSource );
	ClassDB::bind_method( D_METHOD( "set_output_size", "size" ), &T3DPackedPBRTexture::setOutputSize );
	ClassDB::bind_method( D_METHOD( "get_output_size" ), &T3DPackedPBRTexture::getOutputSize );
	ClassDB::bind_method(
			D_METHOD( "set_invert_normal_green", "enabled" ), &T3DPackedPBRTexture::setInvertNormalGreen );
	ClassDB::bind_method( D_METHOD( "get_invert_normal_green" ), &T3DPackedPBRTexture::getInvertNormalGreen );
	ClassDB::bind_method( D_METHOD( "set_roughness_channel", "channel" ), &T3DPackedPBRTexture::setRoughnessChannel );
	ClassDB::bind_method( D_METHOD( "get_roughness_channel" ), &T3DPackedPBRTexture::getRoughnessChannel );
	ClassDB::bind_method(
			D_METHOD( "set_displacement_channel", "channel" ), &T3DPackedPBRTexture::setDisplacementChannel );
	ClassDB::bind_method( D_METHOD( "get_displacement_channel" ), &T3DPackedPBRTexture::getDisplacementChannel );
	ClassDB::bind_method( D_METHOD( "set_ao_channel", "channel" ), &T3DPackedPBRTexture::setAoChannel );
	ClassDB::bind_method( D_METHOD( "get_ao_channel" ), &T3DPackedPBRTexture::getAoChannel );
	ClassDB::bind_method( D_METHOD( "set_metallic_channel", "channel" ), &T3DPackedPBRTexture::setMetallicChannel );
	ClassDB::bind_method( D_METHOD( "get_metallic_channel" ), &T3DPackedPBRTexture::getMetallicChannel );
	ClassDB::bind_method( D_METHOD( "set_generate_mipmaps", "enabled" ), &T3DPackedPBRTexture::setGenerateMipmaps );
	ClassDB::bind_method( D_METHOD( "get_generate_mipmaps" ), &T3DPackedPBRTexture::getGenerateMipmaps );
	ClassDB::bind_method( D_METHOD( "set_auto_rebuild", "enabled" ), &T3DPackedPBRTexture::setAutoRebuild );
	ClassDB::bind_method( D_METHOD( "get_auto_rebuild" ), &T3DPackedPBRTexture::getAutoRebuild );
	ClassDB::bind_method( D_METHOD( "rebuild" ), &T3DPackedPBRTexture::rebuild );
	ClassDB::bind_method( D_METHOD( "is_dirty" ), &T3DPackedPBRTexture::isDirty );
	ClassDB::bind_method( D_METHOD( "has_required_sources" ), &T3DPackedPBRTexture::hasRequiredSources );
	ClassDB::bind_method( D_METHOD( "is_valid_surface_texture" ), &T3DPackedPBRTexture::isValidSurfaceTexture );
	ClassDB::bind_method(
			D_METHOD( "is_valid_displacement_texture" ), &T3DPackedPBRTexture::isValidDisplacementTexture );
	ClassDB::bind_method( D_METHOD( "is_valid_packed_texture" ), &T3DPackedPBRTexture::isValidPackedTexture );
	ClassDB::bind_method( D_METHOD( "get_last_error" ), &T3DPackedPBRTexture::getLastError );
	ClassDB::bind_method( D_METHOD( "get_surface_texture" ), &T3DPackedPBRTexture::getSurfaceTexture );
	ClassDB::bind_method( D_METHOD( "get_displacement_texture" ), &T3DPackedPBRTexture::getDisplacementTexture );
	ClassDB::bind_method( D_METHOD( "get_texture" ), &T3DPackedPBRTexture::getTexture );
	ClassDB::bind_method( D_METHOD( "get_generated_texture" ), &T3DPackedPBRTexture::getGeneratedTexture );
	ClassDB::bind_method(
			D_METHOD( "get_generated_surface_texture" ), &T3DPackedPBRTexture::getGeneratedSurfaceTexture );
	ClassDB::bind_method(
			D_METHOD( "get_generated_displacement_texture" ), &T3DPackedPBRTexture::getGeneratedDisplacementTexture );
	ClassDB::bind_method( D_METHOD( "save_displacement_dds", "path" ), &T3DPackedPBRTexture::saveDisplacementDds );
	ClassDB::bind_method( D_METHOD( "get_channel_layout" ), &T3DPackedPBRTexture::getChannelLayout );
	ClassDB::bind_method(
			D_METHOD( "_set_generated_surface_texture", "texture" ), &T3DPackedPBRTexture::setGeneratedSurfaceTexture );
	ClassDB::bind_method( D_METHOD( "_set_generated_displacement_texture", "texture" ),
			&T3DPackedPBRTexture::setGeneratedDisplacementTexture );
	ClassDB::bind_method( D_METHOD( "_set_generated_texture", "texture" ), &T3DPackedPBRTexture::setGeneratedTexture );
	ClassDB::bind_method(
			D_METHOD( "_get_legacy_generated_texture" ), &T3DPackedPBRTexture::getLegacyGeneratedTexture );
	ClassDB::bind_method( D_METHOD( "_deferred_rebuild" ), &T3DPackedPBRTexture::deferredRebuild );

	BIND_ENUM_CONSTANT( CHANNEL_RED );
	BIND_ENUM_CONSTANT( CHANNEL_GREEN );
	BIND_ENUM_CONSTANT( CHANNEL_BLUE );
	BIND_ENUM_CONSTANT( CHANNEL_ALPHA );

	ADD_GROUP( "Runtime Packed Output", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "packed_texture",
						  PROPERTY_HINT_RESOURCE_TYPE,
						  "ImageTexture,CompressedTexture2D",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_packed_texture",
			"get_packed_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "displacement_texture",
						  PROPERTY_HINT_RESOURCE_TYPE,
						  "ImageTexture,CompressedTexture2D",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_displacement_texture",
			"get_direct_displacement_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "generated_surface_texture",
						  PROPERTY_HINT_RESOURCE_TYPE,
						  "ImageTexture",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"_set_generated_surface_texture",
			"get_generated_surface_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "generated_displacement_texture",
						  PROPERTY_HINT_RESOURCE_TYPE,
						  "ImageTexture",
						  PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"_set_generated_displacement_texture",
			"get_generated_displacement_texture" );

	ADD_GROUP( "Source Channels", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "normal_source", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
			"set_normal_source",
			"get_normal_source" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "roughness_source", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
			"set_roughness_source",
			"get_roughness_source" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "displacement_source", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
			"set_displacement_source",
			"get_displacement_source" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "ao_source", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
			"set_ao_source",
			"get_ao_source" );

	ADD_GROUP( "Packing Rules", "" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2I, "output_size", PROPERTY_HINT_NONE, "" ),
			"set_output_size",
			"get_output_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "invert_normal_green" ),
			"set_invert_normal_green",
			"get_invert_normal_green" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "roughness_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha" ),
			"set_roughness_channel",
			"get_roughness_channel" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "displacement_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha" ),
			"set_displacement_channel",
			"get_displacement_channel" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ao_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha" ),
			"set_ao_channel",
			"get_ao_channel" );

	ADD_GROUP( "Build Behavior", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "generate_mipmaps" ), "set_generate_mipmaps", "get_generate_mipmaps" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "auto_rebuild" ), "set_auto_rebuild", "get_auto_rebuild" );

	ADD_GROUP( "Compatibility", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "metallic_source",
						  PROPERTY_HINT_RESOURCE_TYPE,
						  "Texture2D",
						  PROPERTY_USAGE_STORAGE ),
			"set_metallic_source",
			"get_metallic_source" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "metallic_channel",
						  PROPERTY_HINT_ENUM,
						  "Red,Green,Blue,Alpha",
						  PROPERTY_USAGE_STORAGE ),
			"set_metallic_channel",
			"get_metallic_channel" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "generated_texture",
						  PROPERTY_HINT_RESOURCE_TYPE,
						  "ImageTexture",
						  PROPERTY_USAGE_STORAGE ),
			"_set_generated_texture",
			"_get_legacy_generated_texture" );
}
