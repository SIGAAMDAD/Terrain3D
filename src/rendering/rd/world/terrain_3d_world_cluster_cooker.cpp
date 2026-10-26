// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/terrain_3d_world_cluster_cooker.h"

#include <math.h>
#include <string.h>

static uint32_t t3dWorldPackTriangle( const uint32_t a, const uint32_t b, const uint32_t c ) {
	return ( a & 63u ) | ( ( b & 63u ) << 6u ) | ( ( c & 63u ) << 12u );
}

static float t3dWorldHalfToFloat( const uint16_t h ) {
	const uint32_t sign = ( uint32_t )( h & 0x8000u ) << 16u;
	int32_t exponent = ( int32_t )( ( h >> 10u ) & 0x1fu );
	uint32_t mantissa = h & 0x03ffu;
	uint32_t bits = 0u;
	if ( exponent == 0 ) {
		if ( mantissa == 0u ) {
			bits = sign;
		} else {
			exponent = 1;
			while ( ( mantissa & 0x0400u ) == 0u ) {
				mantissa <<= 1u;
				--exponent;
			}
			mantissa &= 0x03ffu;
			bits = sign | ( ( uint32_t )( exponent + 112 ) << 23u ) | ( mantissa << 13u );
		}
	} else if ( exponent == 31 ) {
		bits = sign | 0x7f800000u | ( mantissa << 13u );
	} else {
		bits = sign | ( ( uint32_t )( exponent + 112 ) << 23u ) | ( mantissa << 13u );
	}
	float value = 0.0f;
	memcpy( &value, &bits, sizeof( value ) );
	return value;
}

static uint16_t t3dWorldFloatToHalf( const float value ) {
	uint32_t bits = 0u;
	memcpy( &bits, &value, sizeof( bits ) );
	const uint32_t sign = ( bits >> 16u ) & 0x8000u;
	int32_t exponent = ( int32_t )( ( bits >> 23u ) & 0xffu ) - 127 + 15;
	uint32_t mantissa = bits & 0x007fffffu;
	if ( exponent <= 0 ) {
		if ( exponent < -10 ) {
			return ( uint16_t )sign;
		}
		mantissa = ( mantissa | 0x00800000u ) >> ( uint32_t )( 1 - exponent );
		return ( uint16_t )( sign | ( ( mantissa + 0x00001000u ) >> 13u ) );
	}
	if ( exponent >= 31 ) {
		return ( uint16_t )( sign | 0x7c00u );
	}
	return ( uint16_t )( sign | ( ( uint32_t )exponent << 10u ) | ( ( mantissa + 0x00001000u ) >> 13u ) );
}

static int t3dWorldFindLocalVertex( const uint32_t *pVertices, const uint32_t nCount, const uint32_t nVertex ) {
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		if ( pVertices[i] == nVertex ) {
			return ( int )i;
		}
	}
	return -1;
}

static void t3dWorldComputeClusterDerived( const t3dWorldClusterCookSource_t &rSource,
        const uint32_t *pLocalVertices,
        const uint32_t nVertexCount,
        const uint32_t *pTriangles,
        const uint32_t nTriangleCount,
        t3dWorldCluster_t &rCluster,
        t3dWorldClusterVtMetadata_t &rVt ) {
	float center[3] = { 0.0f, 0.0f, 0.0f };
	float uvMin[2] = { 1.0e30f, 1.0e30f };
	float uvMax[2] = { -1.0e30f, -1.0e30f };
	for ( uint32_t i = 0u; i < nVertexCount; ++i ) {
		const t3dWorldVertex_t &v = rSource.m_pVertices[pLocalVertices[i]];
		center[0] += v.m_fPosition[0];
		center[1] += v.m_fPosition[1];
		center[2] += v.m_fPosition[2];
		const float u = t3dWorldHalfToFloat( ( uint16_t )( v.m_nPackedUv0 & 0xffffu ) );
		const float w = t3dWorldHalfToFloat( ( uint16_t )( v.m_nPackedUv0 >> 16u ) );
		if ( u < uvMin[0] ) {
			uvMin[0] = u;
		}
		if ( w < uvMin[1] ) {
			uvMin[1] = w;
		}
		if ( u > uvMax[0] ) {
			uvMax[0] = u;
		}
		if ( w > uvMax[1] ) {
			uvMax[1] = w;
		}
	}
	const float invCount = nVertexCount ? 1.0f / ( float )nVertexCount : 0.0f;
	center[0] *= invCount;
	center[1] *= invCount;
	center[2] *= invCount;
	float radius2 = 0.0f;
	for ( uint32_t i = 0u; i < nVertexCount; ++i ) {
		const t3dWorldVertex_t &v = rSource.m_pVertices[pLocalVertices[i]];
		const float x = v.m_fPosition[0] - center[0], y = v.m_fPosition[1] - center[1], z = v.m_fPosition[2] - center[2];
		const float d2 = x * x + y * y + z * z;
		if ( d2 > radius2 ) {
			radius2 = d2;
		}
	}
	float axis[3] = { 0.0f, 0.0f, 0.0f };
	for ( uint32_t t = 0u; t < nTriangleCount; ++t ) {
		const uint32_t packed = pTriangles[t];
		const t3dWorldVertex_t &a = rSource.m_pVertices[pLocalVertices[( packed >> 0u ) & 63u]];
		const t3dWorldVertex_t &b = rSource.m_pVertices[pLocalVertices[( packed >> 6u ) & 63u]];
		const t3dWorldVertex_t &c = rSource.m_pVertices[pLocalVertices[( packed >> 12u ) & 63u]];
		const float abx = b.m_fPosition[0] - a.m_fPosition[0], aby = b.m_fPosition[1] - a.m_fPosition[1],
		            abz = b.m_fPosition[2] - a.m_fPosition[2];
		const float acx = c.m_fPosition[0] - a.m_fPosition[0], acy = c.m_fPosition[1] - a.m_fPosition[1],
		            acz = c.m_fPosition[2] - a.m_fPosition[2];
		float nx = aby * acz - abz * acy, ny = abz * acx - abx * acz, nz = abx * acy - aby * acx;
		const float len = sqrtf( nx * nx + ny * ny + nz * nz );
		if ( len > 1.0e-8f ) {
			axis[0] += nx / len;
			axis[1] += ny / len;
			axis[2] += nz / len;
		}
	}
	float axisLen = sqrtf( axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] );
	if ( axisLen > 1.0e-8f ) {
		axis[0] /= axisLen;
		axis[1] /= axisLen;
		axis[2] /= axisLen;
	} else {
		axis[1] = 1.0f;
	}
	float cutoff = 1.0f;
	for ( uint32_t t = 0u; t < nTriangleCount; ++t ) {
		const uint32_t packed = pTriangles[t];
		const t3dWorldVertex_t &a = rSource.m_pVertices[pLocalVertices[( packed >> 0u ) & 63u]];
		const t3dWorldVertex_t &b = rSource.m_pVertices[pLocalVertices[( packed >> 6u ) & 63u]];
		const t3dWorldVertex_t &c = rSource.m_pVertices[pLocalVertices[( packed >> 12u ) & 63u]];
		const float abx = b.m_fPosition[0] - a.m_fPosition[0], aby = b.m_fPosition[1] - a.m_fPosition[1],
		            abz = b.m_fPosition[2] - a.m_fPosition[2];
		const float acx = c.m_fPosition[0] - a.m_fPosition[0], acy = c.m_fPosition[1] - a.m_fPosition[1],
		            acz = c.m_fPosition[2] - a.m_fPosition[2];
		float nx = aby * acz - abz * acy, ny = abz * acx - abx * acz, nz = abx * acy - aby * acx;
		const float len = sqrtf( nx * nx + ny * ny + nz * nz );
		if ( len > 1.0e-8f ) {
			const float d = ( nx * axis[0] + ny * axis[1] + nz * axis[2] ) / len;
			if ( d < cutoff ) {
				cutoff = d;
			}
		}
	}
	rCluster.m_fCenter[0] = center[0];
	rCluster.m_fCenter[1] = center[1];
	rCluster.m_fCenter[2] = center[2];
	rCluster.m_fRadius = sqrtf( radius2 );
	rCluster.m_fConeAxis[0] = axis[0];
	rCluster.m_fConeAxis[1] = axis[1];
	rCluster.m_fConeAxis[2] = axis[2];
	rCluster.m_fConeCutoff = cutoff;
	rVt.m_nPackedUvMin = ( uint32_t )t3dWorldFloatToHalf( uvMin[0] ) | ( ( uint32_t )t3dWorldFloatToHalf( uvMin[1] ) << 16u );
	rVt.m_nPackedUvMax = ( uint32_t )t3dWorldFloatToHalf( uvMax[0] ) | ( ( uint32_t )t3dWorldFloatToHalf( uvMax[1] ) << 16u );
	const float du = uvMax[0] - uvMin[0], dv = uvMax[1] - uvMin[1];
	const float uvSpan = sqrtf( du * du + dv * dv );
	rVt.m_nTexelDensity = ( uint32_t )( uvSpan * 65536.0f + 0.5f );
	rVt.m_nReserved = 0u;
}


static bool t3dWorldFlushClusterCold( const t3dWorldClusterCookSource_t &rSource,
        t3dWorldClusterCookBuffers_t &rBuffers,
        t3dWorldClusterCookStats_t &rStats,
        const uint32_t *const pLocalVertices,
        uint32_t &rLocalCount,
        const uint32_t *const pTriangles,
        uint32_t &rTriangleCount ) {
	if ( rTriangleCount == 0u ) {
		return true;
	}
	if ( rBuffers.m_nClusterCount >= rBuffers.m_nClusterCapacity
	        || rBuffers.m_nClusterVertexIndexCount + rLocalCount > rBuffers.m_nClusterVertexIndexCapacity
	        || rBuffers.m_nTriangleWordCount + T3D_WORLD_CLUSTER_MAX_TRIANGLES > rBuffers.m_nTriangleWordCapacity ) {
		return false;
	}
	t3dWorldCluster_t &cluster = rBuffers.m_pClusters[rBuffers.m_nClusterCount];
	t3dWorldClusterVtMetadata_t &vt = rBuffers.m_pVtMetadata[rBuffers.m_nClusterCount];
	memset( &cluster, 0, sizeof( cluster ) );
	cluster.m_nVertexIndexOffset = rBuffers.m_nClusterVertexIndexCount;
	cluster.m_nTriangleOffset = rBuffers.m_nTriangleWordCount;
	cluster.m_nPackedCounts = t3dWorldPackU16Pair( ( uint16_t )rLocalCount, ( uint16_t )rTriangleCount );
	cluster.m_nPackedLodGeometryKind = t3dWorldPackU16Pair( ( uint16_t )rSource.m_nLod, ( uint16_t )rSource.m_nGeometryKind );
	cluster.m_nMaterial = rSource.m_nMaterial;
	cluster.m_nFlags = rSource.m_nClusterFlags;
	cluster.m_nSourceId = rSource.m_nSourceId;
	for ( uint32_t i = 0u; i < rLocalCount; ++i ) {
		rBuffers.m_pClusterVertexIndices[rBuffers.m_nClusterVertexIndexCount + i] = rSource.m_nVertexBase + pLocalVertices[i];
	}
	for ( uint32_t i = 0u; i < rTriangleCount; ++i ) {
		rBuffers.m_pTriangleWords[rBuffers.m_nTriangleWordCount + i] = pTriangles[i];
	}
	for ( uint32_t i = rTriangleCount; i < T3D_WORLD_CLUSTER_MAX_TRIANGLES; ++i ) {
		rBuffers.m_pTriangleWords[rBuffers.m_nTriangleWordCount + i] = 0u;
	}
	t3dWorldComputeClusterDerived( rSource, pLocalVertices, rLocalCount, pTriangles, rTriangleCount, cluster, vt );
	rBuffers.m_nClusterVertexIndexCount += rLocalCount;
	rBuffers.m_nTriangleWordCount += T3D_WORLD_CLUSTER_MAX_TRIANGLES;
	++rBuffers.m_nClusterCount;
	++rStats.m_nClusterCount;
	rStats.m_nRasterTriangleCount += rTriangleCount;
	rStats.m_nPaddedTriangleSlots += T3D_WORLD_CLUSTER_MAX_TRIANGLES - rTriangleCount;
	rLocalCount = 0u;
	rTriangleCount = 0u;
	return true;
}

bool t3dWorldCookMeshClustersCold( const t3dWorldClusterCookSource_t &rSource,
        t3dWorldClusterCookBuffers_t &rBuffers,
        t3dWorldMeshClusterSpan_t &rSpan,
        t3dWorldClusterCookStats_t &rStats ) {
	memset( &rSpan, 0, sizeof( rSpan ) );
	memset( &rStats, 0, sizeof( rStats ) );
	if ( !rSource.m_pVertices || !rSource.m_pIndices || rSource.m_nVertexCount == 0u || rSource.m_nIndexCount == 0u
	        || ( rSource.m_nIndexCount % 3u ) != 0u ) {
		return false;
	}
	if ( !rBuffers.m_pClusterVertexIndices || !rBuffers.m_pTriangleWords || !rBuffers.m_pClusters || !rBuffers.m_pVtMetadata ) {
		return false;
	}
	if ( rSource.m_nLod > 0xffffu || rSource.m_nGeometryKind >= T3D_WORLD_GEOMETRY_KIND_COUNT ) {
		return false;
	}
	rSpan.m_nVertexOffset = rSource.m_nVertexBase;
	rSpan.m_nVertexCount = rSource.m_nVertexCount;
	rSpan.m_nClusterOffset = rBuffers.m_nClusterCount;
	uint32_t localVertices[64];
	uint32_t triangles[124];
	uint32_t localCount = 0u, triCount = 0u;
	const bool twoSided = ( rSource.m_nCookFlags & T3D_WORLD_CLUSTER_COOK_TWO_SIDED ) != 0u;
	const uint32_t sourceTriangles = rSource.m_nIndexCount / 3u;
	rStats.m_nSourceTriangleCount = sourceTriangles;
	for ( uint32_t sourceTri = 0u; sourceTri < sourceTriangles; ++sourceTri ) {
		uint32_t source[3] = {
			rSource.m_pIndices[sourceTri * 3u], rSource.m_pIndices[sourceTri * 3u + 1u], rSource.m_pIndices[sourceTri * 3u + 2u]
		};
		for ( uint32_t i = 0u; i < 3u; ++i ) {
			if ( source[i] >= rSource.m_nVertexCount ) {
				return false;
			}
		}
		const uint32_t windingCount = twoSided ? 2u : 1u;
		for ( uint32_t winding = 0u; winding < windingCount; ++winding ) {
			uint32_t v[3] = { source[0], winding ? source[2] : source[1], winding ? source[1] : source[2] };
			uint32_t newVertices = 0u;
			for ( uint32_t i = 0u; i < 3u; ++i ) {
				if ( t3dWorldFindLocalVertex( localVertices, localCount, v[i] ) < 0 ) {
					++newVertices;
				}
			}
			if ( triCount == T3D_WORLD_CLUSTER_MAX_TRIANGLES || localCount + newVertices > T3D_WORLD_CLUSTER_MAX_VERTICES ) {
				if ( !t3dWorldFlushClusterCold( rSource, rBuffers, rStats, localVertices, localCount, triangles, triCount ) ) {
					return false;
				}
			}
			uint32_t local[3];
			for ( uint32_t i = 0u; i < 3u; ++i ) {
				int found = t3dWorldFindLocalVertex( localVertices, localCount, v[i] );
				if ( found < 0 ) {
					localVertices[localCount] = v[i];
					found = ( int )localCount++;
				}
				local[i] = ( uint32_t )found;
			}
			triangles[triCount++] = t3dWorldPackTriangle( local[0], local[1], local[2] );
		}
	}
	if ( !t3dWorldFlushClusterCold( rSource, rBuffers, rStats, localVertices, localCount, triangles, triCount ) ) {
		return false;
	}
	rSpan.m_nClusterCount = rBuffers.m_nClusterCount - rSpan.m_nClusterOffset;
	return rSpan.m_nClusterCount != 0u;
}
