// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "hlod/format/terrain_3d_hlod_layer_settings.h"
#include "integration/godot/terrain_3d_binding.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <string.h>

#define HLOD_LAYER_SETTER( method, type, field, expression ) \
    void Terrain3DHLODLayerSettings::method( type value ) { \
        const type next = ( expression ); \
        if ( field == next ) { return; } \
        field = next; emit_changed(); \
    }

// Old serialized recipe 0 is migrated to the meshoptimizer SIMPLIFIED cook.
// The v1 package reader continues to interpret layer 0 without mutation.
HLOD_LAYER_SETTER( setLayerType, int32_t, m_nLayerType,
    value == T3D_HLOD_LAYER_LEGACY_SIMPLIFIED ? ( int32_t ) T3D_HLOD_LAYER_SIMPLIFIED :
    CLAMP( value, ( int32_t ) T3D_HLOD_LAYER_INSTANCING, ( int32_t ) T3D_HLOD_LAYER_AUTO ) )
HLOD_LAYER_SETTER( setCellSize, int32_t, m_nCellSize, CLAMP( value, ( int32_t ) 128, ( int32_t ) 8192 ) )
HLOD_LAYER_SETTER( setParentLayer, int32_t, m_nParentLayer, CLAMP( value, ( int32_t ) -1, ( int32_t ) 6 ) )
HLOD_LAYER_SETTER( setAuthoredLod, int32_t, m_nAuthoredLod, CLAMP( value, ( int32_t ) -1, ( int32_t ) 9 ) )
HLOD_LAYER_SETTER( setLoadingDistance, float, m_fLoadingDistance, CLAMP( value, 128.0f, 65536.0f ) )
HLOD_LAYER_SETTER( setTargetErrorPixels, float, m_fTargetErrorPixels, CLAMP( value, 0.1f, 64.0f ) )
HLOD_LAYER_SETTER( setReductionRatio, float, m_fReductionRatio, CLAMP( value, 0.01f, 1.0f ) )
HLOD_LAYER_SETTER( setPartitionTriangleLimit, int32_t, m_nPartitionTriangleLimit, CLAMP( value, ( int32_t ) 1000, ( int32_t ) 20000000 ) )
HLOD_LAYER_SETTER( setPartitionSectionLimit, int32_t, m_nPartitionSectionLimit, CLAMP( value, ( int32_t ) 1, ( int32_t ) 4096 ) )
HLOD_LAYER_SETTER( setSpatialPartitioning, bool, m_bSpatialPartitioning, value )
HLOD_LAYER_SETTER( setRegionLeaves, bool, m_bRegionLeaves, value )
HLOD_LAYER_SETTER( setSubregionLeaves, bool, m_bSubregionLeaves, value )

int32_t Terrain3DHLODLayerSettings::getLayerType( void ) const { return m_nLayerType; }
int32_t Terrain3DHLODLayerSettings::getCellSize( void ) const { return m_nCellSize; }
int32_t Terrain3DHLODLayerSettings::getParentLayer( void ) const { return m_nParentLayer; }
int32_t Terrain3DHLODLayerSettings::getAuthoredLod( void ) const { return m_nAuthoredLod; }
float Terrain3DHLODLayerSettings::getLoadingDistance( void ) const { return m_fLoadingDistance; }
float Terrain3DHLODLayerSettings::getTargetErrorPixels( void ) const { return m_fTargetErrorPixels; }
float Terrain3DHLODLayerSettings::getReductionRatio( void ) const { return m_fReductionRatio; }
int32_t Terrain3DHLODLayerSettings::getPartitionTriangleLimit( void ) const { return m_nPartitionTriangleLimit; }
int32_t Terrain3DHLODLayerSettings::getPartitionSectionLimit( void ) const { return m_nPartitionSectionLimit; }
bool Terrain3DHLODLayerSettings::getSpatialPartitioning( void ) const { return m_bSpatialPartitioning; }
bool Terrain3DHLODLayerSettings::getRegionLeaves( void ) const { return m_bRegionLeaves; }
bool Terrain3DHLODLayerSettings::getSubregionLeaves( void ) const { return m_bSubregionLeaves; }

static uint64_t t3dHlodRecipeHashWord( uint64_t nHash, uint32_t nWord )
{
    // Defined little-endian byte order, stable across builds/platforms.
    for ( int i = 0; i < 4; ++i )
    {
        nHash ^= ( uint8_t ) ( nWord >> ( i * 8 ) );
        nHash *= UINT64_C( 1099511628211 );
    }
    return nHash;
}

static uint32_t t3dHlodRecipeFloatBits( float fValue )
{
    uint32_t nBits = 0;
    memcpy( &nBits, &fValue, sizeof( nBits ) );
    return nBits;
}

int64_t Terrain3DHLODLayerSettings::getRecipeHash( void ) const
{
    uint64_t nHash = UINT64_C( 14695981039346656037 );
    nHash = t3dHlodRecipeHashWord( nHash, 5 ); // recipe schema version (64m independently streamed leaves)
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_nLayerType );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_nCellSize );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_nParentLayer );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_nAuthoredLod );
    nHash = t3dHlodRecipeHashWord( nHash, t3dHlodRecipeFloatBits( m_fLoadingDistance ) );
    nHash = t3dHlodRecipeHashWord( nHash, t3dHlodRecipeFloatBits( m_fTargetErrorPixels ) );
    nHash = t3dHlodRecipeHashWord( nHash, t3dHlodRecipeFloatBits( m_fReductionRatio ) );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_bSpatialPartitioning );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_bRegionLeaves );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_bSubregionLeaves );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_nPartitionTriangleLimit );
    nHash = t3dHlodRecipeHashWord( nHash, ( uint32_t ) m_nPartitionSectionLimit );
    return ( int64_t ) ( nHash & INT64_MAX );
}

void Terrain3DHLODLayerSettings::_bind_methods( void )
{
    ADD_GROUP( "Layer Recipe", "" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setLayerType, getLayerType, "type",
        PropertyInfo( Variant::INT, "layer_type", PROPERTY_HINT_ENUM,
            "Legacy (read-only; converts to Simplified),Instancing,Merged,Simplified,Auto (offline cost)" ), "set_layer_type", "get_layer_type" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setCellSize, getCellSize, "meters",
        PropertyInfo( Variant::INT, "cell_size", PROPERTY_HINT_RANGE, "128,8192,128" ), "set_cell_size", "get_cell_size" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setLoadingDistance, getLoadingDistance, "meters",
        PropertyInfo( Variant::FLOAT, "loading_distance", PROPERTY_HINT_RANGE, "128,65536,1" ), "set_loading_distance", "get_loading_distance" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setParentLayer, getParentLayer, "index",
        PropertyInfo( Variant::INT, "parent_layer", PROPERTY_HINT_RANGE, "-1,6,1" ), "set_parent_layer", "get_parent_layer" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setAuthoredLod, getAuthoredLod, "index",
        PropertyInfo( Variant::INT, "authored_lod", PROPERTY_HINT_RANGE, "-1,9,1" ), "set_authored_lod", "get_authored_lod" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setTargetErrorPixels, getTargetErrorPixels, "pixels",
        PropertyInfo( Variant::FLOAT, "target_error_pixels", PROPERTY_HINT_RANGE, "0.1,64.0,0.1" ),
        "set_target_error_pixels", "get_target_error_pixels" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setReductionRatio, getReductionRatio, "ratio",
        PropertyInfo( Variant::FLOAT, "reduction_ratio", PROPERTY_HINT_RANGE, "0.01,1.0,0.01" ),
        "set_reduction_ratio", "get_reduction_ratio" );
    ADD_GROUP( "Spatial Partitioning", "" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setSpatialPartitioning, getSpatialPartitioning, "enabled",
        PropertyInfo( Variant::BOOL, "spatial_partitioning" ), "set_spatial_partitioning", "get_spatial_partitioning" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setRegionLeaves, getRegionLeaves, "enabled",
        PropertyInfo( Variant::BOOL, "region_leaves" ), "set_region_leaves", "get_region_leaves" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setSubregionLeaves, getSubregionLeaves, "enabled",
        PropertyInfo( Variant::BOOL, "subregion_leaves" ), "set_subregion_leaves", "get_subregion_leaves" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setPartitionTriangleLimit, getPartitionTriangleLimit, "triangles",
        PropertyInfo( Variant::INT, "partition_triangle_limit", PROPERTY_HINT_RANGE, "1000,20000000,1000" ),
        "set_partition_triangle_limit", "get_partition_triangle_limit" );
    T3D_BIND_PROPERTY( Terrain3DHLODLayerSettings, setPartitionSectionLimit, getPartitionSectionLimit, "sections",
        PropertyInfo( Variant::INT, "partition_section_limit", PROPERTY_HINT_RANGE, "1,4096,1" ),
        "set_partition_section_limit", "get_partition_section_limit" );
    ClassDB::bind_method( D_METHOD( "get_recipe_hash" ), &Terrain3DHLODLayerSettings::getRecipeHash );
}
