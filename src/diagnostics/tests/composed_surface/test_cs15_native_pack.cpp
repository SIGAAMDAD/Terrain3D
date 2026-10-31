#include "authoring/vt/build/terrain_3d_composed_surface_v1_pack_sampler.h"
#include "authoring/vt/build/terrain_3d_composed_surface_v1_reference_decode.h"
#include "authoring/vt/io/terrain_3d_vt_pack_writer.h"
#include "data/formats/terrain_3d_vt_pack_format.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static bool decode( void *, uint32_t family, const uint8_t *bytes,
                    uint32_t length, uint8_t *output, uint32_t capacity )
{
    return t3d_cs15_decode_reference_tile( family, bytes, length, output, capacity );
}

static void write_bits( uint8_t *block, uint32_t &position, uint32_t value, uint32_t count )
{
    for ( uint32_t b = 0u; b < count; ++b, ++position )
        if ( value & ( 1u << b ) ) block[position >> 3u] |= uint8_t( 1u << ( position & 7u ) );
}

static void fill_bc7_mode6( uint8_t *data, uint32_t length )
{
    assert( ( length % 16u ) == 0u );
    // An actual V1 BC7 mode-6 constant block: RGBA=(129,65,97,255).
    // This is decoded by the same scalar reference as the authoring adapter.
    const uint32_t endpoint[4] = { 64u, 32u, 48u, 127u };
    for ( uint32_t block = 0u; block < length / 16u; ++block ) {
        uint8_t *dst = data + block * 16u;
        std::memset( dst, 0, 16u );
        uint32_t pos = 0u;
        write_bits( dst, pos, 0x40u, 7u );
        for ( uint32_t c = 0u; c < 4u; ++c ) {
            write_bits( dst, pos, endpoint[c], 7u );
            write_bits( dst, pos, endpoint[c], 7u );
        }
        write_bits( dst, pos, 1u, 1u );
        write_bits( dst, pos, 1u, 1u );
        write_bits( dst, pos, 0u, 3u );
        for ( uint32_t i = 1u; i < 16u; ++i ) write_bits( dst, pos, 0u, 4u );
        assert( pos == 128u );
    }
}

static void test_decoder()
{
    const uint32_t pixels = T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE;
    std::vector<uint8_t> output( pixels*4u, 0xccu );
    std::vector<uint8_t> bc1( t3d_vt_pack_expected_physical_tile_bytes_v1(T3D_VT_PHYSICAL_FORMAT_BC1_UNORM), 0u );
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
    std::vector<uint8_t> bc5( t3d_vt_pack_expected_physical_tile_bytes_v1(T3D_VT_PHYSICAL_FORMAT_BC5_UNORM), 0u );
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

int main()
{
    test_decoder();
    t3dVTPackAllocationRecordV1_t allocation{};
    allocation.m_nGeneration=8u;
    allocation.m_nContentId=17u;
    allocation.m_nChannelMask = uint16_t(T3D_VT_LOGICAL_CHANNEL_BIT(T3D_VT_LOGICAL_CHANNEL_ALBEDO) |
                                              T3D_VT_LOGICAL_CHANNEL_BIT(T3D_VT_LOGICAL_CHANNEL_PBR) |
                                              T3D_VT_LOGICAL_CHANNEL_BIT(T3D_VT_LOGICAL_CHANNEL_HEIGHT));
    allocation.m_nAvailableProfileMask=1u;
    allocation.m_nHighestAvailableProfile=0u;
    allocation.m_nConsumer=T3D_VT_CONSUMER_CLIPMAP;
    t3dVTPackAllocationKeyRecordV1_t key{};
    key.m_nStableKey=t3d_vt_terrain_material_stable_key(0u);
    key.m_nAllocationId=0u;
    key.m_nContentId=allocation.m_nContentId;
    t3dVTChannelAllocation_t channels[T3D_VT_LOGICAL_CHANNEL_COUNT]{};
    t3dVTPackTailRecordV1_t tails[3]{};
    t3dVTPackPayloadRecordV1_t payloads[3]{};
    t3dVTPackClusterRecordV1_t cluster{};
    const uint32_t formats[]={T3D_VT_PHYSICAL_FORMAT_BC1_UNORM,T3D_VT_PHYSICAL_FORMAT_BC7_UNORM,T3D_VT_PHYSICAL_FORMAT_R8_UNORM};
    std::vector<uint8_t> data;
    for(uint32_t c=0;c<T3D_VT_LOGICAL_CHANNEL_COUNT;++c) channels[c].m_nTailIndex=T3D_VT_INVALID_TAIL;
    for(uint32_t c=0;c<3u;++c) {
        channels[c].m_nTailIndex=c;
        channels[c].m_nMip0WidthPages=1u;
        channels[c].m_nMip0HeightPages=1u;
        channels[c].m_nMipCount=1u;
        tails[c].m_nCanonicalAllocationId=0u;
        tails[c].m_nPayloadId=c;
        tails[c].m_nContentId=allocation.m_nContentId;
        tails[c].m_nFirstMip=0u;
        tails[c].m_nMipCount=1u;
        tails[c].m_nFirstMipWidth=64u;
        tails[c].m_nFirstMipHeight=64u;
        tails[c].m_nAtlasInteriorWidth=64u;
        tails[c].m_nAtlasInteriorHeight=64u;
        tails[c].m_nChannel=uint16_t(c);
        tails[c].m_nLayout=T3D_VT_PACK_TAIL_LAYOUT_HORIZONTAL_V1;
        const uint32_t len=t3d_vt_pack_expected_physical_tile_bytes_v1(formats[c]);
        payloads[c].m_nFormat=formats[c];
        payloads[c].m_nClusterId=0u;
        payloads[c].m_nClusterOffset=uint32_t(data.size());
        payloads[c].m_nStoredBytes=payloads[c].m_nDecodedBytes=len;
        payloads[c].m_nFlags=T3D_VT_PACK_PAYLOAD_FLAG_TAIL;
        const size_t start=data.size();
        data.insert(data.end(),len,c==0u ? 0u : c==1u ? 0x80u : 0x66u);
        if (c==0u) {
            for (size_t j=start;j<data.size();j+=8u) { data[j]=0u; data[j+1u]=0xf8u; }
        } else if (c==1u) {
            fill_bc7_mode6( data.data()+start, len );
        }
        payloads[c].m_nChecksum=t3d_vt_pack_checksum32(data.data()+start,len);
        payloads[c].m_nContentSignature=t3d_vt_pack_hash64(data.data()+start,len);
    }
    cluster.m_nStoredBytes=cluster.m_nDecodedBytes=uint32_t(data.size());
    cluster.m_nFirstPayload=0u;
    cluster.m_nPayloadCount=3u;
    cluster.m_nCodec=T3D_VT_PACK_CODEC_NONE;
    cluster.m_nChecksum=t3d_vt_pack_checksum32(data.data(),data.size());
    cluster.m_nContentSignature=t3d_vt_pack_hash64(data.data(),data.size());
    t3dVTPackQualityViewV1_t quality{};
    quality.m_nAllocationId=0u;
    quality.m_nProfile=0u;
    quality.m_nChannelMask=uint8_t(allocation.m_nChannelMask);
    for(uint32_t c=0;c<T3D_VT_LOGICAL_CHANNEL_COUNT;++c) {
        quality.m_nBaseMip[c] = quality.m_nMaxMip[c] = c < 3u ? 0u : T3D_VT_PACK_INVALID_MIP;
    }
    t3dVTPackFormatFamilyRecordV1_t families[T3D_VT_LOGICAL_CHANNEL_COUNT]{};
    assert(t3d_vt_pack_init_format_family_records_v1(families,T3D_VT_LOGICAL_CHANNEL_COUNT));
    t3dVTPackSourceV1_t source{};
    source.m_pAllocations=&allocation;
    source.m_nAllocationCount=1u;
    source.m_pAllocationKeys=&key;
    source.m_pChannelAllocations=channels;
    source.m_nChannelAllocationCount=T3D_VT_LOGICAL_CHANNEL_COUNT;
    source.m_pPayloads=payloads;
    source.m_nPayloadCount=3u;
    source.m_pClusters=&cluster;
    source.m_nClusterCount=1u;
    source.m_pTails=tails;
    source.m_nTailCount=3u;
    source.m_pQualityViews=&quality;
    source.m_nQualityViewCount=1u;
    source.m_pFormatFamilies=families;
    source.m_nFormatFamilyCount=T3D_VT_LOGICAL_CHANNEL_COUNT;
    source.m_pPayloadData=data.data();
    source.m_nPayloadDataBytes=data.size();
    source.m_nPackageGeneration=1u;
    const uint64_t capacity=t3d_vt_pack_compute_bytes_v1(source);
    assert(capacity > data.size());
    std::vector<uint8_t> package(capacity);
    uint64_t written=0u;
    assert(t3d_vt_pack_write_v1(source,package.data(),package.size(),written) && written==capacity);
    t3dVTPackViewV1_t view{};
    assert(t3d_vt_pack_open_view_v1(package.data(),package.size(),view));
    std::vector<uint8_t> scratch(4u*T3D_COMPOSED_SURFACE_PACK_DECODED_TILE_BYTES);
    t3dComposedSurfacePackSampler_t sampler{};
    t3dVTPackViewV1_t opened{};
    assert(t3d_composed_surface_pack_open_source_pack(package.data(),package.size(),opened,sampler,decode,nullptr,scratch.data(),4u));
    uint32_t id=UINT32_MAX;
    assert(t3d_composed_surface_pack_find_material_allocation(opened,0u,id) && id==0u);
    const float uv[2]={0.5f,0.5f},grad[2]={0.0f,0.0f};
    t3dComposedStaticV1Sample_t sample{};
    assert(t3d_composed_surface_pack_sample_static_v1(&sampler,id,uv,grad,grad,0.0f,3u,sample));
    assert(std::abs(sample.m_fAlbedo[0]-1.0f)<0.00001f && sample.m_fAlbedo[1]==0.0f);
    assert(std::abs(sample.m_fPbr[0]-129.0f/255.0f)<0.00001f);
    assert(std::abs(sample.m_fPbr[1]- 65.0f/255.0f)<0.00001f);
    assert(std::abs(sample.m_fPbr[2]- 97.0f/255.0f)<0.00001f);
    assert(std::abs(sample.m_fPbr[3]-1.0f)<0.00001f);
    assert(std::abs(sample.m_fHeight - 102.0f/255.0f)<0.00001f);
    assert(sampler.m_nDecodeCalls==3u);
    assert(t3d_composed_surface_pack_sample_static_v1(&sampler,id,uv,grad,grad,0.0f,3u,sample) && sampler.m_nCacheHits>=3u);
    package.back()^=1u;
    assert(!t3d_vt_pack_open_view_v1(package.data(),package.size(),view));
    package.back()^=1u;
    assert(t3d_vt_pack_open_view_v1(package.data(),package.size(),view));
    std::puts("CS15 native V1 write/open/source-sample/reference-BC test PASS");
}
