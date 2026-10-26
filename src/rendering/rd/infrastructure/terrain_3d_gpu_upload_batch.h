#ifndef TERRAIN3D_GPU_UPLOAD_BATCH_H
#define TERRAIN3D_GPU_UPLOAD_BATCH_H

#include "rendering/rd/infrastructure/terrain_3d_gpu_buffer_layout.h"
#include "rendering/rd/terrain_3d_rd_context.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <string.h>

// Sources and fallback scratch must remain alive until flush(). A batch owns
// only fixed inline request metadata, never copies or allocates upload bytes.
class Terrain3DGpuUploadBatch
{
	typedef struct Request_s
	{
		t3dGpuBufferSlice_t slice;
		uint32_t offset = 0u;
		const uint8_t *source = nullptr;
		uint32_t bytes = 0u;
		PackedByteArray *scratch = nullptr;
		bool persistent = false;
		Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL;
	} Request;

	typedef struct Mapping_s
	{
		RID buffer;
		uint8_t *bytes = nullptr;
	} Mapping;

	Terrain3DRdContext &m_context;
	Terrain3DRdContext::ResourceOwner m_owner;
	eastl::fixed_vector<Request, 64u, false> m_requests;
	eastl::fixed_vector<Mapping, 64u, false> m_mappings;

public:
	explicit Terrain3DGpuUploadBatch( Terrain3DRdContext &rContext,
	        Terrain3DRdContext::ResourceOwner owner = Terrain3DRdContext::OWNER_UNSPECIFIED ) :
	        m_context( rContext ), m_owner( owner ) {}

	bool queueRangeWrite( const t3dGpuBufferSlice_t &rSlice,
	        uint32_t nRelativeOffset,
	        const void *pSource,
	        uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        bool bPersistent = false,
	        Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) {
		if ( nBytes == 0u ) {
			return true;
		}
		if ( !rSlice.m_hBuffer.is_valid() || !pSource || nRelativeOffset > rSlice.m_nCapacityBytes
		        || nBytes > rSlice.m_nCapacityBytes - nRelativeOffset
		        || rSlice.m_nOffsetBytes > UINT32_MAX - nRelativeOffset
		        || ( !bPersistent && rFallbackScratch.size() < nBytes ) ) {
			return false;
		}
		const uint8_t *const source = static_cast<const uint8_t *>( pSource );
		if ( !m_requests.empty() ) {
			Request &previous = m_requests.back();
			const uint32_t previous_absolute = previous.slice.m_nOffsetBytes + previous.offset;
			const uint32_t next_absolute = rSlice.m_nOffsetBytes + nRelativeOffset;
			if ( previous.slice.m_hBuffer == rSlice.m_hBuffer && previous.scratch == &rFallbackScratch
			        && previous.persistent == bPersistent && previous.policy == policy && previous.bytes <= UINT32_MAX - previous_absolute
			        && previous_absolute + previous.bytes == next_absolute
			        && previous.bytes <= UINT32_MAX - nBytes
			        && previous.offset <= previous.slice.m_nCapacityBytes
			        && previous.bytes + nBytes <= previous.slice.m_nCapacityBytes - previous.offset
			        && ( uintptr_t )previous.source <= UINTPTR_MAX - previous.bytes
			        && ( uintptr_t )previous.source + previous.bytes == ( uintptr_t )source
			        && ( bPersistent || rFallbackScratch.size() >= previous.bytes + nBytes ) ) {
				previous.bytes += nBytes;
				return true;
			}
		}
		if ( m_requests.size() == m_requests.capacity() ) {
			return false;
		}
		m_requests.push_back( { rSlice, nRelativeOffset, source, nBytes, &rFallbackScratch, bPersistent, policy } );
		return true;
	}

	bool queueFullWrite( const RID &rBuffer,
	        uint32_t nCapacityBytes,
	        const void *pSource,
	        uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        bool bPersistent = false,
	        Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) {
		const t3dGpuBufferSlice_t slice { rBuffer, 0u, nCapacityBytes, nCapacityBytes, 1u, 0u };
		return queueRangeWrite( slice, 0u, pSource, nBytes, rFallbackScratch, bPersistent, policy );
	}

	bool flush() {
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd ) {
			m_requests.clear();
			return false;
		}
		bool success = true;
		for ( const Request &request : m_requests ) {
			const uint32_t offset = request.slice.m_nOffsetBytes + request.offset;
			if ( request.persistent ) {
				uint8_t *mapped = nullptr;
				for ( const Mapping &entry : m_mappings ) {
					if ( entry.buffer == request.slice.m_hBuffer ) {
						mapped = entry.bytes;
						break;
					}
				}
				if ( !mapped ) {
					const uint64_t address = rd->buffer_persistent_map_advance( request.slice.m_hBuffer );
					mapped = ( uint8_t * )( ( uintptr_t )address );
					if ( !mapped || m_mappings.size() == m_mappings.capacity() ) {
						success = false;
						break;
					}
					m_mappings.push_back( { request.slice.m_hBuffer, mapped } );
				}
				memcpy( mapped + offset, request.source, request.bytes );
			} else {
				if ( request.source != request.scratch->ptr() ) {
					memcpy( request.scratch->ptrw(), request.source, request.bytes );
				}
				if ( rd->buffer_update( request.slice.m_hBuffer, offset, request.bytes, *request.scratch ) != OK ) {
					success = false;
					break;
				}
			}
			m_context.recordBufferUpdate( request.bytes, m_owner, request.policy );
		}
		for ( const Mapping &entry : m_mappings ) {
			rd->buffer_flush( entry.buffer );
		}
		m_mappings.clear();
		m_requests.clear();
		return success;
	}
};

#endif // TERRAIN3D_GPU_UPLOAD_BATCH_H
