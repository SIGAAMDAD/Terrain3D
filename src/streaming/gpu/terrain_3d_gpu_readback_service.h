#ifndef TERRAIN3D_GPU_READBACK_SERVICE_H
#define TERRAIN3D_GPU_READBACK_SERVICE_H

#include "streaming/gpu/terrain_3d_gpu_page_store.h"
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>

// P07: bounded, callback-driven snapshot of the shadow GPU-authoritative store.
// A caller binds (transactionId, channel) to its Godot async callback and returns
// received PackedByteArray data to acceptChannel(). No synchronous readback.
class Terrain3DGpuReadbackService
{
public:
	static constexpr uint32_t TRANSACTIONS = 4u;
	static constexpr uint64_t MAX_STAGING_BYTES = 2u * 1024u * 1024u;
	enum State : uint8_t
	{
		EMPTY = 0u,
		CAPTURING,
		READY,
		FAILED
	};
	struct Channel
	{
		t3dGpuPageTicket_t ticket;
		t3dGpuPageHandle_t handle;
		PackedByteArray	   bytes;
		uint64_t		   submissionEpoch = 0u;
		bool			   requested = false;
		bool			   received = false;
	};
	struct Transaction
	{
		uint64_t id = 0u;
		uint16_t region = UINT16_MAX;
		uint8_t	 channelMask = 0u;
		uint8_t	 state = EMPTY;
		uint32_t sourceGeneration = 0u;
		uint32_t editGeneration = 0u;
		Channel	 channels[T3D_GPU_PAGE_CHANNEL_COUNT];
	};
	uint64_t begin( Terrain3DGpuPageStore &store, uint16_t region, uint8_t channelMask );
	// Callback must accept PackedByteArray; caller binds immutable transaction ID and channel.
	bool		dispatch( uint64_t				 id,
						  uint8_t				 channel,
						  Terrain3DRdContext	&context,
						  Terrain3DGpuPageStore &store,
						  uint64_t				 submissionEpoch,
						  const Callable		&callback );
	static bool dispatchAliasProbe( const Terrain3DGpuPageStore::AliasProbe &probe,
									Terrain3DRdContext						&context,
									const Terrain3DGpuPageStore				&store,
									const Callable							&originalCallback,
									const Callable							&candidateCallback );
	bool acceptChannel( uint64_t id, uint8_t channel, Terrain3DGpuPageStore &store, const PackedByteArray &gpuBytes );
	// One atomic region sidecar containing only the requested channel payloads.
	Error publish( uint64_t id, const String &directory, Terrain3DGpuPageStore &store );
	// Verifies file header and every stored payload checksum (no Image materialization).
	static Error loadSnapshot( const String	  &path,
							   uint16_t		   expectedRegion,
							   PackedByteArray output[T3D_GPU_PAGE_CHANNEL_COUNT],
							   uint8_t		  &channelMask );
	void		 cancel( uint64_t id );
	void		 clear();
	uint64_t	 stagingBytes() const {
		return m_stagingBytes;
	}
	uint32_t	  pendingCount() const;
	static String snapshotPath( const String &directory, uint16_t region );

private:
	Transaction		  *find( uint64_t id );
	const Transaction *find( uint64_t id ) const;
	Transaction		   m_transactions[TRANSACTIONS];
	uint64_t		   m_serial = 1u;
	uint64_t		   m_stagingBytes = 0u;
};
#endif
