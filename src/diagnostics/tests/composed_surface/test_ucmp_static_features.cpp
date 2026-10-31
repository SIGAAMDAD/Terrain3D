// UCMP Wave 1: portable CPU reference cases for deterministic authoring effects.
// This does not authorize composed-surface renderer cutover.
#include "authoring/vt/build/terrain_3d_composed_surface_static_baseline.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct Fixture
{
    uint32_t m_nControl = 0u;
    float m_fHeight = 0.0f;
    float m_fNormalY = 1.0f;
    uint32_t m_nSurfaceSamples = 0u;
    uint32_t m_nTextureSamples = 0u;
    bool m_bSurfaceValid = true;
};

static bool page( void *, const t3dComposedSurfaceCookPage_t &,
                  t3dComposedStaticFootprint_t &r )
{
    r = {};
    r.m_fWorldOrigin[0] = -178.0f;
    r.m_fWorldOrigin[1] = 67.0f;
    r.m_fControlOrigin[0] = -178.0f;
    r.m_fControlOrigin[1] = 67.0f;
    r.m_fWorldStepX[0] = 1.0f;
    r.m_fWorldStepY[1] = 1.0f;
    r.m_fControlStepX[0] = 1.0f;
    r.m_fControlStepY[1] = 1.0f;
    return true;
}
static bool control( void *u, int32_t, int32_t, uint32_t &r )
{
    r = static_cast< Fixture * >( u )->m_nControl;
    return true;
}
static bool surface( void *u, uint32_t x, uint32_t y, float &height, float &normalY )
{
    auto &s = *static_cast< Fixture * >( u );
    if ( x >= T3D_COMP_SURF_PHYSICAL_TILE || y >= T3D_COMP_SURF_PHYSICAL_TILE || !s.m_bSurfaceValid ) return false;
    ++s.m_nSurfaceSamples;
    height = s.m_fHeight;
    normalY = s.m_fNormalY;
    return true;
}
static bool sample( void *u, uint32_t allocation, const float *uv, const float *dx,
                    const float *dy, float, uint32_t, t3dComposedStaticSample_t &r )
{
    auto &s = *static_cast< Fixture * >( u );
    ++s.m_nTextureSamples;
    if ( !std::isfinite( uv[0] ) || !std::isfinite( uv[1] ) ||
         !std::isfinite( dx[0] ) || !std::isfinite( dy[1] ) ) return false;
    r = {};
    r.m_fAlbedo[0] = allocation == 101u ? 0.85f : 0.15f;
    r.m_fAlbedo[1] = allocation == 101u ? 0.12f : 0.89f;
    r.m_fAlbedo[2] = ( 0.75f + 0.25f * sinf( uv[0] * 0.5f + uv[1] * 0.7f ) ) * 0.5f;
    r.m_fPbr[0] = 0.8f;
    r.m_fPbr[1] = 0.6f;
    r.m_fPbr[2] = 0.55f;
    r.m_fPbr[3] = 0.8f;
    r.m_fHeight = 0.5f;
    return allocation == 100u || allocation == 101u;
}

int main()
{
    Fixture fixture{};
    t3dComposedStaticLayer_t layers[2]{};
    for ( uint32_t i = 0u; i < 2u; ++i ) {
        auto &l = layers[i];
        l.m_nAllocation = 100u + i;
        l.m_fColor[0] = l.m_fColor[1] = l.m_fColor[2] = 1.0f;
        l.m_fPbr0[0] = 1.0f;
        l.m_fPbr0[1] = 1.0f;
        l.m_fPbr1[0] = 1.0f;
        l.m_fPbr1[1] = 0.5f;
        l.m_fPbr1[2] = 1.0f;
        l.m_fPbr1[3] = 0.7f;
    }
    t3dComposedStaticCookAdapter_t adapter{};
    auto &src = adapter.m_source;
    src.m_pUser = &fixture;
    src.m_pResolvePage = page;
    src.m_pReadControl = control;
    src.m_pReadTerrainSurface = surface;
    src.m_pSampleSource = sample;
    src.m_pLayers = layers;
    src.m_nLayerCount = 2u;
    src.m_nSamplingMode = T3D_COMP_SURF_STATIC_SAMPLE_BILINEAR;
    src.m_nVtFiltering = 3u;
    src.m_fBlendSharpness = 0.5f;
    src.m_bReferenceOnly = true;
    const t3dComposedSurfaceCookPage_t id{ 1u, 2u, 0u, 0u, 0u, 1u };
    const uint32_t side = T3D_COMP_SURF_RGBA_SCRATCH_BYTES;
    std::vector< float > output( side );
    const uint32_t center = 4u * ( 68u * T3D_COMP_SURF_PHYSICAL_TILE + 68u );

    // Explicit base/overlay results for auto-resolution parity.
    fixture.m_nControl = 0u;
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  output.data(), side ) );
    const float base = output[center];
    fixture.m_nControl = 1u << 27u;
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  output.data(), side ) );
    const float overlay = output[center];
    assert( base < overlay - 0.5f );

    src.m_bAutoShader = true;
    src.m_fAutoSlope = 1.0f;
    src.m_fAutoHeightReduction = 0.0f;
    src.m_nAutoBaseTexture = 0u;
    src.m_nAutoOverlayTexture = 1u;
    fixture.m_nControl = 1u; // Auto bit.
    fixture.m_fNormalY = 1.0f;
    assert( t3d_comp_surf_static_baseline_eligible( src ) );
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  output.data(), side ) );
    assert( fabsf( output[center] - overlay ) < 1.0e-5f );
    assert( fixture.m_nSurfaceSamples > 0u );
    fixture.m_fNormalY = 0.0f;
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  output.data(), side ) );
    assert( fabsf( output[center] - base ) < 1.0e-5f );
    fixture.m_fNormalY = 1.0f;
    fixture.m_fHeight = 100.0f;
    src.m_fAutoHeightReduction = 1.0f;
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  output.data(), side ) );
    assert( fabsf( output[center] - base ) < 1.0e-5f );
    fixture.m_bSurfaceValid = false;
    assert( !t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                   output.data(), side ) );
    fixture.m_bSurfaceValid = true;
    src.m_nAutoBaseTexture = 35u;
    assert( !t3d_comp_surf_static_baseline_eligible( src ) );
    src.m_nAutoBaseTexture = 0u;
    src.m_bAutoShader = false;
    fixture.m_nControl = 0u;

    // Canonical deterministic detiling must affect cooked source sampling,
    // including negative world cells, without depending on a live camera.
    std::vector< float > plain( side ), detiled( side ), repeated( side );
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  plain.data(), side ) );
    src.m_bDetiling = true;
    src.m_nDetilingMode = 3u;
    assert( t3d_comp_surf_static_baseline_eligible( src ) );
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  detiled.data(), side ) );
    assert( memcmp( plain.data(), detiled.data(), side * sizeof(float) ) != 0 );
    assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_ALBEDO,
                                                  repeated.data(), side ) );
    assert( memcmp( detiled.data(), repeated.data(), side * sizeof(float) ) == 0 );
    for ( uint32_t mode : { 1u, 4u, 5u } ) {
        src.m_nDetilingMode = mode;
        assert( t3d_comp_surf_static_baseline_eligible( src ) );
        assert( t3d_comp_surf_static_baseline_evaluate_linear( &adapter, id, T3D_COMP_SURF_CHANNEL_NORMAL,
                                                      output.data(), side ) );
        const float nx = output[center], ny = output[center+1u], nz = output[center+2u];
        assert( fabsf( nx * nx + ny * ny + nz * nz - 1.0f ) < 1.0e-5f );
    }
    src.m_nDetilingMode = 2u;
    assert( !t3d_comp_surf_static_baseline_eligible( src ) );
    src.m_nDetilingMode = 1u;
    src.m_bDualScale = true;
    assert( !t3d_comp_surf_static_baseline_eligible( src ) ); // Camera-dependent blend still excluded.
    src.m_bDualScale = false;
    src.m_bSideProjection = true;
    assert( !t3d_comp_surf_static_baseline_eligible( src ) ); // No invented terrain Y projection.

    std::printf( "UCMP Wave1 auto-control/detiling reference: PASS (%u VT samples, %u terrain samples)\n",
                 fixture.m_nTextureSamples, fixture.m_nSurfaceSamples );
    return 0;
}
