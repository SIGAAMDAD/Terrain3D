#ifndef TERRAIN3D_RD_GPU_BUFFER_BATCH_H
#define TERRAIN3D_RD_GPU_BUFFER_BATCH_H

#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

#include <stdint.h>

// Fixed-capacity multi-buffer upload coordinator. Physical buffer I/O remains
// exclusively inside Terrain3DRdGpuBuffer; this class only coalesces requests.
class Terrain3DRdGpuBufferBatch {
	typedef struct Request_s {

		const Terrain3DRdGpuBuffer *m_pBuffer = NULL;
		uint32_t m_nOffsetBytes = 0u;
		const uint8_t *m_pSource = NULL;
		uint32_t m_nBytes = 0u;
		PackedByteArray *m_pScratch = NULL;
		Terrain3DGpuBufferPolicy m_policy = T3D_GPU_NEUTRAL;
	} Request;


	Terrain3DRdContext &m_context;
	eastl::fixed_vector<Request, 64u, false> m_requests;

public:
	explicit Terrain3DRdGpuBufferBatch( Terrain3DRdContext &rContext ) :
	        m_context( rContext ) {}

	bool queueRangeWrite( const Terrain3DRdGpuBuffer &rBuffer,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) {
		if ( nBytes == 0u ) { return true; }
		if ( !rBuffer.isValid() || !pSource || nOffsetBytes > rBuffer.capacityBytes()
		        || nBytes > rBuffer.capacityBytes() - nOffsetBytes
		        || ( !rBuffer.isPersistent() && rFallbackScratch.size() < nBytes ) ) {
			return false;
		}
		const uint8_t *const source = ( const uint8_t * )( pSource  );
		if ( !m_requests.empty() ) {
			Request &previous = m_requests.back();
			if ( previous.m_pBuffer == &rBuffer && previous.m_pScratch == &rFallbackScratch
			        && previous.m_policy == policy && previous.m_nBytes <= UINT32_MAX - previous.m_nOffsetBytes
			        && previous.m_nOffsetBytes + previous.m_nBytes == nOffsetBytes
			        && previous.m_nBytes <= UINT32_MAX - nBytes
			        && previous.m_nBytes + nBytes <= rBuffer.capacityBytes() - previous.m_nOffsetBytes
			        && ( uintptr_t )previous.m_pSource <= UINTPTR_MAX - previous.m_nBytes
			        && ( uintptr_t )previous.m_pSource + previous.m_nBytes == ( uintptr_t )source
			        && ( rBuffer.isPersistent() || rFallbackScratch.size() >= previous.m_nBytes + nBytes ) ) {
				previous.m_nBytes += nBytes;
				return true;
			}
		}
		if ( m_requests.size() == m_requests.capacity() ) { return false; }
		m_requests.push_back( { &rBuffer, nOffsetBytes, source, nBytes, &rFallbackScratch, policy } );
		return true;
	}

	bool queueFullWrite( const Terrain3DRdGpuBuffer &rBuffer,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) {
		return queueRangeWrite( rBuffer, 0u, pSource, nBytes, rFallbackScratch, policy );
	}

	bool flush( void ) {
		bool success = true;
		for ( const Request &request : m_requests ) {
			if ( !request.m_pBuffer || !request.m_pBuffer->writeTracked( m_context, request.m_nOffsetBytes,
			             request.m_pSource, request.m_nBytes, *request.m_pScratch, request.m_policy ) ) {
				success = false;
				break;
			}
		}
		m_requests.clear();
		return success;
	}
};

#endif // TERRAIN3D_RD_GPU_BUFFER_BATCH_H
