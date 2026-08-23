// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_RD_MESH_CACHE_H
#define TERRAIN3D_RD_MESH_CACHE_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/rd/terrain_3d_rd_texture_manager.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <memory>
#include <vector>

class Terrain3DAssets;

typedef struct t3dRdMeshSourceSurface_s {
	uint64_t m_nSourceKey = 0u;
	uint16_t m_nAssetId = 0u;
	int16_t m_nLod = 0;
	uint16_t m_nSurface = 0u;
	uint16_t m_nMaterial = UINT16_MAX;
	uint16_t m_nOverlayMaterial = UINT16_MAX;
	uint16_t m_nFlags = 0u;
	uint32_t m_nVertexOffset = 0u;
	uint32_t m_nVertexCount = 0u;
	uint32_t m_nIndexOffset = 0u;
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nIndexFormat = ( uint32_t )( RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 );
	AABB m_bounds;
} t3dRdMeshSourceSurface_t;

typedef struct t3dRdMeshSourceLod_s {
	uint64_t m_nSourceKey = 0u;
	uint16_t m_nAssetId = 0u;
	int16_t m_nLod = 0;
	uint32_t m_nFirstSurface = 0u;
	uint32_t m_nSurfaceCount = 0u;
	AABB m_bounds;
} t3dRdMeshSourceLod_t;


typedef struct t3dRdExternalMeshSource_s {
	uint64_t m_nSourceKey = 0u;
	Ref<Mesh> m_mesh;
} t3dRdExternalMeshSource_t;

typedef struct t3dRdImportedTextureSource_s {
	uint64_t m_nSourceId = 0u;
	uint64_t m_nFingerprint = 0u;
	t3dRdTextureLayout_t m_layout;
	PackedByteArray m_bytes;
} t3dRdImportedTextureSource_t;

typedef struct t3dRdImportedTextureGpu_s {
	uint64_t m_nSourceId = 0u;
	uint64_t m_nFingerprint = 0u;
	RID m_hTexture;
	uint64_t m_nBytes = 0u;
} t3dRdImportedTextureGpu_t;

typedef struct t3dRdMeshSourceSnapshot_s {
	uint64_t m_nFingerprint = 0u;
	uint64_t m_nGeneration = 0u;
	PackedByteArray m_vertexBytes;
	PackedByteArray m_index16Bytes;
	PackedByteArray m_index32Bytes;
	std::vector<t3dRdMeshSourceSurface_t> m_surfaces;
	std::vector<t3dRdMeshSourceLod_t> m_lods;
	std::vector<t3dRdInstanceMaterial_t> m_materials;
	std::vector<t3dRdImportedTextureSource_t> m_textures;
} t3dRdMeshSourceSnapshot_t;

class Terrain3DRdMeshCache {
private:
	Terrain3DRdContext &m_context;
	RID m_hVertexArena;
	RID m_hVertexArray;
	RID m_hIndex16Arena;
	RID m_hIndex16Array;
	RID m_hIndex32Arena;
	RID m_hIndex32Array;
	int64_t m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
	uint32_t m_nVertexCapacityBytes = 0u;
	uint32_t m_nIndex16CapacityBytes = 0u;
	uint32_t m_nIndex32CapacityBytes = 0u;
	uint32_t m_nVertexUsedBytes = 0u;
	uint32_t m_nIndex16UsedBytes = 0u;
	uint32_t m_nIndex32UsedBytes = 0u;
	uint32_t m_nGeneration = 0u;
	uint64_t m_nSourceGeneration = 0u;
	std::vector<t3dRdMeshSourceSurface_t> m_surfaces;
	std::vector<t3dRdMeshSourceLod_t> m_lods;
	std::vector<t3dRdInstanceMaterial_t> m_materials;
	std::vector<t3dRdImportedTextureGpu_t> m_textures;
	std::shared_ptr<const t3dRdMeshSourceSnapshot_t> m_lastUploadedSnapshot;

	bool ensureVertexFormat( void );
	bool ensureArena( RID &rBuffer,
			uint32_t &rCapacityBytes,
			uint32_t nRequiredBytes,
			const String &rName,
			Terrain3DRdContext::ResourceType resourceType,
			Terrain3DRdContext::MemoryCategory memoryCategory );
	bool rebuildArrays( void );
	bool syncTextures( const std::shared_ptr<const t3dRdMeshSourceSnapshot_t> &pSnapshot );
	void discardArrays( void );
	void discardTextures( void );

public:
	Terrain3DRdMeshCache( void );
	~Terrain3DRdMeshCache( void );

	static uint32_t nextArenaCapacity( uint32_t nRequiredBytes, uint32_t nMinimumBytes );
	static uint64_t computeSourceFingerprint( const Ref<Terrain3DAssets> &rAssets );
	static uint64_t computeSourceFingerprint( const Ref<Terrain3DAssets> &rAssets,
			const std::vector<t3dRdExternalMeshSource_t> &rExternalMeshes );
	static std::shared_ptr<const t3dRdMeshSourceSnapshot_t>
	buildSourceSnapshot( const Ref<Terrain3DAssets> &rAssets, uint64_t nFingerprint, uint64_t nGeneration, String *pError = nullptr );
	static std::shared_ptr<const t3dRdMeshSourceSnapshot_t>
	buildSourceSnapshot( const Ref<Terrain3DAssets> &rAssets,
			const std::vector<t3dRdExternalMeshSource_t> &rExternalMeshes,
			uint64_t nFingerprint,
			uint64_t nGeneration,
			String *pError = nullptr );

	bool upload( const std::shared_ptr<const t3dRdMeshSourceSnapshot_t> &pSnapshot );
	void shutdown( void );

	t3dRdMeshHandle_t getMeshHandle( uint16_t nAssetId, int nLod ) const;
	t3dRdMeshHandle_t getMeshHandleBySourceKey( uint64_t nSourceKey ) const;
	const t3dRdMeshSourceLod_t *resolveMesh( const t3dRdMeshHandle_t &rHandle ) const;
	t3dRdSurfaceHandle_t getSurfaceHandle( const t3dRdMeshHandle_t &rMesh, uint32_t nSurface ) const;
	const t3dRdMeshSourceSurface_t *resolveSurface( const t3dRdSurfaceHandle_t &rHandle ) const;

	EA_FORCE_INLINE const std::vector<t3dRdInstanceMaterial_t> &getMaterials( void ) const { return m_materials; }
	EA_FORCE_INLINE const std::vector<t3dRdMeshSourceSurface_t> &getSurfaces( void ) const { return m_surfaces; }
	RID getTextureRid( uint64_t nSourceId ) const;
	EA_FORCE_INLINE RID getVertexArray( void ) const { return m_hVertexArray; }
	EA_FORCE_INLINE int64_t getVertexFormat( void ) const { return m_nVertexFormat; }
	EA_FORCE_INLINE RID getIndexArray( const RenderingDevice::IndexBufferFormat format ) const {
		return format == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? m_hIndex16Array : m_hIndex32Array;
	}
	EA_FORCE_INLINE uint32_t getGeneration( void ) const { return m_nGeneration; }
	EA_FORCE_INLINE uint64_t getSourceGeneration( void ) const { return m_nSourceGeneration; }
	EA_FORCE_INLINE uint64_t getGpuBytes( void ) const {
		uint64_t bytes = ( uint64_t )( m_nVertexCapacityBytes ) + ( uint64_t )( m_nIndex16CapacityBytes ) +
				( uint64_t )( m_nIndex32CapacityBytes );
		for ( const t3dRdImportedTextureGpu_t &texture : m_textures ) bytes += texture.m_nBytes;
		return bytes;
	}
	EA_FORCE_INLINE uint32_t getTextureCount( void ) const { return ( uint32_t )( m_textures.size() ); }
	EA_FORCE_INLINE bool isReady( void ) const { return m_hVertexArray.is_valid() && m_nGeneration != 0u; }
};

#endif // TERRAIN3D_RD_MESH_CACHE_H
