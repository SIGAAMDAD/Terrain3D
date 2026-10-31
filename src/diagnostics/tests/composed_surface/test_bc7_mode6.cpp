#include "authoring/vt/build/terrain_3d_composed_surface_reference_decode.h"
#include "data/formats/terrain_3d_vt_pack_format.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

static void write_bits( uint8_t *block, uint32_t &position, uint32_t value, uint32_t count )
{
    for ( uint32_t b = 0u; b < count; ++b, ++position ) {
        if ( value & ( 1u << b ) ) block[position >> 3u] |= uint8_t( 1u << ( position & 7u ) );
    }
}

// Same bit order/field widths as the project's production mode-6 encoder.
static void encode_known_block( uint8_t *dst, const uint32_t endpoint[2][4],
                                uint32_t p0, uint32_t p1, const uint8_t *indices )
{
    for ( uint32_t b = 0u; b < 16u; ++b ) dst[b] = 0u;
    uint32_t bit = 0u;
    write_bits( dst, bit, 0x40u, 7u );
    for ( uint32_t c = 0u; c < 4u; ++c ) {
        write_bits( dst, bit, endpoint[0][c], 7u );
        write_bits( dst, bit, endpoint[1][c], 7u );
    }
    write_bits( dst, bit, p0, 1u );
    write_bits( dst, bit, p1, 1u );
    for ( uint32_t i = 0u; i < 16u; ++i )
        write_bits( dst, bit, indices[i], i == 0u ? 3u : 4u );
    assert( bit == 128u );
}

static uint8_t expected( uint32_t a, uint32_t b, uint32_t weight )
{
    return uint8_t( ( ( 64u - weight ) * a + weight * b + 32u ) >> 6u );
}

int main()
{
    static const uint8_t weights[16] = {
        0u, 4u, 9u, 13u, 17u, 21u, 26u, 30u,
        34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u
    };
    constexpr uint32_t width = T3D_VT_PHYSICAL_TILE;
    constexpr uint32_t blocks = width / 4u;
    const uint32_t ep[2][4] = { { 14u, 24u, 63u, 4u }, { 98u, 80u, 1u, 127u } };
    uint8_t indices[16]{};
    indices[0] = 7u; // Anchor has only 3 bits; the encoder swaps endpoints if necessary.
    for ( uint32_t i = 1u; i < 16u; ++i ) indices[i] = uint8_t( i );
    std::vector<uint8_t> encoded( blocks * blocks * 16u );
    std::vector<uint8_t> pixels( width * width * 4u, 0xacu );
    for ( uint32_t block = 0u; block < blocks * blocks; ++block )
        encode_known_block( encoded.data() + block * 16u, ep, 1u, 0u, indices );
    assert( t3d_comp_surf_decode_reference_tile( T3D_VT_PHYSICAL_FORMAT_BC7_UNORM,
        encoded.data(), uint32_t(encoded.size()), pixels.data(), uint32_t(pixels.size()) ) );
    for ( uint32_t y = 0u; y < width; ++y ) {
        for ( uint32_t x = 0u; x < width; ++x ) {
            const uint32_t idx = indices[(y % 4u) * 4u + x % 4u];
            const uint8_t *const color = pixels.data() + ( y * width + x ) * 4u;
            for ( uint32_t c = 0u; c < 4u; ++c ) {
                const uint32_t a = ep[0][c] * 2u + 1u;
                const uint32_t b = ep[1][c] * 2u;
                assert( color[c] == expected( a, b, weights[idx] ) );
            }
        }
    }
    // A mixed-mode payload is routed to the Godot BPTC decoder; this scalar
    // decoder must refuse it without exposing a partially decoded page.
    encoded[(blocks * blocks / 2u) * 16u] = 0x20u; // Mode 5, not mode 6.
    pixels[0] = 77u;
    pixels.back() = 78u;
    assert( !t3d_comp_surf_decode_reference_tile( T3D_VT_PHYSICAL_FORMAT_BC7_UNORM,
        encoded.data(), uint32_t(encoded.size()), pixels.data(), uint32_t(pixels.size()) ) );
    assert( pixels[0] == 77u && pixels.back() == 78u );
    encoded[(blocks * blocks / 2u) * 16u] = 0x40u;
    assert( !t3d_comp_surf_decode_reference_tile( T3D_VT_PHYSICAL_FORMAT_BC7_UNORM,
        encoded.data(), uint32_t(encoded.size()) - 1u, pixels.data(), uint32_t(pixels.size()) ) );
    assert( pixels[0] == 77u );
    std::puts( "CS16 reference BC7 mode-6 tile decoder PASS" );
    return 0;
}
