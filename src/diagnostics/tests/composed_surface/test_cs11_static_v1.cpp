#include "authoring/vt/build/terrain_3d_composed_surface_static_v1.h"
#include "data/runtime/terrain_3d_composed_surface_sidecar.h"
#include <assert.h>
#include <cmath>
#include <string.h>
#include <vector>
#include <stdio.h>

struct Fixture
{
    uint32_t m_nSamples = 0u;
    uint32_t m_nControls = 0u;
    uint32_t m_nPages = 0u;
    bool m_bCorrupt = false;
    bool m_bMissing = false;
    bool m_bBoundary = false;
};

static bool locate( void *ctx, const t3dComposedSurfaceCookPage_t &p, t3dComposedStaticV1Footprint_t &r )
{
    Fixture &f = *static_cast< Fixture * >( ctx );
    ++f.m_nPages;
    r = {};
    r.m_fWorldOrigin[0] = float( p.m_nPageX * 128u ) - 128.0f;
    r.m_fWorldOrigin[1] = float( p.m_nPageY * 128u ) - 64.0f;
    r.m_fControlOrigin[0] = -2.0f;
    r.m_fControlOrigin[1] = 0.0f;
    r.m_fWorldStepX[0] = 1.0f;
    r.m_fWorldStepY[1] = 1.0f;
    r.m_fControlStepX[0] = 0.25f;
    r.m_fControlStepY[1] = 0.25f;
    return true;
}
static uint32_t pack( uint32_t base, uint32_t overlay, uint32_t blend, uint32_t rotation=0, uint32_t scale=0 )
{
    return ( base << 27u ) | ( overlay << 22u ) | ( blend << 14u ) | ( rotation << 10u ) | ( scale << 7u );
}
static bool control( void *ctx, int32_t x, int32_t y, uint32_t &r )
{
    Fixture &f = *static_cast< Fixture * >( ctx );
    ++f.m_nControls;
    if ( f.m_bMissing && x < -1 ) return false;
    r = f.m_bBoundary && x > 2 ? pack( 0u, 1u, 255u, 4u, 2u ) : pack( 0u, 1u, 128u );
    (void)y;
    return true;
}
static bool sample( void *ctx, uint32_t allocation, const float *uv, const float *dx, const float *dy,
                    float lod, uint32_t filtering, t3dComposedStaticV1Sample_t &r )
{
    Fixture &f = *static_cast< Fixture * >( ctx );
    ++f.m_nSamples;
    assert( filtering == 7u && lod == 0.25f );
    assert( std::isfinite(uv[0]) && std::isfinite(uv[1]) && std::isfinite(dx[0]) && std::isfinite(dy[1]) );
    r = {};
    const bool second = allocation == 22u;
    r.m_fAlbedo[0] = second ? 0.25f : 1.0f;
    r.m_fAlbedo[1] = second ? 0.5f : 0.0f;
    r.m_fAlbedo[2] = 0.0f;
    r.m_fPbr[0] = 0.5f;
    r.m_fPbr[1] = 0.5f;
    r.m_fPbr[2] = second ? 0.8f : 0.4f;
    r.m_fPbr[3] = second ? 0.6f : 1.0f;
    r.m_fHeight = second ? 0.9f : 0.1f;
    if ( f.m_bCorrupt ) r.m_fPbr[3] = NAN;
    return allocation == 11u || allocation == 22u;
}
static bool encode( void *, uint32_t channel, const uint8_t *src, uint32_t size,
                    uint8_t *dst, uint32_t capacity, uint32_t &written )
{
    assert( channel < 3u && size == T3D_CS_RGBA_SCRATCH_BYTES );
    assert( capacity == t3d_cs_channel_bytes( channel ) );
    for ( uint32_t i = 0u; i < capacity; ++i ) dst[i] = uint8_t( src[( i * 31u ) % size] ^ uint8_t( channel ) );
    written = capacity;
    return true;
}

int main()
{
    Fixture fixture{};
    t3dComposedStaticV1Layer_t layers[2]{};
    layers[0].m_nAllocation = 11u;
    layers[1].m_nAllocation = 22u;
    for ( auto &l : layers ) {
        l.m_fColor[0] = l.m_fColor[1] = l.m_fColor[2] = 1.0f;
        l.m_fPbr0[0] = 1.0f; l.m_fPbr0[1] = 1.0f;
        l.m_fPbr0[3] = 0.7f;
        l.m_fPbr1[0] = 1.0f; l.m_fPbr1[1] = 0.4f;
    }
    t3dComposedStaticV1CookAdapter_t adapter{};
    adapter.m_source.m_pUser = &fixture;
    adapter.m_source.m_pResolvePage = locate;
    adapter.m_source.m_pReadControl = control;
    adapter.m_source.m_pSampleSource = sample;
    adapter.m_source.m_pLayers = layers;
    adapter.m_source.m_nLayerCount = 2u;
    adapter.m_source.m_nSamplingMode = T3D_CS_STATIC_V1_SAMPLE_NEAREST;
    adapter.m_source.m_nVtFiltering = 7u;
    adapter.m_source.m_fStaticMipBias = 0.25f;
    adapter.m_source.m_fBlendSharpness = 0.5f;
    adapter.m_pEncoder = encode;
    t3dComposedSurfaceCookSource_t cook{};
    assert( t3d_cs_static_v1_eligible( adapter.m_source ) );
    assert( !t3d_cs_static_v1_make_cook_source( adapter, 77u, cook ) );
    const t3dComposedSurfaceCookPage_t p = { 21u, 66u, 0u, 0u, 0u, 1u };
    // P09: a diagnostic uncompressed evaluation is allowed before the source
    // parity gate, but publishing encoded composed pages is still forbidden.
    std::vector<float> linear( T3D_CS_RGBA_SCRATCH_BYTES, -1.0f );
    std::vector<uint8_t> rgba( T3D_CS_RGBA_SCRATCH_BYTES, 0u );
    assert( t3d_cs_static_v1_evaluate_linear_f32( &adapter, p, T3D_CS_CHANNEL_ALBEDO,
                                                  linear.data(), uint32_t(linear.size()) ) );
    assert( !t3d_cs_static_v1_evaluate( &adapter, p, T3D_CS_CHANNEL_ALBEDO,
                                       rgba.data(), uint32_t(rgba.size()) ) );
    assert( !t3d_cs_static_v1_evaluate_linear_f32( &adapter, p, T3D_CS_CHANNEL_ALBEDO,
                                                   linear.data(), uint32_t(linear.size()) - 1u ) );
    assert( !t3d_cs_static_v1_evaluate_linear_f32( &adapter, p, T3D_CS_CHANNEL_COUNT,
                                                   linear.data(), uint32_t(linear.size()) ) );
    assert( !t3d_cs_static_v1_evaluate_linear_f32( &adapter, p, T3D_CS_CHANNEL_ALBEDO,
                                                   NULL, uint32_t(linear.size()) ) );
    adapter.m_source.m_bCanonicalMaterialParityAccepted = true;
    assert( t3d_cs_static_v1_make_cook_source( adapter, 77u, cook ) );
    assert( t3d_cs_static_v1_evaluate( &adapter, p, T3D_CS_CHANNEL_ALBEDO, rgba.data(), uint32_t(rgba.size()) ) );
    // Overlay has larger height: almost all result uses overlay color.
    size_t center = 4u * ( 68u * 136u + 68u );
    assert( rgba[center] > 100u && rgba[center] < 155u );
    assert( rgba[center + 1u] > 170u && rgba[center + 1u] < 205u );
    assert( rgba[center + 3u] == 255u );
    // Identical source material math, separate float and quantized output
    // encoding. Compare the quantization explicitly without requiring a GPU.
    auto encode_albedo = []( float value ) -> uint8_t {
        const float clamped = fminf( 1.0f, fmaxf( 0.0f, value ) );
        const float srgb = clamped <= 0.0031308f ? 12.92f * clamped :
                            1.055f * powf( clamped, 1.0f / 2.4f ) - 0.055f;
        return uint8_t( fminf( 255.0f, fmaxf( 0.0f, floorf( srgb * 255.0f + 0.5f ) ) ) );
    };
    for ( uint32_t channel = 0u; channel < 3u; ++channel ) {
        assert( std::isfinite(linear[center + channel]) );
        assert( rgba[center + channel] == encode_albedo( linear[center + channel] ) );
    }
    assert( t3d_cs_static_v1_evaluate_linear_f32( &adapter, p, T3D_CS_CHANNEL_NORMAL,
                                                  linear.data(), uint32_t(linear.size()) ) );
    assert( t3d_cs_static_v1_evaluate( &adapter, p, T3D_CS_CHANNEL_NORMAL, rgba.data(), uint32_t(rgba.size()) ) );
    float normal_length = sqrtf( linear[center]*linear[center] + linear[center+1u]*linear[center+1u] +
                                  linear[center+2u]*linear[center+2u] );
    assert( fabsf(normal_length - 1.0f) < 0.00001f );
    assert( rgba[center] == uint8_t( floorf( fminf( 1.0f, fmaxf( 0.0f, linear[center] * 0.5f + 0.5f ) ) * 255.0f + 0.5f ) ) );
    assert( rgba[center + 1u] == uint8_t( floorf( fminf( 1.0f, fmaxf( 0.0f, linear[center + 2u] * 0.5f + 0.5f ) ) * 255.0f + 0.5f ) ) );
    assert( rgba[center] >= 127u && rgba[center] <= 128u && rgba[center + 1u] >= 127u && rgba[center + 1u] <= 128u );
    assert( t3d_cs_static_v1_evaluate_linear_f32( &adapter, p, T3D_CS_CHANNEL_SURFACE,
                                                  linear.data(), uint32_t(linear.size()) ) );
    assert( t3d_cs_static_v1_evaluate( &adapter, p, T3D_CS_CHANNEL_SURFACE, rgba.data(), uint32_t(rgba.size()) ) );
    for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
        assert( std::isfinite(linear[center + channel]) );
        const float clamped = fminf( 1.0f, fmaxf( 0.0f, linear[center + channel] ) );
        assert( rgba[center + channel] == uint8_t( floorf( clamped * 255.0f + 0.5f ) ) );
    }
    assert( rgba[center] >= 140u && rgba[center] <= 160u );
    assert( rgba[center + 1u] >= 190u && rgba[center + 1u] <= 215u );
    assert( rgba[center + 2u] >= 178u && rgba[center + 2u] <= 180u );
    assert( rgba[center + 3u] >= 101u && rgba[center + 3u] <= 103u );
    for ( uint32_t sampling : { 1u, 5u } ) {
        fixture.m_bBoundary = true;
        adapter.m_source.m_nSamplingMode = sampling;
        assert( t3d_cs_static_v1_evaluate( &adapter, p, T3D_CS_CHANNEL_ALBEDO, rgba.data(), uint32_t(rgba.size()) ) );
    }
    fixture.m_bCorrupt = true;
    assert( !t3d_cs_static_v1_evaluate( &adapter, p, T3D_CS_CHANNEL_SURFACE, rgba.data(), uint32_t(rgba.size()) ) );
    fixture.m_bCorrupt = false;
    fixture.m_bMissing = true;
    assert( !t3d_cs_static_v1_evaluate( &adapter, p, T3D_CS_CHANNEL_SURFACE, rgba.data(), uint32_t(rgba.size()) ) );
    fixture.m_bMissing = false;
    for ( uint32_t reason = 0u; reason < 7u; ++reason ) {
        bool *flags[7] = { &adapter.m_source.m_bAutoShader, &adapter.m_source.m_bDualScale,
                           &adapter.m_source.m_bSideProjection, &adapter.m_source.m_bDetiling,
                           &adapter.m_source.m_bAdaptiveSampling, &adapter.m_source.m_bImplicitControls,
                           &adapter.m_source.m_bViewDependentMipBias };
        *flags[reason] = true;
        assert( !t3d_cs_static_v1_eligible( adapter.m_source ) );
        assert( !t3d_cs_static_v1_make_cook_source( adapter, 77u, cook ) );
        *flags[reason] = false;
    }
    assert( !t3d_cs_static_v1_make_cook_source( adapter, 0u, cook ) );
    adapter.m_source.m_nSamplingMode = T3D_CS_STATIC_V1_SAMPLE_BILINEAR;
    fixture.m_bBoundary = false;
    assert( t3d_cs_static_v1_make_cook_source( adapter, 77u, cook ) );
    uint64_t size = 0u;
    assert( t3d_cs_cook_total_bytes( 1u, size ) );
    std::vector<uint8_t> scratch( T3D_CS_COOK_SCRATCH_BYTES );
    std::vector<uint8_t> output( size );
    uint64_t written = 0u;
    assert( t3d_cs_cook_pages_cold( &p, 1u, cook, scratch.data(), uint32_t( scratch.size() ),
                                   output.data(), output.size(), written ) );
    assert( written == size && t3d_cs_validate_sidecar_bytes( output.data(), output.size(), true ) );
    printf( "CS11 static V1 producer: PASS (%u source samples, %u control loads, %u page locations)\n",
            fixture.m_nSamples, fixture.m_nControls, fixture.m_nPages );
}
