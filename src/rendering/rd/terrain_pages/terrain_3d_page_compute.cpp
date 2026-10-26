#include "rendering/rd/terrain_pages/terrain_3d_page_compute.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_recipe.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_cache.h"
#include <string.h>

struct t3dPageComputePush_t
{
	uint32_t width, height, operation, reserved;
	float	 value[4];
};
static_assert( sizeof( t3dPageComputePush_t ) == 32u, "Page compute GLSL push constants" );
// P30b: bounded 16x16 workgroup partials for pages up to 1024x1024.
// A fixed 16 KiB scratch buffer is reserved by the normal page-reduction path.
static constexpr uint32_t T3D_PAGE_PARTIAL_TILE = 16u;
static constexpr uint32_t T3D_PAGE_PARTIAL_MAX_EXTENT = 1024u;
static constexpr uint32_t T3D_PAGE_PARTIAL_CAPACITY =
	( T3D_PAGE_PARTIAL_MAX_EXTENT / T3D_PAGE_PARTIAL_TILE ) *
	( T3D_PAGE_PARTIAL_MAX_EXTENT / T3D_PAGE_PARTIAL_TILE );

bool Terrain3DPageCompute::initializeCold(
	Terrain3DRdContext		   &context,
	RenderingDevice::DataFormat format ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !context.getDevice() || ready() )
		return false;
	if ( format != RenderingDevice::DATA_FORMAT_R32_SFLOAT && format != RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM &&
		 format != RenderingDevice::DATA_FORMAT_R16_UINT && format != RenderingDevice::DATA_FORMAT_R32_UINT )
		return false;
	const t3dRdShaderId_t id =
		format == RenderingDevice::DATA_FORMAT_R32_SFLOAT
			? T3D_RD_SHADER_PAGE_COMPUTE_R32F
			: ( format == RenderingDevice::DATA_FORMAT_R16_UINT
					? T3D_RD_SHADER_PAGE_COMPUTE_R16UI
					: ( format == RenderingDevice::DATA_FORMAT_R32_UINT ? T3D_RD_SHADER_PAGE_COMPUTE_R32UI
																		: T3D_RD_SHADER_PAGE_COMPUTE_RGBA8 ) );
	if ( !m_shader.createTracked( context, T3D_RD_CONTEXT_OWNER_TERRAIN, "T3D.PageCompute.Shader", id ) )
		return false;
	if ( !m_pipeline.createTrackedCompute( context,
										   T3D_RD_CONTEXT_OWNER_TERRAIN,
										   "T3D.PageCompute.Pipeline",
										   m_shader.rid() ) ) {
		shutdownCold( context );
		return false;
	}
	// Hash/compare use one size-based production policy: 8x8 local reduction
	// through 32x32, 16x16 partial/final through 1024x1024, then the
	// existing atomic shader for larger pages. Editing always uses baseline.
	if ( Terrain3DRdShaderLibrary::isPageReductionEnabledCold() ) {
		const t3dRdShaderId_t reduceId = ( t3dRdShaderId_t ) (
			T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE + ( id - T3D_RD_SHADER_PAGE_COMPUTE_R32F ) );
		const bool reduceReady = m_reduceShader.createTracked( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
			"T3D.PageCompute.ReduceShader", reduceId ) &&
			m_reducePipeline.createTrackedCompute( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
			"T3D.PageCompute.ReducePipeline", m_reduceShader.rid() );
		if ( !reduceReady ) {
			m_reducePipeline.releaseTracked( context );
			m_reduceShader.releaseTracked( context );
		}
		const t3dRdShaderId_t partialId = ( t3dRdShaderId_t ) (
			T3D_RD_SHADER_PAGE_COMPUTE_R32F_PARTIAL + ( id - T3D_RD_SHADER_PAGE_COMPUTE_R32F ) );
		t3dRdGpuBufferSpec_t partialSpec;
		partialSpec.m_pName = "T3D.PageCompute.Partials";
		partialSpec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		partialSpec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		partialSpec.m_memory = T3D_RD_CONTEXT_MEMORY_SCRATCH;
		const bool partialReady = m_partialShader.createTracked( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
			"T3D.PageCompute.PartialShader", partialId ) &&
			m_partialPipeline.createTrackedCompute( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
				"T3D.PageCompute.PartialPipeline", m_partialShader.rid() ) &&
			m_finalShader.createTracked( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
				"T3D.PageCompute.FinalShader", T3D_RD_SHADER_PAGE_COMPUTE_REDUCE_FINAL ) &&
			m_finalPipeline.createTrackedCompute( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
				"T3D.PageCompute.FinalPipeline", m_finalShader.rid() ) &&
			m_partialBuffer.configure( partialSpec ) &&
			m_partialBuffer.createTracked( context, T3D_PAGE_PARTIAL_CAPACITY * sizeof( uint32_t ) );
		if ( !partialReady ) {
			m_partialBuffer.releaseTracked( context );
			m_finalPipeline.releaseTracked( context );
			m_finalShader.releaseTracked( context );
			m_partialPipeline.releaseTracked( context );
			m_partialShader.releaseTracked( context );
		}
		// Unsupported kernels or exhausted scratch fall back to the baseline.
	}

	t3dRdGpuBufferSpec_t spec;
	spec.m_pName = "T3D.PageCompute.Result";
	spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
	spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
	spec.m_memory = T3D_RD_CONTEXT_MEMORY_SCRATCH;
	const uint32_t	zeros[2] = { 0u, 0u };
	PackedByteArray bytes;
	bytes.resize( sizeof( zeros ) );
	memcpy( bytes.ptrw(), zeros, sizeof( zeros ) );
	if ( !m_result.configure( spec ) || !m_result.createTracked( context, sizeof( zeros ), bytes ) ) {
		shutdownCold( context );
		return false;
	}
	m_nDeviceGeneration = context.getDeviceGeneration();
	m_format = format;
	return true;
}

bool Terrain3DPageCompute::dispatchHot(
	Terrain3DRdContext &context,
	Terrain3DRdTexture &target,
	Terrain3DRdTexture &reference,
	uint32_t			targetLayer,
	uint32_t			referenceLayer,
	uint32_t			width,
	uint32_t			height,
	Operation			operation,
	const Color		   &value,
	uint64_t			submissionEpoch ) {
	return dispatchRectHot( context,
							target,
							reference,
							targetLayer,
							referenceLayer,
							width,
							height,
							0u,
							0u,
							width,
							height,
							operation,
							value,
							submissionEpoch );
}

bool Terrain3DPageCompute::dispatchRectHot(
	Terrain3DRdContext &context,
	Terrain3DRdTexture &target,
	Terrain3DRdTexture &reference,
	uint32_t			targetLayer,
	uint32_t			referenceLayer,
	uint32_t			width,
	uint32_t			height,
	uint32_t			x,
	uint32_t			y,
	uint32_t			rectWidth,
	uint32_t			rectHeight,
	Operation			operation,
	const Color		   &value,
	uint64_t			submissionEpoch,
	uint32_t			packedControlBits ) {
	return dispatchInternal( context,
							 target,
							 reference,
							 targetLayer,
							 referenceLayer,
							 width,
							 height,
							 operation,
							 value,
							 submissionEpoch,
							 x,
							 y,
							 rectWidth,
							 rectHeight,
							 packedControlBits,
							 0u );
}

bool Terrain3DPageCompute::dispatchCompareMipHot(
	Terrain3DRdContext &context,
	Terrain3DRdTexture &pages,
	uint32_t			lhsLayer,
	uint32_t			rhsLayer,
	uint32_t			mip,
	uint64_t			submissionEpoch ) {
	if ( mip >= pages.layout().m_nMipmaps || lhsLayer == rhsLayer )
		return false;
	const uint32_t width = pages.layout().m_nWidth >> mip;
	const uint32_t height = pages.layout().m_nHeight >> mip;
	return dispatchInternal( context,
							 pages,
							 pages,
							 lhsLayer,
							 rhsLayer,
							 width ? width : 1u,
							 height ? height : 1u,
							 COMPARE,
							 Color(),
							 submissionEpoch,
							 0u,
							 0u,
							 0u,
							 0u,
							 0u,
							 mip );
}

bool Terrain3DPageCompute::dispatchInternal(
	Terrain3DRdContext &context,
	Terrain3DRdTexture &target,
	Terrain3DRdTexture &reference,
	uint32_t			targetLayer,
	uint32_t			referenceLayer,
	uint32_t			width,
	uint32_t			height,
	Operation			operation,
	const Color		   &value,
	uint64_t			submissionEpoch,
	uint32_t			x,
	uint32_t			y,
	uint32_t			rectWidth,
	uint32_t			rectHeight,
	uint32_t			packedControlBits,
	uint32_t			mip ) {
	if ( mip >= target.layout().m_nMipmaps || mip >= reference.layout().m_nMipmaps ||
		 ( mip != 0u && operation != COMPARE ) )
		return false;
	if ( ( operation == RECT_FILL || operation == RECT_ADD )
			 ? ( x > 255u || y > 255u || rectWidth == 0u || rectHeight == 0u || rectWidth > 255u || rectHeight > 255u ||
				 x + rectWidth > width || y + rectHeight > height || width > 255u || height > 255u ||
				 target.layout().m_nMipmaps != 1u )
			 : ( operation > COMPARE ) )
		return false;
	RenderingDevice *const rd = context.getDevice();
	if ( !Terrain3DRdContext::isOnRenderThread() || !rd || !ready() || !submissionEpoch ||
		 context.getDeviceGeneration() != m_nDeviceGeneration || target.layout().m_format != m_format ||
		 reference.layout().m_format != m_format || !target.isValid() || !reference.isValid() || width == 0u ||
		 height == 0u || width > target.layout().m_nWidth || height > target.layout().m_nHeight ||
		 width > reference.layout().m_nWidth || height > reference.layout().m_nHeight ||
		 targetLayer >= target.layout().m_nLayers || referenceLayer >= reference.layout().m_nLayers ||
		 ( target.layout().m_nUsageBits & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT ) == 0u ||
		 ( reference.layout().m_nUsageBits & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT ) == 0u ||
		 !context.ownsResource( target.rid() ) || !context.ownsResource( reference.rid() ) )
		return false;
	Terrain3DRdGlobalDescriptorCache *const cache = context.tryGetGlobalDescriptorCacheHot();
	if ( !cache )
		return false;
	if ( m_targetView.isValid() ) {
		// A dependent descriptor may reference old views. Hold them until a proven
		// GPU-safe quiescence point, not merely until the next frame.
		if ( m_bSubmitted )
			return false;
		cache->releaseDescriptorCold( m_finalLease );
		cache->releaseDescriptorCold( m_lease );
		m_targetView.releaseTracked( context );
		m_referenceView.releaseTracked( context );
	}
	t3dRdTextureSliceSpec_t slice;
	slice.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
	slice.m_name = "T3D.PageCompute.TargetSlice";
	slice.m_nLayer = targetLayer;
	slice.m_nMipmap = mip;
	if ( !m_targetView.createSharedTracked( context, target, slice ) )
		return false;
	slice.m_name = "T3D.PageCompute.ReferenceSlice";
	slice.m_nLayer = referenceLayer;
	if ( !m_referenceView.createSharedTracked( context, reference, slice ) ) {
		m_targetView.releaseTracked( context );
		return false;
	}
	t3dRdDescriptorRecipe_t			 recipe{};
	const t3dRdDescriptorDeviceKey_t device{ T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID,
											 context.getDeviceGeneration(),
											 T3D_RD_DESCRIPTOR_DEVICE_GLOBAL };
	// P30b routes larger hash/compare pages through bounded 16x16 tile
	// partials plus an explicit final reduction. The legacy path remains for
	// pages beyond the sealed scratch capacity and for all edit operations.
	const bool reductionOperation = operation == HASH || operation == COMPARE;
	const bool reduce = reductionOperation && m_reducePipeline.isValid() && m_reduceShader.isValid() &&
		width <= 32u && height <= 32u;
	const bool partial = reductionOperation && !reduce && m_partialPipeline.isValid() &&
		m_finalPipeline.isValid() && m_partialBuffer.isValid() &&
		width <= T3D_PAGE_PARTIAL_MAX_EXTENT && height <= T3D_PAGE_PARTIAL_MAX_EXTENT;
	const RID shaderRid = reduce ? m_reduceShader.rid() : ( partial ? m_partialShader.rid() : m_shader.rid() );
	if ( t3dRdDescriptorRecipeBeginCold( recipe, device, shaderRid, 0u ) != T3D_RD_DESCRIPTOR_OK )
		return false;
	const RID handles[4] = { m_targetView.rid(), m_referenceView.rid(), m_result.rid(), m_partialBuffer.rid() };
	for ( uint32_t i = 0u; i < ( partial ? 4u : 3u ); ++i ) {
		if ( t3dRdDescriptorRecipeAddBindingCold( recipe,
												  i,
												  i >= 2u ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER
														  : RenderingDevice::UNIFORM_TYPE_IMAGE,
												  &handles[i],
												  1u ) != T3D_RD_DESCRIPTOR_OK )
			return false;
	}
	if ( cache->acquireDescriptorCold( recipe, T3D_RD_CONTEXT_OWNER_TERRAIN, m_lease ) != T3D_RD_DESCRIPTOR_OK )
		return false;
	RID set;
	if ( cache->resolveDescriptorHot( m_lease, cache->currentSubmissionEpochHot(), set ) != T3D_RD_DESCRIPTOR_OK ) {
		cache->releaseDescriptorCold( m_lease );
		return false;
	}
	RID finalSet;
	if ( partial ) {
		t3dRdDescriptorRecipe_t finalRecipe{};
		if ( t3dRdDescriptorRecipeBeginCold( finalRecipe, device, m_finalShader.rid(), 0u ) != T3D_RD_DESCRIPTOR_OK ||
			 t3dRdDescriptorRecipeAddBindingCold( finalRecipe, 2u, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
													&handles[2], 1u ) != T3D_RD_DESCRIPTOR_OK ||
			 t3dRdDescriptorRecipeAddBindingCold( finalRecipe, 3u, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
													&handles[3], 1u ) != T3D_RD_DESCRIPTOR_OK ||
			 cache->acquireDescriptorCold( finalRecipe, T3D_RD_CONTEXT_OWNER_TERRAIN, m_finalLease ) != T3D_RD_DESCRIPTOR_OK ||
			 cache->resolveDescriptorHot( m_finalLease, cache->currentSubmissionEpochHot(), finalSet ) != T3D_RD_DESCRIPTOR_OK ) {
			cache->releaseDescriptorCold( m_finalLease );
			cache->releaseDescriptorCold( m_lease );
			return false;
		}
	}
	// Result is initialized for hash/compare in the same render-thread transaction.
	if ( ( operation == HASH || operation == COMPARE ) && !reduce && !partial ) {
		uint32_t		zeros[2] = { 0u, 0u };
		PackedByteArray reset;
		reset.resize( sizeof( zeros ) );
		memcpy( reset.ptrw(), zeros, sizeof( zeros ) );
		if ( rd->buffer_update( m_result.rid(), 0u, sizeof( zeros ), reset ) != OK )
			return false;
	}
	t3dPageComputePush_t push{};
	push.width = width;
	push.height = height;
	push.operation = operation;
	if ( operation == RECT_FILL || operation == RECT_ADD )
		push.reserved = x | ( y << 8u ) | ( rectWidth << 16u ) | ( rectHeight << 24u );
	// Full R32_UINT control bits cannot round-trip through a numerical float.
	// Pass the exact unsigned payload in the existing float push slot's bits.
	if ( m_format == RenderingDevice::DATA_FORMAT_R32_UINT && operation == RECT_FILL )
		memcpy( &push.value[0], &packedControlBits, sizeof( uint32_t ) );
	else
		push.value[0] = value.r;
	push.value[1] = value.g;
	push.value[2] = value.b;
	push.value[3] = value.a;
	PackedByteArray pushBytes;
	pushBytes.resize( sizeof( push ) );
	memcpy( pushBytes.ptrw(), &push, sizeof( push ) );
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID )
		return false;
	rd->compute_list_bind_compute_pipeline( list, reduce ? m_reducePipeline.rid() : ( partial ? m_partialPipeline.rid() : m_pipeline.rid() ) );
	rd->compute_list_bind_uniform_set( list, set, 0u );
	rd->compute_list_set_push_constant( list, pushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list,
		reduce ? 1u : ( partial ? ( width + 15u ) / 16u : ( width + 7u ) / 8u ),
		reduce ? 1u : ( partial ? ( height + 15u ) / 16u : ( height + 7u ) / 8u ), 1u );
	rd->compute_list_add_barrier( list );
	if ( partial ) {
		rd->compute_list_bind_compute_pipeline( list, m_finalPipeline.rid() );
		rd->compute_list_bind_uniform_set( list, finalSet, 0u );
		rd->compute_list_set_push_constant( list, pushBytes, sizeof( push ) );
		rd->compute_list_dispatch( list, 1u, 1u, 1u );
		rd->compute_list_add_barrier( list );
	}
	rd->compute_list_end();
	m_bSubmitted = true;
	m_nLastSubmittedEpoch = submissionEpoch;
	return true;
}
bool Terrain3DPageCompute::requestResultAsync(
	Terrain3DRdContext &context,
	const Callable	   &callback ) const {
	RenderingDevice *const rd = context.getDevice();
	return Terrain3DRdContext::isOnRenderThread() && rd && m_bSubmitted && m_result.isValid() && callback.is_valid() &&
		   context.getDeviceGeneration() == m_nDeviceGeneration &&
		   rd->buffer_get_data_async( m_result.rid(), callback, 0u, 8u ) == OK;
}

bool Terrain3DPageCompute::comparisonEqual(
	const PackedByteArray &result ) {
	if ( result.size() != 8 )
		return false;
	uint32_t difference = UINT32_MAX;
	memcpy( &difference, result.ptr() + sizeof( uint32_t ), sizeof( difference ) );
	return difference == 0u;
}

void Terrain3DPageCompute::markCompletedCold(
	uint64_t confirmedCompletedEpoch ) {
	if ( m_bSubmitted && confirmedCompletedEpoch >= m_nLastSubmittedEpoch )
		m_bSubmitted = false;
}
PackedByteArray Terrain3DPageCompute::readResultCold(
	Terrain3DRdContext &context ) const {
	return !m_bSubmitted && m_result.isValid() ? m_result.readbackTracked( context, 0u, 8u ) : PackedByteArray();
}
void Terrain3DPageCompute::shutdownCold(
	Terrain3DRdContext &context ) {
	// Caller proves that all submitted dispatches have finished before entering.
	if ( Terrain3DRdGlobalDescriptorCache *cache = context.tryGetGlobalDescriptorCacheHot() ) {
		cache->releaseDescriptorCold( m_finalLease );
		cache->releaseDescriptorCold( m_lease );
	}
	m_targetView.releaseTracked( context );
	m_referenceView.releaseTracked( context );
	m_result.releaseTracked( context );
	m_partialBuffer.releaseTracked( context );
	m_finalPipeline.releaseTracked( context );
	m_finalShader.releaseTracked( context );
	m_partialPipeline.releaseTracked( context );
	m_partialShader.releaseTracked( context );
	m_reducePipeline.releaseTracked( context );
	m_reduceShader.releaseTracked( context );
	m_pipeline.releaseTracked( context );
	m_shader.releaseTracked( context );
	m_bSubmitted = false;
	m_nLastSubmittedEpoch = 0u;
	m_nDeviceGeneration = 0u;
	m_format = RenderingDevice::DATA_FORMAT_MAX;
}
void Terrain3DPageCompute::abandonDevice() {
	m_targetView.abandon();
	m_referenceView.abandon();
	m_result.abandon();
	m_partialBuffer.abandon();
	m_finalPipeline.abandon();
	m_finalShader.abandon();
	m_partialPipeline.abandon();
	m_partialShader.abandon();
	m_reducePipeline.abandon();
	m_reduceShader.abandon();
	m_pipeline.abandon();
	m_shader.abandon();
	m_lease = t3dRdDescriptorLease_t{};
	m_finalLease = t3dRdDescriptorLease_t{};
	m_bSubmitted = false;
	m_nLastSubmittedEpoch = 0u;
	m_nDeviceGeneration = 0u;
	m_format = RenderingDevice::DATA_FORMAT_MAX;
}
