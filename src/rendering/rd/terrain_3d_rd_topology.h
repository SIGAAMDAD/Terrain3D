// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_TOPOLOGY_H
#define TERRAIN3D_RD_TOPOLOGY_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/terrain_3d_mesher.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

class Terrain3DRdTopology {
private:
	PackedByteArray m_vertexBytes;
	PackedByteArray m_indexBytes;
	RID m_hVertexBuffer;
	RID m_hVertexArray;
	RID m_hIndexBuffer;
	RID m_hIndexArray;
	RenderingDevice::IndexBufferFormat m_indexFormat = RenderingDevice::INDEX_BUFFER_FORMAT_UINT16;
	uint32_t m_nVertexCount = 0u;
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nUploadCount = 0u;
	uint64_t m_nDeviceGeneration = 0u;
	uint64_t m_nGpuBytes = 0u;
	uint64_t m_nVertexGpuBytes = 0u;
	uint64_t m_nIndexGpuBytes = 0u;
	uint64_t m_nVertexChecksum = 0u;
	uint64_t m_nIndexChecksum = 0u;
	uint64_t m_nUploadedVertexChecksum = 0u;
	uint64_t m_nUploadedIndexChecksum = 0u;
	Vector2i m_gridMinimum;
	Vector2i m_gridMaximum;
	String m_name;

	static uint64_t checksumBytes( const PackedByteArray &rBytes );
	bool hasAnyGpuResource( void ) const;
	void discardGpuHandles( void );

public:
	Terrain3DRdTopology( void ) = default;
	~Terrain3DRdTopology( void ) = default;

	bool buildCpu( const t3dClipmapGeometry_t &rGeometry, const String &rName, bool bReportErrors = true );
	bool upload( Terrain3DRdContext &rContext, int64_t nVertexFormat );
	void releaseGpu( Terrain3DRdContext &rContext );
	void clearCpu( void );

	EA_FORCE_INLINE bool isCpuReady( void ) const {
		return m_nVertexCount > 0u && m_nIndexCount > 0u && !m_vertexBytes.is_empty() && !m_indexBytes.is_empty();
	}
	EA_FORCE_INLINE bool isGpuReady( void ) const {
		return m_hVertexArray.is_valid() && m_hIndexArray.is_valid();
	}
	EA_FORCE_INLINE const RID &getVertexArray( void ) const { return m_hVertexArray; }
	EA_FORCE_INLINE const RID &getIndexArray( void ) const { return m_hIndexArray; }
	EA_FORCE_INLINE uint32_t getVertexCount( void ) const { return m_nVertexCount; }
	EA_FORCE_INLINE uint32_t getIndexCount( void ) const { return m_nIndexCount; }
	EA_FORCE_INLINE uint32_t getUploadCount( void ) const { return m_nUploadCount; }
	EA_FORCE_INLINE uint64_t getDeviceGeneration( void ) const { return m_nDeviceGeneration; }
	EA_FORCE_INLINE uint64_t getGpuBytes( void ) const { return m_nGpuBytes; }
	EA_FORCE_INLINE uint64_t getVertexChecksum( void ) const { return m_nVertexChecksum; }
	EA_FORCE_INLINE uint64_t getIndexChecksum( void ) const { return m_nIndexChecksum; }
	EA_FORCE_INLINE uint64_t getUploadedVertexChecksum( void ) const { return m_nUploadedVertexChecksum; }
	EA_FORCE_INLINE uint64_t getUploadedIndexChecksum( void ) const { return m_nUploadedIndexChecksum; }
	EA_FORCE_INLINE const Vector2i &getGridMinimum( void ) const { return m_gridMinimum; }
	EA_FORCE_INLINE const Vector2i &getGridMaximum( void ) const { return m_gridMaximum; }
	EA_FORCE_INLINE RenderingDevice::IndexBufferFormat getIndexFormat( void ) const { return m_indexFormat; }
	bool getPackedVertex( uint32_t nVertex, t3dRdClipmapVertex_t &rVertex ) const;
	bool getIndex( uint32_t nIndex, uint32_t &rIndex ) const;
};

#endif // TERRAIN3D_RD_TOPOLOGY_H
