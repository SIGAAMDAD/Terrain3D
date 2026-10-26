// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/formats/terrain_3d_half.h"
#include "Terrain3DTypes.h"

#include "rendering/rd/world/geometry/terrain_3d_world_geometry_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

// t3dWorldGeometryClusterBuilder_s centralized in Terrain3DTypes.h.


// t3dWorldGeometryRequirements_s centralized in Terrain3DTypes.h.


static bool t3d_world_geometry_u32_add( uint32_t pA, uint32_t pB, uint32_t &rValue ) {
	if ( pA > UINT32_MAX - pB ) {
		return false;
	}

	rValue = pA + pB;
	return true;
}

static bool t3d_world_geometry_u64_add( uint64_t pA, uint64_t pB, uint64_t &rValue ) {
	if ( pA > UINT64_MAX - pB ) {
		return false;
	}

	rValue = pA + pB;
	return true;
}

uint32_t t3d_world_geometry_float_bits( float pValue ) {
	uint32_t nBits = 0u;
	memcpy( &nBits, &pValue, sizeof( nBits ) );
	return nBits;
}

static float t3d_world_geometry_bits_float( uint32_t pBits ) {
	float fValue = 0.0f;
	memcpy( &fValue, &pBits, sizeof( fValue ) );
	return fValue;
}

bool t3d_world_geometry_finite( float pValue ) {
	return isfinite( pValue ) != 0;
}

static float t3d_world_geometry_clamp( float pValue, float pMin, float pMax ) {
	if ( pValue < pMin ) {
		return pMin;
	}
	if ( pValue > pMax ) {
		return pMax;
	}
	return pValue;
}

float t3d_world_geometry_length3( float pX, float pY, float pZ ) {
	return sqrtf( pX * pX + pY * pY + pZ * pZ );
}

static uint32_t t3d_world_geometry_pack_half2( float pX, float pY ) {
	const float fX = t3d_world_geometry_clamp( pX, -65504.0f, 65504.0f );
	const float fY = t3d_world_geometry_clamp( pY, -65504.0f, 65504.0f );
	return ( uint32_t )t3dHalfEncode( fX ) | ( ( uint32_t )t3dHalfEncode( fY ) << 16u );
}

static void t3d_world_geometry_unpack_vertex_uv( const t3dWorldVertex_t &rVertex, float &rU, float &rV ) {
	rU = t3dHalfDecode( ( uint16_t )( rVertex.m_nPackedUv0 & 0xffffu ) );
	rV = t3dHalfDecode( ( uint16_t )( rVertex.m_nPackedUv0 >> 16u ) );
}

uint32_t t3d_world_geometry_pack_local_triangle( uint32_t pLocal0, uint32_t pLocal1, uint32_t pLocal2 ) {
	if ( pLocal0 >= T3D_WORLD_CLUSTER_MAX_VERTICES || pLocal1 >= T3D_WORLD_CLUSTER_MAX_VERTICES || pLocal2 >= T3D_WORLD_CLUSTER_MAX_VERTICES ) {
		return UINT32_MAX;
	}

	return ( pLocal0 & 0x3fu ) | ( ( pLocal1 & 0x3fu ) << 6u ) | ( ( pLocal2 & 0x3fu ) << 12u );
}

void t3d_world_geometry_unpack_local_triangle( uint32_t pPacked, uint32_t &rLocal0, uint32_t &rLocal1, uint32_t &rLocal2 ) {
	rLocal0 = pPacked & 0x3fu;
	rLocal1 = ( pPacked >> 6u ) & 0x3fu;
	rLocal2 = ( pPacked >> 12u ) & 0x3fu;
}

float t3d_world_geometry_unpack_texel_density( uint32_t pPacked ) {
	return t3d_world_geometry_bits_float( pPacked );
}

float t3d_world_geometry_cluster_fill_ratio( const t3dWorldGeometryCookStats_t &pStats ) {
	if ( pStats.nClusterTriangleSlotCount == 0u ) {
		return 0.0f;
	}

	return ( float )( ( double )pStats.nSourceTriangleCount / ( double )pStats.nClusterTriangleSlotCount );
}

static void t3d_world_geometry_builder_reset( t3dWorldGeometryClusterBuilder_t &rBuilder ) {
	memset( &rBuilder, 0, sizeof( rBuilder ) );
}

static uint32_t t3d_world_geometry_builder_find_vertex( const t3dWorldGeometryClusterBuilder_t &pBuilder, uint32_t pSourceVertex ) {
	for ( uint32_t iVertex = 0u; iVertex < pBuilder.nVertexCount; ++iVertex ) {
		if ( pBuilder.aSourceVertex[iVertex] == pSourceVertex ) {
			return iVertex;
		}
	}

	return T3D_WORLD_INVALID_ID;
}

static uint32_t t3d_world_geometry_builder_new_vertex_count(
        const t3dWorldGeometryClusterBuilder_t &pBuilder, uint32_t pVertex0, uint32_t pVertex1, uint32_t pVertex2 ) {
	uint32_t nNew = 0u;
	const uint32_t aVertex[3] = { pVertex0, pVertex1, pVertex2 };

	for ( uint32_t i = 0u; i < 3u; ++i ) {
		if ( t3d_world_geometry_builder_find_vertex( pBuilder, aVertex[i] ) != T3D_WORLD_INVALID_ID ) {
			continue;
		}

		bool bDuplicateNew = false;
		for ( uint32_t j = 0u; j < i; ++j ) {
			if ( aVertex[j] == aVertex[i] && t3d_world_geometry_builder_find_vertex( pBuilder, aVertex[j] ) == T3D_WORLD_INVALID_ID ) {
				bDuplicateNew = true;
				break;
			}
		}

		if ( !bDuplicateNew ) {
			++nNew;
		}
	}

	return nNew;
}

static uint32_t t3d_world_geometry_builder_add_vertex( t3dWorldGeometryClusterBuilder_t &rBuilder, uint32_t pSourceVertex ) {
	uint32_t nLocal = t3d_world_geometry_builder_find_vertex( rBuilder, pSourceVertex );
	if ( nLocal != T3D_WORLD_INVALID_ID ) {
		return nLocal;
	}

	if ( rBuilder.nVertexCount >= T3D_WORLD_CLUSTER_MAX_VERTICES ) {
		return T3D_WORLD_INVALID_ID;
	}

	nLocal = rBuilder.nVertexCount;
	rBuilder.aSourceVertex[rBuilder.nVertexCount++] = pSourceVertex;
	return nLocal;
}

static bool t3d_world_geometry_builder_add_triangle( t3dWorldGeometryClusterBuilder_t &rBuilder,
        uint32_t pVertex0,
        uint32_t pVertex1,
        uint32_t pVertex2,
        uint32_t pMaterialId,
        uint32_t pTier ) {
	if ( rBuilder.nTriangleCount >= T3D_WORLD_CLUSTER_MAX_TRIANGLES ) {
		return false;
	}

	if ( rBuilder.nTriangleCount == 0u ) {
		rBuilder.nMaterialId = pMaterialId;
		rBuilder.nTier = pTier;
	} else if ( rBuilder.nMaterialId != pMaterialId || rBuilder.nTier != pTier ) {
		return false;
	}

	const uint32_t nLocal0 = t3d_world_geometry_builder_add_vertex( rBuilder, pVertex0 );
	const uint32_t nLocal1 = t3d_world_geometry_builder_add_vertex( rBuilder, pVertex1 );
	const uint32_t nLocal2 = t3d_world_geometry_builder_add_vertex( rBuilder, pVertex2 );
	if ( nLocal0 == T3D_WORLD_INVALID_ID || nLocal1 == T3D_WORLD_INVALID_ID || nLocal2 == T3D_WORLD_INVALID_ID ) {
		return false;
	}

	const uint32_t nPacked = t3d_world_geometry_pack_local_triangle( nLocal0, nLocal1, nLocal2 );
	if ( nPacked == UINT32_MAX ) {
		return false;
	}

	rBuilder.aTriangles[rBuilder.nTriangleCount++].nLocalIndices = nPacked;
	return true;
}

bool t3d_world_geometry_get_triangle_classification( const t3dWorldGeometrySourceMesh_t &pSource,
        const t3dWorldGeometrySourceLod_t &pLod,
        uint32_t pTriangle,
        uint32_t &rMaterialId,
        uint32_t &rTier ) {
	uint32_t nMaterialOffset = 0u;
	if ( pLod.pTriangleMaterialOffsets != NULL ) {
		nMaterialOffset = pLod.pTriangleMaterialOffsets[pTriangle];
	}

	if ( !t3d_world_geometry_u32_add( pSource.nMaterialBase, nMaterialOffset, rMaterialId ) ) {
		return false;
	}

	rTier = pLod.nDefaultTier;
	if ( pLod.pTriangleTiers != NULL ) {
		rTier = ( uint32_t )pLod.pTriangleTiers[pTriangle];
	}

	return rTier < T3D_WORLD_TIER_COUNT;
}

bool t3d_world_geometry_validate_source( const t3dWorldGeometrySourceMesh_t &pSource ) {
	if ( pSource.pVertices == NULL || pSource.pLods == NULL ) {
		return false;
	}
	if ( pSource.nVertexCount == 0u || pSource.nLodCount == 0u ) {
		return false;
	}
	if ( pSource.nLodCount > 65536u ) {
		return false;
	}

	for ( uint32_t iVertex = 0u; iVertex < pSource.nVertexCount; ++iVertex ) {
		const t3dWorldVertex_t &Vertex = pSource.pVertices[iVertex];
		if ( !t3d_world_geometry_finite( Vertex.m_fPosition[0] ) ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Vertex.m_fPosition[1] ) ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Vertex.m_fPosition[2] ) ) {
			return false;
		}
		float fUvU = 0.0f;
		float fUvV = 0.0f;
		t3d_world_geometry_unpack_vertex_uv( Vertex, fUvU, fUvV );
		if ( !t3d_world_geometry_finite( fUvU ) || !t3d_world_geometry_finite( fUvV ) ) {
			return false;
		}
	}

	for ( uint32_t iLod = 0u; iLod < pSource.nLodCount; ++iLod ) {
		const t3dWorldGeometrySourceLod_t &Lod = pSource.pLods[iLod];
		if ( Lod.pIndices == NULL || Lod.nIndexCount == 0u || ( Lod.nIndexCount % 3u ) != 0u ) {
			return false;
		}
		if ( Lod.nDefaultTier >= T3D_WORLD_TIER_COUNT ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Lod.fMinDistance ) || Lod.fMinDistance < 0.0f ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Lod.fMaxDistance ) || Lod.fMaxDistance < Lod.fMinDistance ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Lod.fScreenError ) || Lod.fScreenError < 0.0f ) {
			return false;
		}

		const uint32_t nTriangleCount = Lod.nIndexCount / 3u;
		for ( uint32_t iTriangle = 0u; iTriangle < nTriangleCount; ++iTriangle ) {
			const uint32_t nIndex = iTriangle * 3u;
			if ( Lod.pIndices[nIndex + 0u] >= pSource.nVertexCount ) {
				return false;
			}
			if ( Lod.pIndices[nIndex + 1u] >= pSource.nVertexCount ) {
				return false;
			}
			if ( Lod.pIndices[nIndex + 2u] >= pSource.nVertexCount ) {
				return false;
			}

			uint32_t nMaterialId = 0u;
			uint32_t nTier = 0u;
			if ( !t3d_world_geometry_get_triangle_classification( pSource, Lod, iTriangle, nMaterialId, nTier ) ) {
				return false;
			}
		}
	}

	return true;
}

static bool t3d_world_geometry_accumulate_cluster_requirements(
        const t3dWorldGeometryClusterBuilder_t &pBuilder, t3dWorldGeometryRequirements_t &rRequirements ) {
	if ( pBuilder.nTriangleCount == 0u || pBuilder.nVertexCount == 0u ) {
		return false;
	}

	if ( rRequirements.nClusterCount == UINT32_MAX ) {
		return false;
	}

	++rRequirements.nClusterCount;
	if ( !t3d_world_geometry_u64_add(
	             rRequirements.nClusterVertexReferenceCount, pBuilder.nVertexCount, rRequirements.nClusterVertexReferenceCount ) ) {
		return false;
	}
	if ( !t3d_world_geometry_u64_add( rRequirements.nTriangleSlotCount, T3D_WORLD_CLUSTER_MAX_TRIANGLES, rRequirements.nTriangleSlotCount ) ) {
		return false;
	}

	if ( rRequirements.nMinimumClusterTriangleCount == 0u || pBuilder.nTriangleCount < rRequirements.nMinimumClusterTriangleCount ) {
		rRequirements.nMinimumClusterTriangleCount = pBuilder.nTriangleCount;
	}
	if ( pBuilder.nTriangleCount > rRequirements.nMaximumClusterTriangleCount ) {
		rRequirements.nMaximumClusterTriangleCount = pBuilder.nTriangleCount;
	}

	return true;
}

static bool t3d_world_geometry_preflight_lod(
        const t3dWorldGeometrySourceMesh_t &pSource, const t3dWorldGeometrySourceLod_t &pLod, t3dWorldGeometryRequirements_t &rRequirements ) {
	t3dWorldGeometryClusterBuilder_t Builder = {};
	t3d_world_geometry_builder_reset( Builder );

	const uint32_t nTriangleCount = pLod.nIndexCount / 3u;
	if ( !t3d_world_geometry_u64_add( rRequirements.nSourceTriangleCount, nTriangleCount, rRequirements.nSourceTriangleCount ) ) {
		return false;
	}

	for ( uint32_t iTriangle = 0u; iTriangle < nTriangleCount; ++iTriangle ) {
		const uint32_t nIndex = iTriangle * 3u;
		const uint32_t nVertex0 = pLod.pIndices[nIndex + 0u];
		const uint32_t nVertex1 = pLod.pIndices[nIndex + 1u];
		const uint32_t nVertex2 = pLod.pIndices[nIndex + 2u];
		uint32_t nMaterialId = 0u;
		uint32_t nTier = 0u;
		if ( !t3d_world_geometry_get_triangle_classification( pSource, pLod, iTriangle, nMaterialId, nTier ) ) {
			return false;
		}

		const uint32_t nNewVertices = t3d_world_geometry_builder_new_vertex_count( Builder, nVertex0, nVertex1, nVertex2 );
		const bool bClassChange = Builder.nTriangleCount != 0u && ( Builder.nMaterialId != nMaterialId || Builder.nTier != nTier );
		const bool bTriangleFull = Builder.nTriangleCount == T3D_WORLD_CLUSTER_MAX_TRIANGLES;
		const bool bVertexFull = Builder.nVertexCount + nNewVertices > T3D_WORLD_CLUSTER_MAX_VERTICES;

		if ( bClassChange || bTriangleFull || bVertexFull ) {
			if ( !t3d_world_geometry_accumulate_cluster_requirements( Builder, rRequirements ) ) {
				return false;
			}
			t3d_world_geometry_builder_reset( Builder );
		}

		if ( !t3d_world_geometry_builder_add_triangle( Builder, nVertex0, nVertex1, nVertex2, nMaterialId, nTier ) ) {
			return false;
		}
	}

	if ( Builder.nTriangleCount != 0u ) {
		if ( !t3d_world_geometry_accumulate_cluster_requirements( Builder, rRequirements ) ) {
			return false;
		}
	}

	return true;
}

static t3dWorldGeometryCookResult_t t3d_world_geometry_preflight_mesh(
        const t3dWorldGeometryDatabase_t &pDatabase, const t3dWorldGeometrySourceMesh_t &pSource, t3dWorldGeometryRequirements_t &rRequirements ) {
	memset( &rRequirements, 0, sizeof( rRequirements ) );
	if ( !t3d_world_geometry_validate_source( pSource ) ) {
		return T3D_WORLD_GEOMETRY_COOK_INVALID_INPUT;
	}

	for ( uint32_t iLod = 0u; iLod < pSource.nLodCount; ++iLod ) {
		if ( !t3d_world_geometry_preflight_lod( pSource, pSource.pLods[iLod], rRequirements ) ) {
			return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
		}
	}

	if ( rRequirements.nClusterVertexReferenceCount > UINT32_MAX || rRequirements.nTriangleSlotCount > UINT32_MAX ) {
		return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
	}

	uint32_t nEnd = 0u;
	if ( !t3d_world_geometry_u32_add( pDatabase.nVertexCount, pSource.nVertexCount, nEnd ) ) {
		return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
	}
	if ( nEnd > pDatabase.Buffers.nVertexCapacity ) {
		return T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED;
	}
	if ( !t3d_world_geometry_u32_add( pDatabase.nClusterVertexIndexCount, ( uint32_t )rRequirements.nClusterVertexReferenceCount, nEnd ) ) {
		return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
	}
	if ( nEnd > pDatabase.Buffers.nClusterVertexIndexCapacity ) {
		return T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED;
	}
	if ( !t3d_world_geometry_u32_add( pDatabase.nTriangleCount, ( uint32_t )rRequirements.nTriangleSlotCount, nEnd ) ) {
		return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
	}
	if ( nEnd > pDatabase.Buffers.nTriangleCapacity ) {
		return T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED;
	}
	if ( !t3d_world_geometry_u32_add( pDatabase.nMeshCount, 1u, nEnd ) ) {
		return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
	}
	if ( nEnd > pDatabase.Buffers.nMeshCapacity ) {
		return T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED;
	}
	if ( !t3d_world_geometry_u32_add( pDatabase.nLodCount, pSource.nLodCount, nEnd ) ) {
		return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
	}
	if ( nEnd > pDatabase.Buffers.nLodCapacity ) {
		return T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED;
	}
	if ( !t3d_world_geometry_u32_add( pDatabase.nClusterCount, rRequirements.nClusterCount, nEnd ) ) {
		return T3D_WORLD_GEOMETRY_COOK_NUMERIC_OVERFLOW;
	}
	if ( nEnd > pDatabase.Buffers.nClusterCapacity ) {
		return T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED;
	}

	return T3D_WORLD_GEOMETRY_COOK_OK;
}

static float t3d_world_geometry_edge_density( const t3dWorldVertex_t &pA, const t3dWorldVertex_t &pB ) {
	const float fDx = pB.m_fPosition[0] - pA.m_fPosition[0];
	const float fDy = pB.m_fPosition[1] - pA.m_fPosition[1];
	const float fDz = pB.m_fPosition[2] - pA.m_fPosition[2];
	const float fWorldLength = t3d_world_geometry_length3( fDx, fDy, fDz );
	if ( fWorldLength <= 1.0e-20f ) {
		return 0.0f;
	}

	float fAu = 0.0f, fAv = 0.0f, fBu = 0.0f, fBv = 0.0f;
	t3d_world_geometry_unpack_vertex_uv( pA, fAu, fAv );
	t3d_world_geometry_unpack_vertex_uv( pB, fBu, fBv );
	const float fDu = fBu - fAu;
	const float fDv = fBv - fAv;
	const float fUvLength = sqrtf( fDu * fDu + fDv * fDv );
	return fUvLength / fWorldLength;
}

bool t3d_world_geometry_compute_derived_cluster( const t3dWorldVertex_t *pVertexHeap,
        uint32_t pVertexHeapCount,
        const uint32_t *pClusterVertexIndices,
        uint32_t pVertexCount,
        const t3dWorldPackedClusterTriangle_t *pTriangles,
        uint32_t pTriangleCount,
        t3dWorldGeometryDerivedCluster_t &rDerived ) {
	if ( pVertexHeap == NULL || pClusterVertexIndices == NULL || pTriangles == NULL ) {
		return false;
	}
	if ( pVertexCount == 0u || pVertexCount > T3D_WORLD_CLUSTER_MAX_VERTICES ) {
		return false;
	}
	if ( pTriangleCount == 0u || pTriangleCount > T3D_WORLD_CLUSTER_MAX_TRIANGLES ) {
		return false;
	}

	float vMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float vMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	float fUvMinU = FLT_MAX;
	float fUvMinV = FLT_MAX;
	float fUvMaxU = -FLT_MAX;
	float fUvMaxV = -FLT_MAX;

	for ( uint32_t iVertex = 0u; iVertex < pVertexCount; ++iVertex ) {
		const uint32_t nGlobalVertex = pClusterVertexIndices[iVertex];
		if ( nGlobalVertex >= pVertexHeapCount ) {
			return false;
		}
		const t3dWorldVertex_t &Vertex = pVertexHeap[nGlobalVertex];
		for ( uint32_t iAxis = 0u; iAxis < 3u; ++iAxis ) {
			if ( Vertex.m_fPosition[iAxis] < vMin[iAxis] ) {
				vMin[iAxis] = Vertex.m_fPosition[iAxis];
			}
			if ( Vertex.m_fPosition[iAxis] > vMax[iAxis] ) {
				vMax[iAxis] = Vertex.m_fPosition[iAxis];
			}
		}
		float fUvU = 0.0f;
		float fUvV = 0.0f;
		t3d_world_geometry_unpack_vertex_uv( Vertex, fUvU, fUvV );
		if ( fUvU < fUvMinU ) {
			fUvMinU = fUvU;
		}
		if ( fUvV < fUvMinV ) {
			fUvMinV = fUvV;
		}
		if ( fUvU > fUvMaxU ) {
			fUvMaxU = fUvU;
		}
		if ( fUvV > fUvMaxV ) {
			fUvMaxV = fUvV;
		}
	}

	rDerived.vCenter[0] = ( vMin[0] + vMax[0] ) * 0.5f;
	rDerived.vCenter[1] = ( vMin[1] + vMax[1] ) * 0.5f;
	rDerived.vCenter[2] = ( vMin[2] + vMax[2] ) * 0.5f;
	rDerived.fRadius = 0.0f;

	for ( uint32_t iVertex = 0u; iVertex < pVertexCount; ++iVertex ) {
		const t3dWorldVertex_t &Vertex = pVertexHeap[pClusterVertexIndices[iVertex]];
		const float fDx = Vertex.m_fPosition[0] - rDerived.vCenter[0];
		const float fDy = Vertex.m_fPosition[1] - rDerived.vCenter[1];
		const float fDz = Vertex.m_fPosition[2] - rDerived.vCenter[2];
		const float fDistance = t3d_world_geometry_length3( fDx, fDy, fDz );
		if ( fDistance > rDerived.fRadius ) {
			rDerived.fRadius = fDistance;
		}
	}

	rDerived.nPackedUvMin = t3d_world_geometry_pack_half2( fUvMinU, fUvMinV );
	rDerived.nPackedUvMax = t3d_world_geometry_pack_half2( fUvMaxU, fUvMaxV );

	float vNormalSum[3] = { 0.0f, 0.0f, 0.0f };
	float fMaxDensity = 0.0f;
	uint32_t nValidNormalCount = 0u;

	for ( uint32_t iTriangle = 0u; iTriangle < pTriangleCount; ++iTriangle ) {
		uint32_t nLocal0 = 0u;
		uint32_t nLocal1 = 0u;
		uint32_t nLocal2 = 0u;
		t3d_world_geometry_unpack_local_triangle( pTriangles[iTriangle].nLocalIndices, nLocal0, nLocal1, nLocal2 );
		if ( nLocal0 >= pVertexCount || nLocal1 >= pVertexCount || nLocal2 >= pVertexCount ) {
			return false;
		}

		const t3dWorldVertex_t &V0 = pVertexHeap[pClusterVertexIndices[nLocal0]];
		const t3dWorldVertex_t &V1 = pVertexHeap[pClusterVertexIndices[nLocal1]];
		const t3dWorldVertex_t &V2 = pVertexHeap[pClusterVertexIndices[nLocal2]];

		const float fE10x = V1.m_fPosition[0] - V0.m_fPosition[0];
		const float fE10y = V1.m_fPosition[1] - V0.m_fPosition[1];
		const float fE10z = V1.m_fPosition[2] - V0.m_fPosition[2];
		const float fE20x = V2.m_fPosition[0] - V0.m_fPosition[0];
		const float fE20y = V2.m_fPosition[1] - V0.m_fPosition[1];
		const float fE20z = V2.m_fPosition[2] - V0.m_fPosition[2];
		float fNx = fE10y * fE20z - fE10z * fE20y;
		float fNy = fE10z * fE20x - fE10x * fE20z;
		float fNz = fE10x * fE20y - fE10y * fE20x;
		const float fNormalLength = t3d_world_geometry_length3( fNx, fNy, fNz );
		if ( fNormalLength > 1.0e-20f ) {
			fNx /= fNormalLength;
			fNy /= fNormalLength;
			fNz /= fNormalLength;
			vNormalSum[0] += fNx;
			vNormalSum[1] += fNy;
			vNormalSum[2] += fNz;
			++nValidNormalCount;
		}

		const float fDensity01 = t3d_world_geometry_edge_density( V0, V1 );
		const float fDensity12 = t3d_world_geometry_edge_density( V1, V2 );
		const float fDensity20 = t3d_world_geometry_edge_density( V2, V0 );
		if ( fDensity01 > fMaxDensity ) {
			fMaxDensity = fDensity01;
		}
		if ( fDensity12 > fMaxDensity ) {
			fMaxDensity = fDensity12;
		}
		if ( fDensity20 > fMaxDensity ) {
			fMaxDensity = fDensity20;
		}
	}

	const float fAxisLength = t3d_world_geometry_length3( vNormalSum[0], vNormalSum[1], vNormalSum[2] );
	if ( nValidNormalCount == 0u || fAxisLength <= 1.0e-20f ) {
		rDerived.vConeAxis[0] = 0.0f;
		rDerived.vConeAxis[1] = 0.0f;
		rDerived.vConeAxis[2] = 1.0f;
		rDerived.fConeCutoff = -1.0f;
	} else {
		rDerived.vConeAxis[0] = vNormalSum[0] / fAxisLength;
		rDerived.vConeAxis[1] = vNormalSum[1] / fAxisLength;
		rDerived.vConeAxis[2] = vNormalSum[2] / fAxisLength;
		rDerived.fConeCutoff = 1.0f;

		for ( uint32_t iTriangle = 0u; iTriangle < pTriangleCount; ++iTriangle ) {
			uint32_t nLocal0 = 0u;
			uint32_t nLocal1 = 0u;
			uint32_t nLocal2 = 0u;
			t3d_world_geometry_unpack_local_triangle( pTriangles[iTriangle].nLocalIndices, nLocal0, nLocal1, nLocal2 );
			const t3dWorldVertex_t &V0 = pVertexHeap[pClusterVertexIndices[nLocal0]];
			const t3dWorldVertex_t &V1 = pVertexHeap[pClusterVertexIndices[nLocal1]];
			const t3dWorldVertex_t &V2 = pVertexHeap[pClusterVertexIndices[nLocal2]];
			const float fE10x = V1.m_fPosition[0] - V0.m_fPosition[0];
			const float fE10y = V1.m_fPosition[1] - V0.m_fPosition[1];
			const float fE10z = V1.m_fPosition[2] - V0.m_fPosition[2];
			const float fE20x = V2.m_fPosition[0] - V0.m_fPosition[0];
			const float fE20y = V2.m_fPosition[1] - V0.m_fPosition[1];
			const float fE20z = V2.m_fPosition[2] - V0.m_fPosition[2];
			float fNx = fE10y * fE20z - fE10z * fE20y;
			float fNy = fE10z * fE20x - fE10x * fE20z;
			float fNz = fE10x * fE20y - fE10y * fE20x;
			const float fNormalLength = t3d_world_geometry_length3( fNx, fNy, fNz );
			if ( fNormalLength <= 1.0e-20f ) {
				continue;
			}
			fNx /= fNormalLength;
			fNy /= fNormalLength;
			fNz /= fNormalLength;
			const float fDot = rDerived.vConeAxis[0] * fNx + rDerived.vConeAxis[1] * fNy + rDerived.vConeAxis[2] * fNz;
			if ( fDot < rDerived.fConeCutoff ) {
				rDerived.fConeCutoff = fDot;
			}
		}
		rDerived.fConeCutoff = t3d_world_geometry_clamp( rDerived.fConeCutoff, -1.0f, 1.0f );
	}

	if ( !t3d_world_geometry_finite( fMaxDensity ) || fMaxDensity < 0.0f ) {
		return false;
	}
	rDerived.nTexelDensity = t3d_world_geometry_float_bits( fMaxDensity );
	return true;
}

static bool t3d_world_geometry_write_cluster( t3dWorldGeometryDatabase_t &rDatabase,
        const t3dWorldGeometrySourceMesh_t &pSource,
        const t3dWorldGeometryClusterBuilder_t &pBuilder,
        uint32_t pVertexBase,
        uint32_t pLodIndex,
        uint32_t &rClusterCursor,
        uint32_t &rVertexIndexCursor,
        uint32_t &rTriangleCursor ) {
	if ( pBuilder.nVertexCount == 0u || pBuilder.nTriangleCount == 0u ) {
		return false;
	}
	if ( pLodIndex > UINT16_MAX ) {
		return false;
	}

	for ( uint32_t iVertex = 0u; iVertex < pBuilder.nVertexCount; ++iVertex ) {
		uint32_t nGlobalVertex = 0u;
		if ( !t3d_world_geometry_u32_add( pVertexBase, pBuilder.aSourceVertex[iVertex], nGlobalVertex ) ) {
			return false;
		}
		rDatabase.Buffers.pClusterVertexIndexHeap[rVertexIndexCursor + iVertex] = nGlobalVertex;
	}

	for ( uint32_t iTriangle = 0u; iTriangle < pBuilder.nTriangleCount; ++iTriangle ) {
		rDatabase.Buffers.pTriangleHeap[rTriangleCursor + iTriangle] = pBuilder.aTriangles[iTriangle];
	}
	for ( uint32_t iTriangle = pBuilder.nTriangleCount; iTriangle < T3D_WORLD_CLUSTER_MAX_TRIANGLES; ++iTriangle ) {
		rDatabase.Buffers.pTriangleHeap[rTriangleCursor + iTriangle].nLocalIndices = 0u;
	}

	t3dWorldGeometryDerivedCluster_t Derived = {};
	if ( !t3d_world_geometry_compute_derived_cluster( rDatabase.Buffers.pVertexHeap,
	             pVertexBase + pSource.nVertexCount,
	             rDatabase.Buffers.pClusterVertexIndexHeap + rVertexIndexCursor,
	             pBuilder.nVertexCount,
	             rDatabase.Buffers.pTriangleHeap + rTriangleCursor,
	             pBuilder.nTriangleCount,
	             Derived ) ) {
		return false;
	}

	t3dWorldClusterRecord_t &Cluster = rDatabase.Buffers.pClusterRecords[rClusterCursor];
	memset( &Cluster, 0, sizeof( Cluster ) );
	Cluster.vCenter[0] = Derived.vCenter[0];
	Cluster.vCenter[1] = Derived.vCenter[1];
	Cluster.vCenter[2] = Derived.vCenter[2];
	Cluster.fRadius = Derived.fRadius;
	Cluster.vConeAxis[0] = Derived.vConeAxis[0];
	Cluster.vConeAxis[1] = Derived.vConeAxis[1];
	Cluster.vConeAxis[2] = Derived.vConeAxis[2];
	Cluster.fConeCutoff = Derived.fConeCutoff;
	Cluster.nVertexIndexOffset = rVertexIndexCursor;
	Cluster.nTriangleOffset = rTriangleCursor;
	Cluster.nVertexCount = ( uint16_t )pBuilder.nVertexCount;
	Cluster.nTriangleCount = ( uint16_t )pBuilder.nTriangleCount;
	Cluster.nLod = ( uint16_t )pLodIndex;
	Cluster.nTier = ( uint16_t )pBuilder.nTier;
	Cluster.nMaterialId = pBuilder.nMaterialId;
	Cluster.nPackedUvMin = Derived.nPackedUvMin;
	Cluster.nPackedUvMax = Derived.nPackedUvMax;
	Cluster.nTexelDensity = Derived.nTexelDensity;

	if ( rDatabase.Buffers.pWorldClusterRecords && rDatabase.Buffers.pWorldClusterVtMetadata ) {
		t3dWorldCluster_t &World = rDatabase.Buffers.pWorldClusterRecords[rClusterCursor];
		memset( &World, 0, sizeof( World ) );
		World.m_fCenter[0] = Derived.vCenter[0];
		World.m_fCenter[1] = Derived.vCenter[1];
		World.m_fCenter[2] = Derived.vCenter[2];
		World.m_fRadius = Derived.fRadius;
		World.m_fConeAxis[0] = Derived.vConeAxis[0];
		World.m_fConeAxis[1] = Derived.vConeAxis[1];
		World.m_fConeAxis[2] = Derived.vConeAxis[2];
		World.m_fConeCutoff = Derived.fConeCutoff;
		World.m_nVertexIndexOffset = rVertexIndexCursor;
		World.m_nTriangleOffset = rTriangleCursor;
		World.m_nPackedCounts = t3dWorldPackU16Pair( ( uint16_t )pBuilder.nVertexCount, ( uint16_t )pBuilder.nTriangleCount );
		World.m_nPackedLodGeometryKind = t3dWorldPackU16Pair( ( uint16_t )pLodIndex, ( uint16_t )T3D_WORLD_GEOMETRY_STATIC_CLUSTER );
		World.m_nMaterial = pBuilder.nMaterialId;
		World.m_nFlags = pSource.nFlags;
		World.m_nSourceId = rClusterCursor;

		t3dWorldClusterVTMetadata_t &WorldVt = rDatabase.Buffers.pWorldClusterVtMetadata[rClusterCursor];
		WorldVt.m_nPackedUvMin = Derived.nPackedUvMin;
		WorldVt.m_nPackedUvMax = Derived.nPackedUvMax;
		WorldVt.m_nTexelDensity = Derived.nTexelDensity;
		WorldVt.m_nReserved = 0u;
	}

	++rClusterCursor;
	rVertexIndexCursor += pBuilder.nVertexCount;
	rTriangleCursor += T3D_WORLD_CLUSTER_MAX_TRIANGLES;
	return true;
}

static bool t3d_world_geometry_write_lod( t3dWorldGeometryDatabase_t &rDatabase,
        const t3dWorldGeometrySourceMesh_t &pSource,
        const t3dWorldGeometrySourceLod_t &pLod,
        uint32_t pLodIndex,
        uint32_t pVertexBase,
        uint32_t &rLodCursor,
        uint32_t &rClusterCursor,
        uint32_t &rVertexIndexCursor,
        uint32_t &rTriangleCursor ) {
	const uint32_t nLodClusterOffset = rClusterCursor;
	t3dWorldGeometryClusterBuilder_t Builder = {};
	t3d_world_geometry_builder_reset( Builder );
	const uint32_t nTriangleCount = pLod.nIndexCount / 3u;

	for ( uint32_t iTriangle = 0u; iTriangle < nTriangleCount; ++iTriangle ) {
		const uint32_t nIndex = iTriangle * 3u;
		const uint32_t nVertex0 = pLod.pIndices[nIndex + 0u];
		const uint32_t nVertex1 = pLod.pIndices[nIndex + 1u];
		const uint32_t nVertex2 = pLod.pIndices[nIndex + 2u];
		uint32_t nMaterialId = 0u;
		uint32_t nTier = 0u;
		if ( !t3d_world_geometry_get_triangle_classification( pSource, pLod, iTriangle, nMaterialId, nTier ) ) {
			return false;
		}

		const uint32_t nNewVertices = t3d_world_geometry_builder_new_vertex_count( Builder, nVertex0, nVertex1, nVertex2 );
		const bool bClassChange = Builder.nTriangleCount != 0u && ( Builder.nMaterialId != nMaterialId || Builder.nTier != nTier );
		const bool bTriangleFull = Builder.nTriangleCount == T3D_WORLD_CLUSTER_MAX_TRIANGLES;
		const bool bVertexFull = Builder.nVertexCount + nNewVertices > T3D_WORLD_CLUSTER_MAX_VERTICES;

		if ( bClassChange || bTriangleFull || bVertexFull ) {
			if ( !t3d_world_geometry_write_cluster(
			             rDatabase, pSource, Builder, pVertexBase, pLodIndex, rClusterCursor, rVertexIndexCursor, rTriangleCursor ) ) {
				return false;
			}
			t3d_world_geometry_builder_reset( Builder );
		}

		if ( !t3d_world_geometry_builder_add_triangle( Builder, nVertex0, nVertex1, nVertex2, nMaterialId, nTier ) ) {
			return false;
		}
	}

	if ( Builder.nTriangleCount != 0u ) {
		if ( !t3d_world_geometry_write_cluster(
		             rDatabase, pSource, Builder, pVertexBase, pLodIndex, rClusterCursor, rVertexIndexCursor, rTriangleCursor ) ) {
			return false;
		}
	}

	t3dWorldMeshLodRecord_t &LodRecord = rDatabase.Buffers.pLodRecords[rLodCursor];
	memset( &LodRecord, 0, sizeof( LodRecord ) );
	LodRecord.nClusterOffset = nLodClusterOffset;
	LodRecord.nClusterCount = rClusterCursor - nLodClusterOffset;
	LodRecord.fMinDistance = pLod.fMinDistance;
	LodRecord.fMaxDistance = pLod.fMaxDistance;
	LodRecord.fScreenError = pLod.fScreenError;
	LodRecord.nFlags = pLod.nFlags;
	++rLodCursor;
	return true;
}

bool t3d_world_geometry_database_buffers_valid( const t3dWorldGeometryDatabaseBuffers_t &pBuffers ) {
	if ( pBuffers.pVertexHeap == NULL || pBuffers.nVertexCapacity == 0u ) {
		return false;
	}
	if ( pBuffers.pClusterVertexIndexHeap == NULL || pBuffers.nClusterVertexIndexCapacity == 0u ) {
		return false;
	}
	if ( pBuffers.pTriangleHeap == NULL || pBuffers.nTriangleCapacity == 0u ) {
		return false;
	}
	if ( pBuffers.pMeshRecords == NULL || pBuffers.nMeshCapacity == 0u ) {
		return false;
	}
	if ( pBuffers.pLodRecords == NULL || pBuffers.nLodCapacity == 0u ) {
		return false;
	}
	if ( pBuffers.pClusterRecords == NULL || pBuffers.nClusterCapacity == 0u ) {
		return false;
	}
	return true;
}

void t3d_world_geometry_database_initialize_cold( t3dWorldGeometryDatabase_t &rDatabase, const t3dWorldGeometryDatabaseBuffers_t &pBuffers ) {
	memset( &rDatabase, 0, sizeof( rDatabase ) );
	rDatabase.Buffers = pBuffers;
	rDatabase.nContentSignature = t3d_world_geometry_compute_signature_reference_debug( rDatabase );
}

t3dWorldGeometryCookResult_t t3d_world_geometry_append_mesh_cold( t3dWorldGeometryDatabase_t &rDatabase,
        const t3dWorldGeometrySourceMesh_t &pSource,
        t3dWorldGeometryCookedMeshSpan_t &rSpan,
        t3dWorldGeometryCookStats_t &rStats ) {
	memset( &rSpan, 0, sizeof( rSpan ) );
	memset( &rStats, 0, sizeof( rStats ) );
	if ( !t3d_world_geometry_database_buffers_valid( rDatabase.Buffers ) ) {
		return T3D_WORLD_GEOMETRY_COOK_INVALID_INPUT;
	}

	t3dWorldGeometryRequirements_t Requirements = {};
	const t3dWorldGeometryCookResult_t nPreflight = t3d_world_geometry_preflight_mesh( rDatabase, pSource, Requirements );
	if ( nPreflight != T3D_WORLD_GEOMETRY_COOK_OK ) {
		return nPreflight;
	}

	const uint32_t nVertexBase = rDatabase.nVertexCount;
	const uint32_t nMeshId = rDatabase.nMeshCount;
	const uint32_t nLodOffset = rDatabase.nLodCount;
	const uint32_t nClusterOffset = rDatabase.nClusterCount;
	uint32_t nVertexIndexCursor = rDatabase.nClusterVertexIndexCount;
	uint32_t nTriangleCursor = rDatabase.nTriangleCount;
	uint32_t nLodCursor = rDatabase.nLodCount;
	uint32_t nClusterCursor = rDatabase.nClusterCount;

	memmove(
	        rDatabase.Buffers.pVertexHeap + nVertexBase, pSource.pVertices, ( size_t )pSource.nVertexCount * sizeof( t3dWorldVertex_t ) );

	for ( uint32_t iLod = 0u; iLod < pSource.nLodCount; ++iLod ) {
		if ( !t3d_world_geometry_write_lod( rDatabase,
		             pSource,
		             pSource.pLods[iLod],
		             iLod,
		             nVertexBase,
		             nLodCursor,
		             nClusterCursor,
		             nVertexIndexCursor,
		             nTriangleCursor ) ) {
			return T3D_WORLD_GEOMETRY_COOK_INTERNAL_ERROR;
		}
	}

	t3dWorldMeshRecord_t &MeshRecord = rDatabase.Buffers.pMeshRecords[nMeshId];
	memset( &MeshRecord, 0, sizeof( MeshRecord ) );
	MeshRecord.nClusterOffset = nClusterOffset;
	MeshRecord.nClusterCount = nClusterCursor - nClusterOffset;
	MeshRecord.nLodOffset = nLodOffset;
	MeshRecord.nLodCount = pSource.nLodCount;
	MeshRecord.nMaterialBase = pSource.nMaterialBase;
	MeshRecord.nFlags = pSource.nFlags;

	rDatabase.nVertexCount = nVertexBase + pSource.nVertexCount;
	rDatabase.nClusterVertexIndexCount = nVertexIndexCursor;
	rDatabase.nTriangleCount = nTriangleCursor;
	rDatabase.nMeshCount = nMeshId + 1u;
	rDatabase.nLodCount = nLodCursor;
	rDatabase.nClusterCount = nClusterCursor;
	rDatabase.nContentSignature = t3d_world_geometry_compute_signature_reference_debug( rDatabase );

	rSpan.nMeshId = nMeshId;
	rSpan.nVertexOffset = nVertexBase;
	rSpan.nVertexCount = pSource.nVertexCount;
	rSpan.nLodOffset = nLodOffset;
	rSpan.nLodCount = pSource.nLodCount;
	rSpan.nClusterOffset = nClusterOffset;
	rSpan.nClusterCount = nClusterCursor - nClusterOffset;

	rStats.nSourceTriangleCount = Requirements.nSourceTriangleCount;
	rStats.nClusterTriangleSlotCount = Requirements.nTriangleSlotCount;
	rStats.nClusterVertexReferenceCount = Requirements.nClusterVertexReferenceCount;
	rStats.nClusterCount = Requirements.nClusterCount;
	rStats.nMinimumClusterTriangleCount = Requirements.nMinimumClusterTriangleCount;
	rStats.nMaximumClusterTriangleCount = Requirements.nMaximumClusterTriangleCount;
	return T3D_WORLD_GEOMETRY_COOK_OK;
}
