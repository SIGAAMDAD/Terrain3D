// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/world/geometry/terrain_3d_world_geometry_internal.h"

#include <math.h>
#include <string.h>
static uint64_t t3d_world_geometry_hash_byte( uint64_t pHash, uint8_t pByte ) {
	return ( pHash ^ ( uint64_t )pByte ) * 1099511628211ull;
}

static uint64_t t3d_world_geometry_hash_u32( uint64_t pHash, uint32_t pValue ) {
	pHash = t3d_world_geometry_hash_byte( pHash, ( uint8_t )( pValue & 0xffu ) );
	pHash = t3d_world_geometry_hash_byte( pHash, ( uint8_t )( ( pValue >> 8u ) & 0xffu ) );
	pHash = t3d_world_geometry_hash_byte( pHash, ( uint8_t )( ( pValue >> 16u ) & 0xffu ) );
	pHash = t3d_world_geometry_hash_byte( pHash, ( uint8_t )( ( pValue >> 24u ) & 0xffu ) );
	return pHash;
}

static uint64_t t3d_world_geometry_hash_u64( uint64_t pHash, uint64_t pValue ) {
	pHash = t3d_world_geometry_hash_u32( pHash, ( uint32_t )( pValue & 0xffffffffull ) );
	pHash = t3d_world_geometry_hash_u32( pHash, ( uint32_t )( pValue >> 32u ) );
	return pHash;
}

static uint64_t t3d_world_geometry_hash_float( uint64_t pHash, float pValue ) {
	return t3d_world_geometry_hash_u32( pHash, t3d_world_geometry_float_bits( pValue ) );
}

uint64_t t3d_world_geometry_compute_signature_reference_debug( const t3dWorldGeometryDatabase_t &pDatabase ) {
	uint64_t nHash = 1469598103934665603ull;
	nHash = t3d_world_geometry_hash_u32( nHash, 0x47443354u );
	nHash = t3d_world_geometry_hash_u32( nHash, T3D_WORLD_GEOMETRY_SIGNATURE_VERSION );
	nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.nVertexCount );
	nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.nClusterVertexIndexCount );
	nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.nTriangleCount );
	nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.nMeshCount );
	nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.nLodCount );
	nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.nClusterCount );

	for ( uint32_t i = 0u; i < pDatabase.nVertexCount; ++i ) {
		const t3dWorldVertex_t &V = pDatabase.Buffers.pVertexHeap[i];
		nHash = t3d_world_geometry_hash_float( nHash, V.m_fPosition[0] );
		nHash = t3d_world_geometry_hash_float( nHash, V.m_fPosition[1] );
		nHash = t3d_world_geometry_hash_float( nHash, V.m_fPosition[2] );
		nHash = t3d_world_geometry_hash_u32( nHash, V.m_nPackedNormal );
		nHash = t3d_world_geometry_hash_u32( nHash, V.m_nPackedTangent );
		nHash = t3d_world_geometry_hash_u32( nHash, V.m_nPackedUv0 );
		nHash = t3d_world_geometry_hash_u32( nHash, V.m_nPackedUv1 );
		nHash = t3d_world_geometry_hash_u32( nHash, V.m_nColorRgba8 );
	}

	for ( uint32_t i = 0u; i < pDatabase.nClusterVertexIndexCount; ++i ) {
		nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.Buffers.pClusterVertexIndexHeap[i] );
	}
	for ( uint32_t i = 0u; i < pDatabase.nTriangleCount; ++i ) {
		nHash = t3d_world_geometry_hash_u32( nHash, pDatabase.Buffers.pTriangleHeap[i].nLocalIndices );
	}
	for ( uint32_t i = 0u; i < pDatabase.nMeshCount; ++i ) {
		const t3dWorldMeshRecord_t &R = pDatabase.Buffers.pMeshRecords[i];
		nHash = t3d_world_geometry_hash_u32( nHash, R.nClusterOffset );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nClusterCount );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nLodOffset );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nLodCount );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nMaterialBase );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nFlags );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nReserved0 );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nReserved1 );
	}
	for ( uint32_t i = 0u; i < pDatabase.nLodCount; ++i ) {
		const t3dWorldMeshLodRecord_t &R = pDatabase.Buffers.pLodRecords[i];
		nHash = t3d_world_geometry_hash_u32( nHash, R.nClusterOffset );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nClusterCount );
		nHash = t3d_world_geometry_hash_float( nHash, R.fMinDistance );
		nHash = t3d_world_geometry_hash_float( nHash, R.fMaxDistance );
		nHash = t3d_world_geometry_hash_float( nHash, R.fScreenError );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nFlags );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nReserved0 );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nReserved1 );
	}
	for ( uint32_t i = 0u; i < pDatabase.nClusterCount; ++i ) {
		const t3dWorldClusterRecord_t &R = pDatabase.Buffers.pClusterRecords[i];
		nHash = t3d_world_geometry_hash_float( nHash, R.vCenter[0] );
		nHash = t3d_world_geometry_hash_float( nHash, R.vCenter[1] );
		nHash = t3d_world_geometry_hash_float( nHash, R.vCenter[2] );
		nHash = t3d_world_geometry_hash_float( nHash, R.fRadius );
		nHash = t3d_world_geometry_hash_float( nHash, R.vConeAxis[0] );
		nHash = t3d_world_geometry_hash_float( nHash, R.vConeAxis[1] );
		nHash = t3d_world_geometry_hash_float( nHash, R.vConeAxis[2] );
		nHash = t3d_world_geometry_hash_float( nHash, R.fConeCutoff );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nVertexIndexOffset );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nTriangleOffset );
		nHash = t3d_world_geometry_hash_u32( nHash, ( uint32_t )R.nVertexCount | ( ( uint32_t )R.nTriangleCount << 16u ) );
		nHash = t3d_world_geometry_hash_u32( nHash, ( uint32_t )R.nLod | ( ( uint32_t )R.nTier << 16u ) );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nMaterialId );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nPackedUvMin );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nPackedUvMax );
		nHash = t3d_world_geometry_hash_u32( nHash, R.nTexelDensity );
	}

	return t3d_world_geometry_hash_u64( nHash, 0x4d30322d5445524dull );
}

bool t3d_world_geometry_reconstruct_triangle_reference_debug( const t3dWorldGeometryDatabase_t &pDatabase,
        uint32_t pClusterId,
        uint32_t pTriangleIndex,
        uint32_t &rGlobalVertex0,
        uint32_t &rGlobalVertex1,
        uint32_t &rGlobalVertex2 ) {
	if ( pClusterId >= pDatabase.nClusterCount ) {
		return false;
	}
	const t3dWorldClusterRecord_t &Cluster = pDatabase.Buffers.pClusterRecords[pClusterId];
	if ( pTriangleIndex >= Cluster.nTriangleCount ) {
		return false;
	}
	if ( Cluster.nVertexIndexOffset > pDatabase.nClusterVertexIndexCount ) {
		return false;
	}
	if ( ( uint64_t )Cluster.nVertexIndexOffset + Cluster.nVertexCount > pDatabase.nClusterVertexIndexCount ) {
		return false;
	}
	if ( Cluster.nTriangleOffset > pDatabase.nTriangleCount ) {
		return false;
	}
	if ( ( uint64_t )Cluster.nTriangleOffset + T3D_WORLD_CLUSTER_MAX_TRIANGLES > pDatabase.nTriangleCount ) {
		return false;
	}

	uint32_t nLocal0 = 0u;
	uint32_t nLocal1 = 0u;
	uint32_t nLocal2 = 0u;
	t3d_world_geometry_unpack_local_triangle(
	        pDatabase.Buffers.pTriangleHeap[Cluster.nTriangleOffset + pTriangleIndex].nLocalIndices, nLocal0, nLocal1, nLocal2 );
	if ( nLocal0 >= Cluster.nVertexCount || nLocal1 >= Cluster.nVertexCount || nLocal2 >= Cluster.nVertexCount ) {
		return false;
	}

	rGlobalVertex0 = pDatabase.Buffers.pClusterVertexIndexHeap[Cluster.nVertexIndexOffset + nLocal0];
	rGlobalVertex1 = pDatabase.Buffers.pClusterVertexIndexHeap[Cluster.nVertexIndexOffset + nLocal1];
	rGlobalVertex2 = pDatabase.Buffers.pClusterVertexIndexHeap[Cluster.nVertexIndexOffset + nLocal2];
	return rGlobalVertex0 < pDatabase.nVertexCount && rGlobalVertex1 < pDatabase.nVertexCount && rGlobalVertex2 < pDatabase.nVertexCount;
}

static bool t3d_world_geometry_nearly_equal( float pA, float pB, float pTolerance ) {
	const float fDiff = fabsf( pA - pB );
	const float fScaleA = fabsf( pA );
	const float fScaleB = fabsf( pB );
	float fScale = fScaleA > fScaleB ? fScaleA : fScaleB;
	if ( fScale < 1.0f ) {
		fScale = 1.0f;
	}
	return fDiff <= pTolerance * fScale;
}

bool t3d_world_geometry_validate_database_reference_debug( const t3dWorldGeometryDatabase_t &pDatabase ) {
	if ( !t3d_world_geometry_database_buffers_valid( pDatabase.Buffers ) ) {
		return false;
	}
	if ( pDatabase.nVertexCount > pDatabase.Buffers.nVertexCapacity ) {
		return false;
	}
	if ( pDatabase.nClusterVertexIndexCount > pDatabase.Buffers.nClusterVertexIndexCapacity ) {
		return false;
	}
	if ( pDatabase.nTriangleCount > pDatabase.Buffers.nTriangleCapacity ) {
		return false;
	}
	if ( pDatabase.nMeshCount > pDatabase.Buffers.nMeshCapacity ) {
		return false;
	}
	if ( pDatabase.nLodCount > pDatabase.Buffers.nLodCapacity ) {
		return false;
	}
	if ( pDatabase.nClusterCount > pDatabase.Buffers.nClusterCapacity ) {
		return false;
	}
	if ( pDatabase.nContentSignature != t3d_world_geometry_compute_signature_reference_debug( pDatabase ) ) {
		return false;
	}

	uint32_t nExpectedMeshCluster = 0u;
	uint32_t nExpectedMeshLod = 0u;
	for ( uint32_t iMesh = 0u; iMesh < pDatabase.nMeshCount; ++iMesh ) {
		const t3dWorldMeshRecord_t &Mesh = pDatabase.Buffers.pMeshRecords[iMesh];
		if ( Mesh.nClusterOffset != nExpectedMeshCluster ) {
			return false;
		}
		if ( Mesh.nLodOffset != nExpectedMeshLod ) {
			return false;
		}
		if ( ( uint64_t )Mesh.nClusterOffset + Mesh.nClusterCount > pDatabase.nClusterCount ) {
			return false;
		}
		if ( ( uint64_t )Mesh.nLodOffset + Mesh.nLodCount > pDatabase.nLodCount ) {
			return false;
		}
		uint32_t nExpectedCluster = Mesh.nClusterOffset;
		uint64_t nLodClusterCount = 0u;
		for ( uint32_t iLod = 0u; iLod < Mesh.nLodCount; ++iLod ) {
			const t3dWorldMeshLodRecord_t &Lod = pDatabase.Buffers.pLodRecords[Mesh.nLodOffset + iLod];
			if ( Lod.nClusterOffset != nExpectedCluster ) {
				return false;
			}
			for ( uint32_t iCluster = 0u; iCluster < Lod.nClusterCount; ++iCluster ) {
				if ( pDatabase.Buffers.pClusterRecords[Lod.nClusterOffset + iCluster].nLod != iLod ) {
					return false;
				}
			}
			if ( ( uint64_t )Lod.nClusterOffset + Lod.nClusterCount > pDatabase.nClusterCount ) {
				return false;
			}
			if ( !t3d_world_geometry_finite( Lod.fMinDistance ) || !t3d_world_geometry_finite( Lod.fMaxDistance )
			        || !t3d_world_geometry_finite( Lod.fScreenError ) ) {
				return false;
			}
			if ( Lod.fMinDistance < 0.0f || Lod.fMaxDistance < Lod.fMinDistance || Lod.fScreenError < 0.0f ) {
				return false;
			}
			nExpectedCluster += Lod.nClusterCount;
			nLodClusterCount += Lod.nClusterCount;
		}
		if ( nLodClusterCount != Mesh.nClusterCount ) {
			return false;
		}
		nExpectedMeshCluster += Mesh.nClusterCount;
		nExpectedMeshLod += Mesh.nLodCount;
	}

	if ( nExpectedMeshCluster != pDatabase.nClusterCount ) {
		return false;
	}
	if ( nExpectedMeshLod != pDatabase.nLodCount ) {
		return false;
	}

	uint32_t nExpectedVertexIndexOffset = 0u;
	uint32_t nExpectedTriangleOffset = 0u;
	for ( uint32_t iCluster = 0u; iCluster < pDatabase.nClusterCount; ++iCluster ) {
		const t3dWorldClusterRecord_t &Cluster = pDatabase.Buffers.pClusterRecords[iCluster];
		if ( Cluster.nVertexIndexOffset != nExpectedVertexIndexOffset ) {
			return false;
		}
		if ( Cluster.nTriangleOffset != nExpectedTriangleOffset ) {
			return false;
		}
		if ( Cluster.nVertexCount == 0u || Cluster.nVertexCount > T3D_WORLD_CLUSTER_MAX_VERTICES ) {
			return false;
		}
		if ( Cluster.nTriangleCount == 0u || Cluster.nTriangleCount > T3D_WORLD_CLUSTER_MAX_TRIANGLES ) {
			return false;
		}
		if ( Cluster.nTier >= T3D_WORLD_TIER_COUNT ) {
			return false;
		}
		if ( ( uint64_t )Cluster.nVertexIndexOffset + Cluster.nVertexCount > pDatabase.nClusterVertexIndexCount ) {
			return false;
		}
		if ( ( uint64_t )Cluster.nTriangleOffset + T3D_WORLD_CLUSTER_MAX_TRIANGLES > pDatabase.nTriangleCount ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Cluster.vCenter[0] ) || !t3d_world_geometry_finite( Cluster.vCenter[1] )
		        || !t3d_world_geometry_finite( Cluster.vCenter[2] ) ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Cluster.fRadius ) || Cluster.fRadius < 0.0f ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Cluster.vConeAxis[0] ) || !t3d_world_geometry_finite( Cluster.vConeAxis[1] )
		        || !t3d_world_geometry_finite( Cluster.vConeAxis[2] ) ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( Cluster.fConeCutoff ) || Cluster.fConeCutoff < -1.0f || Cluster.fConeCutoff > 1.0f ) {
			return false;
		}
		if ( !t3d_world_geometry_finite( t3d_world_geometry_unpack_texel_density( Cluster.nTexelDensity ) ) ) {
			return false;
		}

		for ( uint32_t iTail = Cluster.nTriangleCount; iTail < T3D_WORLD_CLUSTER_MAX_TRIANGLES; ++iTail ) {
			if ( pDatabase.Buffers.pTriangleHeap[Cluster.nTriangleOffset + iTail].nLocalIndices != 0u ) {
				return false;
			}
		}

		t3dWorldGeometryDerivedCluster_t Derived = {};
		if ( !t3d_world_geometry_compute_derived_cluster( pDatabase.Buffers.pVertexHeap,
		             pDatabase.nVertexCount,
		             pDatabase.Buffers.pClusterVertexIndexHeap + Cluster.nVertexIndexOffset,
		             Cluster.nVertexCount,
		             pDatabase.Buffers.pTriangleHeap + Cluster.nTriangleOffset,
		             Cluster.nTriangleCount,
		             Derived ) ) {
			return false;
		}

		if ( !t3d_world_geometry_nearly_equal( Cluster.vCenter[0], Derived.vCenter[0], 1.0e-6f ) ) {
			return false;
		}
		if ( !t3d_world_geometry_nearly_equal( Cluster.vCenter[1], Derived.vCenter[1], 1.0e-6f ) ) {
			return false;
		}
		if ( !t3d_world_geometry_nearly_equal( Cluster.vCenter[2], Derived.vCenter[2], 1.0e-6f ) ) {
			return false;
		}
		if ( !t3d_world_geometry_nearly_equal( Cluster.fRadius, Derived.fRadius, 1.0e-6f ) ) {
			return false;
		}
		if ( !t3d_world_geometry_nearly_equal( Cluster.vConeAxis[0], Derived.vConeAxis[0], 1.0e-5f ) ) {
			return false;
		}
		if ( !t3d_world_geometry_nearly_equal( Cluster.vConeAxis[1], Derived.vConeAxis[1], 1.0e-5f ) ) {
			return false;
		}
		if ( !t3d_world_geometry_nearly_equal( Cluster.vConeAxis[2], Derived.vConeAxis[2], 1.0e-5f ) ) {
			return false;
		}
		if ( !t3d_world_geometry_nearly_equal( Cluster.fConeCutoff, Derived.fConeCutoff, 1.0e-5f ) ) {
			return false;
		}
		if ( Cluster.nPackedUvMin != Derived.nPackedUvMin || Cluster.nPackedUvMax != Derived.nPackedUvMax ) {
			return false;
		}
		if ( Cluster.nTexelDensity != Derived.nTexelDensity ) {
			return false;
		}

		const float fRadiusTolerance = Cluster.fRadius * 1.0e-5f + 1.0e-5f;
		for ( uint32_t iVertex = 0u; iVertex < Cluster.nVertexCount; ++iVertex ) {
			const uint32_t nGlobal = pDatabase.Buffers.pClusterVertexIndexHeap[Cluster.nVertexIndexOffset + iVertex];
			if ( nGlobal >= pDatabase.nVertexCount ) {
				return false;
			}
			const t3dWorldVertex_t &Vertex = pDatabase.Buffers.pVertexHeap[nGlobal];
			const float fDx = Vertex.m_fPosition[0] - Cluster.vCenter[0];
			const float fDy = Vertex.m_fPosition[1] - Cluster.vCenter[1];
			const float fDz = Vertex.m_fPosition[2] - Cluster.vCenter[2];
			if ( t3d_world_geometry_length3( fDx, fDy, fDz ) > Cluster.fRadius + fRadiusTolerance ) {
				return false;
			}
		}
		nExpectedVertexIndexOffset += Cluster.nVertexCount;
		nExpectedTriangleOffset += T3D_WORLD_CLUSTER_MAX_TRIANGLES;
	}
	if ( nExpectedVertexIndexOffset != pDatabase.nClusterVertexIndexCount ) {
		return false;
	}
	if ( nExpectedTriangleOffset != pDatabase.nTriangleCount ) {
		return false;
	}

	return true;
}

bool t3d_world_geometry_validate_source_reconstruction_reference_debug(
        const t3dWorldGeometryDatabase_t &pDatabase, const t3dWorldGeometrySourceMesh_t &pSource, const t3dWorldGeometryCookedMeshSpan_t &pSpan ) {
	if ( !t3d_world_geometry_validate_source( pSource ) ) {
		return false;
	}
	if ( pSpan.nMeshId >= pDatabase.nMeshCount ) {
		return false;
	}
	if ( pSpan.nVertexCount != pSource.nVertexCount || pSpan.nLodCount != pSource.nLodCount ) {
		return false;
	}
	if ( ( uint64_t )pSpan.nVertexOffset + pSpan.nVertexCount > pDatabase.nVertexCount ) {
		return false;
	}
	if ( ( uint64_t )pSpan.nLodOffset + pSpan.nLodCount > pDatabase.nLodCount ) {
		return false;
	}
	if ( ( uint64_t )pSpan.nClusterOffset + pSpan.nClusterCount > pDatabase.nClusterCount ) {
		return false;
	}

	const t3dWorldMeshRecord_t &Mesh = pDatabase.Buffers.pMeshRecords[pSpan.nMeshId];
	if ( Mesh.nClusterOffset != pSpan.nClusterOffset || Mesh.nClusterCount != pSpan.nClusterCount ) {
		return false;
	}
	if ( Mesh.nLodOffset != pSpan.nLodOffset || Mesh.nLodCount != pSpan.nLodCount ) {
		return false;
	}
	if ( Mesh.nMaterialBase != pSource.nMaterialBase || Mesh.nFlags != pSource.nFlags ) {
		return false;
	}

	for ( uint32_t iVertex = 0u; iVertex < pSource.nVertexCount; ++iVertex ) {
		if ( memcmp( &pDatabase.Buffers.pVertexHeap[pSpan.nVertexOffset + iVertex],
		             &pSource.pVertices[iVertex],
		             sizeof( t3dWorldVertex_t ) )
		        != 0 ) {
			return false;
		}
	}

	for ( uint32_t iLod = 0u; iLod < pSource.nLodCount; ++iLod ) {
		const t3dWorldGeometrySourceLod_t &SourceLod = pSource.pLods[iLod];
		const t3dWorldMeshLodRecord_t &Lod = pDatabase.Buffers.pLodRecords[pSpan.nLodOffset + iLod];
		uint32_t nSourceTriangle = 0u;
		const uint32_t nSourceTriangleCount = SourceLod.nIndexCount / 3u;

		for ( uint32_t iCluster = 0u; iCluster < Lod.nClusterCount; ++iCluster ) {
			const uint32_t nClusterId = Lod.nClusterOffset + iCluster;
			const t3dWorldClusterRecord_t &Cluster = pDatabase.Buffers.pClusterRecords[nClusterId];
			if ( Cluster.nLod != iLod ) {
				return false;
			}
			for ( uint32_t iTriangle = 0u; iTriangle < Cluster.nTriangleCount; ++iTriangle ) {
				if ( nSourceTriangle >= nSourceTriangleCount ) {
					return false;
				}
				uint32_t nGlobal0 = 0u;
				uint32_t nGlobal1 = 0u;
				uint32_t nGlobal2 = 0u;
				if ( !t3d_world_geometry_reconstruct_triangle_reference_debug(
				             pDatabase, nClusterId, iTriangle, nGlobal0, nGlobal1, nGlobal2 ) ) {
					return false;
				}
				const uint32_t nSourceIndex = nSourceTriangle * 3u;
				if ( nGlobal0 != pSpan.nVertexOffset + SourceLod.pIndices[nSourceIndex + 0u] ) {
					return false;
				}
				if ( nGlobal1 != pSpan.nVertexOffset + SourceLod.pIndices[nSourceIndex + 1u] ) {
					return false;
				}
				if ( nGlobal2 != pSpan.nVertexOffset + SourceLod.pIndices[nSourceIndex + 2u] ) {
					return false;
				}
				uint32_t nExpectedMaterial = 0u;
				uint32_t nExpectedTier = 0u;
				if ( !t3d_world_geometry_get_triangle_classification(
				             pSource, SourceLod, nSourceTriangle, nExpectedMaterial, nExpectedTier ) ) {
					return false;
				}
				if ( Cluster.nMaterialId != nExpectedMaterial || Cluster.nTier != nExpectedTier ) {
					return false;
				}
				++nSourceTriangle;
			}
		}

		if ( nSourceTriangle != nSourceTriangleCount ) {
			return false;
		}
	}

	return true;
}
