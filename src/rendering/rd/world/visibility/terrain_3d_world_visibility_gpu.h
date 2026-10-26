// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_VISIBILITY_GPU_H
#define TERRAIN3D_WORLD_VISIBILITY_GPU_H

#include "Terrain3DTypes.h"

#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/world/visibility/terrain_3d_world_hiz.h"
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_lease_table.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>

class Terrain3DWorldVisibilityGpu
{
private:
	typedef struct t3dDescriptorKey_s
	{
		godot::RID shader;
		class Signature
		{
		public:
			uint64_t resources = 0u;
			uint64_t hiz = 0u;
			bool	 operator==(
				const Signature &rOther ) const {
				return resources == rOther.resources && hiz == rOther.hiz;
			}
		} signature;
		bool operator==(
			const t3dDescriptorKey_s &rOther ) const {
			return shader == rOther.shader && signature == rOther.signature;
		}
	} t3dDescriptorKey_t;

	// P28b fixed-capacity, owned scratch: 64K candidates, 1024 groups,
	// 4 words per candidate and 22 words per group (counts/reject/prefix/results).
	static constexpr uint32_t SCAN_MAX_CANDIDATES = 65536u;
	static constexpr uint32_t SCAN_MAX_GROUPS = 1024u;
	static constexpr uint32_t SCAN_WORDS = SCAN_MAX_CANDIDATES * 4u + SCAN_MAX_GROUPS * 22u;
	Terrain3DRdGpuBuffer m_scanScratch;
	Terrain3DRdContext													 &m_context;
	Terrain3DRdShader													  m_hShader[PIPELINE_COUNT];
	godot::RID															  m_hPipeline[PIPELINE_COUNT];
	Terrain3DRdPipelineCache< godot::RID, PIPELINE_COUNT >				  m_pipelineCache;
	Terrain3DRdDescriptorLeaseTable< t3dDescriptorKey_t, PIPELINE_COUNT > m_descriptorCache;
	godot::RID															  m_hSet[PIPELINE_COUNT];

	godot::PackedByteArray m_pushBytes;

	t3dDescriptorKey_t::Signature m_boundSignature;
	uint64_t					  m_nPatchBoundsGeneration = 0u;
	uint32_t					  m_nCachedPatchCount = UINT32_MAX;
	// Only CPU geometry is cached. Each pass still writes its own HOT record,
	// since Forward+ gives each pass a distinct dynamic-buffer index.
	t3dWorldTerrainClusterBounds_t m_szCachedPatchBounds[T3D_MAX_CLIPMAP_LEVELS]{};
	uint64_t					   m_nDispatchCount = 0u;
	uint64_t					   m_nPreparedFrameCount = 0u;
	uint64_t					   m_nSetRebuildCount = 0u;
	uint64_t					   m_nFailureCount = 0u;
	uint32_t					   m_nLastCandidateCount = 0u;
	bool						   m_bInitialized = false;

	bool createPipelineCold( t3dVisibilityPipeline_t ePipeline );
	bool createSetsCold( Terrain3DWorldResources &rResources, const Terrain3DWorldHiZ &rHiZ );
	void freeSetsCold( void );

public:
	explicit Terrain3DWorldVisibilityGpu( Terrain3DRdContext &rContext );
	~Terrain3DWorldVisibilityGpu( void );
	bool	 initializeCold( void );
	void	 shutdownCold( void );
	bool	 prepareBindingsCold( Terrain3DWorldResources &rResources, const Terrain3DWorldHiZ &rHiZ );
	bool	 dispatchHot( Terrain3DWorldResources	 &rResources,
						  const Terrain3DWorldHiZ	 &rHiZ,
						  const t3dRdFrameSnapshot_t &rSnapshot,
						  uint32_t					  nCandidateCount,
						  uint32_t					  nVisibilityMask,
						  uint32_t					  nPassKind,
						  bool						  bHiZValid );
	bool	 isReady( void ) const;
	uint64_t getDispatchCount( void ) const;
	uint64_t getPreparedFrameCount( void ) const;
	uint64_t getSetRebuildCount( void ) const;
	uint64_t getFailureCount( void ) const;
	uint32_t getLastCandidateCount( void ) const;
};

#endif // TERRAIN3D_WORLD_VISIBILITY_GPU_H
