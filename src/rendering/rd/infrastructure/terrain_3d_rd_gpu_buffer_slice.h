#ifndef TERRAIN3D_RD_GPU_BUFFER_SLICE_H
#define TERRAIN3D_RD_GPU_BUFFER_SLICE_H

#include "rendering/rd/infrastructure/terrain_3d_rd_gpu_buffer_types.h"
#include "rendering/rd/terrain_3d_rd_context.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <stdint.h>
#include <string.h>

inline bool t3dRdGpuBufferWriteTrackedSlice( Terrain3DRdContext &rContext,
        const t3dRdGpuBufferSlice_t &rSlice,
        const uint32_t nRelativeOffset,
        const void *const pSource,
        const uint32_t nBytes,
        PackedByteArray &rFallbackScratch,
        const Terrain3DRdContext::ResourceOwner owner,
        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) {
	if ( nBytes == 0u ) return true;
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd || !rSlice.m_hBuffer.is_valid() || !pSource || nRelativeOffset > rSlice.m_nCapacityBytes
	        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
	        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset
	        || rFallbackScratch.size() < nBytes ) {
		return false;
	}
	const uint32_t offset = rSlice.m_nOffsetBytes + nRelativeOffset;
	if ( pSource != rFallbackScratch.ptr() ) {
		memcpy( rFallbackScratch.ptrw(), pSource, nBytes );
	}
	const bool success = rd->buffer_update( rSlice.m_hBuffer, offset, nBytes, rFallbackScratch ) == OK;
	if ( success ) {
		rContext.recordBufferUpdate( nBytes, owner, policy );
	}
	return success;
}

inline bool t3dRdGpuBufferWritePackedTrackedSlice( Terrain3DRdContext &rContext,
        const t3dRdGpuBufferSlice_t &rSlice,
        const uint32_t nRelativeOffset,
        const PackedByteArray &rBytes,
        const uint32_t nBytes,
        const Terrain3DRdContext::ResourceOwner owner,
        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) {
	if ( nBytes == 0u ) return true;
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd || !rSlice.m_hBuffer.is_valid() || rBytes.size() < nBytes || nRelativeOffset > rSlice.m_nCapacityBytes
	        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
	        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
		return false;
	}
	const uint32_t offset = rSlice.m_nOffsetBytes + nRelativeOffset;
	const bool success = rd->buffer_update( rSlice.m_hBuffer, offset, nBytes, rBytes ) == OK;
	if ( success ) {
		rContext.recordBufferUpdate( nBytes, owner, policy );
	}
	return success;
}

inline PackedByteArray t3dRdGpuBufferReadbackTrackedSlice( Terrain3DRdContext &rContext,
        const t3dRdGpuBufferSlice_t &rSlice,
        const uint32_t nRelativeOffset = 0u,
        const uint32_t nBytes = 0u ) {
	RenderingDevice *const rd = rContext.getDevice();
	const uint32_t bytes = nBytes == 0u ? rSlice.m_nCapacityBytes - MIN( nRelativeOffset, rSlice.m_nCapacityBytes ) : nBytes;
	if ( !rd || !rSlice.m_hBuffer.is_valid() || nRelativeOffset > rSlice.m_nCapacityBytes
	        || bytes > rSlice.m_nCapacityBytes - nRelativeOffset
	        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
		return PackedByteArray();
	}
	return rd->buffer_get_data( rSlice.m_hBuffer, rSlice.m_nOffsetBytes + nRelativeOffset, bytes );
}

#endif // TERRAIN3D_RD_GPU_BUFFER_SLICE_H
