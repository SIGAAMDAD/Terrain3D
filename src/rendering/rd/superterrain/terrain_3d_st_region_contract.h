// Frozen SuperTerrain ST_REGION_V1 producer/consumer ABI (ST05).
#ifndef T3D_ST_PRODUCTION_REGION_CONTRACT_H
#define T3D_ST_PRODUCTION_REGION_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#define T3D_ST_REGION_VERSION                    1u
#define T3D_ST_REGION_SIZE_BYTES                64u
#define T3D_ST_REGION_MAP_SIDE                  32u
#define T3D_ST_REGION_SIDE                     128u
#define T3D_ST_REGION_COUNT                   1024u
#define T3D_ST_INVALID_ID                 0xffffffffu
#define T3D_ST_REGION_READY_FLAG                 1u
#define T3D_ST_REGION_HAS_MATERIAL_FLAG          2u
#define T3D_ST_REGION_HAS_HEIGHT_FLAG            4u
#define T3D_ST_REGION_HAS_MASK_FLAG              8u
#define T3D_ST_REGION_VALID_MASK                15u

#define T3D_ST_MATERIAL_CHANNELS                  3u
#define T3D_ST_MATERIAL_PAGE_INTERIOR           128u
#define T3D_ST_MATERIAL_PAGE_GUTTER               4u
#define T3D_ST_MATERIAL_PHYSICAL_TILE           136u

#define T3D_ST_VHM_PAGE_INTERIOR                 64u
#define T3D_ST_VHM_PAGE_GUTTER                    2u
#define T3D_ST_VHM_PHYSICAL_TILE                 68u
#define T3D_ST_VHM_MIP_COUNT                      8u
#define T3D_ST_VHM_PAGES_PER_REGION              11u
#define T3D_ST_VHM_TAIL_FIRST_MIP                 3u
#define T3D_ST_VHM_TAIL_MIP_COUNT                 5u
#define T3D_ST_VHM_TAIL_SLOT_WIDTH               52u
#define T3D_ST_VHM_TAIL_SLOT_HEIGHT              20u
#define T3D_ST_VHM_TAIL_ATLAS_WIDTH            1664u
#define T3D_ST_VHM_TAIL_ATLAS_HEIGHT            640u
#define T3D_ST_VHM_TAIL_BYTES               2129920u
#define T3D_ST_VHM_PAGE_BYTES                  9248u
#define T3D_ST_VHM_PAGE_MIP_MASK             0x0000ffffu
#define T3D_ST_VHM_PAGE_VALID_FLAG           0x00010000u
#define T3D_ST_VHM_PAGE_RESIDENT_FLAG        0x00020000u
#define T3D_ST_VHM_PAGE_PINNED_FLAG          0x00040000u
#define T3D_ST_VHM_TAIL_VALID_FLAG                    1u

#define T3D_ST_MASK_PAGE_INTERIOR                64u
#define T3D_ST_MASK_PAGES_PER_REGION              4u
#define T3D_ST_MASK_BITPLANES                     3u
#define T3D_ST_MASK_PLANE_WORDS                 128u
#define T3D_ST_MASK_PAGE_BYTES                 1536u
#define T3D_ST_MASK_MAX_PINNED_BYTES         6291456u
#define T3D_ST_MASK_ENTRY_VALID_FLAG             0x40000000u
#define T3D_ST_MASK_ENTRY_UNIFORM_FLAG           0x80000000u
#define T3D_ST_MASK_ENTRY_VALUE_MASK                    7u

#define T3D_ST_SIDECAR_FORMAT_VERSION             1u
#define T3D_ST_SIDECAR_VHM_MAGIC           0x31484d56u // "VMH1" in LE
#define T3D_ST_SIDECAR_MASK_MAGIC          0x314b4d53u // "SMK1" in LE
#define T3D_ST_RECIPE_VERSION                     1u

// 64-byte replacement for t3dRdTerrainRegionGpu_t. std430-compatible.
// Existing MegaVT fields and physical flags retain their original offsets.
// Source revision (u64) and recipe fingerprint (u64) stay in the cold
// transaction/sidecar manifest, not the GPU region record.
typedef struct alignas( 16 ) t3dStRegionRecord_s
{
    uint32_t m_nPackedPhysicalFlags;        // +0
    uint32_t m_nMegaVTAllocation;          // +4
    uint32_t m_nMegaVTGeneration;          // +8
    uint32_t m_nMegaVTFlags;               // +12
    float    m_vMegaVTWorldToUv[4];        // +16
    uint32_t m_nComposedVTAllocation;      // +32
    uint32_t m_nComposedVTGeneration;      // +36
    uint32_t m_nVhmRootIndex;              // +40
    uint32_t m_nVhmGeneration;             // +44
    uint32_t m_nMaskRootIndex;             // +48
    uint32_t m_nMaskGeneration;            // +52
    uint32_t m_nPublicationGeneration;     // +56
    uint32_t m_nRegionStateFlags;          // +60
} t3dStRegionRecord_t;

// All VHM page IDs are stable: region * 11 + mip prefix + row-major page.
// Low 16 bits of m_nPackedMipFlags = mip; bits 16..18 = valid/resident/pinned.
typedef struct t3dStRegionVhmPageEntry_s
{
    uint32_t m_nPhysicalSlot;
    uint32_t m_nParentPage;
    uint32_t m_nGeneration;
    uint32_t m_nPackedMipFlags;
} t3dStRegionVhmPageEntry_t;

// A 52x20 tail slot holds mip 3..7 with 2-texel gutters per mip.
typedef struct t3dStRegionVhmTail_s
{
    uint32_t m_nAtlasX;
    uint32_t m_nAtlasY;
    uint32_t m_nFirstMip;
    uint32_t m_nMipCount;
    uint32_t m_nGeneration;
    uint32_t m_nFlags;
    uint32_t m_nReserved0;
    uint32_t m_nReserved1;
} t3dStRegionVhmTail_t;

// Bit 30 = entry valid; bit 31 = uniform (low three bits hold 1+2 mask bits).
// Non-uniform entries address an immutable pinned exact bitplane fallback.
typedef struct t3dStMaskPageEntry_s
{
    uint32_t m_nPhysicalSlot;
    uint32_t m_nPinnedWordOffset;
    uint32_t m_nGeneration;
    uint32_t m_nPackedState;
} t3dStMaskPageEntry_t;

// The VHM and MASK sidecar chunks use the same 48-byte little-endian header.
// CRC32 is computed over the payload bytes, not the header. No native padding.
typedef struct t3dStSidecarHeader_s
{
    uint32_t m_nMagic;
    uint32_t m_nVersion;
    uint32_t m_nRegionIndex;
    uint32_t m_nGeneration;
    uint64_t m_nSourceSignature;
    uint64_t m_nRecipeSignature;
    uint32_t m_nEntryCount;
    uint32_t m_nPayloadBytes;
    uint32_t m_nPayloadCrc32;
    uint32_t m_nFlags;
} t3dStSidecarHeader_t;

static_assert( sizeof( t3dStRegionRecord_t ) == T3D_ST_REGION_SIZE_BYTES, "ST_REGION_V1 size" );
static_assert( alignof( t3dStRegionRecord_t ) == 16u, "ST_REGION_V1 std430 alignment" );
static_assert( offsetof( t3dStRegionRecord_t, m_vMegaVTWorldToUv ) == 16u, "ST_REGION_V1 vec4 offset" );
static_assert( offsetof( t3dStRegionRecord_t, m_nComposedVTAllocation ) == 32u, "ST_REGION_V1 composed offset" );
static_assert( offsetof( t3dStRegionRecord_t, m_nVhmRootIndex ) == 40u, "ST_REGION_V1 VHM offset" );
static_assert( offsetof( t3dStRegionRecord_t, m_nMaskRootIndex ) == 48u, "ST_REGION_V1 mask offset" );
static_assert( offsetof( t3dStRegionRecord_t, m_nPublicationGeneration ) == 56u, "ST_REGION_V1 generation offset" );
static_assert( offsetof( t3dStRegionRecord_t, m_nRegionStateFlags ) == 60u, "ST_REGION_V1 state offset" );
static_assert( sizeof( t3dStRegionVhmPageEntry_t ) == 16u, "ST VHM page entry size" );
static_assert( sizeof( t3dStRegionVhmTail_t ) == 32u, "ST VHM tail size" );
static_assert( sizeof( t3dStMaskPageEntry_t ) == 16u, "ST mask page entry size" );
static_assert( sizeof( t3dStSidecarHeader_t ) == 48u, "ST sidecar header size" );
static_assert( offsetof( t3dStSidecarHeader_t, m_nPayloadCrc32 ) == 40u, "ST sidecar CRC offset" );
static_assert( T3D_ST_VHM_TAIL_ATLAS_WIDTH * T3D_ST_VHM_TAIL_ATLAS_HEIGHT * 2u == T3D_ST_VHM_TAIL_BYTES,
               "ST VHM pinned root footprint" );
static_assert( T3D_ST_MASK_PAGES_PER_REGION * T3D_ST_REGION_COUNT * T3D_ST_MASK_PAGE_BYTES ==
               T3D_ST_MASK_MAX_PINNED_BYTES, "ST mask worst-case exact fallback" );

// Functions are test helpers only. Hot-path integration belongs to ST03/ST04.
static inline bool t3dStRegionReady( const t3dStRegionRecord_t *pRegion )
{
    if ( pRegion == nullptr ) {
        return false;
    }
    if ( ( pRegion->m_nPackedPhysicalFlags & 0x80000000u ) == 0u ||
         pRegion->m_nPublicationGeneration == 0u ||
         ( pRegion->m_nRegionStateFlags & T3D_ST_REGION_VALID_MASK ) != T3D_ST_REGION_VALID_MASK ) {
        return false;
    }
    if ( pRegion->m_nComposedVTAllocation == T3D_ST_INVALID_ID ||
         pRegion->m_nVhmRootIndex == T3D_ST_INVALID_ID ||
         pRegion->m_nMaskRootIndex == T3D_ST_INVALID_ID ) {
        return false;
    }
    const uint32_t generation = pRegion->m_nPublicationGeneration;
    return pRegion->m_nComposedVTGeneration == generation &&
           pRegion->m_nVhmGeneration == generation &&
           pRegion->m_nMaskGeneration == generation;
}

static inline uint32_t t3dStVhmMipPageBase( const uint32_t nMip )
{
    if ( nMip >= T3D_ST_VHM_MIP_COUNT ) {
        return T3D_ST_INVALID_ID;
    }
    return nMip == 0u ? 0u : 3u + nMip;
}

static inline uint32_t t3dStVhmPageId( const uint32_t nRegion, const uint32_t nMip,
                                      const uint32_t nPageX, const uint32_t nPageY )
{
    if ( nRegion >= T3D_ST_REGION_COUNT || nMip >= T3D_ST_VHM_MIP_COUNT ) {
        return T3D_ST_INVALID_ID;
    }
    const uint32_t width = nMip == 0u ? 2u : 1u;
    if ( nPageX >= width || nPageY >= width ) {
        return T3D_ST_INVALID_ID;
    }
    return nRegion * T3D_ST_VHM_PAGES_PER_REGION + t3dStVhmMipPageBase( nMip ) + nPageY * width + nPageX;
}

// Each mip0 quadrant has one parent at mip1. The pinned mip7 root has none.
static inline uint32_t t3dStVhmParentPageId( const uint32_t nRegion, const uint32_t nMip,
                                            const uint32_t nPageX, const uint32_t nPageY )
{
    if ( t3dStVhmPageId( nRegion, nMip, nPageX, nPageY ) == T3D_ST_INVALID_ID ||
         nMip + 1u >= T3D_ST_VHM_MIP_COUNT ) {
        return T3D_ST_INVALID_ID;
    }
    return t3dStVhmPageId( nRegion, nMip + 1u, nPageX >> 1u, nPageY >> 1u );
}

static inline uint32_t t3dStMaskPageId( const uint32_t nRegion, const uint32_t nPageX, const uint32_t nPageY )
{
    if ( nRegion >= T3D_ST_REGION_COUNT || nPageX >= 2u || nPageY >= 2u ) {
        return T3D_ST_INVALID_ID;
    }
    return nRegion * T3D_ST_MASK_PAGES_PER_REGION + nPageY * 2u + nPageX;
}

static inline uint32_t t3dStMaskSampleBit( const uint32_t *pPlanes, const uint32_t nX,
                                          const uint32_t nY )
{
    const uint32_t sample = nY * T3D_ST_MASK_PAGE_INTERIOR + nX;
    const uint32_t word = sample >> 5u;
    const uint32_t bit = sample & 31u;
    const uint32_t solid = ( pPlanes[word] >> bit ) & 1u;
    const uint32_t alphaLo = ( pPlanes[T3D_ST_MASK_PLANE_WORDS + word] >> bit ) & 1u;
    const uint32_t alphaHi = ( pPlanes[2u * T3D_ST_MASK_PLANE_WORDS + word] >> bit ) & 1u;
    return solid | ( alphaLo << 1u ) | ( alphaHi << 2u );
}

#endif // T3D_ST_PRODUCTION_REGION_CONTRACT_H
