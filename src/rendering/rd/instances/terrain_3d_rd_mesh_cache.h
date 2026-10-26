// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_MESH_CACHE_H
#define TERRAIN3D_RD_MESH_CACHE_H

#include "Terrain3DTypes.h"

#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/targets/terrain_3d_rd_geometry_view.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/world/build/terrain_3d_world_cluster_cooker.h"

#include "eastl_config.h"
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_rendering_material.h"

typedef const Terrain3DRenderingMaterial *( *t3dRdExternalRenderingMaterialResolver_t )( const void *pContext,
																						 uint32_t	 nSurface );

typedef struct t3dRdArenaSurfaceView_s
{
	Terrain3DRdGeometryView m_view;
} t3dRdArenaSurfaceView_t;

typedef struct t3dRdExternalMeshSource_s
{
	uint64_t								 m_nSourceKey = 0u;
	Ref< Mesh >								 m_mesh;
	const void								*m_pMaterialContext = NULL;
	t3dRdExternalRenderingMaterialResolver_t m_pResolveRenderingMaterial = NULL;
	uint32_t								 m_nFlags = 0u;
} t3dRdExternalMeshSource_t;

typedef struct t3dRdMeshSourceSnapshot_s
{
	uint64_t															  m_nFingerprint = 0u;
	uint64_t															  m_nGeneration = 0u;
	PackedByteArray														  m_vertexBytes;
	PackedByteArray														  m_index32Bytes;
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					  m_vertexChunkHashes;
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					  m_index32ChunkHashes;
	eastl::vector< t3dRdMeshSourceSurface_t, Terrain3DEastlAllocator >	  m_surfaces;
	eastl::vector< t3dRdMeshSourceLod_t, Terrain3DEastlAllocator >		  m_lods;
	eastl::vector< t3dRdInstanceMaterial_t, Terrain3DEastlAllocator >	  m_materials;
	eastl::vector< t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator >	  m_vtMaterials;
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					  m_materialKeys;
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					  m_vtStableKeys;
	eastl::vector< t3dWorldVertex_t, Terrain3DEastlAllocator >			  m_worldVertices;
	eastl::vector< uint32_t, Terrain3DEastlAllocator >					  m_worldClusterVertexIndices;
	eastl::vector< uint32_t, Terrain3DEastlAllocator >					  m_worldTriangleWords;
	eastl::vector< t3dWorldCluster_t, Terrain3DEastlAllocator >			  m_worldClusters;
	eastl::vector< t3dWorldClusterVTMetadata_t, Terrain3DEastlAllocator > m_worldClusterVtMetadata;
	// Payload bytes and retained capacities only; excludes this record's inline storage.
	size_t getRetainedCpuBytes(
		void ) const {
		return m_vertexBytes.size() + m_index32Bytes.size() + t3dContainerResidentBytes( m_vertexChunkHashes ) +
			   t3dContainerResidentBytes( m_index32ChunkHashes ) + t3dContainerResidentBytes( m_surfaces ) +
			   t3dContainerResidentBytes( m_lods ) + t3dContainerResidentBytes( m_materials ) +
			   t3dContainerResidentBytes( m_vtMaterials ) + t3dContainerResidentBytes( m_materialKeys ) +
			   t3dContainerResidentBytes( m_vtStableKeys ) + t3dContainerResidentBytes( m_worldVertices ) +
			   t3dContainerResidentBytes( m_worldClusterVertexIndices ) +
			   t3dContainerResidentBytes( m_worldTriangleWords ) + t3dContainerResidentBytes( m_worldClusters ) +
			   t3dContainerResidentBytes( m_worldClusterVtMetadata );
	}
	EA_FORCE_INLINE void bindRuntimeAllocator(
		const Terrain3DEastlAllocator &rAllocator ) {
		m_vertexChunkHashes.set_allocator( rAllocator );
		m_index32ChunkHashes.set_allocator( rAllocator );
		m_surfaces.set_allocator( rAllocator );
		m_lods.set_allocator( rAllocator );
		m_materials.set_allocator( rAllocator );
		m_vtMaterials.set_allocator( rAllocator );
		m_materialKeys.set_allocator( rAllocator );
		m_vtStableKeys.set_allocator( rAllocator );
		m_worldVertices.set_allocator( rAllocator );
		m_worldClusterVertexIndices.set_allocator( rAllocator );
		m_worldTriangleWords.set_allocator( rAllocator );
		m_worldClusters.set_allocator( rAllocator );
		m_worldClusterVtMetadata.set_allocator( rAllocator );
	}
	bool copyFrom(
		const t3dRdMeshSourceSnapshot_s &rOther ) {
		if ( rOther.m_vertexChunkHashes.size() > m_vertexChunkHashes.capacity() ||
			 rOther.m_index32ChunkHashes.size() > m_index32ChunkHashes.capacity() ||
			 rOther.m_surfaces.size() > m_surfaces.capacity() || rOther.m_lods.size() > m_lods.capacity() ||
			 rOther.m_materials.size() > m_materials.capacity() ||
			 rOther.m_vtMaterials.size() > m_vtMaterials.capacity() ||
			 rOther.m_materialKeys.size() > m_materialKeys.capacity() ||
			 rOther.m_vtStableKeys.size() > m_vtStableKeys.capacity() ||
			 rOther.m_worldVertices.size() > m_worldVertices.capacity() ||
			 rOther.m_worldClusterVertexIndices.size() > m_worldClusterVertexIndices.capacity() ||
			 rOther.m_worldTriangleWords.size() > m_worldTriangleWords.capacity() ||
			 rOther.m_worldClusters.size() > m_worldClusters.capacity() ||
			 rOther.m_worldClusterVtMetadata.size() > m_worldClusterVtMetadata.capacity() ) {
			return false;
		}
		if ( !t3dVectorCopyWithinCapacity( m_vertexChunkHashes, rOther.m_vertexChunkHashes ) ||
			 !t3dVectorCopyWithinCapacity( m_index32ChunkHashes, rOther.m_index32ChunkHashes ) ||
			 !t3dVectorCopyWithinCapacity( m_surfaces, rOther.m_surfaces ) ||
			 !t3dVectorCopyWithinCapacity( m_lods, rOther.m_lods ) ||
			 !t3dVectorCopyWithinCapacity( m_materials, rOther.m_materials ) ||
			 !t3dVectorCopyWithinCapacity( m_vtMaterials, rOther.m_vtMaterials ) ||
			 !t3dVectorCopyWithinCapacity( m_materialKeys, rOther.m_materialKeys ) ||
			 !t3dVectorCopyWithinCapacity( m_vtStableKeys, rOther.m_vtStableKeys ) ||
			 !t3dVectorCopyWithinCapacity( m_worldVertices, rOther.m_worldVertices ) ||
			 !t3dVectorCopyWithinCapacity( m_worldClusterVertexIndices, rOther.m_worldClusterVertexIndices ) ||
			 !t3dVectorCopyWithinCapacity( m_worldTriangleWords, rOther.m_worldTriangleWords ) ||
			 !t3dVectorCopyWithinCapacity( m_worldClusters, rOther.m_worldClusters ) ||
			 !t3dVectorCopyWithinCapacity( m_worldClusterVtMetadata, rOther.m_worldClusterVtMetadata ) ) {
			return false;
		}
		m_nFingerprint = rOther.m_nFingerprint;
		m_nGeneration = rOther.m_nGeneration;
		m_vertexBytes = rOther.m_vertexBytes;
		m_index32Bytes = rOther.m_index32Bytes;
		return true;
	}
} t3dRdMeshSourceSnapshot_t;

class Terrain3DRdMeshCache
{
private:
	CLASS_NAME_STATIC( "Terrain3DRdMeshCache" );

	Terrain3DRdContext	   &m_context;
	Terrain3DRdGpuBuffer	m_vertexArena;
	Terrain3DRdGpuBuffer	m_index32Arena;
	Terrain3DRdGeometryView m_arenaView;
	int64_t					m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
	uint32_t				m_nGeneration = 0u;
	uint64_t				m_nSourceGeneration = 0u;
	uint64_t				m_nArenaCapacityMissCount = 0u;
	uint64_t				m_nArenaGrowthRequestCount = 0u;
	uint64_t				m_nArenaRebuildCount = 0u;
	uint64_t				m_nArenaRebuildFailureCount = 0u;
	uint64_t				m_nUploadAttemptCount = 0u;
	uint64_t				m_nUploadFailureCount = 0u;
	String					m_lastError;
	eastl::vector< t3dRdMeshSourceSurface_t, Terrain3DEastlAllocator >	m_surfaces;
	eastl::vector< t3dRdMeshSourceLod_t, Terrain3DEastlAllocator >		m_lods;
	eastl::vector< t3dRdInstanceMaterial_t, Terrain3DEastlAllocator >	m_materials;
	eastl::vector< t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator > m_vtMaterials;
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					m_vtStableKeys;
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					m_vertexChunkHashes;
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					m_index32ChunkHashes;
	PackedByteArray														m_dirtyUploadScratch;
	eastl::vector< t3dRdArenaSurfaceView_t, Terrain3DEastlAllocator >	m_surfaceViews;

	bool ensureVertexFormat( void );
	bool ensureArena( Terrain3DRdGpuBuffer &rBuffer,
					  uint32_t				nRequiredBytes,
					  const char		   *pName,
					  t3dRdResourceType_t	resourceType,
					  t3dRdMemoryCategory_t memoryCategory );
	bool rebuildArrays( void );
	bool rebuildSurfaceViews( const t3dRdMeshSourceSnapshot_t *pSnapshot );
	void discardSurfaceViews( void );
	bool uploadChangedChunks( const Terrain3DRdGpuBuffer							   &rBuffer,
							  const PackedByteArray									   &rCurrent,
							  const eastl::vector< uint64_t, Terrain3DEastlAllocator > &rCurrentHashes,
							  const eastl::vector< uint64_t, Terrain3DEastlAllocator > &rPreviousHashes,
							  bool														bForceFull,
							  uint64_t												   &nUploadedBytes );
	void discardArrays( void );
	void setError( const String &rError );

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	Terrain3DRdMeshCache( void );
	~Terrain3DRdMeshCache( void );

	static uint32_t nextArenaCapacity( uint32_t nRequiredBytes, uint32_t nMinimumBytes );
	static uint64_t computeSourceFingerprintForAsset( const Ref< Terrain3DAssets > &rAssets, uint16_t nAssetId );
	static bool		buildSourceSnapshotForAsset( const Ref< Terrain3DAssets > &rAssets,
												 uint16_t					   nAssetId,
												 t3dRdMeshSourceSnapshot_t	  &rResult,
												 uint64_t					   nFingerprint,
												 uint64_t					   nGeneration,
												 String						  *pError = NULL );
	static uint64_t computeSourceFingerprint( const Ref< Terrain3DAssets > &rAssets );
	static uint64_t computeSourceFingerprint( const Ref< Terrain3DAssets >	  &rAssets,
											  const t3dRdExternalMeshSource_t *pExternalMeshes,
											  size_t						   nExternalMeshCount );
	static bool		buildSourceSnapshot( const Ref< Terrain3DAssets > &rAssets,
										 t3dRdMeshSourceSnapshot_t	  &rResult,
										 uint64_t					   nFingerprint,
										 uint64_t					   nGeneration,
										 String						  *pError = NULL,
										 bool						   bIncludeDisabled = false );
	static bool		buildSourceSnapshot( const Ref< Terrain3DAssets >	 &rAssets,
										 const t3dRdExternalMeshSource_t *pExternalMeshes,
										 size_t							  nExternalMeshCount,
										 t3dRdMeshSourceSnapshot_t		 &rResult,
										 uint64_t						  nFingerprint,
										 uint64_t						  nGeneration,
										 String							 *pError = NULL,
										 bool							  bIncludeDisabled = false );

	bool upload( const t3dRdMeshSourceSnapshot_t *pSnapshot );
	void shutdown( void );

	t3dRdMeshHandle_t				getMeshHandle( uint16_t nAssetId, int nLod ) const;
	t3dRdMeshHandle_t				getOcclusionProxyMeshHandle( uint16_t nAssetId ) const;
	t3dRdMeshHandle_t				getMeshHandleBySourceKey( uint64_t nSourceKey ) const;
	const t3dRdMeshSourceLod_t	   *resolveMesh( const t3dRdMeshHandle_t &rHandle ) const;
	t3dRdSurfaceHandle_t			getSurfaceHandle( const t3dRdMeshHandle_t &rMesh, uint32_t nSurface ) const;
	const t3dRdMeshSourceSurface_t *resolveSurface( const t3dRdSurfaceHandle_t &rHandle ) const;

	EA_FORCE_INLINE const t3dRdInstanceMaterial_t *getMaterials(
		void ) const {
		return m_materials.data();
	}
	EA_FORCE_INLINE size_t getMaterialCount(
		void ) const {
		return m_materials.size();
	}
	EA_FORCE_INLINE const t3dRenderingMaterialGpu_t *getVtMaterials(
		void ) const {
		return m_vtMaterials.data();
	}
	EA_FORCE_INLINE size_t getVtMaterialCount(
		void ) const {
		return m_vtMaterials.size();
	}
	EA_FORCE_INLINE const uint64_t *getVtStableMaterialKeys(
		void ) const {
		return m_vtStableKeys.data();
	}
	EA_FORCE_INLINE size_t getVtStableMaterialKeyCount(
		void ) const {
		return m_vtStableKeys.size();
	}
	EA_FORCE_INLINE const t3dRdMeshSourceSurface_t *getSurfaces(
		void ) const {
		return m_surfaces.data();
	}
	EA_FORCE_INLINE size_t getSurfaceCount(
		void ) const {
		return m_surfaces.size();
	}
	EA_FORCE_INLINE size_t getLodCount(
		void ) const {
		return m_lods.size();
	}
	EA_FORCE_INLINE RID getVertexArray(
		void ) const {
		return m_arenaView.vertexRid();
	}
	EA_FORCE_INLINE int64_t getVertexFormat(
		void ) const {
		return m_nVertexFormat;
	}
	EA_FORCE_INLINE RID getIndexArray(
		void ) const {
		return m_arenaView.indexRid();
	}
	EA_FORCE_INLINE RID getSurfaceVertexArray(
		const uint32_t nSurfaceSlot ) const {
		return nSurfaceSlot < m_surfaceViews.size() ? m_surfaceViews[nSurfaceSlot].m_view.vertexRid() : RID();
	}
	EA_FORCE_INLINE RID getSurfaceIndexArray(
		const uint32_t nSurfaceSlot ) const {
		return nSurfaceSlot < m_surfaceViews.size() ? m_surfaceViews[nSurfaceSlot].m_view.indexRid() : RID();
	}
	EA_FORCE_INLINE uint32_t getSurfaceViewCount(
		void ) const {
		return ( uint32_t ) ( m_surfaceViews.size() );
	}
	EA_FORCE_INLINE uint32_t getGeneration(
		void ) const {
		return m_nGeneration;
	}
	EA_FORCE_INLINE uint64_t getSourceGeneration(
		void ) const {
		return m_nSourceGeneration;
	}
	EA_FORCE_INLINE uint64_t getGpuBytes(
		void ) const {
		return ( uint64_t ) ( m_vertexArena.capacityBytes() ) + ( uint64_t ) ( m_index32Arena.capacityBytes() );
	}
	bool					 isReady( void ) const;
	EA_FORCE_INLINE uint64_t getArenaCapacityMissCount(
		void ) const {
		return m_nArenaCapacityMissCount;
	}
	EA_FORCE_INLINE uint64_t getArenaGrowthRequestCount(
		void ) const {
		return m_nArenaGrowthRequestCount;
	}
	EA_FORCE_INLINE uint64_t getArenaRebuildCount(
		void ) const {
		return m_nArenaRebuildCount;
	}
	EA_FORCE_INLINE uint64_t getArenaRebuildFailureCount(
		void ) const {
		return m_nArenaRebuildFailureCount;
	}
	EA_FORCE_INLINE uint64_t getUploadAttemptCount(
		void ) const {
		return m_nUploadAttemptCount;
	}
	EA_FORCE_INLINE uint64_t getUploadFailureCount(
		void ) const {
		return m_nUploadFailureCount;
	}
	EA_FORCE_INLINE String getLastError(
		void ) const {
		return m_lastError;
	}
	EA_FORCE_INLINE uint32_t getVertexCapacityBytes(
		void ) const {
		return m_vertexArena.capacityBytes();
	}
	EA_FORCE_INLINE uint32_t getIndex32CapacityBytes(
		void ) const {
		return m_index32Arena.capacityBytes();
	}
	EA_FORCE_INLINE uint32_t getVertexUsedBytes(
		void ) const {
		return m_vertexArena.usedBytes();
	}
	EA_FORCE_INLINE uint32_t getIndex32UsedBytes(
		void ) const {
		return m_index32Arena.usedBytes();
	}
};

#endif // TERRAIN3D_RD_MESH_CACHE_H
