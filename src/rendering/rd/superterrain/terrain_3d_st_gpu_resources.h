// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_ST_GPU_RESOURCES_H
#define TERRAIN3D_ST_GPU_RESOURCES_H

#include "rendering/rd/superterrain/terrain_3d_st_publication.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"

// Borrowed descriptor snapshot. The six objects are owned by one immutable
// Terrain3DStGpuImage; no individual family is published independently.
typedef struct t3dStGpuBindings_s
{
    RID m_hVhmPages;
    RID m_hVhmTails;
    RID m_hVhmFine;
    RID m_hVhmRoot;
    RID m_hMaskEntries;
    RID m_hMaskWords;
    uint64_t m_nSignature = 0u;
    uint32_t m_nDeviceGeneration = 0u;

    bool valid( void ) const
    {
        return m_hVhmPages.is_valid() && m_hVhmTails.is_valid() &&
               m_hVhmFine.is_valid() && m_hVhmRoot.is_valid() &&
               m_hMaskEntries.is_valid() && m_hMaskWords.is_valid() &&
               m_nSignature != 0u && m_nDeviceGeneration != 0u;
    }
} t3dStGpuBindings_t;

class Terrain3DStGpuImage
{
    Terrain3DRdGpuBuffer m_vhmPages;
    Terrain3DRdGpuBuffer m_vhmTails;
    Terrain3DRdGpuBuffer m_maskEntries;
    Terrain3DRdGpuBuffer m_maskWords;
    Terrain3DRdTexture m_vhmFine;
    Terrain3DRdTexture m_vhmRoot;
    uint64_t m_nSignature = 0u;
    uint32_t m_nDeviceGeneration = 0u;

public:
    Terrain3DStGpuImage( void ) = default;
    Terrain3DStGpuImage( const Terrain3DStGpuImage & ) = delete;
    Terrain3DStGpuImage &operator=( const Terrain3DStGpuImage & ) = delete;

    // Runs only on the render/device thread and only for an already fully
    // validated immutable candidate. Failure leaves the previous image live.
    bool uploadCandidateCold( Terrain3DRdContext &rContext,
                              const Terrain3DStPublicationImage &rImage,
                              uint32_t nFinePageSlots, uint32_t nDeviceGeneration );
    void releaseCold( Terrain3DRdContext &rContext );
    void swap( Terrain3DStGpuImage &rOther );
    bool valid( void ) const;
    t3dStGpuBindings_t bindings( void ) const;
    uint64_t signature( void ) const { return m_nSignature; }
};

#endif // TERRAIN3D_ST_GPU_RESOURCES_H
