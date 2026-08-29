// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "assets/terrain_3d_render_world_material_resource.h"

#include <godot_cpp/core/class_db.hpp>

#include <string.h>

using namespace godot;

static EA_FORCE_INLINE float t3dRwMaterialClamp01( const float fValue ) {
	return fValue < 0.0f ? 0.0f : ( fValue > 1.0f ? 1.0f : fValue );
}

static uint16_t t3dRwMaterialFloatToHalf( const float fValue ) {
	uint32_t bits = 0u;
	memcpy( &bits, &fValue, sizeof( bits ) );
	const uint32_t sign = ( bits >> 16u ) & 0x8000u;
	int32_t exponent = ( int32_t )( ( bits >> 23u ) & 0xffu ) - 127 + 15;
	uint32_t mantissa = bits & 0x7fffffu;
	if ( exponent <= 0 ) {
		if ( exponent < -10 ) {
			return ( uint16_t )( sign );
		}
		mantissa = ( mantissa | 0x800000u ) >> ( uint32_t )( 1 - exponent );
		return ( uint16_t )( sign | ( ( mantissa + 0x1000u ) >> 13u ) );
	}
	if ( exponent >= 31 ) {
		return ( uint16_t )( sign | 0x7c00u );
	}
	return ( uint16_t )( sign | ( ( uint32_t )( exponent ) << 10u ) | ( ( mantissa + 0x1000u ) >> 13u ) );
}

static EA_FORCE_INLINE uint32_t t3dRwMaterialPackHalf2( const float fX, const float fY ) {
	return ( uint32_t )( t3dRwMaterialFloatToHalf( fX ) ) | ( ( uint32_t )( t3dRwMaterialFloatToHalf( fY ) ) << 16u );
}

void Terrain3DRenderWorldMaterial::_bind_methods( void ) {
#define T3D_RW_BIND_PROPERTY( method_name, property_name )                                                                                 \
	ClassDB::bind_method( D_METHOD( #method_name, property_name ), &Terrain3DRenderWorldMaterial::method_name )
	ClassDB::bind_method( D_METHOD( "set_albedo_texture", "texture" ), &Terrain3DRenderWorldMaterial::setAlbedoTexture );
	ClassDB::bind_method( D_METHOD( "get_albedo_texture" ), &Terrain3DRenderWorldMaterial::getAlbedoTexture );
	ClassDB::bind_method( D_METHOD( "set_normal_texture", "texture" ), &Terrain3DRenderWorldMaterial::setNormalTexture );
	ClassDB::bind_method( D_METHOD( "get_normal_texture" ), &Terrain3DRenderWorldMaterial::getNormalTexture );
	ClassDB::bind_method( D_METHOD( "set_surface_texture", "texture" ), &Terrain3DRenderWorldMaterial::setSurfaceTexture );
	ClassDB::bind_method( D_METHOD( "get_surface_texture" ), &Terrain3DRenderWorldMaterial::getSurfaceTexture );
	ClassDB::bind_method( D_METHOD( "set_height_texture", "texture" ), &Terrain3DRenderWorldMaterial::setHeightTexture );
	ClassDB::bind_method( D_METHOD( "get_height_texture" ), &Terrain3DRenderWorldMaterial::getHeightTexture );
	ClassDB::bind_method( D_METHOD( "set_base_color", "color" ), &Terrain3DRenderWorldMaterial::setBaseColor );
	ClassDB::bind_method( D_METHOD( "get_base_color" ), &Terrain3DRenderWorldMaterial::getBaseColor );
	ClassDB::bind_method( D_METHOD( "set_uv_scale", "scale" ), &Terrain3DRenderWorldMaterial::setUvScale );
	ClassDB::bind_method( D_METHOD( "get_uv_scale" ), &Terrain3DRenderWorldMaterial::getUvScale );
	ClassDB::bind_method( D_METHOD( "set_uv_offset", "offset" ), &Terrain3DRenderWorldMaterial::setUvOffset );
	ClassDB::bind_method( D_METHOD( "get_uv_offset" ), &Terrain3DRenderWorldMaterial::getUvOffset );
	ClassDB::bind_method( D_METHOD( "set_roughness", "value" ), &Terrain3DRenderWorldMaterial::setRoughness );
	ClassDB::bind_method( D_METHOD( "get_roughness" ), &Terrain3DRenderWorldMaterial::getRoughness );
	ClassDB::bind_method( D_METHOD( "set_metallic", "value" ), &Terrain3DRenderWorldMaterial::setMetallic );
	ClassDB::bind_method( D_METHOD( "get_metallic" ), &Terrain3DRenderWorldMaterial::getMetallic );
	ClassDB::bind_method( D_METHOD( "set_normal_scale", "value" ), &Terrain3DRenderWorldMaterial::setNormalScale );
	ClassDB::bind_method( D_METHOD( "get_normal_scale" ), &Terrain3DRenderWorldMaterial::getNormalScale );
	ClassDB::bind_method( D_METHOD( "set_alpha_scissor_threshold", "value" ), &Terrain3DRenderWorldMaterial::setAlphaScissorThreshold );
	ClassDB::bind_method( D_METHOD( "get_alpha_scissor_threshold" ), &Terrain3DRenderWorldMaterial::getAlphaScissorThreshold );
	ClassDB::bind_method( D_METHOD( "set_alpha_scissor", "enabled" ), &Terrain3DRenderWorldMaterial::setAlphaScissor );
	ClassDB::bind_method( D_METHOD( "is_alpha_scissor" ), &Terrain3DRenderWorldMaterial::isAlphaScissor );
	ClassDB::bind_method( D_METHOD( "set_two_sided", "enabled" ), &Terrain3DRenderWorldMaterial::setTwoSided );
	ClassDB::bind_method( D_METHOD( "is_two_sided" ), &Terrain3DRenderWorldMaterial::isTwoSided );
	ClassDB::bind_method( D_METHOD( "set_foliage", "enabled" ), &Terrain3DRenderWorldMaterial::setFoliage );
	ClassDB::bind_method( D_METHOD( "is_foliage" ), &Terrain3DRenderWorldMaterial::isFoliage );
	ClassDB::bind_method( D_METHOD( "set_cast_shadows", "enabled" ), &Terrain3DRenderWorldMaterial::setCastShadows );
	ClassDB::bind_method( D_METHOD( "is_cast_shadows" ), &Terrain3DRenderWorldMaterial::isCastShadows );
	ClassDB::bind_method( D_METHOD( "set_receive_shadows", "enabled" ), &Terrain3DRenderWorldMaterial::setReceiveShadows );
	ClassDB::bind_method( D_METHOD( "is_receive_shadows" ), &Terrain3DRenderWorldMaterial::isReceiveShadows );
	ClassDB::bind_method( D_METHOD( "set_sampler_class", "sampler_class" ), &Terrain3DRenderWorldMaterial::setSamplerClass );
	ClassDB::bind_method( D_METHOD( "get_sampler_class" ), &Terrain3DRenderWorldMaterial::getSamplerClass );
	ClassDB::bind_method( D_METHOD( "set_render_tier", "tier" ), &Terrain3DRenderWorldMaterial::setRenderTier );
	ClassDB::bind_method( D_METHOD( "get_render_tier" ), &Terrain3DRenderWorldMaterial::getRenderTier );
#undef T3D_RW_BIND_PROPERTY

	ADD_GROUP( "Textures", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
	        "set_albedo_texture",
	        "get_albedo_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
	        "set_normal_texture",
	        "get_normal_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "surface_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
	        "set_surface_texture",
	        "get_surface_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "height_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ),
	        "set_height_texture",
	        "get_height_texture" );
	ADD_GROUP( "Surface", "" );
	ADD_PROPERTY( PropertyInfo( Variant::COLOR, "base_color" ), "set_base_color", "get_base_color" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01" ), "set_roughness", "get_roughness" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0,1,0.01" ), "set_metallic", "get_metallic" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "normal_scale", PROPERTY_HINT_RANGE, "0,4,0.01" ), "set_normal_scale", "get_normal_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2, "uv_scale" ), "set_uv_scale", "get_uv_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR2, "uv_offset" ), "set_uv_offset", "get_uv_offset" );
	ADD_GROUP( "Pipeline", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "alpha_scissor" ), "set_alpha_scissor", "is_alpha_scissor" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "alpha_scissor_threshold", PROPERTY_HINT_RANGE, "0,1,0.01" ),
	        "set_alpha_scissor_threshold",
	        "get_alpha_scissor_threshold" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "two_sided" ), "set_two_sided", "is_two_sided" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "foliage" ), "set_foliage", "is_foliage" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "cast_shadows" ), "set_cast_shadows", "is_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "receive_shadows" ), "set_receive_shadows", "is_receive_shadows" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "sampler_class", PROPERTY_HINT_ENUM, "Repeat,Clamp" ), "set_sampler_class", "get_sampler_class" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "render_tier", PROPERTY_HINT_ENUM, "Auto,World,Clutter,Foliage" ),
	        "set_render_tier",
	        "get_render_tier" );
}

void Terrain3DRenderWorldMaterial::setAlbedoTexture( const Ref<Texture2D> &rValue ) {
	if ( m_albedoTexture == rValue ) {
		return;
	}
	m_albedoTexture = rValue;
	emit_changed();
}

void Terrain3DRenderWorldMaterial::setNormalTexture( const Ref<Texture2D> &rValue ) {
	if ( m_normalTexture == rValue ) {
		return;
	}
	m_normalTexture = rValue;
	emit_changed();
}

void Terrain3DRenderWorldMaterial::setSurfaceTexture( const Ref<Texture2D> &rValue ) {
	if ( m_surfaceTexture == rValue ) {
		return;
	}
	m_surfaceTexture = rValue;
	emit_changed();
}

void Terrain3DRenderWorldMaterial::setHeightTexture( const Ref<Texture2D> &rValue ) {
	if ( m_heightTexture == rValue ) {
		return;
	}
	m_heightTexture = rValue;
	emit_changed();
}

void Terrain3DRenderWorldMaterial::setBaseColor( const Color &rColor ) {
	if ( m_baseColor != rColor ) {
		m_baseColor = rColor;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setUvScale( const Vector2 &rScale ) {
	if ( m_uvScale != rScale ) {
		m_uvScale = rScale;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setUvOffset( const Vector2 &rOffset ) {
	if ( m_uvOffset != rOffset ) {
		m_uvOffset = rOffset;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setRoughness( const float fValue ) {
	const float value = t3dRwMaterialClamp01( fValue );
	if ( m_fRoughness != value ) {
		m_fRoughness = value;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setMetallic( const float fValue ) {
	const float value = t3dRwMaterialClamp01( fValue );
	if ( m_fMetallic != value ) {
		m_fMetallic = value;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setNormalScale( const float fValue ) {
	const float value = MAX( fValue, 0.0f );
	if ( m_fNormalScale != value ) {
		m_fNormalScale = value;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setAlphaScissorThreshold( const float fValue ) {
	const float value = t3dRwMaterialClamp01( fValue );
	if ( m_fAlphaScissorThreshold != value ) {
		m_fAlphaScissorThreshold = value;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setAlphaScissor( const bool bEnabled ) {
	if ( m_bAlphaScissor != bEnabled ) {
		m_bAlphaScissor = bEnabled;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setTwoSided( const bool bEnabled ) {
	if ( m_bTwoSided != bEnabled ) {
		m_bTwoSided = bEnabled;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setFoliage( const bool bEnabled ) {
	if ( m_bFoliage != bEnabled ) {
		m_bFoliage = bEnabled;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setCastShadows( const bool bEnabled ) {
	if ( m_bCastShadows != bEnabled ) {
		m_bCastShadows = bEnabled;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setReceiveShadows( const bool bEnabled ) {
	if ( m_bReceiveShadows != bEnabled ) {
		m_bReceiveShadows = bEnabled;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setSamplerClass( const int32_t iClass ) {
	const int32_t value = iClass == ( int32_t )( T3D_RW_SAMPLER_CLAMP ) ? iClass : ( int32_t )( T3D_RW_SAMPLER_REPEAT );
	if ( m_iSamplerClass != value ) {
		m_iSamplerClass = value;
		emit_changed();
	}
}
void Terrain3DRenderWorldMaterial::setRenderTier( const int32_t iTier ) {
	const int32_t value = iTier >= 0 && iTier <= ( int32_t )( T3D_RW_IMPORT_FOLIAGE ) ? iTier : ( int32_t )( T3D_RW_IMPORT_AUTO );
	if ( m_iRenderTier != value ) {
		m_iRenderTier = value;
		emit_changed();
	}
}

uint32_t Terrain3DRenderWorldMaterial::getMaterialFlags( void ) const {
	uint32_t flags = 0u;
	if ( m_bAlphaScissor ) {
		flags |= T3D_RW_MATERIAL_FLAG_ALPHA_SCISSOR;
	}
	if ( m_bFoliage ) {
		flags |= T3D_RW_MATERIAL_FLAG_FOLIAGE;
	}
	if ( m_bCastShadows ) {
		flags |= T3D_RW_MATERIAL_FLAG_CAST_SHADOWS;
	}
	if ( m_bReceiveShadows ) {
		flags |= T3D_RW_MATERIAL_FLAG_RECEIVE_SHADOWS;
	}
	if ( m_bTwoSided ) {
		flags |= T3D_RW_MATERIAL_FLAG_TWO_SIDED;
	}
	return flags;
}

bool Terrain3DRenderWorldMaterial::compileRecordCold( const t3dRenderWorldMaterialVtBindings_t &rVt, T3DMaterialRecord &rRecord ) const {
	memset( &rRecord, 0, sizeof( rRecord ) );
	rRecord.nAlbedoVT = rVt.m_nAlbedoVt;
	rRecord.nNormalVT = rVt.m_nNormalVt;
	rRecord.nSurfaceVT = rVt.m_nSurfaceVt;
	rRecord.nHeightVT = rVt.m_nHeightVt;
	rRecord.vBaseColor[0] = m_baseColor.r;
	rRecord.vBaseColor[1] = m_baseColor.g;
	rRecord.vBaseColor[2] = m_baseColor.b;
	rRecord.vBaseColor[3] = m_baseColor.a;
	rRecord.vUvScaleBias[0] = m_uvScale.x;
	rRecord.vUvScaleBias[1] = m_uvScale.y;
	rRecord.vUvScaleBias[2] = m_uvOffset.x;
	rRecord.vUvScaleBias[3] = m_uvOffset.y;
	rRecord.nFlags = getMaterialFlags();
	rRecord.nSamplerClass = ( uint32_t )( m_iSamplerClass );
	rRecord.nPackedScalars0 = t3dRwMaterialPackHalf2( m_fRoughness, m_fMetallic );
	rRecord.nPackedScalars1 = t3dRwMaterialPackHalf2( m_fNormalScale, m_fAlphaScissorThreshold );
	return true;
}
