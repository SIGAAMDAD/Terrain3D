// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_TEXTURE_CLASS_H
#define TERRAIN3D_TEXTURE_CLASS_H

#include "Terrain3DConstants.h"
#include "assets/resources/terrain_3d_asset_resource.h"
#include "assets/resources/terrain_3d_packed_pbr_texture.h"
#include "core/config/terrain_3d_compiler.h"

#include <godot_cpp/classes/texture2d.hpp>

class Terrain3DTextureAsset : public Terrain3DAssetResource {
	GDCLASS( Terrain3DTextureAsset, Terrain3DAssetResource );
	CLASS_NAME();
	friend class Terrain3DAssets;

	// Saved Data
	Color m_albedoColor = Color( 1.f, 1.f, 1.f, 1.f );
	Ref<Texture2D> m_albedoTexture;
	Ref<Texture2D> m_normalTexture;
	// Optimized runtime inputs. Packed PBR is authored externally (for example in
	// GIMP) and loaded directly; Terrain3D does not expose separate PBR channels.
	Ref<Texture2D> m_surfaceTexture;
	Ref<Texture2D> m_displacementTexture;
	Ref<Terrain3DPackedPBRTexture> m_packedPbrResource;
	// Serialized compatibility input for the former RGBA16UI implementation.
	Ref<Texture2D> m_pbrTexture;
	real_t m_fMetallic = 0.0f;
	real_t m_fNormalDepth = 1.0f;
	real_t m_fAoStrength = 0.5f;
	real_t m_fAoLightAffect = 0.0f;
	real_t m_fRoughness = 0.f;
	real_t m_fDisplacementScale = 0.0f;
	real_t m_fDisplacementOffset = 0.0f;
	real_t m_fUvScale = 0.1f;
	real_t m_fDetilingRotation = 0.0f;
	real_t m_fDetilingShift = 0.0f;

	// Offline erosion profile. These defaults preserve the legacy global solver behavior.
	bool m_bErosionEnabled = true;
	real_t m_fErosionHardness = -1.0f;
	real_t m_fErosionHydraulicErodibility = 1.0f;
	real_t m_fErosionWindErodibility = 1.0f;
	real_t m_fErosionWaveErodibility = 1.0f;
	real_t m_fErosionWeatheringSusceptibility = 1.0f;
	real_t m_fErosionOxidationSusceptibility = 1.0f;
	real_t m_fErosionSaltSusceptibility = 1.0f;
	real_t m_fErosionWetDrySusceptibility = 1.0f;
	real_t m_fErosionInfiltrationMultiplier = 1.0f;
	real_t m_fErosionTrafficErodibility = 1.0f;
	real_t m_fErosionCohesion = 0.0f;
	real_t m_fErosionGrainSize = 0.5f;
	int32_t m_nErosionExposedTextureId = -1;
	int32_t m_nErosionWeatheredTextureId = -1;
	int32_t m_nErosionOxidizedTextureId = -1;
	int32_t m_nErosionCoarseDepositTextureId = -1;
	int32_t m_nErosionFineDepositTextureId = -1;
	int32_t m_nErosionSaltWeatheredTextureId = -1;
	int32_t m_nErosionPavementTextureId = -1;
	int32_t m_nErosionVarnishedTextureId = -1;

	// Working Data
	Color m_highlightColor = Color();
	bool m_bPackedPbrChangeQueued = false;
	bool m_bPackedPbrMode = false;

	bool isValidFormat( const Ref<Texture2D> &rTexture ) const;
	void packedPbrResourceChanged( void );
	void emitPackedPbrFileChanged( void );
	void disconnectPackedPbrResource( void );
	void connectPackedPbrResource( void );
	void setPackedPbrMode( bool bEnabled );

public:
	EA_FORCE_INLINE Terrain3DTextureAsset( void ) {
		clear();
	}
	EA_FORCE_INLINE ~Terrain3DTextureAsset( void ) {
	}
	void initialize( void ) override;
	void clear( void ) override;

	void setName( const String &rName ) override;
	EA_FORCE_INLINE String getName( void ) const override {
		return m_name;
	}

	void setId( const int nNewId ) override;
	EA_FORCE_INLINE int getId( void ) const override {
		return m_nId;
	}

	void setHighlighted( const bool bHighlighted ) override;
	EA_FORCE_INLINE bool isHighlighted( void ) const override {
		return m_bHighlighted;
	}
	EA_FORCE_INLINE Color getHighlightColor( void ) const override {
		return m_bHighlighted ? m_highlightColor : s_colorWhite;
	}

	EA_FORCE_INLINE Ref<Texture2D> getThumbnail( void ) const override {
		return m_thumbnail;
	}

	void setAlbedoColor( const Color &rColor );
	EA_FORCE_INLINE Color getAlbedoColor( void ) const {
		return m_albedoColor;
	}

	void setAlbedoTexture( const Ref<Texture2D> &rTexture );
	EA_FORCE_INLINE Ref<Texture2D> getAlbedoTexture( void ) const {
		return m_albedoTexture;
	}

	void setNormalTexture( const Ref<Texture2D> &rTexture );
	EA_FORCE_INLINE Ref<Texture2D> getNormalTexture( void ) const {
		return m_normalTexture;
	}

	void setSurfaceTexture( const Ref<Texture2D> &rTexture );
	EA_FORCE_INLINE Ref<Texture2D> getSurfaceTexture( void ) const {
		return m_surfaceTexture;
	}
	EA_FORCE_INLINE void setPackedPbrTexture( const Ref<Texture2D> &rTexture ) {
		setSurfaceTexture( rTexture );
	}
	EA_FORCE_INLINE Ref<Texture2D> getPackedPbrTexture( void ) const {
		return m_surfaceTexture;
	}
	void setDisplacementTexture( const Ref<Texture2D> &rTexture );
	EA_FORCE_INLINE Ref<Texture2D> getDisplacementTexture( void ) const {
		return m_displacementTexture;
	}

	// Compatibility setter/getter for serialized RGBA16UI assets. New code should
	// use packed_pbr_texture and displacement_texture.
	void setPbrTexture( const Ref<Texture2D> &rTexture );
	EA_FORCE_INLINE Ref<Texture2D> getPbrTexture( void ) const {
		return m_pbrTexture;
	}

	void setPackedPbrResource( const Ref<Terrain3DPackedPBRTexture> &rResource );
	EA_FORCE_INLINE Ref<Terrain3DPackedPBRTexture> getPackedPbrResource( void ) const {
		return m_packedPbrResource;
	}
	Ref<Texture2D> getEffectiveSurfaceTexture( void );
	Ref<Texture2D> getEffectiveDisplacementTexture( void );

	void setMetallic( const real_t fMetallic );
	EA_FORCE_INLINE real_t getMetallic( void ) const {
		return m_fMetallic;
	}

	void setNormalDepth( const real_t fNormalDepth );
	EA_FORCE_INLINE real_t getNormalDepth( void ) const {
		return m_fNormalDepth;
	}

	void setAoStrength( const real_t fAoStrength );
	EA_FORCE_INLINE real_t getAoStrength( void ) const {
		return m_fAoStrength;
	}

	void setAoLightAffect( const real_t fAoLightAffect );
	EA_FORCE_INLINE real_t getAoLightAffect( void ) const {
		return m_fAoLightAffect;
	}

	void setRoughness( const real_t fRoughness );
	EA_FORCE_INLINE real_t getRoughness( void ) const {
		return m_fRoughness;
	}

	void setDisplacementScale( const real_t fDisplacementScale );
	EA_FORCE_INLINE real_t getDisplacementScale( void ) const {
		return m_fDisplacementScale;
	}

	void setDisplacementOffset( const real_t fDisplacementOffset );
	EA_FORCE_INLINE real_t getDisplacementOffset( void ) const {
		return m_fDisplacementOffset;
	}

	void setUvScale( const real_t fScale );
	EA_FORCE_INLINE real_t getUvScale( void ) const {
		return m_fUvScale;
	}

	void setDetilingRotation( const real_t fDetilingRotation );
	EA_FORCE_INLINE real_t getDetilingRotation( void ) const {
		return m_fDetilingRotation;
	}

	void setDetilingShift( const real_t fDetilingShift );
	EA_FORCE_INLINE real_t getDetilingShift( void ) const {
		return m_fDetilingShift;
	}

	void setErosionEnabled( bool bValue );
	EA_FORCE_INLINE bool getErosionEnabled( void ) const {
		return m_bErosionEnabled;
	}
	void setErosionHardness( real_t fValue );
	EA_FORCE_INLINE real_t getErosionHardness( void ) const {
		return m_fErosionHardness;
	}
	void setErosionHydraulicErodibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionHydraulicErodibility( void ) const {
		return m_fErosionHydraulicErodibility;
	}
	void setErosionWindErodibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionWindErodibility( void ) const {
		return m_fErosionWindErodibility;
	}
	void setErosionWaveErodibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionWaveErodibility( void ) const {
		return m_fErosionWaveErodibility;
	}
	void setErosionWeatheringSusceptibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionWeatheringSusceptibility( void ) const {
		return m_fErosionWeatheringSusceptibility;
	}
	void setErosionOxidationSusceptibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionOxidationSusceptibility( void ) const {
		return m_fErosionOxidationSusceptibility;
	}
	void setErosionSaltSusceptibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionSaltSusceptibility( void ) const {
		return m_fErosionSaltSusceptibility;
	}
	void setErosionWetDrySusceptibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionWetDrySusceptibility( void ) const {
		return m_fErosionWetDrySusceptibility;
	}
	void setErosionInfiltrationMultiplier( real_t fValue );
	EA_FORCE_INLINE real_t getErosionInfiltrationMultiplier( void ) const {
		return m_fErosionInfiltrationMultiplier;
	}
	void setErosionTrafficErodibility( real_t fValue );
	EA_FORCE_INLINE real_t getErosionTrafficErodibility( void ) const {
		return m_fErosionTrafficErodibility;
	}
	void setErosionCohesion( real_t fValue );
	EA_FORCE_INLINE real_t getErosionCohesion( void ) const {
		return m_fErosionCohesion;
	}
	void setErosionGrainSize( real_t fValue );
	EA_FORCE_INLINE real_t getErosionGrainSize( void ) const {
		return m_fErosionGrainSize;
	}
	void setErosionExposedTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionExposedTextureId( void ) const {
		return m_nErosionExposedTextureId;
	}
	void setErosionWeatheredTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionWeatheredTextureId( void ) const {
		return m_nErosionWeatheredTextureId;
	}
	void setErosionOxidizedTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionOxidizedTextureId( void ) const {
		return m_nErosionOxidizedTextureId;
	}
	void setErosionCoarseDepositTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionCoarseDepositTextureId( void ) const {
		return m_nErosionCoarseDepositTextureId;
	}
	void setErosionFineDepositTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionFineDepositTextureId( void ) const {
		return m_nErosionFineDepositTextureId;
	}
	void setErosionSaltWeatheredTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionSaltWeatheredTextureId( void ) const {
		return m_nErosionSaltWeatheredTextureId;
	}
	void setErosionPavementTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionPavementTextureId( void ) const {
		return m_nErosionPavementTextureId;
	}
	void setErosionVarnishedTextureId( int32_t nValue );
	EA_FORCE_INLINE int32_t getErosionVarnishedTextureId( void ) const {
		return m_nErosionVarnishedTextureId;
	}

protected:
	void _validate_property( PropertyInfo &rProperty ) const;
	static void _bind_methods( void );
};
#endif // TERRAIN3D_TEXTURE_CLASS_H
