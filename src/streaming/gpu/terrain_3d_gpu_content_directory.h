#ifndef TERRAIN3D_GPU_CONTENT_DIRECTORY_H
#define TERRAIN3D_GPU_CONTENT_DIRECTORY_H

#include "streaming/gpu/terrain_3d_gpu_page_pool.h"

// Render-thread owned physical index. Payload bytes are never retained here.
// Hash hits are only probes; aliasing requires complete GPU payload comparison.
class Terrain3DGpuContentDirectory
{
public:
	static constexpr uint32_t ENTRY_COUNT = T3D_GPU_PAGE_CHANNEL_COUNT * Terrain3DGpuPagePool::MAX_LAYERS;
	struct Entry
	{
		t3dGpuPageHandle_t	   handle;
		t3dGpuPageContentKey_t content;
		bool				   present = false;
	};
	static t3dGpuPageContentKey_t
	fingerprint( uint8_t channel, const Terrain3DGpuPagePool::Configuration &config, const PackedByteArray &raw );
	static bool equalBytes( const PackedByteArray &a, const PackedByteArray &b, uint64_t expectedBytes );
	void		reset();
	void		record( const t3dGpuPageHandle_t &handle, const t3dGpuPageContentKey_t &content );
	void		forget( const t3dGpuPageHandle_t &handle );
	bool		findCandidate( uint8_t						 channel,
							   const t3dGpuPageContentKey_t &content,
							   const t3dGpuPageHandle_t		&exclude,
							   const Terrain3DGpuPagePool	&pool,
							   t3dGpuPageHandle_t			&candidate,
							   uint16_t						 afterPhysicalLayer = UINT16_MAX ) const;
	uint32_t	entries() const;

private:
	Entry m_entries[ENTRY_COUNT]{};
};
#endif
