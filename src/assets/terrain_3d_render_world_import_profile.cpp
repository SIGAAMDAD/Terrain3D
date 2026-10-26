// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "assets/terrain_3d_render_world_import_profile.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void Terrain3DRenderWorldImportProfile::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "set_enabled", "enabled" ), &Terrain3DRenderWorldImportProfile::setEnabled );
	ClassDB::bind_method( D_METHOD( "is_enabled" ), &Terrain3DRenderWorldImportProfile::isEnabled );
	ClassDB::bind_method( D_METHOD( "set_category", "category" ), &Terrain3DRenderWorldImportProfile::setCategory );
	ClassDB::bind_method( D_METHOD( "get_category" ), &Terrain3DRenderWorldImportProfile::getCategory );
	ClassDB::bind_method( D_METHOD( "set_material_policy", "policy" ), &Terrain3DRenderWorldImportProfile::setMaterialPolicy );
	ClassDB::bind_method( D_METHOD( "get_material_policy" ), &Terrain3DRenderWorldImportProfile::getMaterialPolicy );
	ClassDB::bind_method( D_METHOD( "set_transparency_policy", "policy" ), &Terrain3DRenderWorldImportProfile::setTransparencyPolicy );
	ClassDB::bind_method( D_METHOD( "get_transparency_policy" ), &Terrain3DRenderWorldImportProfile::getTransparencyPolicy );
	ClassDB::bind_method( D_METHOD( "set_static_only", "enabled" ), &Terrain3DRenderWorldImportProfile::setStaticOnly );
	ClassDB::bind_method( D_METHOD( "is_static_only" ), &Terrain3DRenderWorldImportProfile::isStaticOnly );
	ClassDB::bind_method( D_METHOD( "set_cast_shadows", "enabled" ), &Terrain3DRenderWorldImportProfile::setCastShadows );
	ClassDB::bind_method( D_METHOD( "is_cast_shadows" ), &Terrain3DRenderWorldImportProfile::isCastShadows );
	ClassDB::bind_method( D_METHOD( "set_receive_shadows", "enabled" ), &Terrain3DRenderWorldImportProfile::setReceiveShadows );
	ClassDB::bind_method( D_METHOD( "is_receive_shadows" ), &Terrain3DRenderWorldImportProfile::isReceiveShadows );
	ClassDB::bind_method( D_METHOD( "set_max_distance", "distance" ), &Terrain3DRenderWorldImportProfile::setMaxDistance );
	ClassDB::bind_method( D_METHOD( "get_max_distance" ), &Terrain3DRenderWorldImportProfile::getMaxDistance );

	ClassDB::bind_method( D_METHOD( "set_vt_cluster_target_pages", "pages" ), &Terrain3DRenderWorldImportProfile::setVtClusterTargetPages );
	ClassDB::bind_method( D_METHOD( "get_vt_cluster_target_pages" ), &Terrain3DRenderWorldImportProfile::getVtClusterTargetPages );
	ClassDB::bind_method( D_METHOD( "set_vt_cluster_target_bytes", "bytes" ), &Terrain3DRenderWorldImportProfile::setVtClusterTargetBytes );
	ClassDB::bind_method( D_METHOD( "get_vt_cluster_target_bytes" ), &Terrain3DRenderWorldImportProfile::getVtClusterTargetBytes );
	ClassDB::bind_method(
	        D_METHOD( "set_vt_outer_compression", "compression" ), &Terrain3DRenderWorldImportProfile::setVtOuterCompression );
	ClassDB::bind_method( D_METHOD( "get_vt_outer_compression" ), &Terrain3DRenderWorldImportProfile::getVtOuterCompression );
	ClassDB::bind_method( D_METHOD( "set_vt_mip_quality", "quality" ), &Terrain3DRenderWorldImportProfile::setVtMipQuality );
	ClassDB::bind_method( D_METHOD( "get_vt_mip_quality" ), &Terrain3DRenderWorldImportProfile::getVtMipQuality );
	ClassDB::bind_method(
	        D_METHOD( "set_vt_compression_quality", "quality" ), &Terrain3DRenderWorldImportProfile::setVtCompressionQuality );
	ClassDB::bind_method( D_METHOD( "get_vt_compression_quality" ), &Terrain3DRenderWorldImportProfile::getVtCompressionQuality );
	ClassDB::bind_method( D_METHOD( "get_vt_page_interior" ), &Terrain3DRenderWorldImportProfile::getVtPageInterior );
	ClassDB::bind_method( D_METHOD( "get_vt_page_gutter" ), &Terrain3DRenderWorldImportProfile::getVtPageGutter );

	ADD_GROUP( "Conversion", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "enabled" ), "set_enabled", "is_enabled" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "category", PROPERTY_HINT_ENUM, "Auto,World,Clutter,Foliage" ), "set_category", "get_category" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
	                      "material_policy",
	                      PROPERTY_HINT_ENUM,
	                      "Auto Convert,Require RenderWorld Material,Native On Unsupported" ),
	        "set_material_policy",
	        "get_material_policy" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "transparency_policy", PROPERTY_HINT_ENUM, "Native Sorted,Alpha Scissor Only" ),
	        "set_transparency_policy",
	        "get_transparency_policy" );

	ADD_GROUP( "Virtual Texture Cook", "vt_" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "vt_cluster_target_pages", PROPERTY_HINT_RANGE, "1,4096,1", PROPERTY_USAGE_STORAGE ),
	        "set_vt_cluster_target_pages",
	        "get_vt_cluster_target_pages" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "vt_cluster_target_bytes", PROPERTY_HINT_RANGE, "65536,268435456,65536", PROPERTY_USAGE_STORAGE ),
	        "set_vt_cluster_target_bytes",
	        "get_vt_cluster_target_bytes" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "vt_outer_compression", PROPERTY_HINT_ENUM, "None,Zstd", PROPERTY_USAGE_STORAGE ),
	        "set_vt_outer_compression",
	        "get_vt_outer_compression" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "vt_mip_quality", PROPERTY_HINT_RANGE, "0,3,1", PROPERTY_USAGE_STORAGE ),
	        "set_vt_mip_quality",
	        "get_vt_mip_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "vt_compression_quality", PROPERTY_HINT_RANGE, "0,3,1", PROPERTY_USAGE_STORAGE ),
	        "set_vt_compression_quality",
	        "get_vt_compression_quality" );
	ADD_GROUP( "Reserved Compatibility Metadata", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "static_only" ), "set_static_only", "is_static_only" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "cast_shadows" ), "set_cast_shadows", "is_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "receive_shadows" ), "set_receive_shadows", "is_receive_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "max_distance", PROPERTY_HINT_RANGE, "0,100000,1,or_greater,suffix:m" ),
	        "set_max_distance",
	        "get_max_distance" );
}

void Terrain3DRenderWorldImportProfile::setEnabled( const bool bEnabled ) {
	if ( m_bEnabled == bEnabled ) {
		return;
	}
	m_bEnabled = bEnabled;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setCategory( const int32_t iCategory ) {
	const int32_t category =
	        iCategory >= 0 && iCategory <= ( int32_t )( T3D_RW_IMPORT_FOLIAGE ) ? iCategory : ( int32_t )( T3D_RW_IMPORT_AUTO );
	if ( m_iCategory == category ) {
		return;
	}
	m_iCategory = category;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setMaterialPolicy( const int32_t iPolicy ) {
	const int32_t policy = iPolicy >= 0 && iPolicy <= ( int32_t )( T3D_RW_MATERIAL_NATIVE_ON_UNSUPPORTED )
	        ? iPolicy
	        : ( int32_t )( T3D_RW_MATERIAL_AUTO_CONVERT );
	if ( m_iMaterialPolicy == policy ) {
		return;
	}
	m_iMaterialPolicy = policy;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setTransparencyPolicy( const int32_t iPolicy ) {
	const int32_t policy =
	        iPolicy == ( int32_t )( T3D_RW_TRANSPARENCY_ALPHA_SCISSOR_ONLY ) ? iPolicy : ( int32_t )( T3D_RW_TRANSPARENCY_NATIVE_SORTED );
	if ( m_iTransparencyPolicy == policy ) {
		return;
	}
	m_iTransparencyPolicy = policy;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setStaticOnly( const bool bEnabled ) {
	if ( m_bStaticOnly == bEnabled ) {
		return;
	}
	m_bStaticOnly = bEnabled;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setCastShadows( const bool bEnabled ) {
	if ( m_bCastShadows == bEnabled ) {
		return;
	}
	m_bCastShadows = bEnabled;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setReceiveShadows( const bool bEnabled ) {
	if ( m_bReceiveShadows == bEnabled ) {
		return;
	}
	m_bReceiveShadows = bEnabled;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setMaxDistance( const float fDistance ) {
	const float distance = fDistance < 0.0f ? 0.0f : fDistance;
	if ( m_fMaxDistance == distance ) {
		return;
	}
	m_fMaxDistance = distance;
	emit_changed();
}
void Terrain3DRenderWorldImportProfile::setVtClusterTargetPages( const int32_t iPages ) {
	const int32_t iResolved = CLAMP( iPages, 1, 4096 );
	if ( m_iVtClusterTargetPages == iResolved ) {
		return;
	}
	m_iVtClusterTargetPages = iResolved;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setVtClusterTargetBytes( const int32_t iBytes ) {
	const int32_t iResolved = CLAMP( iBytes, 65536, 268435456 );
	if ( m_iVtClusterTargetBytes == iResolved ) {
		return;
	}
	m_iVtClusterTargetBytes = iResolved;
	emit_changed();
}
void Terrain3DRenderWorldImportProfile::setVtOuterCompression( const int32_t iCompression ) {
	const int32_t iResolved =
	        iCompression == ( int32_t )T3D_VT_OUTER_COMPRESSION_ZSTD ? iCompression : ( int32_t )T3D_VT_OUTER_COMPRESSION_NONE;
	if ( m_iVtOuterCompression == iResolved ) {
		return;
	}
	m_iVtOuterCompression = iResolved;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setVtMipQuality( const int32_t iQuality ) {
	const int32_t iResolved = CLAMP( iQuality, 0, 3 );
	if ( m_iVtMipQuality == iResolved ) {
		return;
	}
	m_iVtMipQuality = iResolved;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::setVtCompressionQuality( const int32_t iQuality ) {
	const int32_t iResolved = CLAMP( iQuality, 0, 3 );
	if ( m_iVtCompressionQuality == iResolved ) {
		return;
	}
	m_iVtCompressionQuality = iResolved;
	emit_changed();
}

void Terrain3DRenderWorldImportProfile::resolveVtCookerSettings( t3dVTCookerSettings_t &rSettings ) const {
	t3d_vt_cooker_default_settings( rSettings );
	rSettings.m_nClusterTargetPages = ( uint32_t )m_iVtClusterTargetPages;
	rSettings.m_nClusterTargetBytes = ( uint32_t )m_iVtClusterTargetBytes;
	rSettings.m_nOuterCompression = ( uint32_t )m_iVtOuterCompression;
	rSettings.m_nMipQuality = ( uint32_t )m_iVtMipQuality;
	rSettings.m_nCompressionQuality = ( uint32_t )m_iVtCompressionQuality;
}

uint32_t Terrain3DRenderWorldImportProfile::resolveTier( const uint32_t nDefaultTier ) const {
	switch ( ( T3DRenderWorldImportCategory )( m_iCategory ) ) {
		case T3D_RW_IMPORT_WORLD:
			return T3D_RW_TIER_WORLD;
		case T3D_RW_IMPORT_CLUTTER:
			return T3D_RW_TIER_CLUTTER;
		case T3D_RW_IMPORT_FOLIAGE:
			return T3D_RW_TIER_FOLIAGE;
		default:
			return nDefaultTier;
	}
}
