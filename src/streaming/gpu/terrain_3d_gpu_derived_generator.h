#ifndef TERRAIN3D_GPU_DERIVED_GENERATOR_H
#define TERRAIN3D_GPU_DERIVED_GENERATOR_H

#include "streaming/gpu/terrain_3d_gpu_page_store.h"
#include "streaming/gpu/terrain_3d_gpu_dirty_ledger.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_contract.h"

// Optional P09 render-thread producer: R16_UINT height -> RG8 normal X/Z.
// Updates an unpublished destination and commits only after confirmed GPU
// completion. The old derived page remains sampled while compute runs.
class Terrain3DGpuDerivedGenerator
{
public:
	bool initializeCold( Terrain3DRdContext &context );
	bool dispatchHot( Terrain3DRdContext	  &context,
					  Terrain3DGpuPageStore	  &store,
					  Terrain3DGpuDirtyLedger &dirty,
					  uint64_t				   submissionEpoch,
					  float					   spacing,
					  float					   heightMin,
					  float					   heightRange );
	bool completeCold( Terrain3DRdContext	   &context,
					   Terrain3DGpuPageStore   &store,
					   Terrain3DGpuDirtyLedger &dirty,
					   uint64_t					completedEpoch );
	void shutdownCold( Terrain3DRdContext &context );
	void abandonDevice();
	bool busy() const {
		return m_busy;
	}
	static bool validDimensions( uint32_t w, uint32_t h );

private:
	struct Source
	{
		t3dGpuPageTicket_t ticket;
		t3dGpuPageHandle_t handle;
		bool			   present = false;
	};
	bool				   resolveHeight( Terrain3DGpuPageStore &store, uint16_t region, Source &source );
	void				   releaseViews( Terrain3DRdContext &context );
	Terrain3DRdShader	   m_shader;
	Terrain3DRdPipeline	   m_pipeline;
	Terrain3DRdTexture	   m_views[4];
	t3dRdDescriptorLease_t m_lease;
	Source				   m_sources[3]{};
	t3dGpuPageTicket_t	   m_outputTicket;
	t3dGpuPageTicket_t	   m_dirtyTicket;
	uint64_t			   m_submissionEpoch = 0u;
	uint64_t			   m_deviceGeneration = 0u;
	bool				   m_busy = false;
};
#endif
