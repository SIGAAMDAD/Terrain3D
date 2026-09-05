// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_CONTRACT_H
#define TERRAIN3D_RENDER_WORLD_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

static const uint32_t T3D_RW_INVALID_ID = 0xffffffffu;
static const uint32_t T3D_RW_CLUSTER_MAX_VERTICES = 64u;
static const uint32_t T3D_RW_CLUSTER_MAX_TRIANGLES = 124u;
static const uint32_t T3D_RW_CLUSTER_RASTER_VERTEX_COUNT = T3D_RW_CLUSTER_MAX_TRIANGLES * 3u;

enum T3DRenderWorldTier : uint32_t {
	T3D_RW_TIER_WORLD = 0,
	T3D_RW_TIER_CLUTTER = 1,
	T3D_RW_TIER_FOLIAGE = 2,
	T3D_RW_TIER_COUNT = 3
};

enum T3DOperationClass : uint32_t {
	T3D_OPCLASS_HOT = 0,
	T3D_OPCLASS_ASYNC,
	T3D_OPCLASS_WARM,
	T3D_OPCLASS_COLD,
	T3D_OPCLASS_DEBUG,
	T3D_OPCLASS_FORBIDDEN
};

enum T3DOperationType : uint32_t {
	T3D_OP_ALLOC_CPU = 0,
	T3D_OP_FREE_CPU,
	T3D_OP_GPU_CREATE,
	T3D_OP_GPU_FREE,
	T3D_OP_GPU_UPLOAD,
	T3D_OP_GPU_COPY,
	T3D_OP_GPU_DISPATCH,
	T3D_OP_GPU_DRAW,
	T3D_OP_GPU_READBACK_ASYNC,
	T3D_OP_IO_READ_ASYNC,
	T3D_OP_IO_WRITE_COLD,
	T3D_OP_DECOMPRESS_ASYNC,
	T3D_OP_SHADER_COMPILE_COLD,
	T3D_OP_PIPELINE_CREATE_COLD,
	T3D_OP_EVICT,
	T3D_OP_REMAP,
	T3D_OP_PUBLISH,
	T3D_OP_WAIT_BLOCKING,
	T3D_OP_FALLBACK,
	T3D_OP_OVERFLOW,
	T3D_OP_ERROR,
	T3D_OP_COUNT
};

struct T3DPrototypeVertex {
	float vPosition[3];
	uint32_t nPackedNormal;
	uint32_t nPackedTangent;
	float vUv[2];
	uint32_t nPackedColor;
};

struct T3DPackedClusterTriangle {
	uint32_t nLocalIndices;
};

struct T3DSceneCell {
	float vCenter[3];
	float fRadius;
	uint32_t nInstanceOffset;
	uint32_t nInstanceCount;
	uint32_t nFlags;
	uint32_t nGeneration;
};

struct T3DMeshRecord {
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nMaterialBase;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
};

struct T3DMeshLodRecord {
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	float fMinDistance;
	float fMaxDistance;
	float fScreenError;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
};

struct T3DClusterRecord {
	float vCenter[3];
	float fRadius;
	float vConeAxis[3];
	float fConeCutoff;
	uint32_t nVertexIndexOffset;
	uint32_t nTriangleOffset;
	uint16_t nVertexCount;
	uint16_t nTriangleCount;
	uint16_t nLod;
	uint16_t nTier;
	uint32_t nMaterialId;
	uint32_t nPackedUvMin;
	uint32_t nPackedUvMax;
	uint32_t nTexelDensity;
};

struct T3DInstanceRecord {
	float mTransform3x4[12];
	uint32_t nMeshId;
	uint32_t nMaterialOverride;
	uint32_t nFlags;
	uint32_t nCellId;
};

enum T3DRenderWorldMaterialFlags : uint32_t {
	T3D_RW_MATERIAL_FLAG_ALPHA_SCISSOR = 1u << 0u,
	T3D_RW_MATERIAL_FLAG_FOLIAGE = 1u << 1u,
	T3D_RW_MATERIAL_FLAG_CAST_SHADOWS = 1u << 2u,
	T3D_RW_MATERIAL_FLAG_RECEIVE_SHADOWS = 1u << 3u,
	T3D_RW_MATERIAL_FLAG_TWO_SIDED = 1u << 4u
};

enum T3DRenderWorldSamplerClass : uint32_t {
	T3D_RW_SAMPLER_REPEAT = 0u,
	T3D_RW_SAMPLER_CLAMP = 1u
};

struct T3DMaterialRecord {
	uint32_t nAlbedoVT;
	uint32_t nNormalVT;
	uint32_t nSurfaceVT;
	uint32_t nHeightVT;
	float vBaseColor[4];
	float vUvScaleBias[4];
	uint32_t nFlags;
	uint32_t nSamplerClass;
	uint32_t nPackedScalars0;
	uint32_t nPackedScalars1;
};

struct T3DVisibleCluster {
	uint32_t nClusterId;
	uint32_t nInstanceId;
};

struct T3DDrawIndirectArgs {
	uint32_t nVertexCount;
	uint32_t nInstanceCount;
	uint32_t nFirstVertex;
	uint32_t nFirstInstance;
};

enum T3DVTPageFlags : uint16_t {
	T3D_VT_PAGE_VALID = 1u << 0u,
	T3D_VT_PAGE_RESIDENT = 1u << 1u,
	T3D_VT_PAGE_PINNED = 1u << 2u,
	T3D_VT_PAGE_NEUTRAL = 1u << 3u,
	T3D_VT_PAGE_ROOT = 1u << 4u
};

struct T3DVTAllocationRecord {
	uint32_t nBaseVirtualPage;
	uint32_t nPageCount;
	uint32_t nMip0WidthPages;
	uint32_t nMip0HeightPages;
	uint16_t nMipCount;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nReserved0;
	uint32_t nRootPage;
	uint32_t nReserved1;
};

struct T3DVTPageTableEntry {
	uint32_t nPhysicalSlot;
	uint32_t nParentPage;
	uint32_t nGeneration;
	uint16_t nResolvedMip;
	uint16_t nFlags;
};

struct T3DVTGpuCounters {
	uint32_t nRequestCount;
	uint32_t nPredictedRequestCount;
	uint32_t nDedupRequestCount;
	uint32_t nHashOverflowCount;
	uint32_t nListOverflowCount;
	uint32_t nFragmentFaultCount;
	uint32_t nReadbackIssueCount;
	uint32_t nReserved;
};

struct T3DVTRequest {
	uint32_t nVirtualPage;
	uint16_t nWantedMip;
	uint16_t nPriority;
};

struct T3DVisibilityFrame {
	float mViewProjection[16];
	float mPreviousViewProjection[16];
	float mViewMatrix[16];
	float vFrustumPlanes[24];
	float vCameraPositionAndMaxDistance[4];
	float vViewportAndMotion[4];
	float vMotionJitter[4];
	uint32_t nFrameId;
	uint32_t nFlags;
	uint32_t nHiZInvalidReason;
	uint32_t nCellCount;
	uint32_t nInstanceCount;
	uint32_t nMeshCount;
	uint32_t nLodCount;
	uint32_t nClusterCount;
	uint32_t nVisibleCellCapacity;
	uint32_t nVisibleInstanceCapacity;
	uint32_t nVisibleWorldCapacity;
	uint32_t nVisibleClutterCapacity;
	uint32_t nVisibleFoliageCapacity;
	uint32_t nHiZWidth;
	uint32_t nHiZHeight;
	uint32_t nHiZMipCount;
	uint32_t nMaterialCount;
	uint32_t nVtAllocationCount;
	uint32_t nVtPageCount;
	uint32_t nResourceCountReserved;
};

struct T3DVisibilityCounters {
	uint32_t nVisibleCellCount;
	uint32_t nVisibleInstanceCount;
	uint32_t nVisibleClusterCount[T3D_RW_TIER_COUNT];
	uint32_t nCandidateClusterCount;
	uint32_t nCellRejectCount;
	uint32_t nInstanceRejectCount;
	uint32_t nClusterFrustumRejectCount;
	uint32_t nClusterConeRejectCount;
	uint32_t nClusterHiZRejectCount;
	uint32_t nOverflowBits;
	uint32_t nFallbackCount;
	uint32_t nReserved[3];
};

struct T3DHiZDispatchConstants {
	uint32_t nSourceWidth;
	uint32_t nSourceHeight;
	uint32_t nDestinationWidth;
	uint32_t nDestinationHeight;
};

struct T3DRenderWorldLimits {
	uint32_t nMaxCells;
	uint32_t nMaxInstances;
	uint32_t nMaxMeshes;
	uint32_t nMaxMeshLods;
	uint32_t nMaxClusters;
	uint32_t nMaxMaterials;
	uint32_t nMaxVisibleCells;
	uint32_t nMaxVisibleInstances;
	uint32_t nMaxVisibleClusters[T3D_RW_TIER_COUNT];
	uint32_t nMaxVtVirtualPages;
	uint32_t nMaxVtPhysicalAlbedoPages;
	uint32_t nMaxVtPhysicalNormalPages;
	uint32_t nMaxVtPhysicalSurfacePages;
	uint32_t nMaxVtPhysicalHeightPages;
	uint32_t nMaxVtRequests;
	uint32_t nMaxStreamJobs;
	uint32_t nStreamScratchLaneCount;
	uint64_t nMaxStreamPayloadBytes;
	uint64_t nMaxUploadBytesPerFrame;
	uint64_t nMaxReadbackBytesPerFrame;
	uint64_t nGeometryVertexHeapBytes;
	uint64_t nGeometryClusterVertexIndexHeapBytes;
	uint64_t nGeometryTriangleHeapBytes;
	uint64_t nSnapshotBytesPerFrame;
	uint64_t nFrameScratchBytesPerFrame;
	uint64_t nStreamScratchBytesPerLane;
	uint32_t nOperationEventCapacity;
	uint32_t nReserved;
};

struct T3DRenderWorldMemoryPlan {
	uint64_t nCpuPermanentBytes;
	uint64_t nCpuResidentBytes;
	uint64_t nCpuSnapshotBytes;
	uint64_t nCpuFrameScratchBytes;
	uint64_t nCpuStreamingScratchBytes;
	uint64_t nGpuGeometryBytes;
	uint64_t nGpuSceneRecordBytes;
	uint64_t nGpuVisibilityBytes;
	uint64_t nGpuVTPhysicalBytes;
	uint64_t nGpuHiZBytes;
	uint64_t nGpuMiscBytes;
};

struct T3DOperationEvent {
	uint64_t nFrameId;
	uint64_t nBeginTicks;
	uint64_t nDurationTicks;
	uint64_t nBytes;
	uint32_t nOperationType;
	uint32_t nOperationClass;
	uint32_t nSubsystemId;
	uint32_t nResourceId;
	uint32_t nCount;
	uint32_t nResultCode;
	uint32_t nThreadId;
	uint32_t nSourceTag;
};

static_assert( sizeof( T3DPrototypeVertex ) == 32, "T3DPrototypeVertex ABI mismatch" );
static_assert( sizeof( T3DPackedClusterTriangle ) == 4, "T3DPackedClusterTriangle ABI mismatch" );
static_assert( sizeof( T3DSceneCell ) == 32, "T3DSceneCell ABI mismatch" );
static_assert( sizeof( T3DMeshRecord ) == 32, "T3DMeshRecord ABI mismatch" );
static_assert( sizeof( T3DMeshLodRecord ) == 32, "T3DMeshLodRecord ABI mismatch" );
static_assert( sizeof( T3DClusterRecord ) == 64, "T3DClusterRecord ABI mismatch" );
static_assert( sizeof( T3DInstanceRecord ) == 64, "T3DInstanceRecord ABI mismatch" );
static_assert( sizeof( T3DMaterialRecord ) == 64, "T3DMaterialRecord ABI mismatch" );
static_assert( sizeof( T3DVisibleCluster ) == 8, "T3DVisibleCluster ABI mismatch" );
static_assert( sizeof( T3DDrawIndirectArgs ) == 16, "T3DDrawIndirectArgs ABI mismatch" );
static_assert( sizeof( T3DVTAllocationRecord ) == 32, "T3DVTAllocationRecord ABI mismatch" );
static_assert( sizeof( T3DVTPageTableEntry ) == 16, "T3DVTPageTableEntry ABI mismatch" );
static_assert( sizeof( T3DVTGpuCounters ) == 32, "T3DVTGpuCounters ABI mismatch" );
static_assert( sizeof( T3DVTRequest ) == 8, "T3DVTRequest ABI mismatch" );
static_assert( sizeof( T3DVisibilityFrame ) == 416, "T3DVisibilityFrame ABI mismatch" );
static_assert( sizeof( T3DVisibilityCounters ) == 64, "T3DVisibilityCounters ABI mismatch" );
static_assert( sizeof( T3DHiZDispatchConstants ) == 16, "T3DHiZDispatchConstants ABI mismatch" );

T3DRenderWorldLimits t3d_render_world_make_prototype_limits();
bool t3d_render_world_validate_limits( const T3DRenderWorldLimits &pLimits );
uint64_t t3d_render_world_limits_signature( const T3DRenderWorldLimits &pLimits );
T3DRenderWorldMemoryPlan t3d_render_world_compute_minimum_memory_plan( const T3DRenderWorldLimits &pLimits );

#endif // TERRAIN3D_RENDER_WORLD_CONTRACT_H
