#ifndef TERRAIN3D_GPU_DIRTY_LEDGER_H
#define TERRAIN3D_GPU_DIRTY_LEDGER_H

#include "streaming/residency/terrain_3d_gpu_page_contract.h"
class Terrain3DGpuPageStore;

// Versioned source/consumer fanout. Each channel retains its own pending
// consumer mask, so simultaneous height and color edits cannot overwrite one
// another's invalidation tickets. All mutation stays with the render owner.
class Terrain3DGpuDirtyLedger
{
public:
	enum Consumer : uint8_t
	{
		DERIVED = 1u << 0u,
		COMPOSITE = 1u << 1u,
		SHADOW = 1u << 2u,
		MIDDLE_HEIGHT = 1u << 3u,
		FAR_ATLAS = 1u << 4u,
		MATERIAL_VT = 1u << 5u,
		CPU_COLLISION = 1u << 6u
	};
	struct Entry
	{
		t3dGpuPageTicket_t sources[T3D_GPU_PAGE_CHANNEL_COUNT];
		uint8_t			   pendingByChannel[T3D_GPU_PAGE_CHANNEL_COUNT]{};
	};
	bool invalidate( const t3dGpuPageTicket_t &source );
	bool next( uint8_t consumer, uint16_t &region, t3dGpuPageTicket_t &source ) const;
	bool nextFrom( uint8_t consumer, uint32_t firstRegion, uint16_t &region, t3dGpuPageTicket_t &source ) const;
	bool acknowledge( uint8_t consumer, const t3dGpuPageTicket_t &source );
	bool isPending( uint8_t consumer, const t3dGpuPageTicket_t &source ) const;
	bool invalidateOnly( uint8_t consumer, const t3dGpuPageTicket_t &source );
	// A height edit touching x=0/y=0 alters right/down finite differences of
	// the immediate left/top region. Invalidate only their derived output.
	void		   invalidateHeightHaloNeighbors( Terrain3DGpuPageStore		   &store,
												  const t3dGpuPageLogicalKey_t &edited,
												  bool							touchedWest,
												  bool							touchedNorth );
	uint8_t		   pending( uint16_t region ) const;
	uint8_t		   pendingFor( uint16_t region, uint8_t channel ) const;
	void		   reset();
	static uint8_t maskFor( uint8_t channel );

private:
	Entry m_entries[T3D_REGION_SLOTS]{};
};
#endif
