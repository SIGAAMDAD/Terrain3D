// Cold, camera-independent subset of canonical V3 terrain material composition.
// Source VT footprint/filtering is supplied by the offline VT reader. This
// evaluator refuses view-dependent material configurations rather than baking
// a result that differs from the raster shader.
#ifndef TERRAIN3D_COMPOSED_SURFACE_STATIC_V3_H
#define TERRAIN3D_COMPOSED_SURFACE_STATIC_V3_H

#include "authoring/vt/build/terrain_3d_composed_surface_cooker.h"

#define T3D_CS_STATIC_V3_MAX_LAYERS 32u
#define T3D_CS_STATIC_V3_SAMPLE_NEAREST 2u
#define T3D_CS_STATIC_V3_SAMPLE_BILINEAR 1u
#define T3D_CS_STATIC_V3_SAMPLE_TOP_TWO 5u

struct t3dComposedStaticV3Layer_t
{
    uint32_t m_nAllocation;
    float m_fColor[3];
    float m_fPbr0[4]; // x=source UV scale, y=normal strength, z=roughness offset, w=metallic
    float m_fPbr1[2]; // x=AO weight, y=AO influence
};

// First interior texel has coordinates (origin + 0.5 * step).
// World and control texel coordinates may differ. The page's gutter is sampled
// from real neighboring source data, including negative world coordinates.
struct t3dComposedStaticV3Footprint_t
{
    float m_fWorldOrigin[2];
    float m_fControlOrigin[2];
    float m_fWorldStepX[2];
    float m_fWorldStepY[2];
    float m_fControlStepX[2];
    float m_fControlStepY[2];
};

// The source reader must reproduce clipmap canonical V3 filtering, tail/parent
// fallback and decode-after-filter albedo semantics at the supplied gradients.
// Returned albedo is LINEAR RGB; pbr = (normal_x,normal_z,roughness,AO).
struct t3dComposedStaticV3Sample_t
{
    float m_fAlbedo[3];
    float m_fPbr[4];
    float m_fHeight;
};

typedef bool ( *t3dCsStaticV3PageFn )( void *pUser, const t3dComposedSurfaceCookPage_t &page,
                                        t3dComposedStaticV3Footprint_t &rFootprint );
typedef bool ( *t3dCsStaticV3ControlFn )( void *pUser, int32_t nX, int32_t nY, uint32_t &rControl );
typedef bool ( *t3dCsStaticV3SourceFn )( void *pUser, uint32_t nAllocation,
                                         const float *pUv, const float *pGradX, const float *pGradY,
                                         float fLodBias, uint32_t nFiltering,
                                         t3dComposedStaticV3Sample_t &rSample );

struct t3dComposedStaticV3Source_t
{
    void *m_pUser;
    t3dCsStaticV3PageFn m_pResolvePage;
    t3dCsStaticV3ControlFn m_pReadControl;
    t3dCsStaticV3SourceFn m_pSampleSource;
    const t3dComposedStaticV3Layer_t *m_pLayers;
    uint32_t m_nLayerCount;
    uint32_t m_nSamplingMode; // 1 bilinear, 2 nearest, 5 top-two
    uint32_t m_nVtFiltering;
    float m_fStaticMipBias;
    float m_fBlendSharpness;
    // Reject modes whose shading depends on view, runtime state, derivative,
    // triplanar projection, auto controls or stochastic detiling.
    bool m_bAutoShader;
    bool m_bDualScale;
    bool m_bSideProjection;
    bool m_bDetiling;
    bool m_bAdaptiveSampling;
    bool m_bImplicitControls;
    bool m_bViewDependentMipBias;
    bool m_bCanonicalMaterialParityAccepted;
    bool m_bReferenceOnly = false; // CPU candidate; never a parity certificate.
};

bool t3d_cs_static_v3_eligible( const t3dComposedStaticV3Source_t &source );
bool t3d_cs_static_v3_evaluate( void *pUser, const t3dComposedSurfaceCookPage_t &page,
                                uint32_t nChannel, uint8_t *pRgba, uint32_t nBytes );
// Single evaluation per texel produces the complete three-channel page set.
bool t3d_cs_static_v3_evaluate_all( void *pUser, const t3dComposedSurfaceCookPage_t &page,
                                    uint8_t *pRgbaAll, uint32_t nRgbaBytes );
// P09 uncompressed pre-BC reference. nFloatCount is 136*136*4, not bytes.
// Linear albedo, normalized tangent XYZ normal, or linear AO/R/M/AO-affect.
// This independent diagnostic endpoint does not imply source/parity approval.
bool t3d_cs_static_v3_evaluate_linear_f32( void *pUser, const t3dComposedSurfaceCookPage_t &page,
                                           uint32_t nChannel, float *pLinear, uint32_t nFloatCount );

// Connects this evaluator to the existing cold sidecar cooker and BC encoder;
// it never raises the material-parity flag on behalf of the caller.
struct t3dComposedStaticV3CookAdapter_t
{
    t3dComposedStaticV3Source_t m_source;
    t3dComposedSurfaceEncodeFn m_pEncoder;
    void *m_pEncoderUser;
};

bool t3d_cs_static_v3_encode( void *pUser, uint32_t nChannel,
                              const uint8_t *pRgba, uint32_t nRgbaBytes,
                              uint8_t *pCompressed, uint32_t nCapacity, uint32_t &rWritten );
bool t3d_cs_static_v3_make_cook_source( t3dComposedStaticV3CookAdapter_t &rAdapter,
                                        uint64_t nFingerprint, t3dComposedSurfaceCookSource_t &rCook );

#endif
