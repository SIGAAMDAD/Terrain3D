// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_RESOURCES_H
#define TERRAIN3D_WORLD_RESOURCES_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/world/terrain_3d_world_publication.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

class Terrain3DWorldResources {
public:
	typedef enum t3dWorldBufferId_e : uint32_t {
		WORLD_BUFFER_VERTICES = 0u,
		WORLD_BUFFER_CLUSTER_VERTEX_INDICES,
		WORLD_BUFFER_TRIANGLES,
		WORLD_BUFFER_CLUSTERS,
		WORLD_BUFFER_CLUSTER_VT_METADATA,
		WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS,
		WORLD_BUFFER_TERRAIN_STRIP_CLUSTER_BOUNDS,
		WORLD_BUFFER_TRANSFORMS_CURRENT,
		WORLD_BUFFER_TRANSFORMS_PREVIOUS,
		WORLD_BUFFER_INSTANCES,
		WORLD_BUFFER_CELLS,
		WORLD_BUFFER_HLOD_RECORDS,
		WORLD_BUFFER_MATERIALS,
		WORLD_BUFFER_FRAME,
		WORLD_BUFFER_VISIBILITY_CANDIDATES,
		WORLD_BUFFER_VISIBLE_ITEMS,
		WORLD_BUFFER_VISIBILITY_COUNTERS,
		WORLD_BUFFER_INDIRECT,
		WORLD_BUFFER_COUNT,
	} t3dWorldBufferId_t;

private:
	Terrain3DRdContext *m_pContext = NULL;
	godot::RID m_hBuffers[WORLD_BUFFER_COUNT];
	uint32_t m_nCapacityBytes[WORLD_BUFFER_COUNT]{};
	uint32_t m_nUsedBytes[WORLD_BUFFER_COUNT]{};
	uint64_t m_nResourceGeneration = 0u;
	uint64_t m_nUploadedPublicationGeneration = 0u;
	uint64_t m_nGpuBufferBytes = 0u;
	uint64_t m_nUploadBytes = 0u;
	uint64_t m_nCapacityGrowthCount = 0u;
	uint32_t m_nTerrainStripBucketCounts[T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT]{};
	uint64_t m_nTerrainStripUsefulVertices = 0u;
	uint64_t m_nTerrainStripFixedSubmittedVertices = 0u;
	uint64_t m_nTerrainStripBucketSubmittedVertices = 0u;
	godot::PackedByteArray m_uploadScratch[WORLD_BUFFER_COUNT];

	static uint32_t requiredBytes( const Terrain3DWorldPublication &rPublication, t3dWorldBufferId_t eBuffer );
	static Terrain3DRdContext::MemoryCategory memoryCategory( t3dWorldBufferId_t eBuffer );
	static const char *bufferName( t3dWorldBufferId_t eBuffer );
	bool ensureBufferCold( t3dWorldBufferId_t eBuffer, uint32_t nRequiredBytes );
	bool ensureScratchCold( t3dWorldBufferId_t eBuffer, uint32_t nRequiredBytes );
	bool uploadBufferWarm( t3dWorldBufferId_t eBuffer, const void *pData, uint32_t nBytes );

public:
	bool ensureCapacityCold( const Terrain3DWorldPublication &rPublication );
	static uint64_t estimatePublicationUploadBytes( const Terrain3DWorldPublication &rPublication );
	bool uploadPublicationWarm( const Terrain3DWorldPublication &rPublication );
	bool uploadFrameHot( const t3dWorldFrame_t &rFrame );
	void shutdownCold( void );
	bool isInitialized( void ) const;
	const godot::RID &getBuffer( t3dWorldBufferId_t eBuffer ) const;
	uint32_t getCapacityBytes( t3dWorldBufferId_t eBuffer ) const;
	uint32_t getUsedBytes( t3dWorldBufferId_t eBuffer ) const;
	uint64_t getResourceGeneration( void ) const;
	uint64_t getUploadedPublicationGeneration( void ) const;
	uint64_t getGpuBufferBytes( void ) const;
	uint64_t getUploadBytes( void ) const;
	uint64_t getCapacityGrowthCount( void ) const;
	uint32_t getTerrainStripBucketCount( uint32_t nBucket ) const;
	uint64_t getTerrainStripUsefulVertices( void ) const;
	uint64_t getTerrainStripFixedSubmittedVertices( void ) const;
	uint64_t getTerrainStripBucketSubmittedVertices( void ) const;
	size_t getRetainedCpuBytes( void ) const;
};

#endif // TERRAIN3D_WORLD_RESOURCES_H
