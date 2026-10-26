#ifndef CS12_STUB_PACK_H
#define CS12_STUB_PACK_H
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#define T3D_VT_PHYSICAL_TILE 136u
#define T3D_VT_PAGE_INTERIOR 128u
#define T3D_VT_PAGE_GUTTER 4u
#define T3D_VT_LOGICAL_CHANNEL_COUNT 5u
#define T3D_VT_LOGICAL_CHANNEL_ALBEDO 0u
#define T3D_VT_LOGICAL_CHANNEL_PBR 1u
#define T3D_VT_LOGICAL_CHANNEL_HEIGHT 2u
#define T3D_VT_CONSUMER_CLIPMAP 0u
#define T3D_VT_PACK_CODEC_NONE 0u
#define T3D_VT_NEUTRAL_HEIGHT8 128u
#define T3D_VT_PHYSICAL_FORMAT_BC1_UNORM 0u
#define T3D_VT_PHYSICAL_FORMAT_BC7_UNORM 1u
#define T3D_VT_PHYSICAL_FORMAT_BC5_UNORM 2u
#define T3D_VT_PHYSICAL_FORMAT_R8_UNORM 3u
inline uint64_t t3d_vt_terrain_material_stable_key(uint32_t id){return 0x5433445400000000ULL | uint64_t(id+1u);}
struct t3dVTPackAllocationKeyRecordV3_t{uint64_t m_nStableKey;uint32_t m_nAllocationId,m_nContentId;};
struct t3dVTPackAllocationRecordV3_t{uint32_t m_nGeneration,m_nContentId;uint16_t m_nFlags,m_nChannelMask;uint8_t m_nHighestAvailableProfile,m_nAvailableProfileMask,m_nConsumer,m_nReserved0;};
struct t3dVTChannelAllocation_t { uint32_t m_nBaseVirtualPage,m_nPageCount,m_nTailIndex; uint16_t m_nMip0WidthPages,m_nMip0HeightPages,m_nMipCount,m_nFlags;};
struct t3dVTPackPageRecordV3_t{uint32_t m_nVirtualPage,m_nParentPage,m_nAllocationId,m_nPayloadId,m_nGeneration,m_nContentId;uint16_t m_nMip,m_nFlags,m_nPageX,m_nPageY,m_nChannel,m_nReserved;};
struct t3dVTPackPayloadRecordV3_t{uint64_t m_nContentSignature;uint32_t m_nFormat,m_nClusterId,m_nClusterOffset,m_nStoredBytes,m_nDecodedBytes,m_nCodec,m_nChecksum,m_nFlags,m_nReserved;};
struct t3dVTPackClusterRecordV3_t {uint64_t m_nPayloadDataOffset,m_nContentSignature;uint32_t m_nStoredBytes,m_nDecodedBytes,m_nFirstPayload,m_nPayloadCount,m_nCodec,m_nChecksum;};
struct t3dVTPackTailRecordV3_t{uint64_t m_nContentSignature;uint32_t m_nCanonicalAllocationId,m_nPayloadId,m_nContentId;uint16_t m_nFirstMip,m_nMipCount,m_nFirstMipWidth,m_nFirstMipHeight,m_nAtlasInteriorWidth,m_nAtlasInteriorHeight,m_nChannel,m_nFlags;uint32_t m_nLayout;};
template<typename T> struct t3dVTPackByteRecords_t {const uint8_t *m_pBytes=nullptr;T operator[](size_t i)const{T r{};memcpy(&r,m_pBytes+i*sizeof(T),sizeof(T));return r;} };
struct t3dVTPackHeaderValue_t{bool m_bValid=false;};
struct t3dVTPackViewV3_t {
 t3dVTPackHeaderValue_t m_pHeader;
 t3dVTPackByteRecords_t<t3dVTPackAllocationRecordV3_t>m_pAllocations;
 t3dVTPackByteRecords_t<t3dVTPackAllocationKeyRecordV3_t>m_pAllocationKeys;
 t3dVTPackByteRecords_t<t3dVTChannelAllocation_t>m_pChannelAllocations;
 t3dVTPackByteRecords_t<t3dVTPackPageRecordV3_t>m_pPages;
 t3dVTPackByteRecords_t<t3dVTPackPayloadRecordV3_t>m_pPayloads;
 t3dVTPackByteRecords_t<t3dVTPackClusterRecordV3_t>m_pClusters;
 t3dVTPackByteRecords_t<t3dVTPackTailRecordV3_t>m_pTails;
 const uint8_t *m_pPayloadData=nullptr;
 uint32_t m_nAllocationCount=0,m_nPageCount=0,m_nPayloadCount=0,m_nClusterCount=0,m_nTailCount=0;
 uint64_t m_nPayloadDataBytes=0;
};
bool t3d_vt_pack_open_view_v3(const void *bytes,uint64_t size,t3dVTPackViewV3_t&view);
inline uint32_t t3d_vt_pack_expected_physical_tile_bytes_v3(uint32_t format){ if(format==3u)return 136u*136u;return 34u*34u*(format==0u?8u:16u); }
#endif
