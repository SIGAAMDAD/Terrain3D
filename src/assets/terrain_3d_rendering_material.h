// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDERING_MATERIAL_H
#define TERRAIN3D_RENDERING_MATERIAL_H

#include "assets/terrain_3d_world_material_resource.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

class Terrain3DRenderingMaterial : public Terrain3DWorldMaterial {
	GDCLASS( Terrain3DRenderingMaterial, Terrain3DWorldMaterial );

public:
	typedef enum t3dSurfaceTextureLayout_e {
		SURFACE_TEXTURE_ORM = 0,
		SURFACE_TEXTURE_ROUGHNESS_R = 1,
		SURFACE_TEXTURE_ROUGHNESS_G = 2,
		SURFACE_TEXTURE_ROUGHNESS_B = 3,
		SURFACE_TEXTURE_ROUGHNESS_A = 4,
	} t3dSurfaceTextureLayout_t;

private:
	godot::Color m_emission = godot::Color( 0.0f, 0.0f, 0.0f, 1.0f );
	float m_fEmissionEnergy = 1.0f;
	float m_fAoStrength = 1.0f;
	int32_t m_iTextureFilter = 3;
	int32_t m_iSurfaceTextureLayout = SURFACE_TEXTURE_ORM;
	bool m_bUnshaded = false;
	bool m_bVertexColor = false;
	bool m_bVertexColorSrgb = false;
	bool m_bDirectOrmChannels = false;
	bool m_bStylizedSun = false;
	float m_fAlbedoSaturation = 1.0f;
	float m_fDielectricSpecular = 0.04f;
	uint64_t m_nVtStableKey = 0u;

protected:
	static void _bind_methods( void );

public:
	Terrain3DRenderingMaterial( void ) = default;
	~Terrain3DRenderingMaterial( void ) = default;

	void setEmission( const godot::Color &rColor );
	EA_FORCE_INLINE godot::Color getEmission( void ) const {
		return m_emission;
	}
	void setEmissionEnergy( float fValue );
	EA_FORCE_INLINE float getEmissionEnergy( void ) const {
		return m_fEmissionEnergy;
	}
	void setAoStrength( float fValue );
	EA_FORCE_INLINE float getAoStrength( void ) const {
		return m_fAoStrength;
	}
	void setTextureFilter( int32_t iValue );
	EA_FORCE_INLINE int32_t getTextureFilter( void ) const {
		return m_iTextureFilter;
	}
	void setSurfaceTextureLayout( int32_t iValue );
	EA_FORCE_INLINE int32_t getSurfaceTextureLayout( void ) const {
		return m_iSurfaceTextureLayout;
	}
	void setUnshaded( bool bEnabled );
	EA_FORCE_INLINE bool isUnshaded( void ) const {
		return m_bUnshaded;
	}
	void setVertexColor( bool bEnabled );
	EA_FORCE_INLINE bool isVertexColor( void ) const {
		return m_bVertexColor;
	}
	void setVertexColorSrgb( bool bEnabled );
	EA_FORCE_INLINE bool isVertexColorSrgb( void ) const {
		return m_bVertexColorSrgb;
	}
	void setDirectOrmChannels( bool bEnabled );
	EA_FORCE_INLINE bool usesDirectOrmChannels( void ) const {
		return m_bDirectOrmChannels;
	}
	void setAlbedoSaturation( float fValue );
	EA_FORCE_INLINE float getAlbedoSaturation( void ) const {
		return m_fAlbedoSaturation;
	}
	void setStylizedSun( bool bEnabled );
	EA_FORCE_INLINE bool usesStylizedSun( void ) const {
		return m_bStylizedSun;
	}
	void setDielectricSpecular( float fValue );
	EA_FORCE_INLINE float getDielectricSpecular( void ) const {
		return m_fDielectricSpecular;
	}

	void setVtStableKeyHex( const godot::String &rValue );
	godot::String getVtStableKeyHex( void ) const;
	void setVtStableKey( uint64_t nKey );
	uint64_t getVtStableKey( void ) const;
	bool ensureVtStableKeyCold( const godot::String &rOwnerIdentity, const godot::String &rRoleIdentity );

	bool importBaseMaterialCold( const godot::Ref<godot::Material> &rMaterial );
	bool compileInstanceRecordCold( t3dRdInstanceMaterial_t &rRecord ) const;
	bool compileVtRecordCold( const t3dRenderingMaterialVtBindings_t &rVt, t3dRenderingMaterialGpu_t &rRecord ) const override;
	bool isOneSubmitCompatible( void ) const;
};

#endif // TERRAIN3D_RENDERING_MATERIAL_H
