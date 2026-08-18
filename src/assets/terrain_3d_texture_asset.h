// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_TEXTURE_CLASS_H
#define TERRAIN3D_TEXTURE_CLASS_H

#include <godot_cpp/classes/texture2d.hpp>

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_asset_resource.h"
#include "assets/terrain_3d_packed_pbr_texture.h"

class T3DTextureAsset : public T3DAssetResource {
	using Terrain3DTextureAsset = T3DTextureAsset;
	GDCLASS( Terrain3DTextureAsset, T3DAssetResource );
	CLASS_NAME();
	friend class T3DAssets;

	// Saved Data
	Color m_albedoColor = Color( 1.f, 1.f, 1.f, 1.f );
	Ref<Texture2D> m_pAlbedoTexture;
	Ref<Texture2D> m_pNormalTexture;
	// Optimized runtime inputs. Packed PBR is authored externally (for example in
	// GIMP) and loaded directly; Terrain3D does not expose separate PBR channels.
	Ref<Texture2D> m_pSurfaceTexture;
	Ref<Texture2D> m_pDisplacementTexture;
	Ref<T3DPackedPBRTexture> m_pPackedPbrResource;
	// Serialized compatibility input for the former RGBA16UI implementation.
	Ref<Texture2D> m_pPbrTexture;
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
	T3DTextureAsset( void ) {
		clear();
	}
	~T3DTextureAsset( void ) {}
	void initialize( void ) override;
	void clear( void ) override;

	void setName( const String &rName ) override;
	String getName( void ) const override {
		return m_name;
	}

	void setId( const int nNewId ) override;
	int getId( void ) const override {
		return m_nId;
	}

	void setHighlighted( const bool bHighlighted ) override;
	bool isHighlighted( void ) const override {
		return m_bHighlighted;
	}
	Color getHighlightColor( void ) const override {
		return m_bHighlighted ? m_highlightColor : COLOR_WHITE;
	}

	Ref<Texture2D> getThumbnail( void ) const override {
		return m_pThumbnail;
	}

	void setAlbedoColor( const Color &rColor );
	Color getAlbedoColor( void ) const {
		return m_albedoColor;
	}

	void setAlbedoTexture( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getAlbedoTexture( void ) const {
		return m_pAlbedoTexture;
	}

	void setNormalTexture( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getNormalTexture( void ) const {
		return m_pNormalTexture;
	}

	void setSurfaceTexture( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getSurfaceTexture( void ) const {
		return m_pSurfaceTexture;
	}
	void setPackedPbrTexture( const Ref<Texture2D> &rTexture ) {
		setSurfaceTexture( rTexture );
	}
	Ref<Texture2D> getPackedPbrTexture( void ) const {
		return m_pSurfaceTexture;
	}
	void setDisplacementTexture( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getDisplacementTexture( void ) const {
		return m_pDisplacementTexture;
	}

	// Compatibility setter/getter for serialized RGBA16UI assets. New code should
	// use packed_pbr_texture and displacement_texture.
	void setPbrTexture( const Ref<Texture2D> &rTexture );
	Ref<Texture2D> getPbrTexture( void ) const {
		return m_pPbrTexture;
	}

	void setPackedPbrResource( const Ref<T3DPackedPBRTexture> &rResource );
	Ref<T3DPackedPBRTexture> getPackedPbrResource( void ) const {
		return m_pPackedPbrResource;
	}
	Ref<Texture2D> getEffectiveSurfaceTexture( void );
	Ref<Texture2D> getEffectiveDisplacementTexture( void );
	// Deprecated compatibility alias; returns the effective surface texture.
	Ref<Texture2D> getEffectivePbrTexture( void ) {
		return getEffectiveSurfaceTexture();
	}

	void setMetallic( const real_t fMetallic );
	real_t getMetallic( void ) const {
		return m_fMetallic;
	}

	void setNormalDepth( const real_t fNormalDepth );
	real_t getNormalDepth( void ) const {
		return m_fNormalDepth;
	}

	void setAoStrength( const real_t fAoStrength );
	real_t getAoStrength( void ) const {
		return m_fAoStrength;
	}

	void setAoLightAffect( const real_t fAoLightAffect );
	real_t getAoLightAffect( void ) const {
		return m_fAoLightAffect;
	}

	void setRoughness( const real_t fRoughness );
	real_t getRoughness( void ) const {
		return m_fRoughness;
	}

	void setDisplacementScale( const real_t fDisplacementScale );
	real_t getDisplacementScale( void ) const {
		return m_fDisplacementScale;
	}

	void setDisplacementOffset( const real_t fDisplacementOffset );
	real_t getDisplacementOffset( void ) const {
		return m_fDisplacementOffset;
	}

	void setUvScale( const real_t fScale );
	real_t getUvScale( void ) const {
		return m_fUvScale;
	}

	void setDetilingRotation( const real_t fDetilingRotation );
	real_t getDetilingRotation( void ) const {
		return m_fDetilingRotation;
	}

	void setDetilingShift( const real_t fDetilingShift );
	real_t getDetilingShift( void ) const {
		return m_fDetilingShift;
	}

	void setErosionEnabled( bool bValue );
	bool getErosionEnabled( void ) const {
		return m_bErosionEnabled;
	}
	void setErosionHardness( real_t fValue );
	real_t getErosionHardness( void ) const {
		return m_fErosionHardness;
	}
	void setErosionHydraulicErodibility( real_t fValue );
	real_t getErosionHydraulicErodibility( void ) const {
		return m_fErosionHydraulicErodibility;
	}
	void setErosionWindErodibility( real_t fValue );
	real_t getErosionWindErodibility( void ) const {
		return m_fErosionWindErodibility;
	}
	void setErosionWaveErodibility( real_t fValue );
	real_t getErosionWaveErodibility( void ) const {
		return m_fErosionWaveErodibility;
	}
	void setErosionWeatheringSusceptibility( real_t fValue );
	real_t getErosionWeatheringSusceptibility( void ) const {
		return m_fErosionWeatheringSusceptibility;
	}
	void setErosionOxidationSusceptibility( real_t fValue );
	real_t getErosionOxidationSusceptibility( void ) const {
		return m_fErosionOxidationSusceptibility;
	}
	void setErosionSaltSusceptibility( real_t fValue );
	real_t getErosionSaltSusceptibility( void ) const {
		return m_fErosionSaltSusceptibility;
	}
	void setErosionWetDrySusceptibility( real_t fValue );
	real_t getErosionWetDrySusceptibility( void ) const {
		return m_fErosionWetDrySusceptibility;
	}
	void setErosionInfiltrationMultiplier( real_t fValue );
	real_t getErosionInfiltrationMultiplier( void ) const {
		return m_fErosionInfiltrationMultiplier;
	}
	void setErosionTrafficErodibility( real_t fValue );
	real_t getErosionTrafficErodibility( void ) const {
		return m_fErosionTrafficErodibility;
	}
	void setErosionCohesion( real_t fValue );
	real_t getErosionCohesion( void ) const {
		return m_fErosionCohesion;
	}
	void setErosionGrainSize( real_t fValue );
	real_t getErosionGrainSize( void ) const {
		return m_fErosionGrainSize;
	}
	void setErosionExposedTextureId( int32_t nValue );
	int32_t getErosionExposedTextureId( void ) const {
		return m_nErosionExposedTextureId;
	}
	void setErosionWeatheredTextureId( int32_t nValue );
	int32_t getErosionWeatheredTextureId( void ) const {
		return m_nErosionWeatheredTextureId;
	}
	void setErosionOxidizedTextureId( int32_t nValue );
	int32_t getErosionOxidizedTextureId( void ) const {
		return m_nErosionOxidizedTextureId;
	}
	void setErosionCoarseDepositTextureId( int32_t nValue );
	int32_t getErosionCoarseDepositTextureId( void ) const {
		return m_nErosionCoarseDepositTextureId;
	}
	void setErosionFineDepositTextureId( int32_t nValue );
	int32_t getErosionFineDepositTextureId( void ) const {
		return m_nErosionFineDepositTextureId;
	}
	void setErosionSaltWeatheredTextureId( int32_t nValue );
	int32_t getErosionSaltWeatheredTextureId( void ) const {
		return m_nErosionSaltWeatheredTextureId;
	}
	void setErosionPavementTextureId( int32_t nValue );
	int32_t getErosionPavementTextureId( void ) const {
		return m_nErosionPavementTextureId;
	}
	void setErosionVarnishedTextureId( int32_t nValue );
	int32_t getErosionVarnishedTextureId( void ) const {
		return m_nErosionVarnishedTextureId;
	}

protected:
	void _validate_property( PropertyInfo &rProperty ) const;
	static void _bind_methods( void );
};

using Terrain3DTextureAsset = T3DTextureAsset;

#endif // TERRAIN3D_TEXTURE_CLASS_H
