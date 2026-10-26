// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_RD_CLUSTER_COMPACT_H
#define TERRAIN3D_RD_CLUSTER_COMPACT_H

#include "rendering/rd/forward/terrain_3d_cluster_compact_contract.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_lease_table.h"

class Terrain3DRdClusterCompact
{
	struct DescriptorKey
	{
		godot::RID m_hShader;
		godot::RID m_hSource;
		godot::RID m_hBuffers[6];
		uint64_t m_nGeneration = 0u;
		bool operator==( const DescriptorKey &other ) const {
			if ( m_hShader != other.m_hShader || m_hSource != other.m_hSource ||
				 m_nGeneration != other.m_nGeneration ) return false;
			for ( uint32_t i = 0u; i < 6u; ++i )
				if ( m_hBuffers[i] != other.m_hBuffers[i] ) return false;
			return true;
		}
	};
	Terrain3DRdContext &m_context;
	Terrain3DRdShader m_hShader;
	Terrain3DRdPipeline m_hPipeline;
	Terrain3DRdGpuBuffer m_buffers[6]; // counts, prefix, group sums, group bases, headers, indices
	Terrain3DRdGpuBuffer m_hFallbackHeaders;
	Terrain3DRdGpuBuffer m_hFallbackIndices;
	Terrain3DRdDescriptorLeaseTable< DescriptorKey, 8u > m_computeSets;
	Terrain3DRdDescriptorLeaseTable< DescriptorKey, 16u > m_rasterSets;
	godot::PackedByteArray m_pushBytes;
	godot::RID m_hLastBorrowed;
	uint64_t m_nDeviceGeneration = 0u;
	uint64_t m_nPreparedFrame = 0u;
	uint32_t m_nBorrowedGeneration = 0u;
	uint32_t m_nListCount = 0u;
	uint32_t m_nClusterCount = 0u;
	uint32_t m_nLastTypeStride = 0u;
	uint32_t m_nLastMaxWords = 0u;
	bool m_bReady = false;
	bool m_bConfigured = false;
	bool m_bFallbackConfigured = false;

	bool ensureResources( uint32_t nLists );
	bool ensureFallbackResources();
	DescriptorKey makeKey( const godot::RID &rShader, const godot::RID &rBorrowed ) const;
	godot::RID ensureComputeSet( const godot::RID &rBorrowed );

public:
	explicit Terrain3DRdClusterCompact( Terrain3DRdContext &context );
	~Terrain3DRdClusterCompact();
	bool prepare( const t3dRdForwardPassContext_t &pass );
	godot::RID ensureRasterSet( const godot::RID &rShader );
	void invalidate();
	void clearRasterSetsCold();
	void shutdown();
	bool ready() const { return m_bReady; }
	uint32_t listCount() const { return m_nListCount; }
};
#endif // TERRAIN3D_RD_CLUSTER_COMPACT_H
