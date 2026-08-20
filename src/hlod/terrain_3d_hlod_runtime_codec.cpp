// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "core/terrain_3d_compiler.h"
#include "hlod/terrain_3d_hlod_runtime_codec.h"

#include <EASTL/sort.h>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstring>
#include <limits>

namespace {
static constexpr uint32_t HLOD_RUNTIME_MAGIC = 0x32444c48u; // HLD2
static constexpr uint16_t HLOD_RUNTIME_VERSION = 1u;

EA_FORCE_INLINE static int floorDiv( const int value, const int divisor ) { if ( divisor <= 0 ) return 0; int quotient = value / divisor; if ( value < 0 && ( value % divisor ) != 0 ) --quotient; return quotient; }
EA_FORCE_INLINE static void u8( PackedByteArray &b, uint8_t v ) { const int64_t o=b.size(); b.resize(o+1); b.ptrw()[o]=v; }
EA_FORCE_INLINE static void u16( PackedByteArray &b, uint16_t v ) { const int64_t o=b.size(); b.resize(o+2); auto*p=b.ptrw()+o; p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
EA_FORCE_INLINE static void u32( PackedByteArray &b, uint32_t v ) { const int64_t o=b.size(); b.resize(o+4); auto*p=b.ptrw()+o; for(int i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i)); }
EA_FORCE_INLINE static void i32( PackedByteArray &b, int32_t v ) { u32(b,(uint32_t)v); }
EA_FORCE_INLINE static void u64( PackedByteArray &b, uint64_t v ) { u32(b,(uint32_t)v); u32(b,(uint32_t)(v>>32)); }
EA_FORCE_INLINE static void i64( PackedByteArray &b, int64_t v ) { u64(b,(uint64_t)v); }
EA_FORCE_INLINE static void f32( PackedByteArray &b, float v ) { uint32_t bits=0; std::memcpy(&bits,&v,4); u32(b,bits); }
EA_FORCE_INLINE static uint8_t ru8(const uint8_t*p) { return p[0]; }
EA_FORCE_INLINE static uint16_t ru16(const uint8_t*p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
EA_FORCE_INLINE static uint32_t ru32(const uint8_t*p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
EA_FORCE_INLINE static int32_t ri32(const uint8_t*p) { return (int32_t)ru32(p); }
EA_FORCE_INLINE static uint64_t ru64(const uint8_t*p) { return (uint64_t)ru32(p)|((uint64_t)ru32(p+4)<<32); }
EA_FORCE_INLINE static int64_t ri64(const uint8_t*p) { return (int64_t)ru64(p); }
EA_FORCE_INLINE static float rf32(const uint8_t*p) { uint32_t bits=ru32(p); float v=0; std::memcpy(&v,&bits,4); return v; }

static bool parseEntry( const Variant &rValue, T3DHLODRuntimeEntryRecord &rEntry ) {
	if ( rValue.get_type() != Variant::DICTIONARY ) return false;
	const Dictionary d = rValue;
	if ( !d.has("level") || !d.has("cluster") || !d.has("region_min") || !d.has("region_max") || !d.has("package_path") ) return false;
	rEntry.level = CLAMP( (int)d["level"], 1, 6 );
	rEntry.cluster = d["cluster"]; rEntry.regionMin=d["region_min"]; rEntry.regionMax=d["region_max"];
	rEntry.localBounds = d.get("local_bounds", AABB());
	rEntry.packagePath = String(d["package_path"]);
	rEntry.geometricError = MAX((real_t)d.get("geometric_error",1.0),0.001f);
	rEntry.sourceSignature=(int64_t)d.get("source_signature",0);
	rEntry.buildSignature=(int64_t)d.get("build_signature",0);
	rEntry.estimatedGpuBytes=MAX<int64_t>((int64_t)d.get("estimated_gpu_bytes",0),0);
	return !rEntry.packagePath.is_empty();
}
}

PackedByteArray T3DHLODRuntimeCodec::encode(
	const Array &rEntries, const PackedByteArray &rInstancerMinLevels,
	const PackedInt32Array &rNodeRegionIndices, const PackedInt64Array &rNodeSpawnIds,
	const PackedByteArray &rNodeMinLevels
) {
	PackedByteArray out;
	eastl::vector<T3DHLODRuntimeEntryRecord> entries;
	entries.reserve((size_t)rEntries.size());
	for(int i=0;i<rEntries.size();++i){ T3DHLODRuntimeEntryRecord e; if(!parseEntry(rEntries[i],e)) return PackedByteArray(); entries.push_back(e); }
	for(int i=0;i<(int)entries.size();++i){
		auto &e=entries[(size_t)i]; const Vector2i pc(floorDiv(e.cluster.x,2),floorDiv(e.cluster.y,2));
		int parent=-1; for(int j=0;j<(int)entries.size();++j){if(entries[(size_t)j].level==e.level+1&&entries[(size_t)j].cluster==pc){parent=j;break;}}
		e.parent=parent; if(parent>=0){const int lx=e.cluster.x-pc.x*2, ly=e.cluster.y-pc.y*2; entries[(size_t)parent].children[CLAMP(ly,0,1)*2+CLAMP(lx,0,1)]=i;}
	}
	eastl::vector<int> roots; for(int i=0;i<(int)entries.size();++i) if(entries[(size_t)i].parent<0) roots.push_back(i);
	struct NodeTmp{uint16_t region; int64_t spawn; uint8_t level;}; eastl::vector<NodeTmp> nodes;
	const int node_count=MIN(rNodeRegionIndices.size(),MIN(rNodeSpawnIds.size(),rNodeMinLevels.size())); nodes.reserve((size_t)node_count);
	for(int i=0;i<node_count;++i){const int region=rNodeRegionIndices[i]; const uint8_t level=rNodeMinLevels[i]; if(region>=0&&region<TERRAIN3D_MAX_REGION_SLOTS&&level>0)nodes.push_back({(uint16_t)region,rNodeSpawnIds[i],level});}
	eastl::sort(nodes.begin(),nodes.end(),[](const NodeTmp&a,const NodeTmp&b){return a.region<b.region||(a.region==b.region&&a.spawn<b.spawn);});

	u32(out,HLOD_RUNTIME_MAGIC); u16(out,HLOD_RUNTIME_VERSION); u16(out,0); u32(out,(uint32_t)entries.size()); u32(out,(uint32_t)roots.size());
	u32(out,(uint32_t)rInstancerMinLevels.size()); u32(out,(uint32_t)nodes.size());
	for(const auto&e:entries){
		u8(out,(uint8_t)e.level);u8(out,0);u16(out,0); i32(out,e.cluster.x);i32(out,e.cluster.y);i32(out,e.regionMin.x);i32(out,e.regionMin.y);i32(out,e.regionMax.x);i32(out,e.regionMax.y);
		f32(out,e.localBounds.position.x);f32(out,e.localBounds.position.y);f32(out,e.localBounds.position.z);f32(out,e.localBounds.size.x);f32(out,e.localBounds.size.y);f32(out,e.localBounds.size.z);f32(out,(float)e.geometricError);
		i64(out,e.sourceSignature);i64(out,e.buildSignature);i64(out,e.estimatedGpuBytes);i32(out,e.parent);for(int c:e.children)i32(out,c);
		const CharString utf8=e.packagePath.utf8(); const uint32_t len=(uint32_t)MAX(utf8.length(),0); u32(out,len); const int64_t o=out.size();out.resize(o+len);if(len)std::memcpy(out.ptrw()+o,utf8.get_data(),len);
	}
	for(int root:roots)u32(out,(uint32_t)root);
	if(!rInstancerMinLevels.is_empty()){const int64_t o=out.size();out.resize(o+rInstancerMinLevels.size());std::memcpy(out.ptrw()+o,rInstancerMinLevels.ptr(),(size_t)rInstancerMinLevels.size());}
	for(const auto&n:nodes){u16(out,n.region);u8(out,n.level);u8(out,0);i64(out,n.spawn);}
	return out;
}

bool T3DHLODRuntimeCodec::decode( const PackedByteArray &rBlob, T3DHLODRuntimeData &rData ) {
	rData=T3DHLODRuntimeData{}; if(rBlob.size()<24)return false; const uint8_t*p=rBlob.ptr(); size_t o=0;
	auto need=[&](size_t n){return n<=(size_t)rBlob.size()-o;};
	if(ru32(p)!=HLOD_RUNTIME_MAGIC||ru16(p+4)!=HLOD_RUNTIME_VERSION)return false; o=8;
	const uint32_t ec=ru32(p+o);o+=4;const uint32_t rc=ru32(p+o);o+=4;const uint32_t mesh_bytes=ru32(p+o);o+=4;const uint32_t nc=ru32(p+o);o+=4;
	if(ec>65535u||rc>ec||mesh_bytes>(uint32_t)(TERRAIN3D_MAX_REGION_SLOTS*TERRAIN3D_MAX_MESH_ASSETS)||nc>1000000u)return false;
	rData.entries.reserve(ec);
	for(uint32_t i=0;i<ec;++i){if(!need(104))return false;T3DHLODRuntimeEntryRecord e;e.level=ru8(p+o);o+=4;e.cluster=Vector2i(ri32(p+o),ri32(p+o+4));o+=8;e.regionMin=Vector2i(ri32(p+o),ri32(p+o+4));o+=8;e.regionMax=Vector2i(ri32(p+o),ri32(p+o+4));o+=8;
		e.localBounds=AABB(Vector3(rf32(p+o),rf32(p+o+4),rf32(p+o+8)),Vector3(rf32(p+o+12),rf32(p+o+16),rf32(p+o+20)));o+=24;e.geometricError=rf32(p+o);o+=4;e.sourceSignature=ri64(p+o);o+=8;e.buildSignature=ri64(p+o);o+=8;e.estimatedGpuBytes=ri64(p+o);o+=8;e.parent=ri32(p+o);o+=4;for(int c=0;c<4;++c){e.children[c]=ri32(p+o);o+=4;} if(!need(4))return false;const uint32_t len=ru32(p+o);o+=4;if(!need(len))return false;e.packagePath=String::utf8((const char*)(p+o),(int)len);o+=len;
		if(e.level<1||e.level>6||e.packagePath.is_empty()||e.parent < -1||e.parent>=(int)ec)return false;for(int c:e.children)if(c < -1||c>=(int)ec)return false;rData.entries.push_back(e);}
	if(!need((size_t)rc*4u))return false;rData.roots.reserve(rc);for(uint32_t i=0;i<rc;++i){const int root=(int)ru32(p+o);o+=4;if(root<0||root>=(int)ec)return false;rData.roots.push_back(root);}
	if(!need(mesh_bytes))return false;rData.instancerMinLevels.resize(mesh_bytes);if(mesh_bytes)std::memcpy(rData.instancerMinLevels.data(),p+o,mesh_bytes);o+=mesh_bytes;
	if(!need((size_t)nc*12u))return false; rData.nodePolicies.reserve(nc);rData.nodePolicyOffsets.fill(0u);uint16_t previous_region=0;bool have=false;
	struct DecNode{uint16_t region;T3DHLODRuntimeNodePolicyRecord policy;};eastl::vector<DecNode> decoded;decoded.reserve(nc);
	for(uint32_t i=0;i<nc;++i){const uint16_t region=ru16(p+o);const uint8_t level=ru8(p+o+2);const int64_t spawn=ri64(p+o+4);o+=12;if(region>=TERRAIN3D_MAX_REGION_SLOTS||level==0)return false;if(have&&region<previous_region)return false;previous_region=region;have=true;decoded.push_back({region,{spawn,level}});++rData.nodePolicyOffsets[(size_t)region+1u];}
	for(int i=0;i<TERRAIN3D_MAX_REGION_SLOTS;++i)rData.nodePolicyOffsets[(size_t)i+1u]+=rData.nodePolicyOffsets[(size_t)i];for(const auto&n:decoded)rData.nodePolicies.push_back(n.policy);
	return o==(size_t)rBlob.size();
}
