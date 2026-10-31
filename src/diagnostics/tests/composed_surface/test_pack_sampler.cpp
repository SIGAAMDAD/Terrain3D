#include "authoring/vt/build/terrain_3d_composed_surface_pack_sampler.h"
#include <cassert>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdio>

// Stand-in for the Godot-dependent V1 package opener; tests for the full
// frozen-pack validator are a separate runtime/pack-reader responsibility.
bool t3d_vt_pack_open_baseline_view(const void *bytes,uint64_t size,t3dVTPackBaselineView_t &view) {
    if (!bytes || size!=sizeof(t3dVTPackBaselineView_t)) return false;
    view=*static_cast<const t3dVTPackBaselineView_t*>(bytes);
    return view.m_pHeader.m_bValid;
}
static uint32_t decodes=0;
static bool decode(void *,uint32_t fmt,const uint8_t*data,uint32_t size,uint8_t*output,uint32_t cap){
 ++decodes; assert(cap==T3D_COMPOSED_SURFACE_PACK_DECODED_TILE_BYTES);
 assert(size==t3d_vt_pack_expected_physical_tile_bytes(fmt));
 for(uint32_t i=0;i<136u*136u;++i){
   output[4*i]=data[0]; output[4*i+1]=data[0]; output[4*i+2]=data[0];output[4*i+3]=255u;
 }return true;
}
static bool page(void *user,const t3dComposedSurfaceCookPage_t&,t3dComposedStaticFootprint_t&){return *static_cast<uint32_t*>(user)==42u;}
static bool control(void *user,int32_t,int32_t,uint32_t &v){v=7u;return *static_cast<uint32_t*>(user)==42u;}
static bool surface(void *user,uint32_t x,uint32_t y,float &height,float &normal_y){
  if(*static_cast<uint32_t*>(user)!=42u || x!=7u || y!=9u)return false;
  height=12.5f;normal_y=0.75f;return true;
}
int main(){
  t3dVTPackAllocationRecord_t alloc{};alloc.m_nConsumer=0;
  t3dVTPackAllocationKeyRecord_t key{};
  key.m_nStableKey=t3d_vt_terrain_material_stable_key(3u); key.m_nAllocationId=0u;
  t3dVTChannelAllocation_t ch[5]{};
  t3dVTPackPageRecord_t pages[3]{};
  t3dVTPackTailRecord_t tails[3]{};
  t3dVTPackPayloadRecord_t payloads[6]{};
  t3dVTPackClusterRecord_t cluster{};
  std::vector<uint8_t> data;
  const uint8_t values[6]={128,200,64,32,100,16};
  for(uint32_t k=0;k<3u;++k){
    const uint32_t fmt=k==0?0u:(k==1?1u:3u);
    ch[k].m_nBaseVirtualPage=k;ch[k].m_nPageCount=1u;ch[k].m_nTailIndex=k;
    ch[k].m_nMip0WidthPages=1u;ch[k].m_nMip0HeightPages=1u;ch[k].m_nMipCount=2u;
    pages[k].m_nVirtualPage=k;pages[k].m_nAllocationId=0;pages[k].m_nPayloadId=k;
    pages[k].m_nChannel=uint16_t(k);pages[k].m_nMip=0u;
    tails[k].m_nCanonicalAllocationId=0;tails[k].m_nChannel=uint16_t(k);
    tails[k].m_nFirstMip=1;tails[k].m_nMipCount=1;tails[k].m_nFirstMipWidth=64u;tails[k].m_nFirstMipHeight=64u;tails[k].m_nPayloadId=k+3u;
    for(uint32_t t=0;t<2;++t){
      uint32_t index=k+t*3u;
      uint32_t len=t3d_vt_pack_expected_physical_tile_bytes(fmt);
      payloads[index].m_nFormat=fmt;payloads[index].m_nClusterOffset=uint32_t(data.size());
      payloads[index].m_nDecodedBytes=len;payloads[index].m_nStoredBytes=len;
      data.insert(data.end(),len,values[index]);
      payloads[index].m_nChecksum=t3d_comp_surf_crc32(data.data()+payloads[index].m_nClusterOffset,len);
    }
  }
  cluster.m_nDecodedBytes=uint32_t(data.size());cluster.m_nStoredBytes=uint32_t(data.size());
  t3dVTPackBaselineView_t view{};view.m_pHeader.m_bValid=true;
  view.m_pAllocations.m_pBytes=reinterpret_cast<const uint8_t*>(&alloc);
  view.m_pAllocationKeys.m_pBytes=reinterpret_cast<const uint8_t*>(&key);
  view.m_pChannelAllocations.m_pBytes=reinterpret_cast<const uint8_t*>(ch);
  view.m_pPages.m_pBytes=reinterpret_cast<const uint8_t*>(pages);
  view.m_pTails.m_pBytes=reinterpret_cast<const uint8_t*>(tails);
  view.m_pPayloads.m_pBytes=reinterpret_cast<const uint8_t*>(payloads);
  view.m_pClusters.m_pBytes=reinterpret_cast<const uint8_t*>(&cluster);
  view.m_pPayloadData=data.data();view.m_nPayloadDataBytes=data.size();
  view.m_nAllocationCount=1;view.m_nPageCount=3;view.m_nTailCount=3;view.m_nClusterCount=1;view.m_nPayloadCount=6;
  uint32_t match=UINT32_MAX;
  assert(t3d_composed_surface_pack_find_material_allocation(view,3u,match) && match==0u);
  assert(!t3d_composed_surface_pack_find_material_allocation(view,4u,match));
  std::vector<uint8_t> scratch(T3D_COMPOSED_SURFACE_PACK_DECODED_TILE_BYTES*6u);
  t3dComposedSurfacePackSampler_t sampler{};
  t3dVTPackBaselineView_t opened{};
  assert(!t3d_composed_surface_pack_open_source_pack(nullptr,0u,opened,sampler,decode,nullptr,scratch.data(),6u));
  assert(t3d_composed_surface_pack_open_source_pack(&view,sizeof(view),opened,sampler,decode,nullptr,scratch.data(),6u));
  const float uv[2]={0.2f,0.7f},ddx[2]={0,0},ddy[2]={0,0};
  t3dComposedStaticSample_t out{};
  assert(t3d_composed_surface_pack_sample_static(&sampler,0u,uv,ddx,ddy,0.0f,3u,out));
  assert(fabsf(out.m_fPbr[2]-200.0f/255.0f)<0.001f);
  assert(fabsf(out.m_fHeight-64.0f/255.0f)<0.001f);
  uint32_t prev=decodes;
  assert(t3d_composed_surface_pack_sample_static(&sampler,0u,uv,ddx,ddy,0.0f,3u,out));
  assert(sampler.m_nCacheHits>0u);
  // Mip1 samples the packed 64x64 tail rather than mip0's regular page.
  const float large_dx[2]={0.5f,0.0f};
  assert(t3d_composed_surface_pack_sample_static(&sampler,0u,uv,large_dx,ddy,0.0f,3u,out));
  assert(out.m_fHeight==16.0f/255.0f); // tail mip differs from regular mip
  assert(!t3d_composed_surface_pack_sample_static(&sampler,0u,uv,ddx,ddy,0.0f,0u,out));
  const float negative_uv[2]={-0.8f, 1.7f};
  assert(t3d_composed_surface_pack_sample_static(&sampler,0u,negative_uv,ddx,ddy,0.0f,1u,out));
  const float invalid_uv[2]={NAN,0.2f};
  assert(!t3d_composed_surface_pack_sample_static(&sampler,0u,invalid_uv,ddx,ddy,0.0f,3u,out));
  // Tail-only V1 quality profiles can have 64x64 mip0 while their page
  // directory reports one rounded-up 128x128 page of address space.
  for(uint32_t k=0u;k<3u;++k){
    ch[k].m_nPageCount=0u;ch[k].m_nMipCount=1u;
    tails[k].m_nFirstMip=0u;tails[k].m_nMipCount=1u;
  }
  t3dComposedSurfacePackSampler_t tail_only{};
  assert(t3d_composed_surface_pack_sampler_init(tail_only,view,decode,nullptr,scratch.data(),6u));
  assert(t3d_composed_surface_pack_sample_static(&tail_only,0u,uv,ddx,ddy,0.0f,3u,out));
  assert(out.m_fHeight==16.0f/255.0f);
  for(uint32_t k=0u;k<3u;++k){
    ch[k].m_nPageCount=1u;ch[k].m_nMipCount=2u;
    tails[k].m_nFirstMip=1u;tails[k].m_nMipCount=1u;
  }
  // Corruption must be rejected on cache miss.
  t3dComposedSurfacePackSampler_t corrupt{};
  assert(t3d_composed_surface_pack_sampler_init(corrupt,view,decode,nullptr,scratch.data(),6u));
  data[payloads[0].m_nClusterOffset]^=0xffu;
  assert(!t3d_composed_surface_pack_sample_static(&corrupt,0u,uv,ddx,ddy,0.0f,3u,out));
  assert(corrupt.m_nChecksumFailures==1u);
  data[payloads[0].m_nClusterOffset]^=0xffu;
  // Check adapter preserves the original author's page/control user pointer.
  uint32_t marker=42u;
  t3dComposedStaticSource_t source{};source.m_pUser=&marker;
  source.m_pResolvePage=page;source.m_pReadControl=control;source.m_pReadTerrainSurface=surface;source.m_nVtFiltering=0u;
  t3dComposedSurfacePackProducerBridge_t bridge{};
  assert(!t3d_composed_surface_pack_bind_static_source(sampler,bridge,source));
  source.m_nVtFiltering=3u;
  assert(t3d_composed_surface_pack_bind_static_source(sampler,bridge,source));
  t3dComposedStaticFootprint_t f{};t3dComposedSurfaceCookPage_t p{};
  uint32_t ctrl=0u;
  assert(source.m_pResolvePage(source.m_pUser,p,f));
  assert(source.m_pReadControl(source.m_pUser,0,0,ctrl)&&ctrl==7u);
  float height=0.0f, normal_y=1.0f;
  assert(source.m_pReadTerrainSurface(source.m_pUser,7u,9u,height,normal_y));
  assert(height==12.5f && normal_y==0.75f);
  assert(!source.m_pReadTerrainSurface(source.m_pUser,0u,0u,height,normal_y));
  assert(source.m_pSampleSource(source.m_pUser,0u,uv,ddx,ddy,0.0f,3u,out));
  assert(decodes>=prev);
  std::puts("CS12 pack sampling/CRC/tail/cache/bridge PASS");
}
