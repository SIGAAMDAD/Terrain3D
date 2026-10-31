// BR14: conservative classification of exact authored control-source keys.
// No draw splitting: only the caller can prove a complete draw batch has one
// safe class. Missing data, autotexturing and region borders are UNKNOWN.
#ifndef TERRAIN3D_MATERIAL_TILE_CLASS_H
#define TERRAIN3D_MATERIAL_TILE_CLASS_H

#include <stdint.h>
#include <string.h>

#define T3D_MATERIAL_TILE_CLASS_CONTROL_REGION_SIDE 128u
#define T3D_MATERIAL_TILE_CLASS_CONTROL_TILE_SIDE 16u
#define T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE 8u
#define T3D_MATERIAL_TILE_CLASS_CONTROL_TILES ( T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE * T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE )
#define T3D_MATERIAL_TILE_CLASS_CONTROL_SAMPLE_KEY_MASK ( ( 0x1fu << 27u ) | ( 0x1fu << 22u ) | ( 0xfu << 10u ) | ( 0x7u << 7u ) )
#define T3D_MATERIAL_TILE_CLASS_CONTROL_AUTO_BIT ( 1u )

enum t3dMaterialTileClass_t : uint8_t
{
    T3D_MATERIAL_TILE_UNKNOWN = 0u,
    T3D_MATERIAL_TILE_HOMOGENEOUS = 1u,
    T3D_MATERIAL_TILE_TWO_KEY = 2u,
    T3D_MATERIAL_TILE_GENERAL = 3u
};

struct t3dMaterialTileRegionClasses_t
{
    uint8_t m_szClass[T3D_MATERIAL_TILE_CLASS_CONTROL_TILES]{};
    uint32_t m_nRevision = 0u;
    bool m_bReady = false;
};

inline void t3d_material_tiles_invalidate( t3dMaterialTileRegionClasses_t &rRegion )
{
    memset( rRegion.m_szClass, T3D_MATERIAL_TILE_UNKNOWN, sizeof( rRegion.m_szClass ) );
    rRegion.m_bReady = false;
    if ( ++rRegion.m_nRevision == 0u ) rRegion.m_nRevision = 1u;
}

// A texel's blend/metadata bits may differ while its source key is identical.
// If an auto-control bit occurs anywhere in the halo, source selection depends
// on normals/height and cannot be certified using the authored control map.
inline uint8_t t3d_material_tile_classify( const uint32_t *pControl,
                                          uint32_t nWidth, uint32_t nHeight,
                                          uint32_t nTileX, uint32_t nTileY )
{
    if ( !pControl || nWidth != T3D_MATERIAL_TILE_CLASS_CONTROL_REGION_SIDE ||
         nHeight != T3D_MATERIAL_TILE_CLASS_CONTROL_REGION_SIDE ||
         nTileX >= T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE || nTileY >= T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE ||
         nTileX == 0u || nTileY == 0u ||
         nTileX + 1u == T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE ||
         nTileY + 1u == T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE ) return T3D_MATERIAL_TILE_UNKNOWN;
    // Include BOTH sides of the bilinear four-corner footprint. Border tiles
    // are unknown because sampling can cross into an adjacent region.
    const uint32_t nBeginX = nTileX * T3D_MATERIAL_TILE_CLASS_CONTROL_TILE_SIDE - 1u;
    const uint32_t nBeginY = nTileY * T3D_MATERIAL_TILE_CLASS_CONTROL_TILE_SIDE - 1u;
    const uint32_t nEndX = ( nTileX + 1u ) * T3D_MATERIAL_TILE_CLASS_CONTROL_TILE_SIDE + 1u;
    const uint32_t nEndY = ( nTileY + 1u ) * T3D_MATERIAL_TILE_CLASS_CONTROL_TILE_SIDE + 1u;
    uint32_t keys[3]{};
    uint32_t nKeys = 0u;
    for ( uint32_t y = nBeginY; y <= nEndY; ++y ) {
        for ( uint32_t x = nBeginX; x <= nEndX; ++x ) {
            const uint32_t control = pControl[y * nWidth + x];
            if ( ( control & T3D_MATERIAL_TILE_CLASS_CONTROL_AUTO_BIT ) != 0u ) return T3D_MATERIAL_TILE_UNKNOWN;
            const uint32_t key = control & T3D_MATERIAL_TILE_CLASS_CONTROL_SAMPLE_KEY_MASK;
            bool known = false;
            for ( uint32_t i = 0u; i < nKeys; ++i ) known |= keys[i] == key;
            // Continue checking for auto controls even after the third key:
            // auto selection may invalidate the entire classification.
            if ( !known && nKeys < 3u ) keys[nKeys++] = key;
        }
    }
    return nKeys == 1u ? T3D_MATERIAL_TILE_HOMOGENEOUS :
           nKeys == 2u ? T3D_MATERIAL_TILE_TWO_KEY : T3D_MATERIAL_TILE_GENERAL;
}

inline bool t3d_material_tiles_rebuild( t3dMaterialTileRegionClasses_t &rRegion,
                                       const uint32_t *pControl, uint32_t nWidth, uint32_t nHeight )
{
    t3d_material_tiles_invalidate( rRegion );
    if ( !pControl || nWidth != T3D_MATERIAL_TILE_CLASS_CONTROL_REGION_SIDE ||
         nHeight != T3D_MATERIAL_TILE_CLASS_CONTROL_REGION_SIDE ) return false;
    for ( uint32_t y = 0u; y < T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE; ++y )
        for ( uint32_t x = 0u; x < T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE; ++x )
            rRegion.m_szClass[y * T3D_MATERIAL_TILE_CLASS_CONTROL_TILES_SIDE + x] =
                t3d_material_tile_classify( pControl, nWidth, nHeight, x, y );
    rRegion.m_bReady = true;
    return true;
}

// Caller-owned coverage is an exact *whole draw* tile mask. It may not be
// synthesized from only the visible subset of a GPU-driven batch. Every
// covered tile must have a proof; zero/unknown coverage is never promoted.
inline uint8_t t3d_material_tiles_certify_covered_batch(
    const t3dMaterialTileRegionClasses_t *pRegions, uint32_t nCount,
    const uint32_t *pExpectedRevisions, const uint64_t *pWholeDrawTileCoverage )
{
    if ( !pRegions || !nCount || !pExpectedRevisions || !pWholeDrawTileCoverage )
        return T3D_MATERIAL_TILE_UNKNOWN;
    uint8_t maxClass = T3D_MATERIAL_TILE_HOMOGENEOUS;
    bool anyCoverage = false;
    for ( uint32_t i = 0u; i < nCount; ++i ) {
        const t3dMaterialTileRegionClasses_t &r = pRegions[i];
        if ( !r.m_bReady || r.m_nRevision != pExpectedRevisions[i] || !pExpectedRevisions[i] )
            return T3D_MATERIAL_TILE_UNKNOWN;
        const uint64_t coverage = pWholeDrawTileCoverage[i];
        for ( uint32_t tile = 0u; tile < T3D_MATERIAL_TILE_CLASS_CONTROL_TILES; ++tile ) {
            if ( ( coverage & ( uint64_t( 1 ) << tile ) ) == 0u ) continue;
            const uint8_t eClass = r.m_szClass[tile];
            if ( eClass < T3D_MATERIAL_TILE_HOMOGENEOUS || eClass > T3D_MATERIAL_TILE_GENERAL )
                return T3D_MATERIAL_TILE_UNKNOWN;
            if ( eClass > maxClass ) maxClass = eClass;
            anyCoverage = true;
        }
    }
    return anyCoverage ? maxClass : T3D_MATERIAL_TILE_UNKNOWN;
}

inline uint8_t t3d_material_tiles_certify_batch( const t3dMaterialTileRegionClasses_t *pRegions,
                                                   uint32_t nCount, const uint32_t *pExpectedRevisions )
{
    if ( !pRegions || !nCount || !pExpectedRevisions ) return T3D_MATERIAL_TILE_UNKNOWN;
    // This compatibility helper certifies *all* 64 tiles of every region.
    // Regions whose boundary halo is unknown cannot use a specialized draw.
    uint8_t maxClass = T3D_MATERIAL_TILE_HOMOGENEOUS;
    for ( uint32_t i = 0u; i < nCount; ++i ) {
        const t3dMaterialTileRegionClasses_t &r = pRegions[i];
        if ( !r.m_bReady || r.m_nRevision != pExpectedRevisions[i] || !pExpectedRevisions[i] )
            return T3D_MATERIAL_TILE_UNKNOWN;
        for ( uint32_t t = 0u; t < T3D_MATERIAL_TILE_CLASS_CONTROL_TILES; ++t ) {
            if ( r.m_szClass[t] == T3D_MATERIAL_TILE_UNKNOWN ) return T3D_MATERIAL_TILE_UNKNOWN;
            if ( r.m_szClass[t] > maxClass ) maxClass = r.m_szClass[t];
        }
    }
    return maxClass;
}

#endif // TERRAIN3D_MATERIAL_TILE_CLASS_H
