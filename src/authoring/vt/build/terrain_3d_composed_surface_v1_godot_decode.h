#ifndef TERRAIN3D_COMPOSED_SURFACE_V1_GODOT_DECODE_H
#define TERRAIN3D_COMPOSED_SURFACE_V1_GODOT_DECODE_H
#include <stdint.h>
// Editor-only cold decoder; intended as t3dCs12DecodeFn.
bool t3d_cs12_decode_godot_image( void *pUser, uint32_t nPhysicalFamily,
                                  const uint8_t *pEncoded, uint32_t nEncodedBytes,
                                  uint8_t *pRgba, uint32_t nRgbaCapacity );
#endif
