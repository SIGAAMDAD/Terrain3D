#include "authoring/vt/build/terrain_3d_composed_surface_v3_reference_decode.h"
#include "data/formats/terrain_3d_vt_pack_format.h"
#include <vector>
#include <cassert>
#include <cstdio>
static void test_decoder()
{
    const uint32_t pixels = T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE;
    std::vector<uint8_t> output( pixels*4u, 0xccu );
    std::vector<uint8_t> bc1( t3d_vt_pack_expected_physical_tile_bytes_v3(T3D_VT_PHYSICAL_FORMAT_BC1_UNORM), 0u );
    // Each block has endpoints red, black; every texel selects endpoint 0.
    for ( uint32_t b = 0; b < bc1.size(); b += 8u ) { bc1[b] = 0x00; bc1[b+1u] = 0xf8; }
    assert(t3d_cs15_decode_reference_tile(T3D_VT_PHYSICAL_FORMAT_BC1_UNORM, bc1.data(), uint32_t(bc1.size()), output.data(), uint32_t(output.size())));
    for(uint32_t i=0;i<pixels;++i) {
        assert(output[i*4u]==255u && output[i*4u+1u]==0u && output[i*4u+2u]==0u && output[i*4u+3u]==255u);
    }
    // BC1's 3-color mode and transparent fourth selector.
    for ( uint32_t b = 0; b < bc1.size(); b+=8u ) {
        bc1[b]=0u; bc1[b+1u]=0u; bc1[b+2u]=0xffu; bc1[b+3u]=0xffu;
        bc1[b+4u]=bc1[b+5u]=bc1[b+6u]=bc1[b+7u]=0xffu;
    }
    assert(t3d_cs15_decode_reference_tile(T3D_VT_PHYSICAL_FORMAT_BC1_UNORM, bc1.data(), uint32_t(bc1.size()), output.data(), uint32_t(output.size())));
    assert(output[3]==0u && output.back()==0u);
    std::vector<uint8_t> bc5( t3d_vt_pack_expected_physical_tile_bytes_v3(T3D_VT_PHYSICAL_FORMAT_BC5_UNORM), 0u );
    for ( uint32_t b=0; b<bc5.size(); b+=16u ) {
        bc5[b]=80u; bc5[b+1u]=200u;  // six-step mode
        bc5[b+8u]=250u; bc5[b+9u]=10u; // eight-step mode
    }
    assert(t3d_cs15_decode_reference_tile(T3D_VT_PHYSICAL_FORMAT_BC5_UNORM,bc5.data(),uint32_t(bc5.size()),output.data(),uint32_t(output.size())));
    assert(output[0]==80u && output[1]==250u && output[2]==0u && output[3]==255u);
    // BC4's six-step mode: index 7 -> 255, index 6 -> 0.
    for (uint32_t b=0;b<bc5.size();b+=16u) {
        for(uint32_t i=2;i<8;++i) bc5[b+i]=0xffu;
    }
    assert(t3d_cs15_decode_reference_tile(T3D_VT_PHYSICAL_FORMAT_BC5_UNORM,bc5.data(),uint32_t(bc5.size()),output.data(),uint32_t(output.size())));
    assert(output[0]==255u && output[1]==250u);
    std::vector<uint8_t> height(pixels, 111u);
    assert(t3d_cs15_decode_reference_tile(T3D_VT_PHYSICAL_FORMAT_R8_UNORM,height.data(),uint32_t(height.size()),output.data(),uint32_t(output.size())));
    assert(output[0]==111u && output[1]==0u && output[3]==255u);
    output[0]=79u;
    assert(!t3d_cs15_decode_reference_tile(T3D_VT_PHYSICAL_FORMAT_BC1_UNORM,bc1.data(),1u,output.data(),uint32_t(output.size())));
    assert(output[0]==79u);
    assert(!t3d_cs15_decode_reference_tile(T3D_VT_PHYSICAL_FORMAT_BC7_UNORM,bc1.data(),uint32_t(bc1.size()),output.data(),uint32_t(output.size())));
}

int main(){test_decoder(); std::puts("CS15 BC1/BC5/R8 reference decode PASS");}
