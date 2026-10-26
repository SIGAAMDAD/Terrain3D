// Copyright (c) 2026 Terrain3D contributors.
#ifndef TERRAIN3D_HLOD_INSTANCE_CODEC_H
#define TERRAIN3D_HLOD_INSTANCE_CODEC_H

#include <EASTL/vector.h>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <stdint.h>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

#define T3D_HLOD_GROUP_MAGIC ( 0x474c4854u )
#define T3D_HLOD_GROUP_VERSION ( 1u )

// Compact, versioned, little-endian instance-group payload for U04.
// A prototype identifies globally deduplicatable cooked geometry and a
// canonical material. All transforms use the HLOD cell-local origin.
typedef struct t3dHLODGroupPrototype_s
{
    uint64_t m_nGeometryKey = 0u;
    uint64_t m_nMaterialKey = 0u;
    uint32_t m_nVertexBytes = 0u;
    uint32_t m_nIndexBytes = 0u;
    float m_fGeometricError = 0.0f;
    float m_fBounds[6]{}; // local min xyz, extents xyz
} t3dHLODGroupPrototype_t;

typedef struct t3dHLODGroupInstance_s
{
    uint32_t m_nPrototype = 0u;
    uint32_t m_nSourceId = 0u;
    float m_fAffine[12]{}; // 3x4 row major, cell local
} t3dHLODGroupInstance_t;

typedef struct t3dHLODGroupData_s
{
    eastl::vector< t3dHLODGroupPrototype_t > m_prototypes;
    eastl::vector< t3dHLODGroupInstance_t > m_instances;
} t3dHLODGroupData_t;

class Terrain3DHLODInstanceCodec
{
public:
    static PackedByteArray encode( const t3dHLODGroupData_t &rSource );
    static bool decode( const PackedByteArray &rPayload, t3dHLODGroupData_t &rOutput );
};
#endif
