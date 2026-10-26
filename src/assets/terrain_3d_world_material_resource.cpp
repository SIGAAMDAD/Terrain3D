// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "assets/terrain_3d_world_material_resource.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include <string.h>

using namespace godot;

static EA_FORCE_INLINE float t3dWorldMaterialClamp01( const float fValue ) {
	return fValue < 0.0f ? 0.0f : ( fValue > 1.0f ? 1.0f : fValue );
}

static uint16_t t3dWorldMaterialFloatToHalf( const float fValue ) {
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

static EA_FORCE_INLINE uint32_t t3dWorldMaterialPackUnorm8( const float fValue ) {
	const float value = t3dWorldMaterialClamp01( fValue );
	return ( uint32_t )( value * 255.0f + 0.5f );
}

static EA_FORCE_INLINE uint32_t t3dWorldMaterialPackVtScalars0(
        const float fMetallic, const float fRoughness, const float fAo, const float fAlphaScissor ) {
	return ( t3dWorldMaterialPackUnorm8( fMetallic ) << T3D_VT_MATERIAL_METALLIC_SHIFT )
	        | ( t3dWorldMaterialPackUnorm8( fRoughness ) << T3D_VT_MATERIAL_ROUGHNESS_SHIFT )
	        | ( t3dWorldMaterialPackUnorm8( fAo ) << T3D_VT_MATERIAL_AO_SHIFT )
	        | ( t3dWorldMaterialPackUnorm8( fAlphaScissor ) << T3D_VT_MATERIAL_ALPHA_SCISSOR_SHIFT );
}

static EA_FORCE_INLINE uint32_t t3dWorldMaterialPackVtScalars1(
        const float fNormalScale, const uint32_t nSurfaceLayout, const uint32_t nTextureFilter ) {
	return ( uint32_t )( t3dWorldMaterialFloatToHalf( fNormalScale ) )
	        | ( ( nSurfaceLayout << T3D_VT_MATERIAL_SURFACE_LAYOUT_SHIFT ) & T3D_VT_MATERIAL_SURFACE_LAYOUT_MASK )
	        | ( ( nTextureFilter << T3D_VT_MATERIAL_FILTER_SHIFT ) & T3D_VT_MATERIAL_FILTER_MASK );
}

Terrain3DWorldMaterial::~Terrain3DWorldMaterial( void ) {
	disconnectAllTextureSignals();
}

void Terrain3DWorldMaterial::onTextureChanged( void ) {
	emit_changed();
}

bool Terrain3DWorldMaterial::referencesTexture( const Ref<Texture2D> &rTexture ) const {
	return rTexture.is_valid()
	        && ( m_albedoTexture == rTexture || m_normalTexture == rTexture || m_surfaceTexture == rTexture
	                || m_heightTexture == rTexture );
}

void Terrain3DWorldMaterial::disconnectTextureSignalIfUnused( const Ref<Texture2D> &rTexture ) {
	if ( rTexture.is_null() || referencesTexture( rTexture ) ) {
		return;
	}
	const Callable callback = callable_mp( this, &Terrain3DWorldMaterial::onTextureChanged );
	if ( rTexture->is_connected( "changed", callback ) ) {
		rTexture->disconnect( "changed", callback );
	}
}

void Terrain3DWorldMaterial::connectTextureSignal( const Ref<Texture2D> &rTexture ) {
	if ( rTexture.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &Terrain3DWorldMaterial::onTextureChanged );
	if ( !rTexture->is_connected( "changed", callback ) ) {
		rTexture->connect( "changed", callback );
	}
}

void Terrain3DWorldMaterial::disconnectAllTextureSignals( void ) {
	const Callable callback = callable_mp( this, &Terrain3DWorldMaterial::onTextureChanged );
	Ref<Texture2D> textures[4] = { m_albedoTexture, m_normalTexture, m_surfaceTexture, m_heightTexture };
	for ( uint32_t iTexture = 0u; iTexture < 4u; ++iTexture ) {
		const Ref<Texture2D> &texture = textures[iTexture];
		if ( texture.is_valid() && texture->is_connected( "changed", callback ) ) {
			texture->disconnect( "changed", callback );
		}
	}
}

void Terrain3DWorldMaterial::_bind_methods( void ) {
#define T3D_WORLD_BIND_PROPERTY( method_name, property_name )                                                                                 \
	ClassDB::bind_method( D_METHOD( #method_name, property_name ), &Terrain3DWorldMaterial::method_name )
	ClassDB::bind_method( D_METHOD( "set_albedo_texture", "texture" ), &Terrain3DWorldMaterial::setAlbedoTexture );
	ClassDB::bind_method( D_METHOD( "get_albedo_texture" ), &Terrain3DWorldMaterial::getAlbedoTexture );
	ClassDB::bind_method( D_METHOD( "set_normal_texture", "texture" ), &Terrain3DWorldMaterial::setNormalTexture );
	ClassDB::bind_method( D_METHOD( "get_normal_texture" ), &Terrain3DWorldMaterial::getNormalTexture );
	ClassDB::bind_method( D_METHOD( "set_surface_texture", "texture" ), &Terrain3DWorldMaterial::setSurfaceTexture );
	ClassDB::bind_method( D_METHOD( "get_surface_texture" ), &Terrain3DWorldMaterial::getSurfaceTexture );
	ClassDB::bind_method( D_METHOD( "set_height_texture", "texture" ), &Terrain3DWorldMaterial::setHeightTexture );
	ClassDB::bind_method( D_METHOD( "get_height_texture" ), &Terrain3DWorldMaterial::getHeightTexture );
	ClassDB::bind_method( D_METHOD( "set_base_color", "color" ), &Terrain3DWorldMaterial::setBaseColor );
	ClassDB::bind_method( D_METHOD( "get_base_color" ), &Terrain3DWorldMaterial::getBaseColor );
	ClassDB::bind_method( D_METHOD( "set_uv_scale", "scale" ), &Terrain3DWorldMaterial::setUvScale );
	ClassDB::bind_method( D_METHOD( "get_uv_scale" ), &Terrain3DWorldMaterial::getUvScale );
	ClassDB::bind_method( D_METHOD( "set_uv_offset", "offset" ), &Terrain3DWorldMaterial::setUvOffset );
	ClassDB::bind_method( D_METHOD( "get_uv_offset" ), &Terrain3DWorldMaterial::getUvOffset );
	ClassDB::bind_method( D_METHOD( "set_roughness", "value" ), &Terrain3DWorldMaterial::setRoughness );
	ClassDB::bind_method( D_METHOD( "get_roughness" ), &Terrain3DWorldMaterial::getRoughness );
	ClassDB::bind_method( D_METHOD( "set_metallic", "value" ), &Terrain3DWorldMaterial::setMetallic );
	ClassDB::bind_method( D_METHOD( "get_metallic" ), &Terrain3DWorldMaterial::getMetallic );
	ClassDB::bind_method( D_METHOD( "set_normal_scale", "value" ), &Terrain3DWorldMaterial::setNormalScale );
	ClassDB::bind_method( D_METHOD( "get_normal_scale" ), &Terrain3DWorldMaterial::getNormalScale );
	ClassDB::bind_method( D_METHOD( "set_alpha_scissor_threshold", "value" ), &Terrain3DWorldMaterial::setAlphaScissorThreshold );
	ClassDB::bind_method( D_METHOD( "get_alpha_scissor_threshold" ), &Terrain3DWorldMaterial::getAlphaScissorThreshold );
	ClassDB::bind_method( D_METHOD( "set_alpha_scissor", "enabled" ), &Terrain3DWorldMaterial::setAlphaScissor );
	ClassDB::bind_method( D_METHOD( "is_alpha_scissor" ), &Terrain3DWorldMaterial::isAlphaScissor );
	ClassDB::bind_method( D_METHOD( "set_two_sided", "enabled" ), &Terrain3DWorldMaterial::setTwoSided );
	ClassDB::bind_method( D_METHOD( "is_two_sided" ), &Terrain3DWorldMaterial::isTwoSided );
	ClassDB::bind_method( D_METHOD( "set_foliage", "enabled" ), &Terrain3DWorldMaterial::setFoliage );
	ClassDB::bind_method( D_METHOD( "is_foliage" ), &Terrain3DWorldMaterial::isFoliage );
	ClassDB::bind_method( D_METHOD( "set_cast_shadows", "enabled" ), &Terrain3DWorldMaterial::setCastShadows );
	ClassDB::bind_method( D_METHOD( "is_cast_shadows" ), &Terrain3DWorldMaterial::isCastShadows );
	ClassDB::bind_method( D_METHOD( "set_receive_shadows", "enabled" ), &Terrain3DWorldMaterial::setReceiveShadows );
	ClassDB::bind_method( D_METHOD( "is_receive_shadows" ), &Terrain3DWorldMaterial::isReceiveShadows );
	ClassDB::bind_method( D_METHOD( "set_sampler_class", "sampler_class" ), &Terrain3DWorldMaterial::setSamplerClass );
	ClassDB::bind_method( D_METHOD( "get_sampler_class" ), &Terrain3DWorldMaterial::getSamplerClass );
	ClassDB::bind_method( D_METHOD( "set_render_tier", "tier" ), &Terrain3DWorldMaterial::setRenderTier );
	ClassDB::bind_method( D_METHOD( "get_render_tier" ), &Terrain3DWorldMaterial::getRenderTier );
#undef T3D_WORLD_BIND_PROPERTY

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

void Terrain3DWorldMaterial::setAlbedoTexture( const Ref<Texture2D> &rValue ) {
	if ( m_albedoTexture == rValue ) {
		return;
	}
	const Ref<Texture2D> previous = m_albedoTexture;
	m_albedoTexture = rValue;
	disconnectTextureSignalIfUnused( previous );
	connectTextureSignal( m_albedoTexture );
	emit_changed();
}

void Terrain3DWorldMaterial::setNormalTexture( const Ref<Texture2D> &rValue ) {
	if ( m_normalTexture == rValue ) {
		return;
	}
	const Ref<Texture2D> previous = m_normalTexture;
	m_normalTexture = rValue;
	disconnectTextureSignalIfUnused( previous );
	connectTextureSignal( m_normalTexture );
	emit_changed();
}

void Terrain3DWorldMaterial::setSurfaceTexture( const Ref<Texture2D> &rValue ) {
	if ( m_surfaceTexture == rValue ) {
		return;
	}
	const Ref<Texture2D> previous = m_surfaceTexture;
	m_surfaceTexture = rValue;
	disconnectTextureSignalIfUnused( previous );
	connectTextureSignal( m_surfaceTexture );
	emit_changed();
}

void Terrain3DWorldMaterial::setHeightTexture( const Ref<Texture2D> &rValue ) {
	if ( m_heightTexture == rValue ) {
		return;
	}
	const Ref<Texture2D> previous = m_heightTexture;
	m_heightTexture = rValue;
	disconnectTextureSignalIfUnused( previous );
	connectTextureSignal( m_heightTexture );
	emit_changed();
}

void Terrain3DWorldMaterial::setBaseColor( const Color &rColor ) {
	if ( m_baseColor != rColor ) {
		m_baseColor = rColor;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setUvScale( const Vector2 &rScale ) {
	if ( m_uvScale != rScale ) {
		m_uvScale = rScale;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setUvOffset( const Vector2 &rOffset ) {
	if ( m_uvOffset != rOffset ) {
		m_uvOffset = rOffset;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setRoughness( const float fValue ) {
	const float value = t3dWorldMaterialClamp01( fValue );
	if ( m_fRoughness != value ) {
		m_fRoughness = value;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setMetallic( const float fValue ) {
	const float value = t3dWorldMaterialClamp01( fValue );
	if ( m_fMetallic != value ) {
		m_fMetallic = value;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setNormalScale( const float fValue ) {
	const float value = MAX( fValue, 0.0f );
	if ( m_fNormalScale != value ) {
		m_fNormalScale = value;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setAlphaScissorThreshold( const float fValue ) {
	const float value = t3dWorldMaterialClamp01( fValue );
	if ( m_fAlphaScissorThreshold != value ) {
		m_fAlphaScissorThreshold = value;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setAlphaScissor( const bool bEnabled ) {
	if ( m_bAlphaScissor != bEnabled ) {
		m_bAlphaScissor = bEnabled;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setTwoSided( const bool bEnabled ) {
	if ( m_bTwoSided != bEnabled ) {
		m_bTwoSided = bEnabled;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setFoliage( const bool bEnabled ) {
	if ( m_bFoliage != bEnabled ) {
		m_bFoliage = bEnabled;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setCastShadows( const bool bEnabled ) {
	if ( m_bCastShadows != bEnabled ) {
		m_bCastShadows = bEnabled;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setReceiveShadows( const bool bEnabled ) {
	if ( m_bReceiveShadows != bEnabled ) {
		m_bReceiveShadows = bEnabled;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setSamplerClass( const int32_t iClass ) {
	const int32_t value = iClass == ( int32_t )( T3D_VT_SAMPLER_LINEAR_CLAMP ) ? iClass : ( int32_t )( T3D_VT_SAMPLER_LINEAR_REPEAT );
	if ( m_iSamplerClass != value ) {
		m_iSamplerClass = value;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setRenderTier( const int32_t iTier ) {
	const int32_t value = iTier >= 0 && iTier <= ( int32_t )( T3D_WORLD_IMPORT_FOLIAGE ) ? iTier : ( int32_t )( T3D_WORLD_IMPORT_AUTO );
	if ( m_iRenderTier != value ) {
		m_iRenderTier = value;
		emit_changed();
	}
}

uint32_t Terrain3DWorldMaterial::getMaterialFlags( void ) const {
	uint32_t flags = 0u;
	if ( m_bAlphaScissor ) {
		flags |= T3D_VT_MATERIAL_ALPHA_SCISSOR;
	}
	if ( m_bFoliage ) {
		flags |= T3D_VT_MATERIAL_FOLIAGE;
	}
	if ( m_bCastShadows ) {
		flags |= T3D_VT_MATERIAL_CAST_SHADOWS;
	}
	if ( m_bReceiveShadows ) {
		flags |= T3D_VT_MATERIAL_RECEIVE_SHADOWS;
	}
	if ( m_bTwoSided ) {
		flags |= T3D_VT_MATERIAL_TWO_SIDED;
	}
	return flags;
}

bool Terrain3DWorldMaterial::compileVtRecordCold(
        const t3dRenderingMaterialVtBindings_t &rVt, t3dRenderingMaterialGpu_t &rRecord ) const {
	memset( &rRecord, 0, sizeof( rRecord ) );
	rRecord.m_nVtAllocation = rVt.m_nVtAllocation;
	rRecord.m_nOverlayVtAllocation = rVt.m_nOverlayVtAllocation;
	rRecord.m_nFlags = getMaterialFlags();
	if ( m_normalTexture.is_valid() ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_NORMAL_MAPPING;
	}
	if ( m_heightTexture.is_valid() ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_HAS_HEIGHT;
	}
	if ( m_bAlphaScissor || m_baseColor.a < 1.0f ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_HAS_ALPHA;
	}
	rRecord.m_nPackedScalars0 = t3dWorldMaterialPackVtScalars0( m_fMetallic, m_fRoughness, 1.0f, m_fAlphaScissorThreshold );
	rRecord.m_vBaseColor[0] = ( float )( m_baseColor.r );
	rRecord.m_vBaseColor[1] = ( float )( m_baseColor.g );
	rRecord.m_vBaseColor[2] = ( float )( m_baseColor.b );
	rRecord.m_vBaseColor[3] = ( float )( m_baseColor.a );
	rRecord.m_vUvScaleBias[0] = ( float )( m_uvScale.x );
	rRecord.m_vUvScaleBias[1] = ( float )( m_uvScale.y );
	rRecord.m_vUvScaleBias[2] = ( float )( m_uvOffset.x );
	rRecord.m_vUvScaleBias[3] = ( float )( m_uvOffset.y );
	rRecord.m_nPackedEmission = 0u;
	rRecord.m_nPackedScalars1 = t3dWorldMaterialPackVtScalars1( m_fNormalScale, 0u, 3u );
	rRecord.m_nSamplerClass = ( uint32_t )( m_iSamplerClass );
	rRecord.m_nReserved = 0u;
	return true;
}
