// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_topology.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <climits>
#include <cstring>

bool Terrain3DRdTopology::buildCpu( const t3dClipmapGeometry_t &rGeometry, const String &rName ) {
	m_vertexBytes.clear();
	m_indexBytes.clear();
	m_nVertexCount = 0u;
	m_nIndexCount = 0u;
	m_nGpuBytes = 0u;
	m_name = rName;
	if ( rGeometry.m_vertices.is_empty() || rGeometry.m_indices.is_empty() ||
			rGeometry.m_vertices.size() != rGeometry.m_metadata.size() ) {
		return false;
	}

	m_nVertexCount = ( uint32_t )( rGeometry.m_vertices.size() );
	m_nIndexCount = ( uint32_t )( rGeometry.m_indices.size() );
	m_vertexBytes.resize( ( int64_t )( m_nVertexCount ) * ( int64_t )( sizeof( t3dRdClipmapVertex_t ) ) );
	t3dRdClipmapVertex_t *const pVertices = ( t3dRdClipmapVertex_t * )( m_vertexBytes.ptrw() );
	for ( uint32_t i = 0u; i < m_nVertexCount; ++i ) {
		const Vector3 position = rGeometry.m_vertices[( int64_t )( i )];
		const Vector2 metadata = rGeometry.m_metadata[( int64_t )( i )];
		const int64_t grid_x = ( int64_t )( std::llround( ( double )( position.x ) ) );
		const int64_t grid_z = ( int64_t )( std::llround( ( double )( position.z ) ) );
		if ( grid_x < INT16_MIN || grid_x > INT16_MAX || grid_z < INT16_MIN || grid_z > INT16_MAX ) {
			UtilityFunctions::push_error( "Terrain3D RD topology exceeds signed 16-bit clipmap grid range" );
			return false;
		}
		pVertices[i].m_nGridX = ( int16_t )( grid_x );
		pVertices[i].m_nGridZ = ( int16_t )( grid_z );
		pVertices[i].m_nPartCode = ( uint16_t )( CLAMP( ( int )( std::llround( ( double )( metadata.x ) ) ), 0, 65535 ) );
		pVertices[i].m_nFlags = 0u;
	}

	uint32_t max_index = 0u;
	for ( uint32_t i = 0u; i < m_nIndexCount; ++i ) {
		const int32_t source = rGeometry.m_indices[( int64_t )( i )];
		if ( source < 0 || ( uint32_t )( source ) >= m_nVertexCount ) {
			UtilityFunctions::push_error( "Terrain3D RD topology contains an out-of-range index" );
			return false;
		}
		max_index = MAX( max_index, ( uint32_t )( source ) );
	}

	if ( max_index <= 65535u ) {
		m_indexFormat = RenderingDevice::INDEX_BUFFER_FORMAT_UINT16;
		m_indexBytes.resize( ( int64_t )( m_nIndexCount ) * ( int64_t )( sizeof( uint16_t ) ) );
		uint16_t *const pIndices = ( uint16_t * )( m_indexBytes.ptrw() );
		for ( uint32_t i = 0u; i < m_nIndexCount; ++i ) {
			pIndices[i] = ( uint16_t )( rGeometry.m_indices[( int64_t )( i )] );
		}
	} else {
		m_indexFormat = RenderingDevice::INDEX_BUFFER_FORMAT_UINT32;
		m_indexBytes.resize( ( int64_t )( m_nIndexCount ) * ( int64_t )( sizeof( uint32_t ) ) );
		uint32_t *const pIndices = ( uint32_t * )( m_indexBytes.ptrw() );
		for ( uint32_t i = 0u; i < m_nIndexCount; ++i ) {
			pIndices[i] = ( uint32_t )( rGeometry.m_indices[( int64_t )( i )] );
		}
	}
	return true;
}

bool Terrain3DRdTopology::upload( Terrain3DRdContext &rContext, const int64_t nVertexFormat ) {
	if ( isGpuReady() ) {
		return true;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd || !isCpuReady() || nVertexFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		return false;
	}
	const uint64_t vertex_bytes = ( uint64_t )( m_vertexBytes.size() );
	const uint64_t index_bytes = ( uint64_t )( m_indexBytes.size() );
	m_hVertexBuffer = rd->vertex_buffer_create( ( uint32_t )( m_vertexBytes.size() ), m_vertexBytes );
	if ( !m_hVertexBuffer.is_valid() ) return false;
	rContext.registerResource( m_hVertexBuffer, "T3D.Clipmap." + m_name + ".VB", vertex_bytes );
	m_hIndexBuffer = rd->index_buffer_create( m_nIndexCount, m_indexFormat, m_indexBytes );
	if ( !m_hIndexBuffer.is_valid() ) {
		releaseGpu( rContext );
		return false;
	}
	rContext.registerResource( m_hIndexBuffer,
			"T3D.Clipmap." + m_name + ( m_indexFormat == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? ".IB16" : ".IB32" ),
			index_bytes );
	TypedArray<RID> vertex_buffers;
	vertex_buffers.push_back( m_hVertexBuffer );
	m_hVertexArray = rd->vertex_array_create( m_nVertexCount, nVertexFormat, vertex_buffers );
	if ( !m_hVertexArray.is_valid() ) {
		releaseGpu( rContext );
		return false;
	}
	rContext.registerResource( m_hVertexArray, "T3D.Clipmap." + m_name + ".VA" );
	m_hIndexArray = rd->index_array_create( m_hIndexBuffer, 0u, m_nIndexCount );
	if ( !m_hIndexArray.is_valid() ) {
		releaseGpu( rContext );
		return false;
	}
	rContext.registerResource( m_hIndexArray, "T3D.Clipmap." + m_name + ".IA" );
	m_nGpuBytes = vertex_bytes + index_bytes;
	return true;
}

void Terrain3DRdTopology::releaseGpu( Terrain3DRdContext &rContext ) {
	const uint64_t vertex_bytes = ( uint64_t )( m_vertexBytes.size() );
	const uint64_t index_bytes = ( uint64_t )( m_indexBytes.size() );
	rContext.freeResource( m_hIndexArray );
	rContext.freeResource( m_hVertexArray );
	rContext.freeResource( m_hIndexBuffer, index_bytes );
	rContext.freeResource( m_hVertexBuffer, vertex_bytes );
	m_nGpuBytes = 0u;
}

void Terrain3DRdTopology::clearCpu( void ) {
	m_vertexBytes.clear();
	m_indexBytes.clear();
}
