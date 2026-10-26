#ifndef TERRAIN3D_GPU_PAGE_STORE_H
#define TERRAIN3D_GPU_PAGE_STORE_H

#include "streaming/gpu/terrain_3d_gpu_page_pool.h"
#include "streaming/gpu/terrain_3d_gpu_content_directory.h"
#include "streaming/residency/terrain_3d_residency_plan.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"

// Shadow-only channel publication record. The existing region draw ABI stays 32 bytes.
struct t3dGpuChannelDirectoryEntry_t
{
	uint32_t physicalLayer = UINT32_MAX;
	uint32_t slotGeneration = 0u;
	uint32_t sourceGeneration = 0u;
	uint32_t editGeneration = 0u;
};
static_assert( sizeof( t3dGpuChannelDirectoryEntry_t ) == 16u, "Channel directory GPU record" );

class Terrain3DGpuAliasVerifier;
class Terrain3DGpuPageStore
{
public:
	static constexpr uint32_t REGION_COUNT = T3D_REGION_SLOTS;
	static constexpr uint32_t CHANNEL_COUNT = T3D_GPU_PAGE_CHANNEL_COUNT;
	static constexpr uint32_t DIRECTORY_BYTES = REGION_COUNT * CHANNEL_COUNT * sizeof( t3dGpuChannelDirectoryEntry_t );
	struct PoolConfig
	{
		Terrain3DGpuPagePool::Configuration channels[CHANNEL_COUNT];
	};
	bool configure( const PoolConfig &config, uint64_t maximumPhysicalBytes );
	bool createCold( Terrain3DRdTextureManager &manager );
	bool stageDefault( Terrain3DRdTextureManager &manager,
					   t3dGpuPageChannel_t		  channel,
					   const PackedByteArray	 &bytes,
					   uint64_t					  submissionEpoch );
	bool publishDefault( t3dGpuPageChannel_t channel, uint64_t confirmedCompletedEpoch );
	// Stage/copy only on the render thread; commit requires an externally confirmed GPU completion epoch.
	bool stage( Terrain3DRdTextureManager &manager,
				const t3dGpuPageTicket_t  &ticket,
				const PackedByteArray	  &bytes,
				uint64_t				   submissionEpoch );
	bool commit( const t3dGpuPageTicket_t &ticket, uint64_t confirmedCompletedEpoch );
	// P08: one-channel edit transactions can advance independently. Derived and
	// composite invalidation is requested by the edit journal, not a hidden copy.
	bool commitChannelEdit( const t3dGpuPageTicket_t &ticket, uint64_t confirmedCompletedEpoch );
	bool stagedSnapshot( const t3dGpuPageTicket_t &ticket, t3dGpuPageHandle_t &handle ) const;
	bool retainHistoryHandle( const t3dGpuPageHandle_t &handle );
	bool releaseHistoryHandle( const t3dGpuPageHandle_t &handle );
	bool restoreHistoryHandle( const t3dGpuPageTicket_t &newTicket,
							   const t3dGpuPageHandle_t &historyHandle,
							   uint64_t					 confirmedCompletedEpoch );
	bool commitBatch( const t3dGpuPageTicket_t *tickets, uint32_t count, uint64_t confirmedCompletedEpoch );
	// Copy-on-write: reserve a private, unpublished copy of a published page.
	// Reserve an unpublished derived page that compute fills without a CPU upload.
	bool stageComputedDerived( const t3dGpuPageTicket_t &ticket, uint64_t submissionEpoch );
	bool commitComputedDerived( const t3dGpuPageTicket_t &ticket,
								const t3dGpuPageTicket_t &heightSource,
								uint64_t				  confirmedCompletedEpoch );
	bool stageCopyForEdit( Terrain3DRdTextureManager &manager,
						   Terrain3DRdContext		 &context,
						   const t3dGpuPageTicket_t	 &ticket,
						   uint64_t					  submissionEpoch );
	// A probe pins both published slots for GPU readback/byte comparison.
	struct AliasProbe
	{
		t3dGpuPageTicket_t ticket;
		t3dGpuPageHandle_t original, candidate;
		uint64_t		   readerEpoch = 0u;
		uint32_t		   payloadBytes = 0u;
	};
	// Post-compute pages are fingerprinted only from completed GPU readback bytes.
	bool registerGpuFingerprint( const t3dGpuPageTicket_t &ticket,
								 const t3dGpuPageHandle_t &handle,
								 const PackedByteArray	  &actualGpuBytes );
	bool beginAliasProbe( const t3dGpuPageLogicalKey_t &key,
						  uint64_t						readerEpoch,
						  AliasProbe				   &probe,
						  uint16_t						afterPhysicalLayer = UINT16_MAX );
	bool finishAliasProbe( const AliasProbe		 &probe,
						   const PackedByteArray &originalGpuBytes,
						   const PackedByteArray &candidateGpuBytes,
						   uint64_t				  confirmedCompletedEpoch );
	bool publishedSnapshot( const t3dGpuPageLogicalKey_t &key,
							t3dGpuPageTicket_t			 &ticket,
							t3dGpuPageHandle_t			 &handle ) const;
	bool snapshotCurrent( const t3dGpuPageTicket_t &ticket, const t3dGpuPageHandle_t &handle ) const;
	bool acknowledgeSaved( const t3dGpuPageTicket_t &ticket, const t3dGpuPageHandle_t &handle );
	bool isDirty( const t3dGpuPageLogicalKey_t &key ) const;
	const Terrain3DGpuContentDirectory &contentDirectory() const {
		return m_content;
	}
	bool								 cancelStaged( const t3dGpuPageLogicalKey_t &key );
	bool								 withdraw( const t3dGpuPageLogicalKey_t &key );
	void								 markReader( const t3dGpuPageLogicalKey_t &key, uint64_t epoch );
	void								 collect( uint64_t completedEpoch );
	bool								 buildDirectorySnapshot( PackedByteArray &bytes, uint64_t &generation ) const;
	const t3dGpuChannelDirectoryEntry_t &directoryEntry( uint16_t region, uint8_t channel ) const;
	const Terrain3DGpuPagePool			&pool(
				 uint8_t channel ) const {
		return m_pools[channel];
	}
	Terrain3DGpuPagePool &pool(
		uint8_t channel ) {
		return m_pools[channel];
	}
	void	 destroyCold( Terrain3DRdTextureManager &manager );
	void	 abandonDevice();
	uint64_t physicalBytes() const;
	uint64_t publicationGeneration() const {
		return m_publicationGeneration;
	}
	bool active() const {
		return m_active;
	}

private:
	friend class Terrain3DGpuAliasVerifier;
	bool finishGpuVerifiedAlias( const AliasProbe &probe, uint64_t completedEpoch );
	struct Logical
	{
		t3dGpuPageHandle_t	   published;
		t3dGpuPageHandle_t	   staged;
		t3dGpuPageTicket_t	   ticket;
		t3dGpuPageTicket_t	   publishedTicket;
		t3dGpuPageContentKey_t stagedFingerprint;
		t3dGpuPageContentKey_t committedFingerprint;
		uint64_t			   uploadEpoch = 0u;
		bool				   hasFingerprint = false;
		bool				   dirty = false;
		bool				   hasPublished = false, hasStaged = false;
	};
	static uint32_t index(
		const t3dGpuPageLogicalKey_t &key ) {
		return ( uint32_t ) key.m_nRegionIndex * CHANNEL_COUNT + key.m_nChannel;
	}
	Terrain3DGpuPagePool		  m_pools[CHANNEL_COUNT];
	Terrain3DGpuContentDirectory  m_content;
	Logical						  m_logical[REGION_COUNT * CHANNEL_COUNT]{};
	t3dGpuChannelDirectoryEntry_t m_directory[REGION_COUNT * CHANNEL_COUNT]{};
	uint64_t					  m_publicationGeneration = 1u;
	uint64_t					  m_maximumPhysicalBytes = 0u;
	bool						  m_configured = false, m_active = false;
};

// Render-thread shadow SSBO banks; each bank is pinned through a known GPU reader epoch.
// No shipping raster shader binds these until P04 integration/driver validation.
class Terrain3DGpuChannelDirectoryBuffer
{
public:
	static constexpr uint32_t BANK_COUNT = 3u;
	bool					  initializeCold( Terrain3DRdContext &context );
	bool
	uploadCold( Terrain3DRdContext &context, const Terrain3DGpuPageStore &store, uint64_t confirmedCompletedEpoch );
	const RID &rid() const;
	uint64_t   uploadedGeneration() const {
		  return m_uploadedGeneration;
	}
	bool markReader( uint64_t generation, uint64_t submissionEpoch );
	void releaseCold( Terrain3DRdContext &context );

private:
	Terrain3DRdGpuBuffer m_buffers[BANK_COUNT];
	uint64_t			 m_readerEpoch[BANK_COUNT]{};
	uint32_t			 m_publishedBank = BANK_COUNT;
	PackedByteArray		 m_scratch;
	uint64_t			 m_uploadedGeneration = 0u;
};
#endif
