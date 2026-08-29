// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_MANIFEST_H
#define TERRAIN3D_RENDER_WORLD_MANIFEST_H

#include "rendering/rd/render_world/terrain_3d_render_world_resource_plan.h"

#include <stddef.h>
#include <stdint.h>

static const uint32_t T3D_RW_WORLD_MAGIC = 0x57443354u; // T3DW little-endian.
static const uint16_t T3D_RW_WORLD_VERSION = 1u;
static const uint16_t T3D_RW_WORLD_ENDIAN_LE = 0x4c45u;

enum T3DRenderWorldManifestFlags : uint32_t {
	T3D_RW_WORLD_FLAG_SHIPPING = 1u << 0u,
	T3D_RW_WORLD_FLAG_HAS_HLOD = 1u << 1u,
	T3D_RW_WORLD_FLAG_HAS_LIGHTMAPS = 1u << 2u,
	T3D_RW_WORLD_FLAG_HAS_NODES = 1u << 3u
};

enum T3DRenderWorldManifestStatus : uint32_t {
	T3D_RW_WORLD_STATUS_OK = 0u,
	T3D_RW_WORLD_STATUS_BAD_ARGUMENT,
	T3D_RW_WORLD_STATUS_BAD_MAGIC,
	T3D_RW_WORLD_STATUS_BAD_VERSION,
	T3D_RW_WORLD_STATUS_BAD_ENDIAN,
	T3D_RW_WORLD_STATUS_BAD_HEADER,
	T3D_RW_WORLD_STATUS_BAD_BOUNDS,
	T3D_RW_WORLD_STATUS_BAD_CAPACITY,
	T3D_RW_WORLD_STATUS_STALE_GEOMETRY,
	T3D_RW_WORLD_STATUS_STALE_VT,
	T3D_RW_WORLD_STATUS_SOURCE_REBUILD_FORBIDDEN
};

struct T3DRenderWorldManifestSpan {
	uint64_t nOffset;
	uint32_t nCount;
	uint32_t nStride;
};

struct T3DRenderWorldManifestHeader {
	uint32_t nMagic;
	uint16_t nVersion;
	uint16_t nEndian;
	uint32_t nHeaderBytes;
	uint32_t nFlags;
	uint64_t nContentSignature;
	uint64_t nGeometryPackSignature;
	uint64_t nVTPackSignature;
	uint64_t nSourceCookGeneration;
	T3DRenderWorldLimits Limits;
	T3DRenderWorldManifestSpan Cells;
	T3DRenderWorldManifestSpan Instances;
	T3DRenderWorldManifestSpan MeshDependencies;
	T3DRenderWorldManifestSpan MaterialDependencies;
	T3DRenderWorldManifestSpan TerrainPageReferences;
	T3DRenderWorldManifestSpan HLODReferences;
	T3DRenderWorldManifestSpan LightmapReferences;
	T3DRenderWorldManifestSpan NodeReferences;
	uint64_t nFileBytes;
	uint64_t nReserved[3];
};

struct T3DRenderWorldManifestView {
	const uint8_t *pBytes;
	uint64_t nBytes;
	const T3DRenderWorldManifestHeader *pHeader;
	T3DRenderWorldManifestStatus eStatus;
};

struct T3DRenderWorldShippingAdmission {
	T3DRenderWorldLimits Limits;
	uint64_t nManifestContentSignature;
	uint64_t nGeometryPackSignature;
	uint64_t nVTPackSignature;
	uint64_t nSourceCookGeneration;
	T3DRenderWorldManifestStatus eStatus;
	bool bShippingAuthoritative;
};

bool t3d_render_world_manifest_open_reference_debug( const void *pData, uint64_t pBytes, T3DRenderWorldManifestView &rView );
bool t3d_render_world_manifest_admit_shipping_cold( const T3DRenderWorldManifestView &pView,
        uint64_t pLoadedGeometryPackSignature,
        uint64_t pLoadedVTPackSignature,
        T3DRenderWorldShippingAdmission &rAdmission );
bool t3d_render_world_manifest_compute_resource_plan_cold(
        const T3DRenderWorldShippingAdmission &pAdmission, const T3DRenderWorldGpuProfile &pProfile, T3DRenderWorldGpuResourcePlan &rPlan );
bool t3d_render_world_manifest_span_valid_reference_debug(
        const T3DRenderWorldManifestView &pView, const T3DRenderWorldManifestSpan &pSpan );

#endif // TERRAIN3D_RENDER_WORLD_MANIFEST_H
