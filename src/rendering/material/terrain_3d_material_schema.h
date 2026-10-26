// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MATERIAL_SCHEMA_H
#define TERRAIN3D_MATERIAL_SCHEMA_H

#include "Terrain3DTypes.h"
#include "eastl_config.h"

#include "rendering/material/terrain_3d_material.h"

#include <EASTL/any.h>

typedef struct t3dMaterialSchemaEntry_s
{
	t3dMaterialParameterId_t	  m_id;
	const char					 *m_pszName;
	bool						  m_bGroup;
	Variant::Type				  m_type;
	eastl::any					  m_defaultValue;
	PropertyHint				  m_hint;
	const char					 *m_pszHintString;
	t3dMaterialSchemaVisibility_t m_visibility;
} t3dMaterialSchemaEntry_t;

#define T3D_MATERIAL_SCHEMA_GROUP( name, visibility )                                                                  \
	{ T3D_MATERIAL_PARAM_INVALID, name, true, Variant::NIL, eastl::any(), PROPERTY_HINT_NONE, "", visibility }
#define T3D_MATERIAL_SCHEMA_PARAM( id, name, type, default_value, hint, hint_string, visibility )                      \
	{ id, name, false, type, eastl::any( default_value ), hint, hint_string, visibility }
#define T3D_MATERIAL_SCHEMA_PARAM_EMPTY( id, name, type, hint, hint_string, visibility )                               \
	{ id, name, false, type, eastl::any(), hint, hint_string, visibility }

EA_FORCE_INLINE static const t3dMaterialSchemaEntry_t *t3dMaterialSchemaEntries(
	size_t &rCount ) {
	// Function-local static initialization is intentionally deferred until the schema is first
	// consumed, after GDExtension initialization. No Godot-backed Variant is stored here.
	static const t3dMaterialSchemaEntry_t s_t3dMaterialSchema[] = {
		T3D_MATERIAL_SCHEMA_GROUP( "General", T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_FLAT_TERRAIN_NORMALS,
								   "flat_terrain_normals",
								   Variant::BOOL,
								   false,
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_DISTANT_NORMAL_SCALE,
								   "distant_normal_scale",
								   Variant::FLOAT,
								   2.0,
								   PROPERTY_HINT_RANGE,
								   "1,10,0.1",
								   T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_BLEND_SHARPNESS,
								   "blend_sharpness",
								   Variant::FLOAT,
								   0.5,
								   PROPERTY_HINT_RANGE,
								   "0,1,0.01",
								   T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_SILHOUETTE_COLOR,
								   "silhouette_color",
								   Variant::COLOR,
								   Color( 0.04, 0.04, 0.04, 1.0 ),
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_GROUP( "Mipmaps", T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_BIAS_DISTANCE,
								   "bias_distance",
								   Variant::FLOAT,
								   512.0,
								   PROPERTY_HINT_RANGE,
								   "0,16384,0.1",
								   T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_MIPMAP_BIAS,
								   "mipmap_bias",
								   Variant::FLOAT,
								   1.0,
								   PROPERTY_HINT_RANGE,
								   "0.5,1.5,0.01",
								   T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_DEPTH_BLUR,
								   "depth_blur",
								   Variant::FLOAT,
								   0.0,
								   PROPERTY_HINT_RANGE,
								   "0,35,0.1",
								   T3D_MATERIAL_SCHEMA_ALWAYS ),
		T3D_MATERIAL_SCHEMA_GROUP( "Flat Terrain", T3D_MATERIAL_SCHEMA_WORLD_FLAT ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_GROUND_LEVEL,
								   "ground_level",
								   Variant::FLOAT,
								   0.0,
								   PROPERTY_HINT_RANGE,
								   "-1000,1000,0.1",
								   T3D_MATERIAL_SCHEMA_WORLD_FLAT ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_REGION_BLEND,
								   "region_blend",
								   Variant::FLOAT,
								   0.25,
								   PROPERTY_HINT_RANGE,
								   "0.001,1,0.001",
								   T3D_MATERIAL_SCHEMA_WORLD_FLAT ),
		T3D_MATERIAL_SCHEMA_GROUP( "Auto Shader", T3D_MATERIAL_SCHEMA_AUTO_SHADER ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_AUTO_SLOPE,
								   "auto_slope",
								   Variant::FLOAT,
								   1.0,
								   PROPERTY_HINT_RANGE,
								   "0,10,0.01",
								   T3D_MATERIAL_SCHEMA_AUTO_SHADER ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_AUTO_HEIGHT_REDUCTION,
								   "auto_height_reduction",
								   Variant::FLOAT,
								   0.1,
								   PROPERTY_HINT_RANGE,
								   "0,1,0.01",
								   T3D_MATERIAL_SCHEMA_AUTO_SHADER ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_AUTO_BASE_TEXTURE,
								   "auto_base_texture",
								   Variant::INT,
								   0,
								   PROPERTY_HINT_RANGE,
								   "0,31,1",
								   T3D_MATERIAL_SCHEMA_AUTO_SHADER ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_AUTO_OVERLAY_TEXTURE,
								   "auto_overlay_texture",
								   Variant::INT,
								   1,
								   PROPERTY_HINT_RANGE,
								   "0,31,1",
								   T3D_MATERIAL_SCHEMA_AUTO_SHADER ),
		T3D_MATERIAL_SCHEMA_GROUP( "Dual Scaling", T3D_MATERIAL_SCHEMA_DUAL_SCALING ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_DUAL_SCALE_TEXTURE,
								   "dual_scale_texture",
								   Variant::INT,
								   0,
								   PROPERTY_HINT_RANGE,
								   "0,31,1",
								   T3D_MATERIAL_SCHEMA_DUAL_SCALING ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_DUAL_SCALE_REDUCTION,
								   "dual_scale_reduction",
								   Variant::FLOAT,
								   0.3,
								   PROPERTY_HINT_RANGE,
								   "0.001,1,0.001",
								   T3D_MATERIAL_SCHEMA_DUAL_SCALING ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_TRI_SCALE_REDUCTION,
								   "tri_scale_reduction",
								   Variant::FLOAT,
								   0.3,
								   PROPERTY_HINT_RANGE,
								   "0.001,1,0.001",
								   T3D_MATERIAL_SCHEMA_DUAL_SCALING ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_DUAL_SCALE_FAR,
								   "dual_scale_far",
								   Variant::FLOAT,
								   170.0,
								   PROPERTY_HINT_RANGE,
								   "0,1000,1",
								   T3D_MATERIAL_SCHEMA_DUAL_SCALING ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_DUAL_SCALE_NEAR,
								   "dual_scale_near",
								   Variant::FLOAT,
								   100.0,
								   PROPERTY_HINT_RANGE,
								   "0,1000,1",
								   T3D_MATERIAL_SCHEMA_DUAL_SCALING ),
		T3D_MATERIAL_SCHEMA_GROUP( "Macro Variation", T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_MACRO_VARIATION1,
								   "macro_variation1",
								   Variant::VECTOR3,
								   Vector3( 1, 1, 1 ),
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_MACRO_VARIATION2,
								   "macro_variation2",
								   Variant::VECTOR3,
								   Vector3( 1, 1, 1 ),
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_MACRO_VARIATION_SLOPE,
								   "macro_variation_slope",
								   Variant::FLOAT,
								   0.333,
								   PROPERTY_HINT_RANGE,
								   "0,1,0.001",
								   T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM_EMPTY( T3D_MATERIAL_PARAM_NOISE_TEXTURE,
										 "noise_texture",
										 Variant::OBJECT,
										 PROPERTY_HINT_RESOURCE_TYPE,
										 "Texture2D",
										 T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_NOISE1_SCALE,
								   "noise1_scale",
								   Variant::FLOAT,
								   0.04,
								   PROPERTY_HINT_RANGE,
								   "0.001,1,0.001",
								   T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_NOISE1_ANGLE,
								   "noise1_angle",
								   Variant::FLOAT,
								   0.0,
								   PROPERTY_HINT_RANGE,
								   "0,6.283,0.001",
								   T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_NOISE1_OFFSET,
								   "noise1_offset",
								   Variant::VECTOR2,
								   Vector2( 0.5, 0.5 ),
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_NOISE2_SCALE,
								   "noise2_scale",
								   Variant::FLOAT,
								   0.076,
								   PROPERTY_HINT_RANGE,
								   "0.001,1,0.001",
								   T3D_MATERIAL_SCHEMA_MACRO_VARIATION ),
		T3D_MATERIAL_SCHEMA_GROUP( "World Noise", T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_FRAGMENT_NORMALS,
								   "world_noise_fragment_normals",
								   Variant::BOOL,
								   false,
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_CACHE_ENABLED,
								   "world_noise_cache_enabled",
								   Variant::BOOL,
								   true,
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_CACHE_PERIOD,
								   "world_noise_cache_period",
								   Variant::FLOAT,
								   64.0,
								   PROPERTY_HINT_RANGE,
								   "8,256,1",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_MAX_OCTAVES,
								   "world_noise_max_octaves",
								   Variant::INT,
								   4,
								   PROPERTY_HINT_RANGE,
								   "0,15,1",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_MIN_OCTAVES,
								   "world_noise_min_octaves",
								   Variant::INT,
								   2,
								   PROPERTY_HINT_RANGE,
								   "0,15,1",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_LOD_DISTANCE,
								   "world_noise_lod_distance",
								   Variant::FLOAT,
								   7500.0,
								   PROPERTY_HINT_RANGE,
								   "0,40000,1",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_SCALE,
								   "world_noise_scale",
								   Variant::FLOAT,
								   5.0,
								   PROPERTY_HINT_RANGE,
								   "0.25,20,0.01",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_HEIGHT,
								   "world_noise_height",
								   Variant::FLOAT,
								   32.0,
								   PROPERTY_HINT_RANGE,
								   "-1000,1000,0.1",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_WORLD_NOISE_OFFSET,
								   "world_noise_offset",
								   Variant::VECTOR3,
								   Vector3(),
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_WORLD_NOISE ),
		T3D_MATERIAL_SCHEMA_GROUP( "Debug Heightmap", T3D_MATERIAL_SCHEMA_DEBUG_HEIGHTMAP ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_HEIGHTMAP_BLACK_HEIGHT,
								   "heightmap_black_height",
								   Variant::FLOAT,
								   -100.0,
								   PROPERTY_HINT_RANGE,
								   "-2048,2048,0.5",
								   T3D_MATERIAL_SCHEMA_DEBUG_HEIGHTMAP ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_HEIGHTMAP_WHITE_HEIGHT,
								   "heightmap_white_height",
								   Variant::FLOAT,
								   300.0,
								   PROPERTY_HINT_RANGE,
								   "-2048,2048,0.5",
								   T3D_MATERIAL_SCHEMA_DEBUG_HEIGHTMAP ),
		T3D_MATERIAL_SCHEMA_GROUP( "Contour Lines", T3D_MATERIAL_SCHEMA_CONTOURS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_CONTOUR_INTERVAL,
								   "contour_interval",
								   Variant::FLOAT,
								   1.0,
								   PROPERTY_HINT_RANGE,
								   "0.25,100,0.001",
								   T3D_MATERIAL_SCHEMA_CONTOURS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_CONTOUR_THICKNESS,
								   "contour_thickness",
								   Variant::FLOAT,
								   1.0,
								   PROPERTY_HINT_RANGE,
								   "0,10,0.001",
								   T3D_MATERIAL_SCHEMA_CONTOURS ),
		T3D_MATERIAL_SCHEMA_PARAM( T3D_MATERIAL_PARAM_CONTOUR_COLOR,
								   "contour_color",
								   Variant::COLOR,
								   Color( 0.85, 0.85, 0.19, 1.0 ),
								   PROPERTY_HINT_NONE,
								   "",
								   T3D_MATERIAL_SCHEMA_CONTOURS ),
	};
	rCount = T3D_ARRAY_COUNT( s_t3dMaterialSchema );
	return s_t3dMaterialSchema;
}

#undef T3D_MATERIAL_SCHEMA_GROUP
#undef T3D_MATERIAL_SCHEMA_PARAM
#undef T3D_MATERIAL_SCHEMA_PARAM_EMPTY

static_assert( T3D_MATERIAL_PARAM_COUNT == 40, "Terrain3D material schema ID count changed" );

EA_FORCE_INLINE static Variant t3dMaterialSchemaDefaultToVariant(
	const t3dMaterialSchemaEntry_t &rEntry ) {
	switch ( rEntry.m_type ) {
	case Variant::BOOL: {
		const bool *const pValue = eastl::any_cast< bool >( &rEntry.m_defaultValue );
		return pValue ? Variant( *pValue ) : Variant();
	}
	case Variant::INT: {
		const int *const pValue = eastl::any_cast< int >( &rEntry.m_defaultValue );
		return pValue ? Variant( *pValue ) : Variant();
	}
	case Variant::FLOAT: {
		const double *const pValue = eastl::any_cast< double >( &rEntry.m_defaultValue );
		return pValue ? Variant( *pValue ) : Variant();
	}
	case Variant::COLOR: {
		const Color *const pValue = eastl::any_cast< Color >( &rEntry.m_defaultValue );
		return pValue ? Variant( *pValue ) : Variant();
	}
	case Variant::VECTOR2: {
		const Vector2 *const pValue = eastl::any_cast< Vector2 >( &rEntry.m_defaultValue );
		return pValue ? Variant( *pValue ) : Variant();
	}
	case Variant::VECTOR3: {
		const Vector3 *const pValue = eastl::any_cast< Vector3 >( &rEntry.m_defaultValue );
		return pValue ? Variant( *pValue ) : Variant();
	}
	case Variant::NIL:
	case Variant::OBJECT:
	default:
		return Variant();
	}
}

EA_FORCE_INLINE static const t3dMaterialSchemaEntry_t *t3dMaterialSchemaFindById(
	const t3dMaterialParameterId_t id ) {
	size_t								  nEntryCount = 0u;
	const t3dMaterialSchemaEntry_t *const pEntries = t3dMaterialSchemaEntries( nEntryCount );
	for ( size_t iEntry = 0u; iEntry < nEntryCount; ++iEntry ) {
		if ( pEntries[iEntry].m_id == id ) {
			return &pEntries[iEntry];
		}
	}
	return NULL;
}

EA_FORCE_INLINE static const t3dMaterialSchemaEntry_t *t3dMaterialSchemaFindByName(
	const StringName &rName ) {
	size_t								  nEntryCount = 0u;
	const t3dMaterialSchemaEntry_t *const pEntries = t3dMaterialSchemaEntries( nEntryCount );
	for ( size_t iEntry = 0u; iEntry < nEntryCount; ++iEntry ) {
		const t3dMaterialSchemaEntry_t &entry = pEntries[iEntry];
		if ( !entry.m_bGroup && StringName( entry.m_pszName ) == rName ) {
			return &entry;
		}
	}
	return NULL;
}

#endif // TERRAIN3D_MATERIAL_SCHEMA_H
