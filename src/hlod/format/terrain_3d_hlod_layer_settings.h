// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_HLOD_LAYER_SETTINGS_H
#define TERRAIN3D_HLOD_LAYER_SETTINGS_H

#include <stdint.h>
#include <godot_cpp/classes/resource.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Serialized layer contract: zero remains reserved for reading old W1-W7 packages.
// AUTO is an editor-only selection policy. Runtime packages retain a concrete layer type.
typedef enum t3dHLODLayerType_e : uint8_t
{
    T3D_HLOD_LAYER_LEGACY_SIMPLIFIED = 0,
    T3D_HLOD_LAYER_INSTANCING = 1,
    T3D_HLOD_LAYER_MERGED = 2,
    T3D_HLOD_LAYER_SIMPLIFIED = 3,
    T3D_HLOD_LAYER_AUTO = 4 // editor-only cost policy; cooked packages resolve to 1, 2 or 3
} t3dHLODLayerType_t;

typedef enum t3dHLODPayloadType_e : uint8_t
{
    T3D_HLOD_COOKED_MESH = 0,
    T3D_HLOD_INSTANCE_GROUP = 1
} t3dHLODPayloadType_t;

// An editor-authored recipe; cold hashing does not allocate or use resource IDs.
// Cell size is expressed in meters; 128m remains the minimum world region unit.
class Terrain3DHLODLayerSettings : public Resource
{
    GDCLASS( Terrain3DHLODLayerSettings, Resource );

    int32_t m_nLayerType = T3D_HLOD_LAYER_SIMPLIFIED;
    int32_t m_nCellSize = 128;
    int32_t m_nParentLayer = -1;
    int32_t m_nAuthoredLod = -1;
    float m_fLoadingDistance = 2048.0f;
    float m_fTargetErrorPixels = 2.0f;
    float m_fReductionRatio = 0.42f;
    int32_t m_nPartitionTriangleLimit = 250000;
    int32_t m_nPartitionSectionLimit = 48;
    bool m_bSpatialPartitioning = true;
    bool m_bRegionLeaves = false;
    bool m_bSubregionLeaves = false;

protected:
    static void _bind_methods( void );

public:
    void setLayerType( int32_t nType );
    int32_t getLayerType( void ) const;
    void setCellSize( int32_t nMeters );
    int32_t getCellSize( void ) const;
    void setParentLayer( int32_t nLayer );
    int32_t getParentLayer( void ) const;
    void setAuthoredLod( int32_t nLod );
    int32_t getAuthoredLod( void ) const;
    void setLoadingDistance( float fMeters );
    float getLoadingDistance( void ) const;
    void setTargetErrorPixels( float fPixels );
    float getTargetErrorPixels( void ) const;
    void setReductionRatio( float fRatio );
    float getReductionRatio( void ) const;
    void setSpatialPartitioning( bool bEnabled );
    bool getSpatialPartitioning( void ) const;
    void setRegionLeaves( bool bEnabled );
    bool getRegionLeaves( void ) const;
    void setSubregionLeaves( bool bEnabled );
    bool getSubregionLeaves( void ) const;
    void setPartitionTriangleLimit( int32_t nLimit );
    int32_t getPartitionTriangleLimit( void ) const;
    void setPartitionSectionLimit( int32_t nLimit );
    int32_t getPartitionSectionLimit( void ) const;
    int64_t getRecipeHash( void ) const;
};

#endif // TERRAIN3D_HLOD_LAYER_SETTINGS_H
