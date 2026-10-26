#ifndef TERRAIN3D_PAGE_COMPUTE_H
#define TERRAIN3D_PAGE_COMPUTE_H

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_contract.h"
#include <godot_cpp/variant/color.hpp>

class Terrain3DRdGlobalDescriptorCache;
// Explicit render-thread service. Not selected in the shipping renderer before P03.
// All resources belong to Terrain3DRdContext's GLOBAL RenderingDevice.
class Terrain3DPageCompute
{
public:
	enum Operation : uint32_t
	{
		FILL = 0u,
		ADD = 1u,
		HASH = 2u,
		COMPARE = 3u,
		RECT_FILL = 4u,
		RECT_ADD = 5u
	};
	bool initializeCold( Terrain3DRdContext &context, RenderingDevice::DataFormat format );
	bool dispatchHot( Terrain3DRdContext &context,
					  Terrain3DRdTexture &target,
					  Terrain3DRdTexture &reference,
					  uint32_t			  targetLayer,
					  uint32_t			  referenceLayer,
					  uint32_t			  width,
					  uint32_t			  height,
					  Operation			  operation,
					  const Color		 &value,
					  uint64_t			  submissionEpoch );
	// P08: bounded region-local brush rectangles, encoded in the existing
	// 32-byte push constants. Only one-mip integer channel pages may be edited.
	bool dispatchRectHot( Terrain3DRdContext &context,
						  Terrain3DRdTexture &target,
						  Terrain3DRdTexture &reference,
						  uint32_t			  targetLayer,
						  uint32_t			  referenceLayer,
						  uint32_t			  width,
						  uint32_t			  height,
						  uint32_t			  x,
						  uint32_t			  y,
						  uint32_t			  rectWidth,
						  uint32_t			  rectHeight,
						  Operation			  operation,
						  const Color		 &value,
						  uint64_t			  submissionEpoch,
						  uint32_t			  packedControlBits = 0u );
	// Exact full-mip GPU comparison uses one shared view and one dispatch per
	// mip. Caller owns synchronization and advances only after async result.
	bool		dispatchCompareMipHot( Terrain3DRdContext &context,
									   Terrain3DRdTexture &pages,
									   uint32_t			   lhsLayer,
									   uint32_t			   rhsLayer,
									   uint32_t			   mip,
									   uint64_t			   submissionEpoch );
	bool		requestResultAsync( Terrain3DRdContext &context, const Callable &callback ) const;
	static bool comparisonEqual( const PackedByteArray &result );
	// Caller must supply a verified GPU-completed submission epoch.
	void			markCompletedCold( uint64_t confirmedCompletedEpoch );
	PackedByteArray readResultCold( Terrain3DRdContext &context ) const;
	void			shutdownCold( Terrain3DRdContext &context );
	void			abandonDevice();
	bool			ready() const {
		   return m_pipeline.isValid() && m_shader.isValid() && m_result.isValid();
	}
	RenderingDevice::DataFormat format() const {
		return m_format;
	}

private:
	Terrain3DRdShader			m_shader;
	Terrain3DRdShader			m_reduceShader;
	Terrain3DRdPipeline			m_reducePipeline;
	Terrain3DRdShader         m_partialShader;
	Terrain3DRdPipeline       m_partialPipeline;
	Terrain3DRdShader         m_finalShader;
	Terrain3DRdPipeline       m_finalPipeline;
	Terrain3DRdGpuBuffer      m_partialBuffer;
	t3dRdDescriptorLease_t    m_finalLease;
	Terrain3DRdPipeline			m_pipeline;
	Terrain3DRdGpuBuffer		m_result;
	Terrain3DRdTexture			m_targetView;
	Terrain3DRdTexture			m_referenceView;
	t3dRdDescriptorLease_t		m_lease;
	uint64_t					m_nDeviceGeneration = 0u;
	RenderingDevice::DataFormat m_format = RenderingDevice::DATA_FORMAT_MAX;
	uint64_t					m_nLastSubmittedEpoch = 0u;
	bool						m_bSubmitted = false;
	bool						dispatchInternal( Terrain3DRdContext &context,
												  Terrain3DRdTexture &target,
												  Terrain3DRdTexture &reference,
												  uint32_t			  targetLayer,
												  uint32_t			  referenceLayer,
												  uint32_t			  width,
												  uint32_t			  height,
												  Operation			  operation,
												  const Color		 &value,
												  uint64_t			  submissionEpoch,
												  uint32_t			  x,
												  uint32_t			  y,
												  uint32_t			  rectWidth,
												  uint32_t			  rectHeight,
												  uint32_t			  packedControlBits,
												  uint32_t			  mip );
};
#endif // TERRAIN3D_PAGE_COMPUTE_H
