// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_object_bytes.h"
#include "integration/godot/terrain_3d_binding.h"
#include "core/utility/terrain_3d_numeric.h"
#include "assets/resources/terrain_3d_resource_helpers.h"
#include "assets/resources/terrain_3d_rendering_material.h"

#include "rendering/rd/vt/core/terrain_3d_vt_material_identity.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include <string.h>

using namespace godot;

void Terrain3DRenderingMaterial::_bind_methods(
	void ) {
	ClassDB::bind_method( D_METHOD( "import_base_material", "material" ),
						  &Terrain3DRenderingMaterial::importBaseMaterialCold );
	ClassDB::bind_method( D_METHOD( "is_one_submit_compatible" ), &Terrain3DRenderingMaterial::isOneSubmitCompatible );

	ADD_GROUP( "Lighting", "" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setEmission,
					   getEmission,
					   "color",
					   PropertyInfo( Variant::COLOR, "emission" ),
					   "set_emission",
					   "get_emission" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setEmissionEnergy,
					   getEmissionEnergy,
					   "value",
					   PropertyInfo( Variant::FLOAT, "emission_energy", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater" ),
					   "set_emission_energy",
					   "get_emission_energy" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setAoStrength,
					   getAoStrength,
					   "value",
					   PropertyInfo( Variant::FLOAT, "ao_strength", PROPERTY_HINT_RANGE, "0,1,0.01" ),
					   "set_ao_strength",
					   "get_ao_strength" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setUnshaded,
					   isUnshaded,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "unshaded" ),
					   "set_unshaded",
					   "is_unshaded" );
	ADD_GROUP( "Vertex Input", "" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setVertexColor,
					   isVertexColor,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "vertex_color" ),
					   "set_vertex_color",
					   "is_vertex_color" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setVertexColorSrgb,
					   isVertexColorSrgb,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "vertex_color_srgb" ),
					   "set_vertex_color_srgb",
					   "is_vertex_color_srgb" );
	ADD_GROUP( "Imported Shader Profile", "" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setDirectOrmChannels,
					   usesDirectOrmChannels,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "direct_orm_channels" ),
					   "set_direct_orm_channels",
					   "uses_direct_orm_channels" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setAlbedoSaturation,
					   getAlbedoSaturation,
					   "value",
					   PropertyInfo( Variant::FLOAT, "albedo_saturation", PROPERTY_HINT_RANGE, "0,1.5,0.01" ),
					   "set_albedo_saturation",
					   "get_albedo_saturation" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setStylizedSun,
					   usesStylizedSun,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "stylized_sun" ),
					   "set_stylized_sun",
					   "uses_stylized_sun" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setDielectricSpecular,
					   getDielectricSpecular,
					   "value",
					   PropertyInfo( Variant::FLOAT, "dielectric_specular", PROPERTY_HINT_RANGE, "0,1,0.001" ),
					   "set_dielectric_specular",
					   "get_dielectric_specular" );
	ADD_GROUP( "Texture ABI", "" );
	T3D_BIND_PROPERTY(
		Terrain3DRenderingMaterial,
		setTextureFilter,
		getTextureFilter,
		"filter",
		PropertyInfo(
			Variant::INT,
			"texture_filter",
			PROPERTY_HINT_ENUM,
			"Nearest,Linear,Nearest Mipmap,Linear Mipmap,Nearest Mipmap Anisotropic,Linear Mipmap Anisotropic" ),
		"set_texture_filter",
		"get_texture_filter" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setSurfaceTextureLayout,
					   getSurfaceTextureLayout,
					   "layout",
					   PropertyInfo( Variant::INT,
									 "surface_texture_layout",
									 PROPERTY_HINT_ENUM,
									 "ORM,Roughness R,Roughness G,Roughness B,Roughness A" ),
					   "set_surface_texture_layout",
					   "get_surface_texture_layout" );
	T3D_BIND_PROPERTY( Terrain3DRenderingMaterial,
					   setVtStableKeyHex,
					   getVtStableKeyHex,
					   "key",
					   PropertyInfo( Variant::STRING, "vt_stable_key", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
					   "set_vt_stable_key_hex",
					   "get_vt_stable_key_hex" );
}

void Terrain3DRenderingMaterial::setEmission(
	const Color &rColor ) {
	if ( m_emission != rColor ) {
		m_emission = rColor;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setEmissionEnergy(
	const float fValue ) {
	const float value = MAX( fValue, 0.0f );
	if ( m_fEmissionEnergy != value ) {
		m_fEmissionEnergy = value;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setAoStrength(
	const float fValue ) {
	const float value = t3dClampUnitFloat( fValue );
	if ( m_fAoStrength != value ) {
		m_fAoStrength = value;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setTextureFilter(
	const int32_t iValue ) {
	const int32_t value = CLAMP( iValue, 0, 5 );
	if ( m_iTextureFilter != value ) {
		m_iTextureFilter = value;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setSurfaceTextureLayout(
	const int32_t iValue ) {
	const int32_t value =
		CLAMP( iValue, ( int32_t ) ( SURFACE_TEXTURE_ORM ), ( int32_t ) ( SURFACE_TEXTURE_ROUGHNESS_A ) );
	if ( m_iSurfaceTextureLayout != value ) {
		m_iSurfaceTextureLayout = value;
		advanceVtContentGeneration();
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setUnshaded(
	const bool bEnabled ) {
	if ( m_bUnshaded != bEnabled ) {
		m_bUnshaded = bEnabled;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setVertexColor(
	const bool bEnabled ) {
	if ( m_bVertexColor != bEnabled ) {
		m_bVertexColor = bEnabled;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setVertexColorSrgb(
	const bool bEnabled ) {
	if ( m_bVertexColorSrgb != bEnabled ) {
		m_bVertexColorSrgb = bEnabled;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setDirectOrmChannels(
	const bool bEnabled ) {
	if ( m_bDirectOrmChannels != bEnabled ) {
		m_bDirectOrmChannels = bEnabled;
		advanceVtContentGeneration();
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setAlbedoSaturation(
	const float fValue ) {
	const float value = CLAMP( fValue, 0.0f, 1.5f );
	if ( m_fAlbedoSaturation != value ) {
		m_fAlbedoSaturation = value;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setStylizedSun(
	const bool bEnabled ) {
	if ( m_bStylizedSun != bEnabled ) {
		m_bStylizedSun = bEnabled;
		emit_changed();
	}
}

void Terrain3DRenderingMaterial::setDielectricSpecular(
	const float fValue ) {
	const float value = CLAMP( fValue, 0.0f, 1.0f );
	if ( m_fDielectricSpecular != value ) {
		m_fDielectricSpecular = value;
		emit_changed();
	}
}

static uint32_t t3dRenderingMaterialHexNibble(
	const char32_t c ) {
	if ( c >= '0' && c <= '9' ) {
		return ( uint32_t ) ( c - '0' );
	}
	if ( c >= 'a' && c <= 'f' ) {
		return 10u + ( uint32_t ) ( c - 'a' );
	}
	if ( c >= 'A' && c <= 'F' ) {
		return 10u + ( uint32_t ) ( c - 'A' );
	}
	return UINT32_MAX;
}

void Terrain3DRenderingMaterial::setVtStableKeyHex(
	const String &rValue ) {
	if ( rValue.is_empty() ) {
		setVtStableKey( 0u );
		return;
	}
	if ( rValue.length() > 16 ) {
		return;
	}
	uint64_t nKey = 0u;
	for ( int iCharacter = 0; iCharacter < rValue.length(); ++iCharacter ) {
		const uint32_t nNibble = t3dRenderingMaterialHexNibble( rValue.unicode_at( iCharacter ) );
		if ( nNibble == UINT32_MAX ) {
			return;
		}
		nKey = ( nKey << 4u ) | nNibble;
	}
	setVtStableKey( nKey );
}

String Terrain3DRenderingMaterial::getVtStableKeyHex(
	void ) const {
	if ( m_nVtStableKey == 0u ) {
		return String();
	}
	static const char pDigits[] = "0123456789abcdef";
	char			  pBuffer[17];
	pBuffer[16] = '\0';
	uint64_t nValue = m_nVtStableKey;
	for ( int iDigit = 15; iDigit >= 0; --iDigit ) {
		pBuffer[iDigit] = pDigits[nValue & 0x0fu];
		nValue >>= 4u;
	}
	return String( pBuffer );
}

void Terrain3DRenderingMaterial::setVtStableKey(
	const uint64_t nKey ) {
	if ( m_nVtStableKey != nKey ) {
		m_nVtStableKey = nKey;
		advanceVtContentGeneration();
		emit_changed();
	}
}

uint64_t Terrain3DRenderingMaterial::getVtStableKey(
	void ) const {
	const String path = get_path();
	if ( !path.is_empty() && !path.contains( "::" ) ) {
		return t3d_vt_material_stable_key_from_resource_path( path );
	}
	return m_nVtStableKey;
}

bool Terrain3DRenderingMaterial::ensureVtStableKeyCold(
	const String &rOwnerIdentity,
	const String &rRoleIdentity ) {
	if ( getVtStableKey() != 0u ) {
		return true;
	}
	const uint64_t nKey = t3d_vt_material_stable_key_from_owner_role( rOwnerIdentity, rRoleIdentity );
	if ( nKey == 0u ) {
		return false;
	}
	setVtStableKey( nKey );
	return true;
}

bool Terrain3DRenderingMaterial::importBaseMaterialCold(
	const Ref< Material > &rMaterial ) {
	BaseMaterial3D *const pBase = rMaterial.is_valid() ? Object::cast_to< BaseMaterial3D >( rMaterial.ptr() ) : NULL;
	if ( !pBase ) {
		return false;
	}
	const BaseMaterial3D::Transparency transparency = pBase->get_transparency();
	if ( transparency == BaseMaterial3D::TRANSPARENCY_ALPHA ||
		 transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS ) {
		return false;
	}
	setDirectOrmChannels( false );
	setAlbedoSaturation( 1.0f );
	setStylizedSun( false );
	setDielectricSpecular( 0.04f );
	if ( pBase->has_meta( "terrain3d_direct_orm" ) ) {
		const Variant direct_orm = pBase->get_meta( "terrain3d_direct_orm", false );
		if ( direct_orm.get_type() == Variant::BOOL ) {
			setDirectOrmChannels( ( bool ) ( direct_orm ) );
		}
	}
	if ( pBase->has_meta( "terrain3d_albedo_saturation" ) ) {
		const Variant saturation = pBase->get_meta( "terrain3d_albedo_saturation", 1.0f );
		if ( saturation.get_type() == Variant::FLOAT || saturation.get_type() == Variant::INT ) {
			setAlbedoSaturation( ( float ) ( saturation ) );
		}
	}
	if ( pBase->has_meta( "terrain3d_stylized_sun" ) ) {
		const Variant stylized_sun = pBase->get_meta( "terrain3d_stylized_sun", false );
		if ( stylized_sun.get_type() == Variant::BOOL ) {
			setStylizedSun( ( bool ) ( stylized_sun ) );
		}
	}
	if ( pBase->has_meta( "terrain3d_dielectric_specular" ) ) {
		const Variant dielectric_specular = pBase->get_meta( "terrain3d_dielectric_specular", 0.04f );
		if ( dielectric_specular.get_type() == Variant::FLOAT || dielectric_specular.get_type() == Variant::INT ) {
			setDielectricSpecular( ( float ) ( dielectric_specular ) );
		}
	}
	setBaseColor( pBase->get_albedo() );
	const Vector3 uv_scale = pBase->get_uv1_scale();
	const Vector3 uv_offset = pBase->get_uv1_offset();
	setUvScale( Vector2( uv_scale.x, uv_scale.y ) );
	setUvOffset( Vector2( uv_offset.x, uv_offset.y ) );
	setMetallic( ( float ) ( pBase->get_metallic() ) );
	setRoughness( ( float ) ( pBase->get_roughness() ) );
	setAoStrength( ( float ) ( pBase->get_ao_light_affect() ) );
	setAlphaScissor( transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR );
	setAlphaScissorThreshold( ( float ) ( pBase->get_alpha_scissor_threshold() ) );
	setTwoSided( pBase->get_cull_mode() == BaseMaterial3D::CULL_DISABLED );
	setUnshaded( pBase->get_shading_mode() == BaseMaterial3D::SHADING_MODE_UNSHADED );
	setVertexColor( pBase->get_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR ) );
	setVertexColorSrgb( pBase->get_flag( BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR ) );
	setSamplerClass( pBase->get_flag( BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT ) ? T3D_VT_SAMPLER_LINEAR_REPEAT
																				: T3D_VT_SAMPLER_LINEAR_CLAMP );
	setTextureFilter( ( int32_t ) ( CLAMP( ( int ) ( pBase->get_texture_filter() ), 0, 5 ) ) );
	setNormalScale( ( float ) ( pBase->get_normal_scale() ) );
	setAlbedoTexture( pBase->get_texture( BaseMaterial3D::TEXTURE_ALBEDO ) );
	setNormalTexture( pBase->get_texture( BaseMaterial3D::TEXTURE_NORMAL ) );
	Ref< Texture2D > surface = pBase->get_texture( BaseMaterial3D::TEXTURE_ORM );
	setSurfaceTextureLayout( SURFACE_TEXTURE_ORM );
	if ( surface.is_null() ) {
		surface = pBase->get_texture( BaseMaterial3D::TEXTURE_ROUGHNESS );
		if ( surface.is_valid() ) {
			setSurfaceTextureLayout( SURFACE_TEXTURE_ROUGHNESS_R +
									 ( int32_t ) ( pBase->get_roughness_texture_channel() ) );
		}
	}
	setSurfaceTexture( surface );
	setHeightTexture( pBase->get_texture( BaseMaterial3D::TEXTURE_HEIGHTMAP ) );
	if ( pBase->get_feature( BaseMaterial3D::FEATURE_EMISSION ) ) {
		setEmission( pBase->get_emission() );
		setEmissionEnergy( ( float ) ( pBase->get_emission_energy_multiplier() ) );
	} else {
		setEmission( Color( 0.0f, 0.0f, 0.0f, 1.0f ) );
		setEmissionEnergy( 1.0f );
	}
	return true;
}

bool Terrain3DRenderingMaterial::compileInstanceRecordCold(
	t3dRdInstanceMaterial_t &rRecord ) const {
	t3dClearTrivialObjectBytes( rRecord );
	const Color albedo = getBaseColor();
	rRecord.m_szAlbedo[0] = ( float ) ( albedo.r );
	rRecord.m_szAlbedo[1] = ( float ) ( albedo.g );
	rRecord.m_szAlbedo[2] = ( float ) ( albedo.b );
	rRecord.m_szAlbedo[3] = ( float ) ( albedo.a );
	rRecord.m_fMetallic = getMetallic();
	rRecord.m_fRoughness = getRoughness();
	rRecord.m_fAo = m_fAoStrength;
	rRecord.m_fAlphaScissor = getAlphaScissorThreshold();
	rRecord.m_fNormalScale = getNormalScale();
	rRecord.m_szEmission[0] = ( float ) ( m_emission.r ) * m_fEmissionEnergy;
	rRecord.m_szEmission[1] = ( float ) ( m_emission.g ) * m_fEmissionEnergy;
	rRecord.m_szEmission[2] = ( float ) ( m_emission.b ) * m_fEmissionEnergy;
	if ( isAlphaScissor() ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_ALPHA_SCISSOR;
	}
	if ( isTwoSided() ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_DOUBLE_SIDED;
	}
	if ( getSamplerClass() == ( int32_t ) ( T3D_VT_SAMPLER_LINEAR_REPEAT ) ||
		 getSamplerClass() == ( int32_t ) ( T3D_VT_SAMPLER_NEAREST_REPEAT ) ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT;
	}
	if ( m_bUnshaded ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_UNSHADED;
	}
	if ( m_bVertexColor ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR;
	}
	if ( m_bVertexColorSrgb ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_VERTEX_COLOR_SRGB;
	}
	if ( !isReceiveShadows() ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_DONT_RECEIVE_SHADOWS;
	}
	if ( m_fEmissionEnergy > 0.0f && ( m_emission.r > 0.0f || m_emission.g > 0.0f || m_emission.b > 0.0f ) ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_EMISSIVE;
	}
	if ( m_fAoStrength > 0.0f && getSurfaceTexture().is_valid() && m_iSurfaceTextureLayout == SURFACE_TEXTURE_ORM ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_AO_ENABLED;
	}
	const uint32_t filter = ( uint32_t ) ( CLAMP( m_iTextureFilter, 0, 5 ) );
	rRecord.m_nFlags |= ( filter << T3D_RD_INSTANCE_MATERIAL_FILTER_SHIFT ) & T3D_RD_INSTANCE_MATERIAL_FILTER_MASK;
	const Ref< Texture2D > albedo_texture = getAlbedoTexture();
	if ( albedo_texture.is_valid() ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_HAS_ALBEDO_TEXTURE;
	}
	const Ref< Texture2D > normal_texture = getNormalTexture();
	if ( normal_texture.is_valid() ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_HAS_NORMAL_TEXTURE | T3D_RD_INSTANCE_MATERIAL_NORMAL_MAPPING;
	}
	const Ref< Texture2D > surface_texture = getSurfaceTexture();
	if ( surface_texture.is_valid() ) {
		rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_HAS_ORM_TEXTURE;
		if ( m_iSurfaceTextureLayout != SURFACE_TEXTURE_ORM ) {
			rRecord.m_nFlags |= T3D_RD_INSTANCE_MATERIAL_ROUGHNESS_TEXTURE;
			rRecord.m_nReserved0 = ( uint32_t ) ( m_iSurfaceTextureLayout - SURFACE_TEXTURE_ROUGHNESS_R );
		}
	}
	return true;
}

bool Terrain3DRenderingMaterial::compileVtRecordCold(
	const t3dRenderingMaterialVTBindings_t &rVt,
	t3dRenderingMaterialGpu_t			   &rRecord ) const {
	if ( !Terrain3DWorldMaterial::compileVtRecordCold( rVt, rRecord ) ) {
		return false;
	}
	rRecord.m_nPackedScalars0 =
		t3dMaterialPackVtScalars0( getMetallic(), getRoughness(), m_fAoStrength, getAlphaScissorThreshold() );
	rRecord.m_nPackedScalars1 &= T3D_VT_MATERIAL_NORMAL_SCALE_MASK;
	rRecord.m_nPackedScalars1 |=
		( ( ( uint32_t ) ( m_iSurfaceTextureLayout ) << T3D_VT_MATERIAL_SURFACE_LAYOUT_SHIFT ) &
		  T3D_VT_MATERIAL_SURFACE_LAYOUT_MASK ) |
		( ( ( uint32_t ) ( m_iTextureFilter ) << T3D_VT_MATERIAL_FILTER_SHIFT ) & T3D_VT_MATERIAL_FILTER_MASK );
	if ( m_bUnshaded ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_UNSHADED;
	}
	if ( m_bVertexColor ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_VERTEX_COLOR;
	}
	if ( m_bVertexColorSrgb ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_VERTEX_COLOR_SRGB;
	}
	if ( m_fAoStrength > 0.0f && getSurfaceTexture().is_valid() && m_iSurfaceTextureLayout == SURFACE_TEXTURE_ORM ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_AO_ENABLED;
	}
	if ( m_bDirectOrmChannels && getSurfaceTexture().is_valid() && m_iSurfaceTextureLayout == SURFACE_TEXTURE_ORM ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_DIRECT_ORM;
	}
	const uint32_t saturation_u16 = ( uint32_t ) ( CLAMP( m_fAlbedoSaturation / 1.5f, 0.0f, 1.0f ) * 65535.0f + 0.5f );
	const uint32_t dielectric_specular_u16 =
		( uint32_t ) ( CLAMP( m_fDielectricSpecular, 0.0f, 1.0f ) * 65535.0f + 0.5f );
	rRecord.m_nReserved = saturation_u16 | ( dielectric_specular_u16 << 16u );
	if ( m_fAlbedoSaturation < 0.9999f || m_fAlbedoSaturation > 1.0001f ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_ALBEDO_SATURATION;
	}
	if ( m_bStylizedSun ) {
		rRecord.m_nFlags |= T3D_VT_MATERIAL_STYLIZED_SUN;
	}
	const Color emission( MAX( ( float ) ( m_emission.r ) * m_fEmissionEnergy, 0.0f ),
						  MAX( ( float ) ( m_emission.g ) * m_fEmissionEnergy, 0.0f ),
						  MAX( ( float ) ( m_emission.b ) * m_fEmissionEnergy, 0.0f ),
						  1.0f );
	rRecord.m_nPackedEmission = emission.to_rgbe9995();
	return true;
}

bool Terrain3DRenderingMaterial::isOneSubmitCompatible(
	void ) const {
	return true;
}
