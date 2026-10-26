#ifndef TERRAIN3D_RD_GPU_BUFFER_H
#define TERRAIN3D_RD_GPU_BUFFER_H

#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer_types.h"
#include "rendering/rd/core/terrain_3d_rd_context_access.h"
#include "rendering/rd/core/terrain_3d_rd_context_contract.h"
#include "memory/policy/terrain_3d_memory_account.h"
#include <godot_cpp/classes/rendering_device.hpp>

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <stdint.h>
#include <string.h>


typedef struct t3dRdGpuBufferSpec_s {

	const char *m_pName = NULL;
	t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	t3dRdResourceType_t m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
	t3dRdMemoryCategory_t m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
	Terrain3DGpuGrowthPolicy m_growth = T3D_GPU_EXACT;
	uint32_t m_nMinimumCapacityBytes = 0u;
	BitField<RenderingDevice::StorageBufferUsage> m_usage = BitField<RenderingDevice::StorageBufferUsage>( 0 );
	BitField<RenderingDevice::BufferCreationBits> m_creationBits = BitField<RenderingDevice::BufferCreationBits>( 0 );
	RenderingDevice::IndexBufferFormat m_indexFormat = RenderingDevice::INDEX_BUFFER_FORMAT_UINT32;
} t3dRdGpuBufferSpec_t;


// Cohesive owner for one physical RenderingDevice buffer. Configuration and
// physical state live together: domain code configures the logical buffer once,
// then asks this object to create/grow/replace/write/read/release that buffer.
class Terrain3DRdGpuBuffer {
	Terrain3DMemoryReservation m_memoryReservation;
	t3dRdGpuBufferSpec_t m_spec;
	RID m_hRid;
	uint32_t m_nCapacityBytes = 0u;
	uint32_t m_nUsedBytes = 0u;
	uint32_t m_nGeneration = 0u;
	bool m_bConfigured = false;
	bool m_bPersistent = false;

	static bool isBufferType( const t3dRdResourceType_t type );

	static uint32_t indexStride( const RenderingDevice::IndexBufferFormat format ) {
		return format == RenderingDevice::INDEX_BUFFER_FORMAT_UINT16 ? 2u : 4u;
	}

	static bool persistentMappingAvailable( RenderingDevice *const pRd );

	static uint8_t *persistentMapAdvance( RenderingDevice *const pRd, const RID &rBuffer );

	static bool persistentWrite( RenderingDevice *const pRd,
	        const RID &rBuffer,
	        const void *const pSource,
	        const uint32_t nBytes,
	        const uint32_t nOffset = 0u );

	static RID createPhysical( RenderingDevice *const pRd,
	        const t3dRdGpuBufferSpec_t &rSpec,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData );

	static bool registerTracked( Terrain3DRdContextAccess &rContext,
	        RenderingDevice *const pRd,
	        RID &rRid,
	        const t3dRdGpuBufferSpec_t &rSpec,
	        const uint32_t nCapacityBytes );

	void publishPhysical( const RID &rRid,
	        const uint32_t nCapacityBytes,
	        const uint32_t nUsedBytes,
	        const bool bPersistent,
	        const uint32_t nGeneration );

	void clearPhysical( void );

public:
	Terrain3DRdGpuBuffer( void ) = default;
	Terrain3DRdGpuBuffer( const Terrain3DRdGpuBuffer & ) = delete;
	Terrain3DRdGpuBuffer &operator=( const Terrain3DRdGpuBuffer & ) = delete;

	static BitField<RenderingDevice::BufferCreationBits> persistentCreationBits( void ) {
		return BitField<RenderingDevice::BufferCreationBits>( RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT );
	}

	static bool supportsPersistentMapping( RenderingDevice *const pRd ) {
		return persistentMappingAvailable( pRd );
	}

	bool configure( const t3dRdGpuBufferSpec_t &rSpec );

	bool isConfigured( void ) const { return m_bConfigured; }
	const t3dRdGpuBufferSpec_t &spec( void ) const { return m_spec; }
	bool isValid( void ) const { return m_hRid.is_valid(); }
	bool is_valid( void ) const { return isValid(); }
	const RID &rid( void ) const { return m_hRid; }
	uint32_t capacityBytes( void ) const { return m_nCapacityBytes; }
	uint32_t usedBytes( void ) const { return m_nUsedBytes; }
	uint32_t generation( void ) const { return m_nGeneration; }
	bool isPersistent( void ) const { return m_bPersistent; }
	t3dRdResourceOwner_t owner( void ) const { return m_spec.m_owner; }
	operator const RID &( void ) const { return m_hRid; }

	bool needsCapacity( const uint32_t nRequiredBytes ) const {
		return !m_hRid.is_valid() || nRequiredBytes > m_nCapacityBytes;
	}

	bool createTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() );

	bool replaceTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() );

	bool ensureTracked( Terrain3DRdContextAccess &rContext, const uint32_t nRequiredBytes );

	bool createLocal( RenderingDevice *const pDevice,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() );

	bool replaceLocal( RenderingDevice *const pDevice,
	        const uint32_t nCapacityBytes,
	        const PackedByteArray &rInitialData = PackedByteArray() );

	bool ensureLocal( RenderingDevice *const pDevice, const uint32_t nRequiredBytes );

	void releaseTracked( Terrain3DRdContextAccess &rContext );

	void releaseLocal( RenderingDevice *const pDevice );

	// Device loss invalidates the RID externally; retain logical configuration
	// so the same buffer object can recreate its physical backing afterward.
	void abandon( void ) {
		clearPhysical();
		m_nGeneration = t3dRdGpuBufferNextGeneration( m_nGeneration );
	}

	bool writeTrackedForOwner( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const t3dRdResourceOwner_t owner,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const;

	bool writeTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const;

	bool writePackedTrackedForOwner( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const t3dRdResourceOwner_t owner,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const;

	bool writePackedTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL ) const;

	bool writeLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch ) const;

	bool writePackedLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes ) const;

	PackedByteArray readbackTracked( Terrain3DRdContextAccess &rContext,
	        const uint32_t nOffsetBytes = 0u,
	        const uint32_t nBytes = 0u ) const;

	PackedByteArray readbackLocal( RenderingDevice *const pDevice,
	        const uint32_t nOffsetBytes = 0u,
	        const uint32_t nBytes = 0u ) const;

	t3dRdGpuBufferSlice_t wholeSlice( void ) const {
		return { m_hRid, 0u, m_nCapacityBytes, m_nCapacityBytes, 1u, m_nGeneration };
	}

	bool slice( const uint32_t nOffsetBytes,
	        const uint32_t nSizeBytes,
	        const uint32_t nAlignmentBytes,
	        t3dRdGpuBufferSlice_t &rSlice ) const;

	bool ownsSlice( const t3dRdGpuBufferSlice_t &rSlice ) const;

	bool reserveSlice( uint32_t &rCursorBytes,
	        const uint32_t nSizeBytes,
	        const uint32_t nAlignmentBytes,
	        t3dRdGpuBufferSlice_t &rSlice ) const;

	bool writeSliceTracked( Terrain3DRdContextAccess &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset,
	        const void *const pSource,
	        const uint32_t nBytes,
	        PackedByteArray &rFallbackScratch,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL,
	        const t3dRdResourceOwner_t owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED ) const;

	bool writePackedSliceTracked( Terrain3DRdContextAccess &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset,
	        const PackedByteArray &rBytes,
	        const uint32_t nBytes,
	        const Terrain3DGpuBufferPolicy policy = T3D_GPU_NEUTRAL,
	        const t3dRdResourceOwner_t owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED ) const;

	PackedByteArray readbackSliceTracked( Terrain3DRdContextAccess &rContext,
	        const t3dRdGpuBufferSlice_t &rSlice,
	        const uint32_t nRelativeOffset = 0u,
	        const uint32_t nBytes = 0u ) const;

	bool setSliceUsedBytes( t3dRdGpuBufferSlice_t &rSlice, const uint32_t nBytes ) const;

	bool setUsedBytes( const uint32_t nBytes );

	void swap( Terrain3DRdGpuBuffer &rOther );
};

#endif // TERRAIN3D_RD_GPU_BUFFER_H
