// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_TYPE_SUPPORT_H
#define TERRAIN3D_TYPE_SUPPORT_H

#include "Terrain3DTypes.h"

// Typed profile manifest storage. Kept outside Terrain3DTypes.h so the central
// type authority remains behavior/data-definition only.
#define T3D_PROFILE_TERRAIN_INT( name, value ) { T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_INT, name, ( int64_t )( value ), 0.0, false },
#define T3D_PROFILE_TERRAIN_FLOAT( name, value ) \
    { T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_FLOAT, name, 0, ( double )( value ), false },
#define T3D_PROFILE_TERRAIN_BOOL( name, value ) { T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_BOOL, name, 0, 0.0, ( bool )( value ) },
#define T3D_PROFILE_MATERIAL_INT( name, value ) \
    { T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_INT, name, ( int64_t )( value ), 0.0, false },
#define T3D_PROFILE_MATERIAL_FLOAT( name, value ) \
    { T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_FLOAT, name, 0, ( double )( value ), false },
#define T3D_PROFILE_MATERIAL_BOOL( name, value ) { T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_BOOL, name, 0, 0.0, ( bool )( value ) },

static const t3dPerformanceProfileSetting_t g_t3dTypeSupportXbox360ExperimentalSettings[] = {
#include "streaming/policy/terrain_3d_performance_profile_manifest.inc"
};
#define g_szXbox360ExperimentalSettings g_t3dTypeSupportXbox360ExperimentalSettings
#define g_nXbox360ExperimentalSettingCount ( sizeof( g_szXbox360ExperimentalSettings ) / sizeof( g_szXbox360ExperimentalSettings[0] ) )

#undef T3D_PROFILE_TERRAIN_INT
#undef T3D_PROFILE_TERRAIN_FLOAT
#undef T3D_PROFILE_TERRAIN_BOOL
#undef T3D_PROFILE_MATERIAL_INT
#undef T3D_PROFILE_MATERIAL_FLOAT
#undef T3D_PROFILE_MATERIAL_BOOL

EA_FORCE_INLINE t3dRegionCoord_t t3dPackRegionCoord( const Vector2i &rLocation ) {
    t3dRegionCoord_t coord;
    if ( rLocation.x < -T3D_REGION_MAP_HALF || rLocation.x >= T3D_REGION_MAP_HALF || rLocation.y < -T3D_REGION_MAP_HALF
            || rLocation.y >= T3D_REGION_MAP_HALF ) {
        coord.m_x = T3D_REGION_COORD_INVALID;
        coord.m_y = T3D_REGION_COORD_INVALID;
        return coord;
    }
    coord.m_x = ( int8_t )( rLocation.x );
    coord.m_y = ( int8_t )( rLocation.y );
    return coord;
}

EA_FORCE_INLINE t3dRegionCoord_t t3dInvalidRegionCoord( void ) {
    t3dRegionCoord_t coord;
    coord.m_x = T3D_REGION_COORD_INVALID;
    coord.m_y = T3D_REGION_COORD_INVALID;
    return coord;
}

EA_FORCE_INLINE bool t3dRegionCoordValid( const t3dRegionCoord_t &rCoord ) {
    return rCoord.m_x != T3D_REGION_COORD_INVALID && rCoord.m_y != T3D_REGION_COORD_INVALID;
}

EA_FORCE_INLINE Vector2i t3dUnpackRegionCoord( const t3dRegionCoord_t &rCoord ) {
    return t3dRegionCoordValid( rCoord ) ? Vector2i( ( int )( rCoord.m_x ), ( int )( rCoord.m_y ) ) : s_v2iMax;
}

EA_FORCE_INLINE bool t3dRegionCoordEquals( const t3dRegionCoord_t &rCoord, const Vector2i &rLocation ) {
    return t3dRegionCoordValid( rCoord ) && ( int )( rCoord.m_x ) == rLocation.x && ( int )( rCoord.m_y ) == rLocation.y;
}

EA_FORCE_INLINE bool t3dRegionCoordEquals( const t3dRegionCoord_t &rA, const t3dRegionCoord_t &rB ) {
    return rA.m_x == rB.m_x && rA.m_y == rB.m_y;
}

static_assert( sizeof( t3dRegionCoord_t ) == 2u, "Packed Terrain3D region coordinates must remain two bytes" );

#endif // TERRAIN3D_TYPE_SUPPORT_H
