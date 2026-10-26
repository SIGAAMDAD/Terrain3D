#include "streaming/gpu/terrain_3d_gpu_derived_generator.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_recipe.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_cache.h"
#include <math.h>
#include <string.h>

struct t3dGpuDerivedPush_t
{
	uint32_t width, height, rightPresent, downPresent;
	float	 spacing, heightMin, heightRange, reserved;
};
static_assert( sizeof( t3dGpuDerivedPush_t ) == 32u, "GPU derived push ABI" );

bool Terrain3DGpuDerivedGenerator::validDimensions(
	uint32_t w,
	uint32_t h ) {
	return w == 128u && h == 128u;
}

bool Terrain3DGpuDerivedGenerator::initializeCold(
	Terrain3DRdContext &context ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !context.getDevice() || m_pipeline.isValid() || m_shader.isValid() )
		return false;
	if ( !m_shader.createTracked( context,
								  T3D_RD_CONTEXT_OWNER_TERRAIN,
								  "T3D.GpuDerived.Shader",
								  T3D_RD_SHADER_PAGE_DERIVED_GPU ) )
		return false;
	if ( !m_pipeline.createTrackedCompute( context,
										   T3D_RD_CONTEXT_OWNER_TERRAIN,
										   "T3D.GpuDerived.Pipeline",
										   m_shader.rid() ) ) {
		m_shader.releaseTracked( context );
		return false;
	}
	m_deviceGeneration = context.getDeviceGeneration();
	return true;
}
bool Terrain3DGpuDerivedGenerator::resolveHeight(
	Terrain3DGpuPageStore &store,
	uint16_t			   region,
	Source				  &source ) {
	source = Source{};
	if ( region >= T3D_REGION_SLOTS )
		return false;
	const t3dGpuPageLogicalKey_t key{ region, T3D_GPU_PAGE_HEIGHT, 0u };
	source.present = store.publishedSnapshot( key, source.ticket, source.handle );
	return source.present;
}
void Terrain3DGpuDerivedGenerator::releaseViews(
	Terrain3DRdContext &context ) {
	if ( Terrain3DRdGlobalDescriptorCache *cache = context.tryGetGlobalDescriptorCacheHot() )
		cache->releaseDescriptorCold( m_lease );
	m_lease = t3dRdDescriptorLease_t{};
	for ( uint32_t i = 0u; i < 4u; ++i )
		m_views[i].releaseTracked( context );
}
bool Terrain3DGpuDerivedGenerator::dispatchHot(
	Terrain3DRdContext		&context,
	Terrain3DGpuPageStore	&store,
	Terrain3DGpuDirtyLedger &dirty,
	uint64_t				 submissionEpoch,
	float					 spacing,
	float					 heightMin,
	float					 heightRange ) {
	RenderingDevice *const rd = context.getDevice();
	if ( !Terrain3DRdContext::isOnRenderThread() || !rd || m_busy || !m_shader.isValid() || !m_pipeline.isValid() ||
		 m_deviceGeneration != context.getDeviceGeneration() || !store.active() || !submissionEpoch ||
		 !isfinite( spacing ) || spacing <= 0.0f || !isfinite( heightMin ) || !isfinite( heightRange ) ||
		 heightRange <= 0.0f )
		return false;
	// Scan past pending regions that have not yet entered the physical cache.
	// A missing page must not starve later ready derived-map requests.
	uint16_t		   region = UINT16_MAX;
	t3dGpuPageTicket_t dirtyTicket;
	bool			   found = false;
	uint32_t		   cursor = 0u;
	m_sources[0] = Source{};
	m_sources[1] = Source{};
	m_sources[2] = Source{};
	while ( cursor < T3D_REGION_SLOTS ) {
		if ( !dirty.nextFrom( Terrain3DGpuDirtyLedger::DERIVED, cursor, region, dirtyTicket ) )
			break;
		cursor = uint32_t( region ) + 1u;
		if ( dirtyTicket.m_key.m_nChannel != T3D_GPU_PAGE_HEIGHT )
			continue;
		if ( resolveHeight( store, region, m_sources[0] ) &&
			 t3dGpuPageTicketCurrent( dirtyTicket, m_sources[0].ticket ) ) {
			found = true;
			break;
		}
	}
	if ( !found )
		return false;
	const auto &height = store.pool( T3D_GPU_PAGE_HEIGHT );
	const auto &derived = store.pool( T3D_GPU_PAGE_DERIVED );
	const auto &hc = height.configuration();
	const auto &dc = derived.configuration();
	if ( !validDimensions( hc.width, hc.height ) || dc.width != hc.width || dc.height != hc.height ||
		 hc.format != RenderingDevice::DATA_FORMAT_R16_UINT || hc.mipmaps != 1u || !hc.storageWrites ||
		 dc.format != RenderingDevice::DATA_FORMAT_R8G8_UNORM || dc.mipmaps != 1u || !dc.storageWrites )
		return false;
	// The last row/column samples the adjacent region if one is resident;
	// otherwise the current region edge is clamped, as in CPU halo fallback.
	const uint32_t x = region % T3D_REGION_MAP_SIZE;
	const uint32_t y = region / T3D_REGION_MAP_SIZE;
	const bool	   requireRight = x + 1u < T3D_REGION_MAP_SIZE;
	const bool	   requireDown = y + 1u < T3D_REGION_MAP_SIZE;
	m_sources[1] = Source{};
	m_sources[2] = Source{};
	// If a neighbor is not resident, clamp to this region's last sample.
	// Never stall the entire dirty queue on a missing neighbor.
	if ( requireRight )
		resolveHeight( store, uint16_t( region + 1u ), m_sources[1] );
	if ( requireDown )
		resolveHeight( store, uint16_t( region + T3D_REGION_MAP_SIZE ), m_sources[2] );
	t3dGpuPageTicket_t output = dirtyTicket;
	output.m_key.m_nChannel = T3D_GPU_PAGE_DERIVED;
	output.m_nDeviceEpoch = derived.deviceEpoch();
	if ( !store.stageComputedDerived( output, submissionEpoch ) )
		return false;
	t3dGpuPageHandle_t destination;
	if ( !store.stagedSnapshot( output, destination ) ) {
		store.cancelStaged( output.m_key );
		return false;
	}
	// Every source remains allocated until at least this compute submission.
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		if ( m_sources[i].present &&
			 !store.pool( T3D_GPU_PAGE_HEIGHT ).pinUntil( m_sources[i].handle, submissionEpoch ) ) {
			store.cancelStaged( output.m_key );
			return false;
		}
	}
	Terrain3DRdGlobalDescriptorCache *const cache = context.tryGetGlobalDescriptorCacheHot();
	if ( !cache ) {
		store.cancelStaged( output.m_key );
		return false;
	}
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		const bool				out = i == 3u;
		const auto			   &source = i == 1u && !m_sources[1].present
											 ? m_sources[0]
											 : ( i == 2u && !m_sources[2].present ? m_sources[0] : m_sources[i < 3u ? i : 0u] );
		t3dRdTextureSliceSpec_t slice;
		slice.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		slice.m_name = out ? "T3D.GpuDerived.Destination" : "T3D.GpuDerived.Source";
		slice.m_nLayer = out ? destination.m_nPhysicalLayer : source.handle.m_nPhysicalLayer;
		Terrain3DRdTexture &parent = out ? store.pool( T3D_GPU_PAGE_DERIVED ).mutableTexture()
										 : store.pool( T3D_GPU_PAGE_HEIGHT ).mutableTexture();
		if ( !m_views[i].createSharedTracked( context, parent, slice ) ) {
			releaseViews( context );
			store.cancelStaged( output.m_key );
			return false;
		}
	}
	t3dRdDescriptorRecipe_t			 recipe{};
	const t3dRdDescriptorDeviceKey_t device{ T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID,
											 context.getDeviceGeneration(),
											 T3D_RD_DESCRIPTOR_DEVICE_GLOBAL };
	if ( t3dRdDescriptorRecipeBeginCold( recipe, device, m_shader.rid(), 0u ) != T3D_RD_DESCRIPTOR_OK ) {
		releaseViews( context );
		store.cancelStaged( output.m_key );
		return false;
	}
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		const RID id = m_views[i].rid();
		if ( t3dRdDescriptorRecipeAddBindingCold( recipe, i, RenderingDevice::UNIFORM_TYPE_IMAGE, &id, 1u ) !=
			 T3D_RD_DESCRIPTOR_OK ) {
			releaseViews( context );
			store.cancelStaged( output.m_key );
			return false;
		}
	}
	if ( cache->acquireDescriptorCold( recipe, T3D_RD_CONTEXT_OWNER_TERRAIN, m_lease ) != T3D_RD_DESCRIPTOR_OK ) {
		releaseViews( context );
		store.cancelStaged( output.m_key );
		return false;
	}
	RID set;
	if ( cache->resolveDescriptorHot( m_lease, cache->currentSubmissionEpochHot(), set ) != T3D_RD_DESCRIPTOR_OK ) {
		releaseViews( context );
		store.cancelStaged( output.m_key );
		return false;
	}
	t3dGpuDerivedPush_t push{};
	push.width = hc.width;
	push.height = hc.height;
	push.rightPresent = m_sources[1].present ? 1u : 0u;
	push.downPresent = m_sources[2].present ? 1u : 0u;
	push.spacing = spacing;
	push.heightMin = heightMin;
	push.heightRange = heightRange;
	PackedByteArray bytes;
	bytes.resize( sizeof( push ) );
	memcpy( bytes.ptrw(), &push, sizeof( push ) );
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		releaseViews( context );
		store.cancelStaged( output.m_key );
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_pipeline.rid() );
	rd->compute_list_bind_uniform_set( list, set, 0u );
	rd->compute_list_set_push_constant( list, bytes, sizeof( push ) );
	rd->compute_list_dispatch( list, ( hc.width + 7u ) / 8u, ( hc.height + 7u ) / 8u, 1u );
	rd->compute_list_add_barrier( list );
	rd->compute_list_end();
	m_busy = true;
	m_outputTicket = output;
	m_dirtyTicket = dirtyTicket;
	m_submissionEpoch = submissionEpoch;
	return true;
}
bool Terrain3DGpuDerivedGenerator::completeCold(
	Terrain3DRdContext		&context,
	Terrain3DGpuPageStore	&store,
	Terrain3DGpuDirtyLedger &dirty,
	uint64_t				 completedEpoch ) {
	if ( !Terrain3DRdContext::isOnRenderThread() || !m_busy || !completedEpoch || completedEpoch < m_submissionEpoch )
		return false;
	bool current = store.snapshotCurrent( m_sources[0].ticket, m_sources[0].handle );
	for ( uint32_t i = 1u; i < 3u; ++i )
		if ( m_sources[i].present )
			current = current && store.snapshotCurrent( m_sources[i].ticket, m_sources[i].handle );
	current = current && dirty.isPending( Terrain3DGpuDirtyLedger::DERIVED, m_dirtyTicket );
	if ( current )
		current = store.commitComputedDerived( m_outputTicket, m_sources[0].ticket, completedEpoch );
	if ( current )
		current = dirty.acknowledge( Terrain3DGpuDirtyLedger::DERIVED, m_dirtyTicket );
	if ( !current )
		store.cancelStaged( m_outputTicket.m_key );
	releaseViews( context );
	m_sources[0] = Source{};
	m_sources[1] = Source{};
	m_sources[2] = Source{};
	m_busy = false;
	m_submissionEpoch = 0u;
	return current;
}
void Terrain3DGpuDerivedGenerator::shutdownCold(
	Terrain3DRdContext &context ) {
	if ( m_busy )
		return; // only safe after confirmed completion
	releaseViews( context );
	m_pipeline.releaseTracked( context );
	m_shader.releaseTracked( context );
	m_deviceGeneration = 0u;
}
void Terrain3DGpuDerivedGenerator::abandonDevice() {
	for ( uint32_t i = 0u; i < 4u; ++i )
		m_views[i].abandon();
	m_lease = t3dRdDescriptorLease_t{};
	m_pipeline.abandon();
	m_shader.abandon();
	m_busy = false;
	m_submissionEpoch = 0u;
	m_deviceGeneration = 0u;
}
