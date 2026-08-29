// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_topology.h"

#include <limits.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/variant/utility_functions.hpp>


static bool t3dRdPackSignedGridCoordinate( const real_t fValue, int16_t &rPacked ) {
	const double value = ( double )( fValue );
	if ( !isfinite( value ) || value < ( double )( INT16_MIN ) || value > ( double )( INT16_MAX ) ) {
		return false;
	}
	const double rounded = round( value );
	if ( abs( value - rounded ) > s_fPackedIntegerTolerance ) {
		return false;
	}
	rPacked = ( int16_t )( ( int64_t )( rounded ) );
	return true;
}

static bool t3dRdPackUnsignedMetadata( const real_t fValue, const uint16_t nMaximum, uint16_t &rPacked ) {
	const double value = ( double )( fValue );
	if ( !isfinite( value ) || value < 0.0 || value > ( double )( nMaximum ) ) {
		return false;
	}
	const double rounded = round( value );
	if ( abs( value - rounded ) > s_fPackedIntegerTolerance ) {
		return false;
	}
	rPacked = ( uint16_t )( ( uint64_t )( rounded ) );
	return true;
}

uint64_t Terrain3DRdTopology::checksumBytes( const PackedByteArray &rBytes ) {
	uint64_t checksum = s_nFnv1aOffset;
	const uint8_t *const pBytes = rBytes.ptr();
	for ( int64_t i = 0; i < rBytes.size(); ++i ) {
		checksum ^= ( uint64_t )( pBytes[i] );
		checksum *= s_nFnv1aPrime;
	}
	return checksum;
}

bool Terrain3DRdTopology::hasAnyGpuResource( void ) const {
	return m_hVertexBuffer.is_valid() || m_hVertexArray.is_valid() || m_hIndexBuffer.is_valid() || m_hIndexArray.is_valid();
}

void Terrain3DRdTopology::discardGpuHandles( void ) {
	m_hVertexBuffer = RID();
	m_hVertexArray = RID();
	m_hIndexBuffer = RID();
	m_hIndexArray = RID();
	m_nDeviceGeneration = 0u;
	m_nGpuBytes = 0u;
	m_nVertexGpuBytes = 0u;
	m_nIndexGpuBytes = 0u;
	m_nUploadedVertexChecksum = 0u;
	m_nUploadedIndexChecksum = 0u;
}

bool Terrain3DRdTopology::buildCpu( const t3dClipmapGeometry_t &rGeometry, const String &rName, const bool bReportErrors ) {
	if ( rGeometry.m_vertices.is_empty() || rGeometry.m_indices.is_empty() || rGeometry.m_vertices.size() != rGeometry.m_metadata.size()
	        || ( rGeometry.m_indices.size() % 3 ) != 0 ) {
		return false;
	}
	const uint64_t source_vertex_count = ( uint64_t )( rGeometry.m_vertices.size() );
	const uint64_t source_index_count = ( uint64_t )( rGeometry.m_indices.size() );
	if ( source_vertex_count > ( uint64_t )( UINT32_MAX ) / sizeof( t3dRdClipmapVertex_t )
	        || source_index_count > ( uint64_t )( UINT32_MAX ) / sizeof( uint32_t ) ) {
		if ( bReportErrors ) {
			UtilityFunctions::push_error( "Terrain3D RD topology exceeds buffer-addressable size" );
		}
		return false;
	}

	const uint32_t vertex_count = ( uint32_t )( source_vertex_count );
	const uint32_t index_count = ( uint32_t )( source_index_count );
	PackedByteArray vertex_bytes;
	vertex_bytes.resize( ( int64_t )( vertex_count ) * ( int64_t )( sizeof( t3dRdClipmapVertex_t ) ) );
	if ( vertex_bytes.size() != ( int64_t )( vertex_count ) * ( int64_t )( sizeof( t3dRdClipmapVertex_t ) ) ) {
		return false;
	}
	Vector2i grid_minimum( INT32_MAX, INT32_MAX );
	Vector2i grid_maximum( INT32_MIN, INT32_MIN );
	for ( uint32_t i = 0u; i < vertex_count; ++i ) {
		const Vector3 position = rGeometry.m_vertices[( int64_t )( i )];
		const Vector2 metadata = rGeometry.m_metadata[( int64_t )( i )];
		t3dRdClipmapVertex_t packed;
		if ( !isfinite( ( double )( position.y ) ) || abs( ( double )( position.y ) ) > s_fPackedIntegerTolerance
		        || !t3dRdPackSignedGridCoordinate( position.x, packed.m_nGridX )
		        || !t3dRdPackSignedGridCoordinate( position.z, packed.m_nGridZ ) ) {
			if ( bReportErrors ) {
				UtilityFunctions::push_error( "Terrain3D RD topology requires integral signed 16-bit X/Z coordinates and implicit zero Y" );
			}
			return false;
		}
		if ( !t3dRdPackUnsignedMetadata( metadata.x, ( uint16_t )( s_fClipmapPartEdgeB1 ), packed.m_nPartCode )
		        || !t3dRdPackUnsignedMetadata( metadata.y, UINT16_MAX, packed.m_nFlags ) ) {
			if ( bReportErrors ) {
				UtilityFunctions::push_error( "Terrain3D RD topology contains invalid part/flag metadata" );
			}
			return false;
		}
		memcpy( vertex_bytes.ptrw() + ( int64_t )( i ) * ( int64_t )( sizeof( packed ) ), &packed, sizeof( packed ) );
		grid_minimum.x = MIN( grid_minimum.x, ( int32_t )( packed.m_nGridX ) );
		grid_minimum.y = MIN( grid_minimum.y, ( int32_t )( packed.m_nGridZ ) );
		grid_maximum.x = MAX( grid_maximum.x, ( int32_t )( packed.m_nGridX ) );
		grid_maximum.y = MAX( grid_maximum.y, ( int32_t )( packed.m_nGridZ ) );
	}

	uint32_t max_index = 0u;
	for ( uint32_t i = 0u; i < index_count; ++i ) {
		const int32_t source = rGeometry.m_indices[( int64_t )( i )];
		if ( source < 0 || ( uint32_t )( source ) >= vertex_count ) {
			if ( bReportErrors ) {
				UtilityFunctions::push_error( "Terrain3D RD topology contains an out-of-range index" );
			}
			return false;
		}
		max_index = MAX( max_index, ( uint32_t )( source ) );
	}

	PackedByteArray index_bytes;
	RenderingDevice::IndexBufferFormat index_format;
	if ( max_index <= 65535u ) {
		index_format = RenderingDevice::INDEX_BUFFER_FORMAT_UINT16;
		index_bytes.resize( ( int64_t )( index_count ) * ( int64_t )( sizeof( uint16_t ) ) );
		if ( index_bytes.size() != ( int64_t )( index_count ) * ( int64_t )( sizeof( uint16_t ) ) ) {
			return false;
		}
		for ( uint32_t i = 0u; i < index_count; ++i ) {
			const uint16_t packed = ( uint16_t )( rGeometry.m_indices[( int64_t )( i )] );
			memcpy( index_bytes.ptrw() + ( int64_t )( i ) * ( int64_t )( sizeof( packed ) ), &packed, sizeof( packed ) );
		}
	} else {
		index_format = RenderingDevice::INDEX_BUFFER_FORMAT_UINT32;
		index_bytes.resize( ( int64_t )( index_count ) * ( int64_t )( sizeof( uint32_t ) ) );
		if ( index_bytes.size() != ( int64_t )( index_count ) * ( int64_t )( sizeof( uint32_t ) ) ) {
			return false;
		}
		for ( uint32_t i = 0u; i < index_count; ++i ) {
			const uint32_t packed = ( uint32_t )( rGeometry.m_indices[( int64_t )( i )] );
			memcpy( index_bytes.ptrw() + ( int64_t )( i ) * ( int64_t )( sizeof( packed ) ), &packed, sizeof( packed ) );
		}
	}

	const uint64_t vertex_checksum = checksumBytes( vertex_bytes );
	const uint64_t index_checksum = checksumBytes( index_bytes );
	if ( isGpuReady()
	        && ( vertex_count != m_nVertexCount || index_count != m_nIndexCount || index_format != m_indexFormat
	                || vertex_checksum != m_nUploadedVertexChecksum || index_checksum != m_nUploadedIndexChecksum ) ) {
		if ( bReportErrors ) {
			UtilityFunctions::push_error( "Terrain3D RD topology is immutable after upload" );
		}
		return false;
	}

	m_vertexBytes = vertex_bytes;
	m_indexBytes = index_bytes;
	m_nVertexCount = vertex_count;
	m_nIndexCount = index_count;
	m_indexFormat = index_format;
	m_nVertexChecksum = vertex_checksum;
	m_nIndexChecksum = index_checksum;
	m_gridMinimum = grid_minimum;
	m_gridMaximum = grid_maximum;
	if ( !isGpuReady() ) {
		m_name = rName;
	}
	return true;
}

bool Terrain3DRdTopology::upload( Terrain3DRdContext &rContext, const int64_t nVertexFormat ) {
	if ( isGpuReady() ) {
		if ( m_nDeviceGeneration == rContext.getDeviceGeneration() && rContext.getDevice() ) {
			return true;
		}
		// A new context generation means the context already released or abandoned
		// the old-generation resources. Never feed stale RIDs back to the device.
		discardGpuHandles();
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd || !isCpuReady() || nVertexFormat == RenderingDevice::INVALID_FORMAT_ID ) {
		return false;
	}
	if ( hasAnyGpuResource() ) {
		releaseGpu( rContext );
	}
	const uint64_t vertex_bytes = ( uint64_t )( m_vertexBytes.size() );
	const uint64_t index_bytes = ( uint64_t )( m_indexBytes.size() );
	m_hVertexBuffer = rd->vertex_buffer_create( ( uint32_t )( m_vertexBytes.size() ), m_vertexBytes );
	if ( !m_hVertexBuffer.is_valid() ) {
		rContext.recordResourceCreationFailure( "T3D.Clipmap." + m_name + ".VB", Terrain3DRdContext::RESOURCE_VERTEX_BUFFER );
		return false;
	}
	m_nVertexGpuBytes = vertex_bytes;
	if ( !rContext.registerResource( m_hVertexBuffer,
	             "T3D.Clipmap." + m_name + ".VB",
	             Terrain3DRdContext::RESOURCE_VERTEX_BUFFER,
	             Terrain3DRdContext::MEMORY_TOPOLOGY_VERTEX,
	             vertex_bytes,
	             Terrain3DRdContext::OWNER_TERRAIN ) ) {
		rd->free_rid( m_hVertexBuffer );
		discardGpuHandles();
		return false;
	}
	m_hIndexBuffer = rd->index_buffer_create( m_nIndexCount, m_indexFormat, m_indexBytes );
	if ( !m_hIndexBuffer.is_valid() ) {
		releaseGpu( rContext );
		rContext.recordResourceCreationFailure( "T3D.Clipmap." + m_name + ".IB", Terrain3DRdContext::RESOURCE_INDEX_BUFFER );
		return false;
	}
	m_nIndexGpuBytes = index_bytes;
	const String index_name =
	        "T3D.Clipmap." + m_name + ( m_indexFormat == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? ".IB16" : ".IB32" );
	if ( !rContext.registerResource( m_hIndexBuffer,
	             index_name,
	             Terrain3DRdContext::RESOURCE_INDEX_BUFFER,
	             Terrain3DRdContext::MEMORY_TOPOLOGY_INDEX,
	             index_bytes,
	             Terrain3DRdContext::OWNER_TERRAIN ) ) {
		rd->free_rid( m_hIndexBuffer );
		m_hIndexBuffer = RID();
		m_nIndexGpuBytes = 0u;
		releaseGpu( rContext );
		return false;
	}
	const PackedByteArray uploaded_vertices = rd->buffer_get_data( m_hVertexBuffer, 0u, ( uint32_t )( vertex_bytes ) );
	const PackedByteArray uploaded_indices = rd->buffer_get_data( m_hIndexBuffer, 0u, ( uint32_t )( index_bytes ) );
	m_nUploadedVertexChecksum = checksumBytes( uploaded_vertices );
	m_nUploadedIndexChecksum = checksumBytes( uploaded_indices );
	if ( uploaded_vertices.size() != m_vertexBytes.size() || uploaded_indices.size() != m_indexBytes.size()
	        || m_nUploadedVertexChecksum != m_nVertexChecksum || m_nUploadedIndexChecksum != m_nIndexChecksum ) {
		UtilityFunctions::push_error( "Terrain3D RD topology upload checksum mismatch" );
		releaseGpu( rContext );
		rContext.recordFailure( Terrain3DRdContext::FAILURE_RESOURCE_CREATION, "Terrain3D RD topology upload checksum mismatch" );
		return false;
	}
	TypedArray<RID> vertex_buffers;
	vertex_buffers.push_back( m_hVertexBuffer );
	m_hVertexArray = rd->vertex_array_create( m_nVertexCount, nVertexFormat, vertex_buffers );
	if ( !m_hVertexArray.is_valid() ) {
		releaseGpu( rContext );
		rContext.recordResourceCreationFailure( "T3D.Clipmap." + m_name + ".VA", Terrain3DRdContext::RESOURCE_VERTEX_ARRAY );
		return false;
	}
	if ( !rContext.registerResource( m_hVertexArray,
	             "T3D.Clipmap." + m_name + ".VA",
	             Terrain3DRdContext::RESOURCE_VERTEX_ARRAY,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_TERRAIN ) ) {
		rd->free_rid( m_hVertexArray );
		m_hVertexArray = RID();
		releaseGpu( rContext );
		return false;
	}
	m_hIndexArray = rd->index_array_create( m_hIndexBuffer, 0u, m_nIndexCount );
	if ( !m_hIndexArray.is_valid() ) {
		releaseGpu( rContext );
		rContext.recordResourceCreationFailure( "T3D.Clipmap." + m_name + ".IA", Terrain3DRdContext::RESOURCE_INDEX_ARRAY );
		return false;
	}
	if ( !rContext.registerResource( m_hIndexArray,
	             "T3D.Clipmap." + m_name + ".IA",
	             Terrain3DRdContext::RESOURCE_INDEX_ARRAY,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_TERRAIN ) ) {
		rd->free_rid( m_hIndexArray );
		m_hIndexArray = RID();
		releaseGpu( rContext );
		return false;
	}
	m_nGpuBytes = vertex_bytes + index_bytes;
	m_nDeviceGeneration = rContext.getDeviceGeneration();
	++m_nUploadCount;
	return true;
}

void Terrain3DRdTopology::releaseGpu( Terrain3DRdContext &rContext ) {
	rContext.freeResource( m_hIndexArray );
	rContext.freeResource( m_hVertexArray );
	rContext.freeResource( m_hIndexBuffer, m_nIndexGpuBytes );
	rContext.freeResource( m_hVertexBuffer, m_nVertexGpuBytes );
	discardGpuHandles();
}

void Terrain3DRdTopology::clearCpu( void ) {
	m_vertexBytes.clear();
	m_indexBytes.clear();
}

bool Terrain3DRdTopology::getPackedVertex( const uint32_t nVertex, t3dRdClipmapVertex_t &rVertex ) const {
	if ( nVertex >= m_nVertexCount
	        || m_vertexBytes.size() != ( int64_t )( m_nVertexCount ) * ( int64_t )( sizeof( t3dRdClipmapVertex_t ) ) ) {
		return false;
	}
	memcpy( &rVertex,
	        m_vertexBytes.ptr() + ( int64_t )( nVertex ) * ( int64_t )( sizeof( t3dRdClipmapVertex_t ) ),
	        sizeof( t3dRdClipmapVertex_t ) );
	return true;
}

bool Terrain3DRdTopology::getIndex( const uint32_t nIndex, uint32_t &rIndex ) const {
	if ( nIndex >= m_nIndexCount ) {
		return false;
	}
	if ( m_indexFormat == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ) {
		if ( m_indexBytes.size() != ( int64_t )( m_nIndexCount ) * ( int64_t )( sizeof( uint16_t ) ) ) {
			return false;
		}
		uint16_t packed = 0u;
		memcpy( &packed, m_indexBytes.ptr() + ( int64_t )( nIndex ) * ( int64_t )( sizeof( uint16_t ) ), sizeof( packed ) );
		rIndex = ( uint32_t )( packed );
		return true;
	}
	if ( m_indexFormat == RenderingDevice::INDEX_BUFFER_FORMAT_UINT32 ) {
		if ( m_indexBytes.size() != ( int64_t )( m_nIndexCount ) * ( int64_t )( sizeof( uint32_t ) ) ) {
			return false;
		}
		memcpy( &rIndex, m_indexBytes.ptr() + ( int64_t )( nIndex ) * ( int64_t )( sizeof( uint32_t ) ), sizeof( rIndex ) );
		return true;
	}
	return false;
}
