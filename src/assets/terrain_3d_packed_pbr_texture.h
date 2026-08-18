// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_PACKED_PBR_TEXTURE_H
#define TERRAIN3D_PACKED_PBR_TEXTURE_H

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include "Terrain3DConstants.h"

// Compatibility-preserving container for externally authored packed textures.
// New resources take exactly two files:
//   Packed PBR RGBA8: R=normal X, G=normal Y, B=roughness, A=AO
//   Displacement R8_UNORM: R=height
// Legacy separate-channel fields remain storage-only so old .tres resources load.
class T3DPackedPBRTexture : public Resource {
	using Terrain3DPackedPBRTexture = T3DPackedPBRTexture;
	GDCLASS( Terrain3DPackedPBRTexture, Resource );
	CLASS_NAME();

public:
	typedef enum t3dsourceChannel_e {
		CHANNEL_RED = 0,
		CHANNEL_GREEN = 1,
		CHANNEL_BLUE = 2,
		CHANNEL_ALPHA = 3,
	} t3dsourceChannel_t;

private:
	// Primary externally authored runtime textures.
	Ref<Texture2D> m_pPackedTexture;
	Ref<Texture2D> m_pDisplacementTexture;

	// Legacy saved source data; hidden from the editor.
	Ref<Texture2D> m_pNormalSource;
	Ref<Texture2D> m_pRoughnessSource;
	Ref<Texture2D> m_pDisplacementSource;
	Ref<Texture2D> m_pAoSource;
	// Retained only so older resources deserialize cleanly. Metallic is now a
	// per-material scalar on Terrain3DTextureAsset and is not packed per pixel.
	Ref<Texture2D> m_pMetallicSource;
	Vector2i m_outputSize = V2I( 512 );
	bool m_bInvertNormalGreen = false;
	int32_t m_roughnessChannel = CHANNEL_RED;
	int32_t m_displacementChannel = CHANNEL_RED;
	int32_t m_aoChannel = CHANNEL_RED;
	int32_t m_metallicChannel = CHANNEL_RED;
	bool m_bGenerateMipmaps = true;
	bool m_bAutoRebuild = true;

	// Saved generated data. Runtime builds consume these directly without
	// repacking source images.
	Ref<ImageTexture> m_pGeneratedSurfaceTexture;
	Ref<ImageTexture> m_pGeneratedDisplacementTexture;

	// Working state.
	bool m_bDirty = true;
	bool m_bRebuildQueued = false;
	String m_lastError;

	void disconnectSource( const Ref<Texture2D> &rSource );
	void connectSource( const Ref<Texture2D> &rSource );
	void setSource( Ref<Texture2D> &iSlot, const Ref<Texture2D> &rSource );
	void sourceChanged( void );
	void invalidate( void );
	void queueRebuild( void );
	void deferredRebuild( void );
	Ref<Image> prepareSource(
		const Ref<Texture2D> &rSource, const Vector2i &rTargetSize,
		const String &rLabel
	) const;
	void setGeneratedSurfaceTexture( const Ref<ImageTexture> &rTexture );
	void setGeneratedDisplacementTexture( const Ref<ImageTexture> &rTexture );
	// Old serialized property alias. RGBA16UI data is migrated into the new pair.
	void setGeneratedTexture( const Ref<ImageTexture> &rTexture );
	Ref<ImageTexture> getLegacyGeneratedTexture( void ) const {
		return Ref<ImageTexture>();
	}

protected:
	static void _bind_methods( void );
	void _validate_property( PropertyInfo &rProperty ) const;

public:
	T3DPackedPBRTexture( void ) = default;
	~T3DPackedPBRTexture( void ) = default;


	void setPackedTexture( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getPackedTexture( void ) const {
		return m_pPackedTexture;
	}
	void setDisplacementTexture( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getDirectDisplacementTexture( void ) const {
		return m_pDisplacementTexture;
	}

	void setNormalSource( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getNormalSource( void ) const {
		return m_pNormalSource;
	}
	void setRoughnessSource( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getRoughnessSource( void ) const {
		return m_pRoughnessSource;
	}
	void setDisplacementSource( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getDisplacementSource( void ) const {
		return m_pDisplacementSource;
	}
	void setAoSource( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getAoSource( void ) const {
		return m_pAoSource;
	}
	void setMetallicSource( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getMetallicSource( void ) const {
		return m_pMetallicSource;
	}

	void setOutputSize( const Vector2i &rSize );
	Vector2i getOutputSize( void ) const {
		return m_outputSize;
	}
	void setInvertNormalGreen( bool bEnabled );
	bool getInvertNormalGreen( void ) const {
		return m_bInvertNormalGreen;
	}
	void setRoughnessChannel( int32_t nChannel );
	int32_t getRoughnessChannel( void ) const {
		return m_roughnessChannel;
	}
	void setDisplacementChannel( int32_t nChannel );
	int32_t getDisplacementChannel( void ) const {
		return m_displacementChannel;
	}
	void setAoChannel( int32_t nChannel );
	int32_t getAoChannel( void ) const {
		return m_aoChannel;
	}
	void setMetallicChannel( int32_t nChannel );
	int32_t getMetallicChannel( void ) const {
		return m_metallicChannel;
	}
	void setGenerateMipmaps( bool bEnabled );
	bool getGenerateMipmaps( void ) const {
		return m_bGenerateMipmaps;
	}
	void setAutoRebuild( bool bEnabled );
	bool getAutoRebuild( void ) const {
		return m_bAutoRebuild;
	}

	bool rebuild( void );
	bool isDirty( void ) const {
		return m_bDirty;
	}
	bool hasRequiredSources( void ) const;
	bool isValidSurfaceTexture( void ) const;
	bool isValidDisplacementTexture( void ) const;
	// Compatibility alias for older callers.
	bool isValidPackedTexture( void ) const {
		return isValidSurfaceTexture() && isValidDisplacementTexture();
	}
	String getLastError( void ) const {
		return m_lastError;
	}
	Ref<Texture2D> getSurfaceTexture( void );
	Ref<Texture2D> getDisplacementTexture( void );
	// Compatibility aliases: get_texture()/get_generated_texture() now return the
	// RGBA8 surface texture, not an integer-packed image.
	Ref<Texture2D> getTexture( void ) {
		return getSurfaceTexture();
	}
	Ref<Texture2D> getGeneratedTexture( void ) const {
		return m_pGeneratedSurfaceTexture;
	}
	Ref<Texture2D> getGeneratedSurfaceTexture( void ) const {
		return m_pGeneratedSurfaceTexture;
	}
	Ref<Texture2D> getGeneratedDisplacementTexture( void ) const {
		return m_pGeneratedDisplacementTexture;
	}
	Error saveDisplacementDds( const String &rPath );
	String getChannelLayout( void ) const;
};

VARIANT_ENUM_CAST( T3DPackedPBRTexture::t3dsourceChannel_t );

using Terrain3DPackedPBRTexture = T3DPackedPBRTexture;

#endif // TERRAIN3D_PACKED_PBR_TEXTURE_H
