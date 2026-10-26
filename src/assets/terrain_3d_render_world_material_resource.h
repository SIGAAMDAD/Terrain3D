// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_MATERIAL_RESOURCE_H
#define TERRAIN3D_RENDER_WORLD_MATERIAL_RESOURCE_H

#include "authoring/render_world/terrain_3d_render_world_authoring.h"
#include "core/terrain_3d_compiler.h"
#include "rendering/rd/vt/terrain_3d_vt_contract.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>

class Terrain3DRenderWorldMaterial : public godot::Resource {
	GDCLASS( Terrain3DRenderWorldMaterial, godot::Resource );

private:
	godot::Ref<godot::Texture2D> m_albedoTexture;
	godot::Ref<godot::Texture2D> m_normalTexture;
	godot::Ref<godot::Texture2D> m_surfaceTexture;
	godot::Ref<godot::Texture2D> m_heightTexture;
	godot::Color m_baseColor = godot::Color( 1.0f, 1.0f, 1.0f, 1.0f );
	godot::Vector2 m_uvScale = godot::Vector2( 1.0f, 1.0f );
	godot::Vector2 m_uvOffset;
	float m_fRoughness = 1.0f;
	float m_fMetallic = 0.0f;
	float m_fNormalScale = 1.0f;
	float m_fAlphaScissorThreshold = 0.5f;
	int32_t m_iSamplerClass = ( int32_t )( T3D_RW_SAMPLER_REPEAT );
	int32_t m_iRenderTier = ( int32_t )( T3D_RW_IMPORT_AUTO );
	bool m_bAlphaScissor = false;
	bool m_bTwoSided = false;
	bool m_bFoliage = false;
	bool m_bCastShadows = true;
	bool m_bReceiveShadows = true;

	void onTextureChanged( void );
	bool referencesTexture( const godot::Ref<godot::Texture2D> &rTexture ) const;
	void disconnectTextureSignalIfUnused( const godot::Ref<godot::Texture2D> &rTexture );
	void connectTextureSignal( const godot::Ref<godot::Texture2D> &rTexture );
	void disconnectAllTextureSignals( void );

protected:
	static void _bind_methods( void );

public:
	Terrain3DRenderWorldMaterial( void ) = default;
	~Terrain3DRenderWorldMaterial( void );

	void setAlbedoTexture( const godot::Ref<godot::Texture2D> &rTexture );
	EA_FORCE_INLINE godot::Ref<godot::Texture2D> getAlbedoTexture( void ) const {
		return m_albedoTexture;
	}
	void setNormalTexture( const godot::Ref<godot::Texture2D> &rTexture );
	EA_FORCE_INLINE godot::Ref<godot::Texture2D> getNormalTexture( void ) const {
		return m_normalTexture;
	}
	void setSurfaceTexture( const godot::Ref<godot::Texture2D> &rTexture );
	EA_FORCE_INLINE godot::Ref<godot::Texture2D> getSurfaceTexture( void ) const {
		return m_surfaceTexture;
	}
	void setHeightTexture( const godot::Ref<godot::Texture2D> &rTexture );
	EA_FORCE_INLINE godot::Ref<godot::Texture2D> getHeightTexture( void ) const {
		return m_heightTexture;
	}

	void setBaseColor( const godot::Color &rColor );
	EA_FORCE_INLINE godot::Color getBaseColor( void ) const {
		return m_baseColor;
	}
	void setUvScale( const godot::Vector2 &rScale );
	EA_FORCE_INLINE godot::Vector2 getUvScale( void ) const {
		return m_uvScale;
	}
	void setUvOffset( const godot::Vector2 &rOffset );
	EA_FORCE_INLINE godot::Vector2 getUvOffset( void ) const {
		return m_uvOffset;
	}
	void setRoughness( float fValue );
	EA_FORCE_INLINE float getRoughness( void ) const {
		return m_fRoughness;
	}
	void setMetallic( float fValue );
	EA_FORCE_INLINE float getMetallic( void ) const {
		return m_fMetallic;
	}
	void setNormalScale( float fValue );
	EA_FORCE_INLINE float getNormalScale( void ) const {
		return m_fNormalScale;
	}
	void setAlphaScissorThreshold( float fValue );
	EA_FORCE_INLINE float getAlphaScissorThreshold( void ) const {
		return m_fAlphaScissorThreshold;
	}

	void setAlphaScissor( bool bEnabled );
	EA_FORCE_INLINE bool isAlphaScissor( void ) const {
		return m_bAlphaScissor;
	}
	void setTwoSided( bool bEnabled );
	EA_FORCE_INLINE bool isTwoSided( void ) const {
		return m_bTwoSided;
	}
	void setFoliage( bool bEnabled );
	EA_FORCE_INLINE bool isFoliage( void ) const {
		return m_bFoliage;
	}
	void setCastShadows( bool bEnabled );
	EA_FORCE_INLINE bool isCastShadows( void ) const {
		return m_bCastShadows;
	}
	void setReceiveShadows( bool bEnabled );
	EA_FORCE_INLINE bool isReceiveShadows( void ) const {
		return m_bReceiveShadows;
	}
	void setSamplerClass( int32_t iClass );
	EA_FORCE_INLINE int32_t getSamplerClass( void ) const {
		return m_iSamplerClass;
	}
	void setRenderTier( int32_t iTier );
	EA_FORCE_INLINE int32_t getRenderTier( void ) const {
		return m_iRenderTier;
	}

	virtual bool compileVtRecordCold( const t3dRenderingMaterialVtBindings_t &rVt, t3dRenderingMaterialGpu_t &rRecord ) const;
	uint32_t getMaterialFlags( void ) const;
};

#endif // TERRAIN3D_RENDER_WORLD_MATERIAL_RESOURCE_H
