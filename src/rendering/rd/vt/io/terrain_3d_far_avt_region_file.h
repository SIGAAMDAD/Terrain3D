// Copyright (c) 2026 Terrain3D Contributors.
// IO-00 indexed regional pages. Reads one page without decoding siblings.
#ifndef TERRAIN3D_FAR_AVT_REGION_FILE_H
#define TERRAIN3D_FAR_AVT_REGION_FILE_H

#include "rendering/rd/vt/core/terrain_3d_far_avt_contract.h"
#include "streaming/io/terrain_3d_cold_chunk_file.h"

inline bool t3dFarAvtOpenRegionCold( const godot::Ref<godot::FileAccess> &rFile,
    const int32_t iRegionX, const int32_t iRegionY,
    const uint64_t nExpectedMaterialGeneration,
    t3dColdIndexHeader_t &rHeader, eastl::vector<t3dColdChunkRecord_t> &rRecords )
{
    if ( !nExpectedMaterialGeneration || !t3dColdIndexOpenFile( rFile, rHeader, rRecords ) ||
         rRecords.size() != T3D_FAR_AVT_REGION_RECORD_COUNT ) return false;
    for ( uint32_t kind = 1u; kind <= T3D_FAR_AVT_CHANNEL_COUNT; ++kind ) {
        for ( uint32_t mip = 0u; mip < T3D_FAR_AVT_MIP_COUNT; ++mip ) {
            const t3dColdChunkRecord_t &record = rRecords[( kind - 1u ) * T3D_FAR_AVT_MIP_COUNT + mip];
            if ( !t3dFarAvtValidateRecord( record ) || record.m_iRegionX != iRegionX ||
                 record.m_iRegionY != iRegionY || record.m_nKind != kind ||
                 record.m_nLevel != mip || record.m_nRegionGeneration != rHeader.m_nGeneration ||
                 record.m_nMaterialGeneration != nExpectedMaterialGeneration ) return false;
        }
    }
    return true;
}

inline bool t3dFarAvtReadTileCold( const godot::Ref<godot::FileAccess> &rFile,
    const t3dColdIndexHeader_t &rHeader, const eastl::vector<t3dColdChunkRecord_t> &rRecords,
    const int32_t iRegionX, const int32_t iRegionY, const uint32_t nKind, const uint32_t nMip,
    const uint64_t nMaterialGeneration,
    uint8_t *pEncodedScratch, const size_t nEncodedCapacity,
    uint8_t *pDecoded, const size_t nDecodedCapacity )
{
    if ( rHeader.m_nRecordCount != rRecords.size() || !t3dFarAvtBytesPerTexel( nKind ) ||
         nMip >= T3D_FAR_AVT_MIP_COUNT ) return false;
    const t3dColdChunkRecord_t *const pRecord = t3dColdIndexFind(
        rRecords.data(), ( uint32_t ) rRecords.size(), iRegionX, iRegionY, nKind, nMip );
    return pRecord && t3dFarAvtValidateRecord( *pRecord ) &&
           t3dColdChunkReadFileInto( rFile, *pRecord, rHeader.m_nGeneration,
               nMaterialGeneration, pEncodedScratch, nEncodedCapacity, pDecoded, nDecodedCapacity );
}

#endif // TERRAIN3D_FAR_AVT_REGION_FILE_H
