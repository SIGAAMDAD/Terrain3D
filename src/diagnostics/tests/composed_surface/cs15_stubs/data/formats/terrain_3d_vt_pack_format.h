#ifndef CS15_VT_PACK_TEST_HEADER_H
#define CS15_VT_PACK_TEST_HEADER_H
#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"
inline uint32_t t3d_vt_pack_expected_physical_tile_bytes_v1( uint32_t family ) {
    const uint32_t blocks = (T3D_VT_PHYSICAL_TILE + 3u) / 4u;
    if (family == T3D_VT_PHYSICAL_FORMAT_BC1_UNORM) return blocks*blocks*8u;
    if (family == T3D_VT_PHYSICAL_FORMAT_BC5_UNORM || family == T3D_VT_PHYSICAL_FORMAT_BC7_UNORM) return blocks*blocks*16u;
    if (family == T3D_VT_PHYSICAL_FORMAT_R8_UNORM) return T3D_VT_PHYSICAL_TILE*T3D_VT_PHYSICAL_TILE;
    return 0u;
}
#endif
