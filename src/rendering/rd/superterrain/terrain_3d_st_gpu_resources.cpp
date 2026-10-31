// Copyright 2026 Terrain3D contributors.
#include "rendering/rd/superterrain/terrain_3d_st_gpu_resources.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

#include <limits.h>
#include <string.h>
#include <utility>

static bool t3dStCopyBytesCold( const void *pData, size_t nSize, PackedByteArray &rOut )
{
    if ( pData == nullptr || nSize == 0u || nSize > ( size_t ) INT32_MAX )
    {
        return false;
    }
    rOut.resize( ( int64_t ) nSize );
    if ( ( size_t ) rOut.size() != nSize )
    {
        return false;
    }
    memcpy( rOut.ptrw(), pData, nSize );
    return true;
}

static bool t3dStCreateBufferCold( Terrain3DRdContext &rContext, Terrain3DRdGpuBuffer &rBuffer,
                                   const char *pName, const void *pBytes, size_t nBytes )
{
    t3dRdGpuBufferSpec_t spec;
    spec.m_pName = pName;
    spec.m_owner = T3D_RD_CONTEXT_OWNER_WORLD;
    spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
    spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
    spec.m_growth = T3D_GPU_EXACT;
    PackedByteArray payload;
    if ( !rBuffer.configure( spec ) || !t3dStCopyBytesCold( pBytes, nBytes, payload ) )
    {
        return false;
    }
    return rBuffer.createTracked( rContext, ( uint32_t ) nBytes, payload );
}

static bool t3dStCreateTextureCold( Terrain3DRdContext &rContext, Terrain3DRdTexture &rTexture,
                                    const char *pName, uint32_t nWidth, uint32_t nHeight,
                                    uint32_t nLayers, const PackedByteArray *pInitial )
{
    t3dRdTextureSpec_t spec;
    spec.m_name = String( pName );
    spec.m_owner = T3D_RD_CONTEXT_OWNER_WORLD;
    spec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
    spec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R16_UNORM;
    spec.m_layout.m_type = nLayers > 1u ? RenderingDevice::TEXTURE_TYPE_2D_ARRAY : RenderingDevice::TEXTURE_TYPE_2D;
    spec.m_layout.m_nWidth = nWidth;
    spec.m_layout.m_nHeight = nHeight;
    spec.m_layout.m_nLayers = nLayers;
    spec.m_layout.m_nMipmaps = 1u;
    spec.m_layout.m_nBytes = ( uint64_t ) nWidth * nHeight * nLayers * 2u;
    spec.m_layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                                  RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT;
    TypedArray< PackedByteArray > initial;
    if ( pInitial != nullptr )
    {
        if ( ( uint64_t ) pInitial->size() != spec.m_layout.m_nBytes || nLayers != 1u )
        {
            return false;
        }
        initial.push_back( *pInitial );
    }
    return rTexture.createTracked( rContext, spec, initial );
}

bool Terrain3DStGpuImage::uploadCandidateCold( Terrain3DRdContext &rContext,
                                              const Terrain3DStPublicationImage &rImage,
                                              const uint32_t nFinePageSlots,
                                              const uint32_t nDeviceGeneration )
{
    if ( valid() || !rImage.valid() || nDeviceGeneration == 0u ||
         nFinePageSlots == 0u || nFinePageSlots > 4096u ||
         rImage.regions().size() != T3D_ST_REGION_COUNT ||
         rImage.heightPages().size() != T3D_ST_REGION_COUNT * T3D_ST_VHM_PAGES_PER_REGION ||
         rImage.heightTails().size() != T3D_ST_REGION_COUNT ||
         rImage.maskPages().size() != T3D_ST_REGION_COUNT * T3D_ST_MASK_PAGES_PER_REGION ||
         rImage.heightAtlas().size() != T3D_ST_VHM_TAIL_BYTES ||
         rImage.maskWords().size() > T3D_ST_MASK_MAX_PINNED_BYTES / 4u ||
         rImage.finePageCount() > nFinePageSlots ||
         rImage.finePages().size() != size_t( rImage.finePageCount() ) * T3D_ST_VHM_PAGE_BYTES )
    {
        return false;
    }
    Terrain3DStGpuImage candidate;
    const uint32_t nZero = 0u;
    const void *pMaskWords = rImage.maskWords().empty() ? ( const void * ) &nZero :
                                                        ( const void * ) rImage.maskWords().data();
    const size_t nMaskBytes = rImage.maskWords().empty() ? 4u : rImage.maskWords().size() * 4u;
    PackedByteArray atlas;
    if ( !t3dStCopyBytesCold( rImage.heightAtlas().data(), rImage.heightAtlas().size(), atlas ) ||
         !t3dStCreateBufferCold( rContext, candidate.m_vhmPages, "T3D.SuperTerrain.VhmPages",
                                 rImage.heightPages().data(), rImage.heightPages().size() * sizeof( t3dStRegionVhmPageEntry_t ) ) ||
         !t3dStCreateBufferCold( rContext, candidate.m_vhmTails, "T3D.SuperTerrain.VhmTails",
                                 rImage.heightTails().data(), rImage.heightTails().size() * sizeof( t3dStRegionVhmTail_t ) ) ||
         !t3dStCreateBufferCold( rContext, candidate.m_maskEntries, "T3D.SuperTerrain.MaskEntries",
                                 rImage.maskPages().data(), rImage.maskPages().size() * sizeof( t3dStMaskPageEntry_t ) ) ||
         !t3dStCreateBufferCold( rContext, candidate.m_maskWords, "T3D.SuperTerrain.MaskWords",
                                 pMaskWords, nMaskBytes ) ||
         !t3dStCreateTextureCold( rContext, candidate.m_vhmRoot, "T3D.SuperTerrain.VhmPinnedRoots",
                                  T3D_ST_VHM_TAIL_ATLAS_WIDTH, T3D_ST_VHM_TAIL_ATLAS_HEIGHT, 1u, &atlas ) ||
         !t3dStCreateTextureCold( rContext, candidate.m_vhmFine, "T3D.SuperTerrain.VhmFinePages",
                                  T3D_ST_VHM_PHYSICAL_TILE, T3D_ST_VHM_PHYSICAL_TILE, nFinePageSlots, nullptr ) )
    {
        candidate.releaseCold( rContext );
        return false;
    }
    // Stage immutable R16 fine pages before publishing any GPU binding. A
    // failed upload destroys the complete candidate, including its resident
    // page-table bits; the old snapshot remains the only visible authority.
    PackedByteArray fineUpload;
    fineUpload.resize( T3D_ST_VHM_PAGE_BYTES );
    if ( fineUpload.size() != T3D_ST_VHM_PAGE_BYTES )
    {
        candidate.releaseCold( rContext );
        return false;
    }
    for ( uint32_t slot = 0u; slot < rImage.finePageCount(); ++slot )
    {
        memcpy( fineUpload.ptrw(),
                rImage.finePages().data() + size_t( slot ) * T3D_ST_VHM_PAGE_BYTES,
                T3D_ST_VHM_PAGE_BYTES );
        if ( !candidate.m_vhmFine.updateLayerTracked( rContext, slot, fineUpload,
                                                      T3D_GPU_PUBLICATION ) )
        {
            candidate.releaseCold( rContext );
            return false;
        }
    }
    candidate.m_nSignature = rImage.signature();
    candidate.m_nDeviceGeneration = nDeviceGeneration;
    swap( candidate );
    return true;
}

void Terrain3DStGpuImage::releaseCold( Terrain3DRdContext &rContext )
{
    m_vhmPages.releaseTracked( rContext );
    m_vhmTails.releaseTracked( rContext );
    m_maskEntries.releaseTracked( rContext );
    m_maskWords.releaseTracked( rContext );
    m_vhmFine.releaseTracked( rContext );
    m_vhmRoot.releaseTracked( rContext );
    m_nSignature = 0u;
    m_nDeviceGeneration = 0u;
}

void Terrain3DStGpuImage::swap( Terrain3DStGpuImage &rOther )
{
    m_vhmPages.swap( rOther.m_vhmPages );
    m_vhmTails.swap( rOther.m_vhmTails );
    m_maskEntries.swap( rOther.m_maskEntries );
    m_maskWords.swap( rOther.m_maskWords );
    m_vhmFine.swap( rOther.m_vhmFine );
    m_vhmRoot.swap( rOther.m_vhmRoot );
    std::swap( m_nSignature, rOther.m_nSignature );
    std::swap( m_nDeviceGeneration, rOther.m_nDeviceGeneration );
}

bool Terrain3DStGpuImage::valid( void ) const
{
    return m_vhmPages.isValid() && m_vhmTails.isValid() &&
           m_maskEntries.isValid() && m_maskWords.isValid() &&
           m_vhmFine.isValid() && m_vhmRoot.isValid() &&
           m_nSignature != 0u && m_nDeviceGeneration != 0u;
}

t3dStGpuBindings_t Terrain3DStGpuImage::bindings( void ) const
{
    t3dStGpuBindings_t result;
    if ( !valid() )
    {
        return result;
    }
    result.m_hVhmPages = m_vhmPages.rid();
    result.m_hVhmTails = m_vhmTails.rid();
    result.m_hVhmFine = m_vhmFine.rid();
    result.m_hVhmRoot = m_vhmRoot.rid();
    result.m_hMaskEntries = m_maskEntries.rid();
    result.m_hMaskWords = m_maskWords.rid();
    result.m_nSignature = m_nSignature;
    result.m_nDeviceGeneration = m_nDeviceGeneration;
    return result;
}
