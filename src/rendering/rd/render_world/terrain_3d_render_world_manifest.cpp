// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_manifest.h"

#include <string.h>

static bool t3d_rw_manifest_mul_u64( uint64_t pA, uint64_t pB, uint64_t &rOut ) {
	if ( pA != 0u && pB > UINT64_MAX / pA ) {
		return false;
	}
	rOut = pA * pB;
	return true;
}

bool t3d_render_world_manifest_span_valid_reference_debug(
        const T3DRenderWorldManifestView &pView, const T3DRenderWorldManifestSpan &pSpan ) {
	if ( pView.pBytes == NULL || pView.pHeader == NULL ) {
		return false;
	}
	if ( pSpan.nCount == 0u ) {
		return pSpan.nOffset == 0u || pSpan.nOffset <= pView.nBytes;
	}
	if ( pSpan.nStride == 0u || pSpan.nOffset < pView.pHeader->nHeaderBytes || pSpan.nOffset > pView.nBytes ) {
		return false;
	}
	uint64_t nPayloadBytes = 0u;
	if ( !t3d_rw_manifest_mul_u64( ( uint64_t )pSpan.nCount, ( uint64_t )pSpan.nStride, nPayloadBytes ) ) {
		return false;
	}
	return nPayloadBytes <= pView.nBytes - pSpan.nOffset;
}

bool t3d_render_world_manifest_open_reference_debug( const void *pData, uint64_t pBytes, T3DRenderWorldManifestView &rView ) {
	memset( &rView, 0, sizeof( rView ) );
	rView.eStatus = T3D_RW_WORLD_STATUS_BAD_ARGUMENT;
	if ( pData == NULL || pBytes < sizeof( T3DRenderWorldManifestHeader ) ) {
		return false;
	}
	const T3DRenderWorldManifestHeader *pHeader = ( const T3DRenderWorldManifestHeader * )pData;
	rView.pBytes = ( const uint8_t * )pData;
	rView.nBytes = pBytes;
	rView.pHeader = pHeader;
	if ( pHeader->nMagic != T3D_RW_WORLD_MAGIC ) {
		rView.eStatus = T3D_RW_WORLD_STATUS_BAD_MAGIC;
		return false;
	}
	if ( pHeader->nVersion != T3D_RW_WORLD_VERSION ) {
		rView.eStatus = T3D_RW_WORLD_STATUS_BAD_VERSION;
		return false;
	}
	if ( pHeader->nEndian != T3D_RW_WORLD_ENDIAN_LE ) {
		rView.eStatus = T3D_RW_WORLD_STATUS_BAD_ENDIAN;
		return false;
	}
	if ( pHeader->nHeaderBytes != sizeof( T3DRenderWorldManifestHeader ) || pHeader->nFileBytes != pBytes ) {
		rView.eStatus = T3D_RW_WORLD_STATUS_BAD_HEADER;
		return false;
	}
	if ( ( pHeader->nFlags & T3D_RW_WORLD_FLAG_SHIPPING ) == 0u ) {
		rView.eStatus = T3D_RW_WORLD_STATUS_SOURCE_REBUILD_FORBIDDEN;
		return false;
	}
	if ( !t3d_render_world_validate_limits( pHeader->Limits ) ) {
		rView.eStatus = T3D_RW_WORLD_STATUS_BAD_CAPACITY;
		return false;
	}
	const T3DRenderWorldManifestSpan *pSpans[] = { &pHeader->Cells,
		&pHeader->Instances,
		&pHeader->MeshDependencies,
		&pHeader->MaterialDependencies,
		&pHeader->TerrainPageReferences,
		&pHeader->HLODReferences,
		&pHeader->LightmapReferences,
		&pHeader->NodeReferences };
	for ( uint32_t i = 0u; i < sizeof( pSpans ) / sizeof( pSpans[0] ); ++i ) {
		if ( !t3d_render_world_manifest_span_valid_reference_debug( rView, *pSpans[i] ) ) {
			rView.eStatus = T3D_RW_WORLD_STATUS_BAD_BOUNDS;
			return false;
		}
	}
	if ( pHeader->Cells.nCount > pHeader->Limits.nMaxCells || pHeader->Instances.nCount > pHeader->Limits.nMaxInstances ) {
		rView.eStatus = T3D_RW_WORLD_STATUS_BAD_CAPACITY;
		return false;
	}
	rView.eStatus = T3D_RW_WORLD_STATUS_OK;
	return true;
}


bool t3d_render_world_manifest_compute_resource_plan_cold( const T3DRenderWorldShippingAdmission &pAdmission,
        const T3DRenderWorldGpuProfile &pProfile,
        T3DRenderWorldGpuResourcePlan &rPlan ) {
	memset( &rPlan, 0, sizeof( rPlan ) );
	if ( !pAdmission.bShippingAuthoritative || pAdmission.eStatus != T3D_RW_WORLD_STATUS_OK ) {
		return false;
	}
	return t3d_render_world_compute_gpu_resource_plan( pAdmission.Limits, pProfile, rPlan );
}

bool t3d_render_world_manifest_admit_shipping_cold( const T3DRenderWorldManifestView &pView,
        uint64_t pLoadedGeometryPackSignature,
        uint64_t pLoadedVTPackSignature,
        T3DRenderWorldShippingAdmission &rAdmission ) {
	memset( &rAdmission, 0, sizeof( rAdmission ) );
	rAdmission.eStatus = T3D_RW_WORLD_STATUS_BAD_ARGUMENT;
	if ( pView.pHeader == NULL || pView.eStatus != T3D_RW_WORLD_STATUS_OK ) {
		return false;
	}
	if ( pView.pHeader->nGeometryPackSignature != pLoadedGeometryPackSignature ) {
		rAdmission.eStatus = T3D_RW_WORLD_STATUS_STALE_GEOMETRY;
		return false;
	}
	if ( pView.pHeader->nVTPackSignature != pLoadedVTPackSignature ) {
		rAdmission.eStatus = T3D_RW_WORLD_STATUS_STALE_VT;
		return false;
	}
	rAdmission.Limits = pView.pHeader->Limits;
	rAdmission.nManifestContentSignature = pView.pHeader->nContentSignature;
	rAdmission.nGeometryPackSignature = pView.pHeader->nGeometryPackSignature;
	rAdmission.nVTPackSignature = pView.pHeader->nVTPackSignature;
	rAdmission.nSourceCookGeneration = pView.pHeader->nSourceCookGeneration;
	rAdmission.eStatus = T3D_RW_WORLD_STATUS_OK;
	rAdmission.bShippingAuthoritative = true;
	return true;
}
