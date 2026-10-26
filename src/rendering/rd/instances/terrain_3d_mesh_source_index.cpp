// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/instances/terrain_3d_mesh_source_index.h"
#include <EASTL/sort.h>

void Terrain3DMeshSourceIndex::bindRuntimeAllocator(
	const Terrain3DEastlAllocator &rAllocator )
{
	m_spans.set_allocator( rAllocator );
}

bool Terrain3DMeshSourceIndex::reserveCold(
	const size_t nCount )
{
	return t3dContainerReserveCold( m_spans, nCount );
}

void Terrain3DMeshSourceIndex::reset( void )
{
	m_spans.clear();
	m_nSourceGeneration = 0u;
}

bool Terrain3DMeshSourceIndex::rebuildCold(
	const t3dRdMeshSourceSnapshot_t &rSnapshot )
{
	m_nSourceGeneration = 0u;
	m_spans.clear();
	if ( rSnapshot.m_lods.size() > m_spans.capacity() ) {
		return false;
	}
	for ( const t3dRdMeshSourceLod_t &lod : rSnapshot.m_lods ) {
		if ( lod.m_nFirstSurface > rSnapshot.m_surfaces.size() ||
			lod.m_nSurfaceCount > rSnapshot.m_surfaces.size() - lod.m_nFirstSurface ) {
			return false;
		}
		uint32_t first = UINT32_MAX;
		uint32_t end = 0u;
		for ( uint32_t iSurface = 0u; iSurface < lod.m_nSurfaceCount; ++iSurface ) {
			const t3dRdMeshSourceSurface_t &surface =
				rSnapshot.m_surfaces[( size_t ) lod.m_nFirstSurface + iSurface];
			if ( surface.m_nSourceKey != lod.m_nSourceKey ||
				surface.m_nWorldClusterOffset > UINT32_MAX - surface.m_nWorldClusterCount ) {
				return false;
			}
			if ( surface.m_nWorldClusterCount == 0u ) {
				continue;
			}
			first = MIN( first, surface.m_nWorldClusterOffset );
			end = MAX( end, surface.m_nWorldClusterOffset + surface.m_nWorldClusterCount );
		}
		if ( first == UINT32_MAX ) {
			continue;
		}
		if ( end > rSnapshot.m_worldClusters.size() ||
			!t3dVectorPushBackWithinCapacity( m_spans, t3dWorldMeshSourceSpan_t{ lod.m_nSourceKey, first, end - first } ) ) {
			return false;
		}
	}
	eastl::sort( m_spans.begin(), m_spans.end(), []( const t3dWorldMeshSourceSpan_t &rA,
		const t3dWorldMeshSourceSpan_t &rB ) { return rA.m_nSourceKey < rB.m_nSourceKey; } );
	for ( size_t i = 1u; i < m_spans.size(); ++i ) {
		if ( m_spans[i - 1u].m_nSourceKey == m_spans[i].m_nSourceKey ) {
			return false;
		}
	}
	m_nSourceGeneration = rSnapshot.m_nGeneration;
	return true;
}

bool Terrain3DMeshSourceIndex::findClusterSpan(
	const uint64_t nSourceKey,
	uint32_t &rFirstCluster,
	uint32_t &rClusterCount ) const
{
	size_t low = 0u;
	size_t high = m_spans.size();
	while ( low < high ) {
		const size_t middle = low + ( high - low ) / 2u;
		if ( m_spans[middle].m_nSourceKey < nSourceKey ) {
			low = middle + 1u;
		} else {
			high = middle;
		}
	}
	if ( low == m_spans.size() || m_spans[low].m_nSourceKey != nSourceKey ) {
		rFirstCluster = 0u;
		rClusterCount = 0u;
		return false;
	}
	rFirstCluster = m_spans[low].m_nFirstCluster;
	rClusterCount = m_spans[low].m_nClusterCount;
	return true;
}
