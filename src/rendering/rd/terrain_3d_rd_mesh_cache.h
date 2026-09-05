// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_MESH_CACHE_H
#define TERRAIN3D_RD_MESH_CACHE_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/rd/terrain_3d_rd_texture_manager.h"

#include "eastl_config.h"
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

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
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_vertexChunkHashes;
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_index16ChunkHashes;
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_index32ChunkHashes;
	eastl::vector<t3dRdMeshSourceSurface_t, Terrain3DEastlAllocator> m_surfaces;
	eastl::vector<t3dRdMeshSourceLod_t, Terrain3DEastlAllocator> m_lods;
	eastl::vector<t3dRdInstanceMaterial_t, Terrain3DEastlAllocator> m_materials;
	eastl::vector<t3dRdImportedTextureSource_t, Terrain3DEastlAllocator> m_textures;
	EA_FORCE_INLINE void bindRuntimeAllocator( const Terrain3DEastlAllocator &rAllocator ) {
		m_vertexChunkHashes.set_allocator( rAllocator );
		m_index16ChunkHashes.set_allocator( rAllocator );
		m_index32ChunkHashes.set_allocator( rAllocator );
		m_surfaces.set_allocator( rAllocator );
		m_lods.set_allocator( rAllocator );
		m_materials.set_allocator( rAllocator );
		m_textures.set_allocator( rAllocator );
	}
	bool copyFrom( const t3dRdMeshSourceSnapshot_s &rOther ) {
		if ( rOther.m_vertexChunkHashes.size() > m_vertexChunkHashes.capacity()
		        || rOther.m_index16ChunkHashes.size() > m_index16ChunkHashes.capacity()
		        || rOther.m_index32ChunkHashes.size() > m_index32ChunkHashes.capacity() || rOther.m_surfaces.size() > m_surfaces.capacity()
		        || rOther.m_lods.size() > m_lods.capacity() || rOther.m_materials.size() > m_materials.capacity()
		        || rOther.m_textures.size() > m_textures.capacity() ) {
			return false;
		}
		if ( !t3dVectorCopyWithinCapacity( m_vertexChunkHashes, rOther.m_vertexChunkHashes )
		        || !t3dVectorCopyWithinCapacity( m_index16ChunkHashes, rOther.m_index16ChunkHashes )
		        || !t3dVectorCopyWithinCapacity( m_index32ChunkHashes, rOther.m_index32ChunkHashes )
		        || !t3dVectorCopyWithinCapacity( m_surfaces, rOther.m_surfaces ) || !t3dVectorCopyWithinCapacity( m_lods, rOther.m_lods )
		        || !t3dVectorCopyWithinCapacity( m_materials, rOther.m_materials )
		        || !t3dVectorCopyWithinCapacity( m_textures, rOther.m_textures ) ) {
			return false;
		}
		m_nFingerprint = rOther.m_nFingerprint;
		m_nGeneration = rOther.m_nGeneration;
		m_vertexBytes = rOther.m_vertexBytes;
		m_index16Bytes = rOther.m_index16Bytes;
		m_index32Bytes = rOther.m_index32Bytes;
		return true;
	}
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
	uint64_t m_nArenaCapacityMissCount = 0u;
	uint64_t m_nArenaGrowthRequestCount = 0u;
	uint64_t m_nArenaRebuildCount = 0u;
	uint64_t m_nArenaRebuildFailureCount = 0u;
	eastl::vector<t3dRdMeshSourceSurface_t, Terrain3DEastlAllocator> m_surfaces;
	eastl::vector<t3dRdMeshSourceLod_t, Terrain3DEastlAllocator> m_lods;
	eastl::vector<t3dRdInstanceMaterial_t, Terrain3DEastlAllocator> m_materials;
	eastl::vector<t3dRdImportedTextureGpu_t, Terrain3DEastlAllocator> m_textures;
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_vertexChunkHashes;
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_index16ChunkHashes;
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_index32ChunkHashes;
	PackedByteArray m_dirtyUploadScratch;
	eastl::vector<t3dRdImportedTextureGpu_t, Terrain3DEastlAllocator> m_textureSyncNext;
	eastl::vector<uint8_t, Terrain3DEastlAllocator> m_textureSyncReused;

	bool ensureVertexFormat( void );
	bool ensureArena( RID &rBuffer,
	        uint32_t &rCapacityBytes,
	        uint32_t nRequiredBytes,
	        const String &rName,
	        Terrain3DRdContext::ResourceType resourceType,
	        Terrain3DRdContext::MemoryCategory memoryCategory );
	bool rebuildArrays( void );
	bool syncTextures( const t3dRdMeshSourceSnapshot_t *pSnapshot );
	bool uploadChangedChunks( const RID &rBuffer,
	        const PackedByteArray &rCurrent,
	        const eastl::vector<uint64_t, Terrain3DEastlAllocator> &rCurrentHashes,
	        const eastl::vector<uint64_t, Terrain3DEastlAllocator> &rPreviousHashes,
	        bool bForceFull,
	        uint64_t &nUploadedBytes );
	void discardArrays( void );
	void discardTextures( void );

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	Terrain3DRdMeshCache( void );
	~Terrain3DRdMeshCache( void );

	static uint32_t nextArenaCapacity( uint32_t nRequiredBytes, uint32_t nMinimumBytes );
	static uint64_t computeSourceFingerprint( const Ref<Terrain3DAssets> &rAssets );
	static uint64_t computeSourceFingerprint(
	        const Ref<Terrain3DAssets> &rAssets, const t3dRdExternalMeshSource_t *pExternalMeshes, size_t nExternalMeshCount );
	static bool buildSourceSnapshot( const Ref<Terrain3DAssets> &rAssets,
	        t3dRdMeshSourceSnapshot_t &rResult,
	        uint64_t nFingerprint,
	        uint64_t nGeneration,
	        String *pError = NULL,
	        bool bIncludeDisabled = false );
	static bool buildSourceSnapshot( const Ref<Terrain3DAssets> &rAssets,
	        const t3dRdExternalMeshSource_t *pExternalMeshes,
	        size_t nExternalMeshCount,
	        t3dRdMeshSourceSnapshot_t &rResult,
	        uint64_t nFingerprint,
	        uint64_t nGeneration,
	        String *pError = NULL,
	        bool bIncludeDisabled = false );

	bool upload( const t3dRdMeshSourceSnapshot_t *pSnapshot );
	void shutdown( void );

	t3dRdMeshHandle_t getMeshHandle( uint16_t nAssetId, int nLod ) const;
	t3dRdMeshHandle_t getMeshHandleBySourceKey( uint64_t nSourceKey ) const;
	const t3dRdMeshSourceLod_t *resolveMesh( const t3dRdMeshHandle_t &rHandle ) const;
	t3dRdSurfaceHandle_t getSurfaceHandle( const t3dRdMeshHandle_t &rMesh, uint32_t nSurface ) const;
	const t3dRdMeshSourceSurface_t *resolveSurface( const t3dRdSurfaceHandle_t &rHandle ) const;

	EA_FORCE_INLINE const t3dRdInstanceMaterial_t *getMaterials( void ) const {
		return m_materials.data();
	}
	EA_FORCE_INLINE size_t getMaterialCount( void ) const {
		return m_materials.size();
	}
	EA_FORCE_INLINE const t3dRdMeshSourceSurface_t *getSurfaces( void ) const {
		return m_surfaces.data();
	}
	EA_FORCE_INLINE size_t getSurfaceCount( void ) const {
		return m_surfaces.size();
	}
	RID getTextureRid( uint64_t nSourceId ) const;
	EA_FORCE_INLINE RID getVertexArray( void ) const {
		return m_hVertexArray;
	}
	EA_FORCE_INLINE int64_t getVertexFormat( void ) const {
		return m_nVertexFormat;
	}
	EA_FORCE_INLINE RID getIndexArray( const RenderingDevice::IndexBufferFormat format ) const {
		return format == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? m_hIndex16Array : m_hIndex32Array;
	}
	EA_FORCE_INLINE uint32_t getGeneration( void ) const {
		return m_nGeneration;
	}
	EA_FORCE_INLINE uint64_t getSourceGeneration( void ) const {
		return m_nSourceGeneration;
	}
	EA_FORCE_INLINE uint64_t getGpuBytes( void ) const {
		uint64_t bytes =
		        ( uint64_t )( m_nVertexCapacityBytes ) + ( uint64_t )( m_nIndex16CapacityBytes ) + ( uint64_t )( m_nIndex32CapacityBytes );
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textures ).size() ); iTexture++ ) {
			const t3dRdImportedTextureGpu_t &texture = ( m_textures )[iTexture];
			bytes += texture.m_nBytes;
		}
		return bytes;
	}
	EA_FORCE_INLINE uint32_t getTextureCount( void ) const {
		return ( uint32_t )( m_textures.size() );
	}
	EA_FORCE_INLINE bool isReady( void ) const {
		return m_hVertexArray.is_valid() && m_nGeneration != 0u;
	}
	EA_FORCE_INLINE uint64_t getArenaCapacityMissCount( void ) const { return m_nArenaCapacityMissCount; }
	EA_FORCE_INLINE uint64_t getArenaGrowthRequestCount( void ) const { return m_nArenaGrowthRequestCount; }
	EA_FORCE_INLINE uint64_t getArenaRebuildCount( void ) const { return m_nArenaRebuildCount; }
	EA_FORCE_INLINE uint64_t getArenaRebuildFailureCount( void ) const { return m_nArenaRebuildFailureCount; }
	EA_FORCE_INLINE uint32_t getVertexCapacityBytes( void ) const { return m_nVertexCapacityBytes; }
	EA_FORCE_INLINE uint32_t getIndex16CapacityBytes( void ) const { return m_nIndex16CapacityBytes; }
	EA_FORCE_INLINE uint32_t getIndex32CapacityBytes( void ) const { return m_nIndex32CapacityBytes; }
	EA_FORCE_INLINE uint32_t getVertexUsedBytes( void ) const { return m_nVertexUsedBytes; }
	EA_FORCE_INLINE uint32_t getIndex16UsedBytes( void ) const { return m_nIndex16UsedBytes; }
	EA_FORCE_INLINE uint32_t getIndex32UsedBytes( void ) const { return m_nIndex32UsedBytes; }
};

#endif // TERRAIN3D_RD_MESH_CACHE_H
