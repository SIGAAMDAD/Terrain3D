#include "streaming/residency/terrain_3d_region_residency.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

godot::Dictionary Terrain3DRegionResidency::getSingleCopyBaselineStats() const {
	godot::Dictionary result;
	const uint64_t	  gpuBytes = m_pGpuRegionCache ? m_pGpuRegionCache->getReservedBytes() : 0u;
	const uint64_t	  cpuBytes = m_pCpuRegionCache ? m_pCpuRegionCache->getReservedBytes() : 0u;
	const uint64_t	  activeBytes = m_pSamplingStorage ? m_pSamplingStorage->getActiveReservedBytes() : 0u;
	if ( gpuBytes > m_nBaselineGpuPeakBytes )
		m_nBaselineGpuPeakBytes = gpuBytes;
	if ( cpuBytes > m_nBaselineCpuPeakBytes )
		m_nBaselineCpuPeakBytes = cpuBytes;
	static const char *const channelNames[T3D_GPU_PAGE_CHANNEL_COUNT] =
		{ "height", "control", "color", "derived", "middle_composite", "shadow_mask" };
	godot::Dictionary channelLive, channelPeak;
	for ( uint32_t i = 0u; i < T3D_GPU_PAGE_CHANNEL_COUNT; ++i ) {
		const uint64_t current = m_pGpuRegionCache ? m_pGpuRegionCache->getPhysicalChannelBytes( i ) : 0u;
		if ( current > m_szBaselineChannelPeakBytes[i] )
			m_szBaselineChannelPeakBytes[i] = current;
		channelLive[channelNames[i]] = ( int64_t ) current;
		channelPeak[channelNames[i]] = ( int64_t ) m_szBaselineChannelPeakBytes[i];
	}
	result["gpu_channels_live_bytes"] = channelLive;
	result["gpu_channels_peak_observed_bytes"] = channelPeak;
	result["gpu_authoritative_pages"] = m_gpuPagePlan.enabled();
	result["gpu_region_reserved_bytes"] = ( int64_t ) gpuBytes;
	result["gpu_region_peak_observed_bytes"] = ( int64_t ) m_nBaselineGpuPeakBytes;
	result["cpu_region_reserved_bytes"] = ( int64_t ) cpuBytes;
	result["cpu_region_peak_observed_bytes"] = ( int64_t ) m_nBaselineCpuPeakBytes;
	result["cpu_active_height_reserved_bytes"] = ( int64_t ) activeBytes;
	result["pending_page_requests"] = ( int64_t ) m_gpuPagePlan.queued();
	result["dropped_page_requests"] = ( int64_t ) m_gpuPagePlan.dropped();
	result["page_device_epoch"] = ( int64_t ) m_gpuPagePlan.epoch();
	if ( m_pRuntimePageStore ) {
		const auto prepared = m_pRuntimePageStore->getStats();
		result["cooked_prepared_bytes"] = ( int64_t ) prepared.m_nPreparedReservedBytes;
		result["cooked_worker_scratch_bytes"] = ( int64_t ) prepared.m_nWorkerScratchReservedBytes;
	}
	if ( m_pGpuRegionCache ) {
		const godot::Dictionary gpu = m_pGpuRegionCache->getStatsDictionary( false );
		result["gpu_upload_bytes_last_tick"] = gpu.get( "upload_bytes_last_tick", ( int64_t ) 0 );
		result["gpu_retired_layers"] = gpu.get( "retired_layers", 0 );
		result["gpu_physical_layers"] = gpu.get( "physical_layers", 0 );
		result["gpu_color_resolution_divisor"] = gpu.get( "color_resolution_divisor", 1 );
	}
	if ( m_pCpuRegionCache ) {
		const godot::Dictionary cpu = m_pCpuRegionCache->getStatsDictionary( false, 0 );
		result["cpu_decoded_estimated_total_map_bytes"] = cpu.get( "estimated_total_map_bytes", ( int64_t ) 0 );
		result["cpu_decoded_estimated_pinned_bytes"] = cpu.get( "estimated_pinned_bytes", ( int64_t ) 0 );
	}
	result["source"] = "P00 owner-allocated baseline; legacy source is unchanged";
	return result;
}
