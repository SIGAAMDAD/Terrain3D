#ifndef TERRAIN3D_GPU_UPLOAD_SERVICE_H
#define TERRAIN3D_GPU_UPLOAD_SERVICE_H

#include "rendering/rd/infrastructure/terrain_3d_gpu_buffer_layout.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_persistent.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <string.h>

// Render-thread upload execution for already allocated slices. The caller owns
// its preallocated fallback scratch; this service never resizes it in a frame.
class Terrain3DGpuUploadService
{
	Terrain3DRdContext &m_context;
	Terrain3DRdContext::ResourceOwner m_owner;
	Terrain3DGpuBufferPolicy m_policy;

public:
	explicit Terrain3DGpuUploadService( Terrain3DRdContext &rContext,
	        Terrain3DRdContext::ResourceOwner owner = Terrain3DRdContext::OWNER_UNSPECIFIED,
	        Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) :
	        m_context( rContext ), m_owner( owner ), m_policy( policy ) {}

	bool writeWhole( const RID &rBuffer,
	        uint32_t nCapacityBytes,
	        const void *pSource,
	        uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        bool bPersistent = false ) const {
		const t3dGpuBufferSlice_t slice { rBuffer, 0u, nCapacityBytes, nCapacityBytes, 1u, 0u };
		return write( slice, 0u, pSource, nBytes, rFallbackScratch, bPersistent );
	}

	bool writePacked( const t3dGpuBufferSlice_t &rSlice,
	        uint32_t nRelativeOffset,
	        const PackedByteArray &rBytes,
	        uint32_t nBytes,
	        bool bPersistent = false ) const {
		if ( nBytes == 0u ) {
			return true;
		}
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd || !rSlice.m_hBuffer.is_valid() || rBytes.size() < nBytes || nRelativeOffset > rSlice.m_nCapacityBytes
		        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
			return false;
		}
		const uint32_t offset = rSlice.m_nOffsetBytes + nRelativeOffset;
		const bool success = bPersistent ? t3dRdPersistentWrite( rd, rSlice.m_hBuffer, rBytes.ptr(), nBytes, offset )
		                                 : rd->buffer_update( rSlice.m_hBuffer, offset, nBytes, rBytes ) == OK;
		if ( success ) {
			m_context.recordBufferUpdate( nBytes, m_owner, m_policy );
		}
		return success;
	}

	bool writePackedWhole( const RID &rBuffer, const PackedByteArray &rBytes, bool bPersistent = false ) const {
		if ( rBytes.size() < 0 || ( uint64_t )( rBytes.size() ) > UINT32_MAX ) {
			return false;
		}
		const uint32_t bytes = ( uint32_t )( rBytes.size() );
		const t3dGpuBufferSlice_t slice { rBuffer, 0u, bytes, bytes, 1u, 0u };
		return writePacked( slice, 0u, rBytes, bytes, bPersistent );
	}

	// Adapter for existing dirty-range callers whose physical capacity is
	// validated by their arena owner before submission. Keep the RD call and
	// traffic accounting in this service while those owners move to slices.
	Error updatePacked( const RID &rBuffer,
	        uint32_t nOffset,
	        uint32_t nBytes,
	        const PackedByteArray &rBytes ) const {
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd || !rBuffer.is_valid() || rBytes.size() < nBytes || nOffset > UINT32_MAX - nBytes ) {
			return ERR_INVALID_PARAMETER;
		}
		const Error result = rd->buffer_update( rBuffer, nOffset, nBytes, rBytes );
		if ( result == OK && nBytes > 0u ) {
			m_context.recordBufferUpdate( nBytes, m_owner, m_policy );
		}
		return result;
	}

	bool writePersistentRaw( const RID &rBuffer,
	        uint32_t nCapacityBytes,
	        uint32_t nOffset,
	        const void *pSource,
	        uint32_t nBytes ) const {
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd || !rBuffer.is_valid() || ( !pSource && nBytes > 0u ) || nOffset > nCapacityBytes
		        || nBytes > nCapacityBytes - nOffset ) {
			return false;
		}
		if ( nBytes == 0u ) {
			return true;
		}
		const bool written = t3dRdPersistentWrite( rd, rBuffer, pSource, nBytes, nOffset );
		if ( written ) {
			m_context.recordBufferUpdate( nBytes, m_owner, m_policy );
		}
		return written;
	}

	bool write( const t3dGpuBufferSlice_t &rSlice,
	        uint32_t nRelativeOffset,
	        const void *pSource,
	        uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        bool bPersistent ) const {
		if ( nBytes == 0u ) {
			return true;
		}
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd || !rSlice.m_hBuffer.is_valid() || !pSource || nRelativeOffset > rSlice.m_nCapacityBytes
		        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset ) {
			return false;
		}
		const uint32_t offset = rSlice.m_nOffsetBytes + nRelativeOffset;
		bool success = false;
		if ( bPersistent ) {
			success = t3dRdPersistentWrite( rd, rSlice.m_hBuffer, pSource, nBytes, offset );
		} else if ( rFallbackScratch.size() >= nBytes ) {
			if ( pSource != rFallbackScratch.ptr() ) {
				memcpy( rFallbackScratch.ptrw(), pSource, nBytes );
			}
			success = rd->buffer_update( rSlice.m_hBuffer, offset, nBytes, rFallbackScratch ) == OK;
		}
		if ( success ) {
			m_context.recordBufferUpdate( nBytes, m_owner, m_policy );
		}
		return success;
	}
};

#endif // TERRAIN3D_GPU_UPLOAD_SERVICE_H
