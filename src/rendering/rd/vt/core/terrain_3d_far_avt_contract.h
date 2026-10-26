// Copyright (c) 2026 Terrain3D Contributors.
// Indexed regional AVT payload ABI. The authored region remains authoritative.
#ifndef TERRAIN3D_FAR_AVT_CONTRACT_H
#define TERRAIN3D_FAR_AVT_CONTRACT_H

#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"
#include "streaming/io/terrain_3d_cold_chunk_format.h"

// Schema 2 deliberately invalidates 24-record caches: the V1 import
// consumes only mip zero, and mip generation belongs to the V1 source cook.
#define T3D_FAR_AVT_SCHEMA_VERSION 2u
#define T3D_FAR_AVT_MIP_COUNT 1u
#define T3D_FAR_AVT_CHANNEL_COUNT 3u
#define T3D_FAR_AVT_REGION_RECORD_COUNT ( T3D_FAR_AVT_MIP_COUNT * T3D_FAR_AVT_CHANNEL_COUNT )

// Index and height are intentionally exact. The authoring tint is an unencoded
// RGBA8 input for V1 BC1 cooking, never another physical GPU texture owner.
typedef enum t3dFarAvtChannel_e : uint32_t
{
    T3D_FAR_AVT_TINT_RGBA8 = 1u,
    T3D_FAR_AVT_PACKED_INDEX_R8 = 2u,
    T3D_FAR_AVT_HEIGHT_R16 = 3u
} t3dFarAvtChannel_t;

inline uint32_t t3dFarAvtBytesPerTexel( const uint32_t nChannel )
{
    if ( nChannel == T3D_FAR_AVT_TINT_RGBA8 ) return 4u;
    if ( nChannel == T3D_FAR_AVT_PACKED_INDEX_R8 ) return 1u;
    if ( nChannel == T3D_FAR_AVT_HEIGHT_R16 ) return 2u;
    return 0u;
}

inline bool t3dFarAvtValidateRecord( const t3dColdChunkRecord_t &r )
{
    const uint32_t components = t3dFarAvtBytesPerTexel( r.m_nKind );
    return components && r.m_nLevel < T3D_FAR_AVT_MIP_COUNT &&
           r.m_nFormat == r.m_nKind &&
           r.m_nDecodedBytes == T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE * components &&
           t3dColdChunkValidateCodec( r );
}

#endif // TERRAIN3D_FAR_AVT_CONTRACT_H
