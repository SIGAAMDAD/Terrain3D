#ifndef CS12_STUB_STATIC_V1_H
#define CS12_STUB_STATIC_V1_H
#include <stdint.h>
#include <stddef.h>
struct t3dComposedSurfaceCookPage_t { uint64_t region,revision; uint32_t mip,x,y,generation; };
struct t3dComposedStaticV1Footprint_t { float values[12]; };
struct t3dComposedStaticV1Sample_t { float m_fAlbedo[3]; float m_fPbr[4]; float m_fHeight; };
typedef bool (*t3dCsStaticV1PageFn)(void*, const t3dComposedSurfaceCookPage_t&,t3dComposedStaticV1Footprint_t&);
typedef bool (*t3dCsStaticV1ControlFn)(void*,int32_t,int32_t,uint32_t&);
typedef bool (*t3dCsStaticV1SourceFn)(void*,uint32_t,const float*,const float*,const float*,float,uint32_t,t3dComposedStaticV1Sample_t&);
struct t3dComposedStaticV1Source_t {
 void *m_pUser; t3dCsStaticV1PageFn m_pResolvePage; t3dCsStaticV1ControlFn m_pReadControl;
 t3dCsStaticV1SourceFn m_pSampleSource; uint32_t m_nVtFiltering;
};
inline uint32_t t3d_cs_crc32(const uint8_t*bytes,size_t size){ uint32_t c=0xffffffffu; for(size_t i=0;i<size;++i){c^=bytes[i];for(int b=0;b<8;++b)c=(c>>1)^((c&1u)?0xedb88320u:0u);}return ~c; }
#endif
