// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "core/utility/terrain_3d_numeric.h"
#include "data/formats/terrain_3d_half.h"
#include "assets/resources/terrain_3d_world_material_resource.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include <string.h>

using namespace godot;

static EA_FORCE_INLINE uint32_t t3dWorldMaterialPackVtScalars0(
        const float fMetallic, const float fRoughness, const float fAo, const float fAlphaScissor ) {
	return ( t3dPackUnorm8Nearest( fMetallic ) << T3D_VT_MATERIAL_METALLIC_SHIFT )
	        | ( t3dPackUnorm8Nearest( fRoughness ) << T3D_VT_MATERIAL_ROUGHNESS_SHIFT )
	        | ( t3dPackUnorm8Nearest( fAo ) << T3D_VT_MATERIAL_AO_SHIFT )
	        | ( t3dPackUnorm8Nearest( fAlphaScissor ) << T3D_VT_MATERIAL_ALPHA_SCISSOR_SHIFT );
}

static EA_FORCE_INLINE uint32_t t3dWorldMaterialPackVtScalars1(
        const float fNormalScale, const uint32_t nSurfaceLayout, const uint32_t nTextureFilter ) {
	return ( uint32_t )( t3dHalfEncode( fNormalScale ) )
	        | ( ( nSurfaceLayout << T3D_VT_MATERIAL_SURFACE_LAYOUT_SHIFT ) & T3D_VT_MATERIAL_SURFACE_LAYOUT_MASK )
	        | ( ( nTextureFilter << T3D_VT_MATERIAL_FILTER_SHIFT ) & T3D_VT_MATERIAL_FILTER_MASK );
}

Terrain3DWorldMaterial::~Terrain3DWorldMaterial( void ) {
	disconnectAllTextureSignals();
}

void Terrain3DWorldMaterial::onTextureChanged( void ) {
	advanceVtContentGeneration();
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

	ADD_GROUP( "Textures", "" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setAlbedoTexture, getAlbedoTexture, "texture",
	                   PropertyInfo( Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ), "set_albedo_texture", "get_albedo_texture" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setNormalTexture, getNormalTexture, "texture",
	                   PropertyInfo( Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ), "set_normal_texture", "get_normal_texture" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setSurfaceTexture, getSurfaceTexture, "texture",
	                   PropertyInfo( Variant::OBJECT, "surface_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ), "set_surface_texture", "get_surface_texture" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setHeightTexture, getHeightTexture, "texture",
	                   PropertyInfo( Variant::OBJECT, "height_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ), "set_height_texture", "get_height_texture" );
	ADD_GROUP( "Surface", "" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setBaseColor, getBaseColor, "color",
	                   PropertyInfo( Variant::COLOR, "base_color" ), "set_base_color", "get_base_color" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setRoughness, getRoughness, "value",
	                   PropertyInfo( Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01" ), "set_roughness", "get_roughness" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setMetallic, getMetallic, "value",
	                   PropertyInfo( Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0,1,0.01" ), "set_metallic", "get_metallic" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setNormalScale, getNormalScale, "value",
	                   PropertyInfo( Variant::FLOAT, "normal_scale", PROPERTY_HINT_RANGE, "0,4,0.01" ), "set_normal_scale", "get_normal_scale" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setUvScale, getUvScale, "scale",
	                   PropertyInfo( Variant::VECTOR2, "uv_scale" ), "set_uv_scale", "get_uv_scale" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setUvOffset, getUvOffset, "offset",
	                   PropertyInfo( Variant::VECTOR2, "uv_offset" ), "set_uv_offset", "get_uv_offset" );
	ADD_GROUP( "Pipeline", "" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setAlphaScissor, isAlphaScissor, "enabled",
	                   PropertyInfo( Variant::BOOL, "alpha_scissor" ), "set_alpha_scissor", "is_alpha_scissor" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setAlphaScissorThreshold, getAlphaScissorThreshold, "value",
	                   PropertyInfo( Variant::FLOAT, "alpha_scissor_threshold", PROPERTY_HINT_RANGE, "0,1,0.01" ), "set_alpha_scissor_threshold", "get_alpha_scissor_threshold" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setTwoSided, isTwoSided, "enabled",
	                   PropertyInfo( Variant::BOOL, "two_sided" ), "set_two_sided", "is_two_sided" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setFoliage, isFoliage, "enabled",
	                   PropertyInfo( Variant::BOOL, "foliage" ), "set_foliage", "is_foliage" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setCastShadows, isCastShadows, "enabled",
	                   PropertyInfo( Variant::BOOL, "cast_shadows" ), "set_cast_shadows", "is_cast_shadows" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setReceiveShadows, isReceiveShadows, "enabled",
	                   PropertyInfo( Variant::BOOL, "receive_shadows" ), "set_receive_shadows", "is_receive_shadows" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setSamplerClass, getSamplerClass, "sampler_class",
	                   PropertyInfo( Variant::INT, "sampler_class", PROPERTY_HINT_ENUM, "Repeat,Clamp" ), "set_sampler_class", "get_sampler_class" );
	T3D_BIND_PROPERTY( Terrain3DWorldMaterial, setRenderTier, getRenderTier, "tier",
	                   PropertyInfo( Variant::INT, "render_tier", PROPERTY_HINT_ENUM, "Auto,World,Clutter,Foliage" ), "set_render_tier", "get_render_tier" );
}

void Terrain3DWorldMaterial::setAlbedoTexture( const Ref<Texture2D> &rValue ) {
	if ( m_albedoTexture == rValue ) {
		return;
	}
	const Ref<Texture2D> previous = m_albedoTexture;
	m_albedoTexture = rValue;
	disconnectTextureSignalIfUnused( previous );
	connectTextureSignal( m_albedoTexture );
	advanceVtContentGeneration();
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
	advanceVtContentGeneration();
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
	advanceVtContentGeneration();
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
	advanceVtContentGeneration();
		emit_changed();
}

void Terrain3DWorldMaterial::setBaseColor( const Color &rColor ) {
	if ( m_baseColor != rColor ) {
		if ( (m_baseColor.a < 1.0f) != (rColor.a < 1.0f) ) { advanceVtContentGeneration(); }
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
	const float value = t3dClampUnitFloat( fValue );
	if ( m_fRoughness != value ) {
		m_fRoughness = value;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setMetallic( const float fValue ) {
	const float value = t3dClampUnitFloat( fValue );
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
	const float value = t3dClampUnitFloat( fValue );
	if ( m_fAlphaScissorThreshold != value ) {
		m_fAlphaScissorThreshold = value;
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setAlphaScissor( const bool bEnabled ) {
	if ( m_bAlphaScissor != bEnabled ) {
		m_bAlphaScissor = bEnabled;
		advanceVtContentGeneration();
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
		advanceVtContentGeneration();
		emit_changed();
	}
}
void Terrain3DWorldMaterial::setRenderTier( const int32_t iTier ) {
	const int32_t value = iTier >= 0 && iTier <= ( int32_t )( T3D_WORLD_IMPORT_FOLIAGE ) ? iTier : ( int32_t )( T3D_WORLD_IMPORT_AUTO );
	if ( m_iRenderTier != value ) {
		m_iRenderTier = value;
		advanceVtContentGeneration();
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
        const t3dRenderingMaterialVTBindings_t &rVt, t3dRenderingMaterialGpu_t &rRecord ) const {
	memset( &rRecord, 0, sizeof( rRecord ) );
	rRecord.m_nVtAllocation = rVt.m_nVtAllocation;
	rRecord.m_nOverlayVtAllocation = rVt.m_nOverlayVtAllocation;
	rRecord.m_nFlags = getMaterialFlags();
	rRecord.m_nFlags |= T3D_VT_MATERIAL_SURFACE_TEXTURE_KNOWN;
	if ( m_surfaceTexture.is_valid() ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_HAS_SURFACE_TEXTURE;
	}
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
