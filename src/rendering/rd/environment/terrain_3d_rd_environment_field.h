// Copyright (C) 2023-2026 Terrain3D Contributors.
#ifndef TERRAIN3D_RD_ENVIRONMENT_FIELD_H
#define TERRAIN3D_RD_ENVIRONMENT_FIELD_H

#include "Terrain3DTypes.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_lease_table.h"

// Borrowed for the duration of a PREPARE/SUBMIT pair. Source RIDs and their
// generations belong to Terrain3DRdEnvironmentField and the shared RD context.
typedef struct t3dRdEnvironmentBindings_s
{
    RID m_hState;
    RID m_hStatePair[2];
    RID m_hInteraction;
    RID m_hInteractionPair[2];
    RID m_hHeightPair[2]; // Two independent vertical contact layers per texel.
    uint32_t m_nPing = 0u;
    RID m_hSampler;
    RID m_hHeightSampler;
    RID m_hParams;
    uint64_t m_nGeneration = 0u;
    bool m_bAvailable = false;
} t3dRdEnvironmentBindings_t;

// std140: six vec4s shared by WORLD terrain/ground cover and the ocean renderer.
typedef struct t3dRdEnvironmentParams_s
{
    float m_fStateOriginX;
    float m_fStateOriginZ;
    float m_fStateInvSpanX;
    float m_fStateInvSpanZ;
    float m_fForceOriginX;
    float m_fForceOriginZ;
    float m_fForceInvSpanX;
    float m_fForceInvSpanZ;
    float m_fEnabled;
    float m_fWindX;
    float m_fWindZ;
    float m_fGust;
    float m_fPreviousStateOriginX;
    float m_fPreviousStateOriginZ;
    float m_fPreviousForceOriginX;
    float m_fPreviousForceOriginZ;
    float m_fPreviousWindX;
    float m_fPreviousWindZ;
    float m_fPreviousGust;
    float m_fWindTime;
    float m_fPreviousWindTime;
    float m_fSeaLevel;
    float m_fHistoryValid;
    float m_fPad;
} t3dRdEnvironmentParams_t;
static_assert( sizeof( t3dRdEnvironmentParams_t ) == 96u, "Environment std140 ABI changed" );

class Terrain3DRdEnvironmentField
{
    typedef struct t3dComputeKey_s
    {
        RID m_hShader;
        RID m_hSource;
        RID m_hDestination;
        bool operator==( const t3dComputeKey_s &rOther ) const {
            return m_hShader == rOther.m_hShader && m_hSource == rOther.m_hSource &&
                   m_hDestination == rOther.m_hDestination;
        }
    } t3dComputeKey_t;

    Terrain3DRdContext &m_context;
    Terrain3DRdTexture m_szField[3][2]; // [state/force/height][ping-pong]
    Terrain3DRdShader m_szShader[3];
    Terrain3DRdPipeline m_szPipeline[3];
    Terrain3DRdSampler m_sampler;
    Terrain3DRdSampler m_heightSampler;
    Terrain3DRdGpuBuffer m_impulses;
    Terrain3DRdGpuBuffer m_regionClimate; // Packed immutable owner publication.
    Terrain3DRdGpuBuffer m_params;
    Terrain3DRdDescriptorLeaseTable< t3dComputeKey_t, 6u > m_computeSets;
    RID m_szComputeSet[3][2];
    PackedByteArray m_pushBytes;
    PackedByteArray m_uploadFallback;
    t3dRdEnvironmentParams_t m_paramsData{};
    uint64_t m_nLastRenderFrame = UINT64_MAX;
    uint64_t m_nLastImpulseEpoch = 0u;
    uint64_t m_nLastRegionClimateEpoch = UINT64_MAX;
    uint64_t m_nLastRegionOverrideEpoch = 0u;
    uint64_t m_nFieldGeneration = 0u;
    uint32_t m_nPing = 0u;
    bool m_bInitialized = false;
    bool m_bAttempted = false;
    bool m_bPreviousValid = false;
    bool m_bFaulted = false;
    uint64_t m_nFramesIntegrated = 0u;
    uint64_t m_nImpulsesAccepted = 0u;
    uint64_t m_nIntegrationFailures = 0u;
    uint64_t m_nTeleportsReset = 0u;
    uint64_t m_nTimestampPairs = 0u;
    uint64_t m_nClimateUploads = 0u;

    bool createFieldCold( uint32_t nStage, uint32_t nIndex );
    bool createComputeSetCold( uint32_t nStage, uint32_t nIndex );
    bool dispatchStageHot( uint32_t nStage, uint32_t nSource, uint32_t nDestination,
                           const t3dEnvironmentForcing_t &rForcing, uint32_t nImpulseCount,
                           bool bClimateOverride,
                           float fOriginX, float fOriginZ, float fPreviousX, float fPreviousZ );

public:
    Terrain3DRdEnvironmentField();
    bool initializeCold();
    void shutdownCold();
    void resetAfterRendererTeardownCold();
    // Once per native render frame, before any draw consumer. No allocations.
    void advanceFrameHot( const t3dRdFrameSnapshot_t &rSnapshot,
                          const t3dRdForwardPassContext_t &rContext );
    bool acquireBindingsHot( t3dRdEnvironmentBindings_t &rBindings ) const;
    bool isReady() const { return m_bInitialized && m_bPreviousValid; }
    uint64_t getGeneration() const { return m_nFieldGeneration; }
    uint64_t getFramesIntegrated() const { return m_nFramesIntegrated; }
    uint64_t getImpulsesAccepted() const { return m_nImpulsesAccepted; }
    uint64_t getIntegrationFailures() const { return m_nIntegrationFailures; }
    uint64_t getTeleportsReset() const { return m_nTeleportsReset; }
    uint64_t getTimestampPairs() const { return m_nTimestampPairs; }
    uint64_t getClimateUploads() const { return m_nClimateUploads; }
    bool isFaulted() const { return m_bFaulted; }
    // Physical texture storage only; additional tracked descriptor/buffer costs are elsewhere.
    uint64_t getTextureBytes() const { return m_bInitialized ? 5u * 1024u * 1024u : 0u; }
};

#endif // TERRAIN3D_RD_ENVIRONMENT_FIELD_H
