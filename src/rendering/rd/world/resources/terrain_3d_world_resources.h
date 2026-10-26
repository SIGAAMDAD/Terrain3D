// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_RESOURCES_H
#define TERRAIN3D_WORLD_RESOURCES_H

#include "Terrain3DTypes.h"

#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/world/runtime/terrain_3d_world_publication.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

class Terrain3DVirtualTexture;
class Terrain3DRdGpuBufferBatch;

class Terrain3DWorldResources
{
private:
	Terrain3DRdContext *m_pContext = NULL;
	using Arena = Terrain3DRdContext::SharedArena;
	t3dRdGpuBufferSlice_t	  m_coldSpan, m_scratchSpan;
	uint32_t				  m_hotIndex = UINT32_MAX;
	t3dRdGpuBufferSlice_t	  m_sections[WORLD_BUFFER_COUNT]{};
	static constexpr uint32_t COLD_HEADER_BYTES = 0u;
	static Arena			  arenaFor(
					 t3dWorldBufferId_t id ) {
		return id == WORLD_BUFFER_FRAME ? Terrain3DRdContext::HOT
			   : id == WORLD_BUFFER_VISIBLE_ITEMS || id == WORLD_BUFFER_VISIBILITY_COUNTERS
				   ? Terrain3DRdContext::SCRATCH
				   : Terrain3DRdContext::COLD;
	}
	Terrain3DRdGpuBuffer	  *m_pFixedIndirectBuffer = NULL;
	t3dRdGpuBufferSlice_t	   m_fixedIndirectSlice;
	uint64_t				   m_nResourceGeneration = 0u;
	uint64_t				   m_nUploadedPublicationGeneration = 0u;
	uint64_t m_nUploadedGeometryCacheEpoch = 0u;
    uint64_t m_nUploadedGeometryMutationRevision = 0u;
    const Terrain3DWorldPublication *m_pPendingGeometryPublication = NULL;
	uint64_t m_nPendingGeometryCacheEpoch = 0u;
	uint64_t m_nGeometryDeltaUploadBytes = 0u;
	uint64_t m_nGeometryFullUploadBytes = 0u;
	uint64_t m_nGeometryDeltaUploadCount = 0u;
	uint64_t m_nGeometryFullUploadCount = 0u;
	uint64_t m_nQueuedSectionBytes[WORLD_BUFFER_COUNT]{};
	bool m_bQueuedGeometryDeltas[WORLD_BUFFER_COUNT]{};
	uint64_t				   m_nGpuBufferBytes = 0u;
	uint64_t				   m_nUploadBytes = 0u;
	uint64_t				   m_nSkippedPublicationUploadBytes = 0u;
	bool					   m_bPublicationUploadCached[WORLD_BUFFER_COUNT]{};
	uint64_t				   m_nCapacityGrowthCount = 0u;
	uint32_t				   m_nTerrainStripBucketCounts[T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT]{};
	uint64_t				   m_nTerrainStripUsefulVertices = 0u;
	uint64_t				   m_nTerrainStripFixedSubmittedVertices = 0u;
	uint64_t				   m_nTerrainStripBucketSubmittedVertices = 0u;
	godot::PackedByteArray	   m_materialShadow, m_transfer;
	Terrain3DMemoryReservation m_shadowMemory, m_transferMemory;
	Terrain3DRdGpuBufferBatch *m_pPublicationBatch = NULL;
	bool					   m_dirtySections[WORLD_BUFFER_COUNT]{};
	uint64_t				   m_sectionSignatures[WORLD_BUFFER_COUNT]{};
	uint64_t				   m_nStagingReallocations = 0u;

	Terrain3DRdGpuBuffer &bufferFor(
		t3dWorldBufferId_t id ) const {
		return m_pContext->arena( id == WORLD_BUFFER_FRAME ? Terrain3DRdContext::HOT
								  : arenaFor( id ) == Terrain3DRdContext::COLD && id != WORLD_BUFFER_INDIRECT
									  ? Terrain3DRdContext::COLD
									  : Terrain3DRdContext::SCRATCH );
	}
	bool uploadBufferWarm( t3dWorldBufferId_t eBuffer, const void *pData, uint32_t nBytes );

public:
	static constexpr uint32_t TRANSFER_BYTES = 1024u * 1024u;
	static uint64_t			  estimateColdStagingBytes( const t3dWorldResourceLimits_t &limits );
	// Cold control-plane budget; retains maximum capacities for the selected profile.
	static uint32_t estimateSharedColdArenaBytes( const t3dWorldResourceLimits_t &limits );
	bool ensureCapacityCold( const Terrain3DWorldPublication &rPublication, const t3dWorldResourceLimits_t &limits );
	static uint64_t estimatePublicationUploadBytes( const Terrain3DWorldPublication &rPublication );
	// Exact preflight for the present GPU publication state. No staging or mutation.
	uint64_t estimateChangedPublicationUploadBytes( const Terrain3DWorldPublication &rPublication,
													bool							 bResolveVtMaterials,
													uint32_t						&rChangedClusterCount ) const;
	bool	 uploadPublicationWarm( const Terrain3DWorldPublication &rPublication,
									const Terrain3DVirtualTexture	*pVirtualTexture = NULL );
	bool	 uploadResolvedMaterialsWarm( const Terrain3DWorldPublication &rPublication,
										  const Terrain3DVirtualTexture	  &rVirtualTexture );
	bool	 uploadFrameHot( const t3dWorldFrame_t &rFrame );
	uint32_t hotIndex() const {
		return m_hotIndex;
	}
	uint32_t bufferOffset(
		t3dWorldBufferId_t id ) const {
		return id == WORLD_BUFFER_INDIRECT ? m_fixedIndirectSlice.m_nOffsetBytes : m_sections[id].m_nOffsetBytes;
	}
	void shutdownCold( void );
	// Do not recycle these spans during lane recovery; final owner teardown only.
	void releaseArenaSpansFinal( void );
	bool isInitialized( void ) const;
	// Copy the resolved CPU upload record for a render-thread residency request.
	bool			  getResolvedMaterialHot( uint32_t nMaterial, t3dRenderingMaterialGpu_t &rMaterial ) const;
	const godot::RID &getBuffer( t3dWorldBufferId_t eBuffer ) const;
	uint32_t		  getCapacityBytes( t3dWorldBufferId_t eBuffer ) const;
	uint32_t		  getUsedBytes( t3dWorldBufferId_t eBuffer ) const;
	uint64_t		  getResourceGeneration(
				 void ) const {
		return m_nResourceGeneration;
	}
	uint64_t getUploadedPublicationGeneration(
		void ) const {
		return m_nUploadedPublicationGeneration;
	}
	uint64_t getGpuBufferBytes(
		void ) const {
		return m_nGpuBufferBytes;
	}
	uint64_t getUploadBytes(
		void ) const {
		return m_nUploadBytes;
	}
	uint64_t getSkippedPublicationUploadBytes(
		void ) const {
		return m_nSkippedPublicationUploadBytes;
	}
	uint64_t getCapacityGrowthCount(
		void ) const {
		return m_nCapacityGrowthCount;
	}
	uint32_t getTerrainStripBucketCount( uint32_t nBucket ) const;
	uint64_t getTerrainStripUsefulVertices(
		void ) const {
		return m_nTerrainStripUsefulVertices;
	}
	uint64_t getTerrainStripFixedSubmittedVertices(
		void ) const {
		return m_nTerrainStripFixedSubmittedVertices;
	}
	uint64_t getTerrainStripBucketSubmittedVertices(
		void ) const {
		return m_nTerrainStripBucketSubmittedVertices;
	}
	size_t	   getRetainedCpuBytes( void ) const;
	Dictionary getArenaDiagnostics() const;
};

#endif // TERRAIN3D_WORLD_RESOURCES_H
