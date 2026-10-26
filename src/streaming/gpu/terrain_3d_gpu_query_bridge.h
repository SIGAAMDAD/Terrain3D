#ifndef TERRAIN3D_GPU_QUERY_BRIDGE_H
#define TERRAIN3D_GPU_QUERY_BRIDGE_H

#include "streaming/residency/terrain_3d_gpu_page_contract.h"
#include "data/sampling/terrain_3d_sampling_storage.h"

// P10: a generation-checked bridge into the existing bounded CPU active-height
// storage. Clean cooked pages and completed dirty readbacks use the same exact
// R16 payload. No synchronous per-query GPU readback occurs here.
class Terrain3DGpuQueryBridge
{
public:
	struct Record
	{
		t3dGpuPageTicket_t ticket;
		uint32_t		   generation = 0u;
		bool			   pending = false;
		bool			   ready = false;
	};
	explicit Terrain3DGpuQueryBridge(
		Terrain3DSamplingStorage *storage ) :
		m_storage( storage ) {
	}
	bool acceptClean( const t3dGpuPageTicket_t &ticket, const Vector2i &location, const PackedByteArray &r16Bytes );
	bool markEditPending( const t3dGpuPageTicket_t &ticket, const Vector2i &location );
	bool
	acceptDirtyReadback( const t3dGpuPageTicket_t &ticket, const Vector2i &location, const PackedByteArray &r16Bytes );
	bool		current( const t3dGpuPageTicket_t &ticket ) const;
	void		reset();
	static bool validPayload( const PackedByteArray &bytes );
	static bool matchesRegion( uint16_t region, const Vector2i &location );

private:
	static constexpr uint32_t HEIGHT_SIDE = 128u;
	static constexpr uint32_t HEIGHT_BYTES = HEIGHT_SIDE * HEIGHT_SIDE * 2u;
	Terrain3DSamplingStorage *m_storage = nullptr;
	Record					  m_entries[T3D_REGION_SLOTS]{};
};
#endif
