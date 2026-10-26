// Copyright 2026 Terrain3D contributors.
// ST03/ST04 shared cold sidecar framing. ST05 owns final pack integration.
#ifndef TERRAIN3D_ST_SIDECAR_WIRE_H
#define TERRAIN3D_ST_SIDECAR_WIRE_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

#define T3D_ST_SIDECAR_HEADER_BYTES 48u
#define T3D_ST_SIDECAR_VERSION 1u
#define T3D_ST_SIDECAR_MASK_MAGIC 0x314b4d53u
#define T3D_ST_SIDECAR_VHM_MAGIC 0x31484d56u

struct t3dStSidecarInfo_t
{
    uint32_t m_nMagic = 0u;
    uint32_t m_nVersion = 0u;
    uint32_t m_nRegionIndex = 0u;
    uint32_t m_nGeneration = 0u;
    uint64_t m_nSourceSignature = 0u;
    uint64_t m_nRecipeSignature = 0u;
    uint32_t m_nEntryCount = 0u;
    uint32_t m_nPayloadBytes = 0u;
    uint32_t m_nPayloadCrc32 = 0u;
    uint32_t m_nFlags = 0u;
};

static inline uint16_t t3dStLoadLe16( const uint8_t *p )
{
    return uint16_t( uint16_t( p[0] ) | ( uint16_t( p[1] ) << 8u ) );
}
static inline uint32_t t3dStLoadLe32( const uint8_t *p )
{
    return uint32_t( p[0] ) | ( uint32_t( p[1] ) << 8u ) |
           ( uint32_t( p[2] ) << 16u ) | ( uint32_t( p[3] ) << 24u );
}
static inline uint64_t t3dStLoadLe64( const uint8_t *p )
{
    return uint64_t( t3dStLoadLe32( p ) ) | ( uint64_t( t3dStLoadLe32( p + 4u ) ) << 32u );
}
static inline void t3dStStoreLe16( uint8_t *p, const uint16_t n )
{
    p[0] = uint8_t( n ); p[1] = uint8_t( n >> 8u );
}
static inline void t3dStStoreLe32( uint8_t *p, const uint32_t n )
{
    p[0] = uint8_t( n ); p[1] = uint8_t( n >> 8u );
    p[2] = uint8_t( n >> 16u ); p[3] = uint8_t( n >> 24u );
}
static inline void t3dStStoreLe64( uint8_t *p, const uint64_t n )
{
    t3dStStoreLe32( p, uint32_t( n ) );
    t3dStStoreLe32( p + 4u, uint32_t( n >> 32u ) );
}

// IEEE CRC32 of only the payload. Independent of host byte order.
static inline uint32_t t3dStPayloadCrc32( const uint8_t *pData, const size_t nBytes )
{
    uint32_t crc = 0xffffffffu;
    for ( size_t i = 0u; i < nBytes; ++i )
    {
        crc ^= pData[i];
        for ( uint32_t bit = 0u; bit < 8u; ++bit )
        {
            const uint32_t mask = 0u - ( crc & 1u );
            crc = ( crc >> 1u ) ^ ( 0xedb88320u & mask );
        }
    }
    return ~crc;
}

// A candidate is constructed privately; the caller publishes the resulting
// vector only after every sibling sidecar and the COMPOSED package validates.
static inline bool t3dStWriteSidecar( const t3dStSidecarInfo_t &rInfo,
                                     const std::vector<uint8_t> &rPayload,
                                     std::vector<uint8_t> &rOutput )
{
    if ( rInfo.m_nVersion != T3D_ST_SIDECAR_VERSION ||
         rInfo.m_nRegionIndex >= 1024u || rInfo.m_nGeneration == 0u ||
         rInfo.m_nSourceSignature == 0u || rInfo.m_nRecipeSignature == 0u ||
         rInfo.m_nEntryCount == 0u || rPayload.empty() ||
         rPayload.size() > UINT32_MAX - T3D_ST_SIDECAR_HEADER_BYTES )
    {
        return false;
    }
    std::vector<uint8_t> result( T3D_ST_SIDECAR_HEADER_BYTES + rPayload.size(), 0u );
    t3dStStoreLe32( result.data(), rInfo.m_nMagic );
    t3dStStoreLe32( result.data() + 4u, rInfo.m_nVersion );
    t3dStStoreLe32( result.data() + 8u, rInfo.m_nRegionIndex );
    t3dStStoreLe32( result.data() + 12u, rInfo.m_nGeneration );
    t3dStStoreLe64( result.data() + 16u, rInfo.m_nSourceSignature );
    t3dStStoreLe64( result.data() + 24u, rInfo.m_nRecipeSignature );
    t3dStStoreLe32( result.data() + 32u, rInfo.m_nEntryCount );
    t3dStStoreLe32( result.data() + 36u, uint32_t( rPayload.size() ) );
    t3dStStoreLe32( result.data() + 40u, t3dStPayloadCrc32( rPayload.data(), rPayload.size() ) );
    t3dStStoreLe32( result.data() + 44u, rInfo.m_nFlags );
    for ( size_t i = 0u; i < rPayload.size(); ++i )
    {
        result[T3D_ST_SIDECAR_HEADER_BYTES + i] = rPayload[i];
    }
    rOutput.swap( result );
    return true;
}

// The expected signatures/generation are the enclosing transaction's values;
// zero is not a wildcard. Reject wrong-version/stale/replayed candidates.
static inline bool t3dStReadSidecar( const uint8_t *pBytes, const size_t nBytes,
                                    const uint32_t nMagic, const uint32_t nRegionIndex,
                                    const uint32_t nGeneration, const uint64_t nSourceSignature,
                                    const uint64_t nRecipeSignature, t3dStSidecarInfo_t &rInfo )
{
    if ( pBytes == nullptr || nBytes < T3D_ST_SIDECAR_HEADER_BYTES ||
         nRegionIndex >= 1024u || nGeneration == 0u ||
         nSourceSignature == 0u || nRecipeSignature == 0u )
    {
        return false;
    }
    t3dStSidecarInfo_t info = {};
    info.m_nMagic = t3dStLoadLe32( pBytes );
    info.m_nVersion = t3dStLoadLe32( pBytes + 4u );
    info.m_nRegionIndex = t3dStLoadLe32( pBytes + 8u );
    info.m_nGeneration = t3dStLoadLe32( pBytes + 12u );
    info.m_nSourceSignature = t3dStLoadLe64( pBytes + 16u );
    info.m_nRecipeSignature = t3dStLoadLe64( pBytes + 24u );
    info.m_nEntryCount = t3dStLoadLe32( pBytes + 32u );
    info.m_nPayloadBytes = t3dStLoadLe32( pBytes + 36u );
    info.m_nPayloadCrc32 = t3dStLoadLe32( pBytes + 40u );
    info.m_nFlags = t3dStLoadLe32( pBytes + 44u );
    if ( info.m_nMagic != nMagic || info.m_nVersion != T3D_ST_SIDECAR_VERSION ||
         info.m_nRegionIndex != nRegionIndex || info.m_nGeneration != nGeneration ||
         info.m_nSourceSignature != nSourceSignature || info.m_nRecipeSignature != nRecipeSignature ||
         info.m_nEntryCount == 0u || info.m_nFlags != 0u ||
         uint64_t( info.m_nPayloadBytes ) + T3D_ST_SIDECAR_HEADER_BYTES != nBytes ||
         t3dStPayloadCrc32( pBytes + T3D_ST_SIDECAR_HEADER_BYTES, info.m_nPayloadBytes ) != info.m_nPayloadCrc32 )
    {
        return false;
    }
    rInfo = info;
    return true;
}

#endif // TERRAIN3D_ST_SIDECAR_WIRE_H
