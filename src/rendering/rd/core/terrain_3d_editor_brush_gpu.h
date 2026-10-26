// Editor-only GPU brush tile ABI. Packed source maps remain CPU-owned.
#ifndef TERRAIN3D_EDITOR_BRUSH_GPU_H
#define TERRAIN3D_EDITOR_BRUSH_GPU_H
#include <stdint.h>
typedef struct t3dEditorBrushGpuTile_s
{
    int32_t m_nRegionX;
    int32_t m_nRegionY;
    uint32_t m_nByteOffset;
    uint32_t m_nEncoding;
    uint32_t m_nMinX;
    uint32_t m_nMinY;
    uint32_t m_nWidth;
    uint32_t m_nHeight;
} t3dEditorBrushGpuTile_t;
static_assert( sizeof( t3dEditorBrushGpuTile_t ) == 32u, "BRP tile GPU ABI" );
typedef struct t3dEditorBrushGpuPush_s
{
    uint32_t m_nTileCount;
    uint32_t m_nMapType;
    uint32_t m_nTool;
    uint32_t m_nOperation;
    float m_fCenterX;
    float m_fCenterZ;
    float m_fBrushSize;
    float m_fStrength;
    float m_fRotationCos;
    float m_fRotationSin;
    float m_fVertexSpacing;
    float m_fSelectedHeight;
    uint32_t m_nMaskWidth;
    uint32_t m_nMaskHeight;
    uint32_t m_nFlags;
    uint32_t m_nColorRGBA8;
    float m_fRoughness;
    float m_fCenterHeight;
    uint32_t m_nAssetId;
    uint32_t m_nUvRotation;
    uint32_t m_nUvScale;
    uint32_t m_nReserved[3];
    float m_fGradientX1, m_fGradientY1, m_fGradientZ1;
    float m_fGradientX2, m_fGradientY2, m_fGradientZ2;
    float m_fMotionX, m_fMotionZ;
} t3dEditorBrushGpuPush_t;
static_assert( sizeof( t3dEditorBrushGpuPush_t ) == 128u, "BRP push GPU ABI" );
#endif
