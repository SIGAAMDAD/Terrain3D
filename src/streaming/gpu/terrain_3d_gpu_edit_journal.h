#ifndef TERRAIN3D_GPU_EDIT_JOURNAL_H
#define TERRAIN3D_GPU_EDIT_JOURNAL_H

#include "streaming/gpu/terrain_3d_gpu_page_store.h"
#include "rendering/rd/terrain_pages/terrain_3d_page_compute.h"
#include "streaming/gpu/terrain_3d_gpu_dirty_ledger.h"

// P08: bounded GPU-resident history. Immutable physical pages are retained
// through GPU pool reference counts. All methods are render-thread operations.
// This is the shadow path, not the existing editor's Image-based brush API.
class Terrain3DGpuEditJournal
{
public:
	static constexpr uint32_t HISTORY_LIMIT = 24u;
	struct Edit
	{
		t3dGpuPageTicket_t ticket;
		t3dGpuPageHandle_t before;
		t3dGpuPageHandle_t after;
		uint32_t		   x = 0u, y = 0u, width = 0u, height = 0u;
		uint8_t			   invalidatedChannels = 0u;
	};
	bool begin( Terrain3DGpuPageStore	  &store,
				Terrain3DRdTextureManager &manager,
				Terrain3DRdContext		  &context,
				uint16_t				   region,
				uint8_t					   channel,
				uint32_t				   x,
				uint32_t				   y,
				uint32_t				   width,
				uint32_t				   height,
				uint64_t				   submissionEpoch );
	bool dispatch( Terrain3DGpuPageStore		  &store,
				   Terrain3DRdContext			  &context,
				   Terrain3DPageCompute			  &compute,
				   Terrain3DPageCompute::Operation operation,
				   const Color					  &value,
				   uint64_t						   submissionEpoch,
				   uint32_t						   packedControlBits = 0u );
	bool commit( Terrain3DGpuPageStore &store, uint64_t confirmedCompletedEpoch );
	bool abort( Terrain3DGpuPageStore &store );
	bool abortCompleted( Terrain3DGpuPageStore &store, uint64_t confirmedCompletedEpoch );
	void attachDirtyLedger(
		Terrain3DGpuDirtyLedger *ledger ) {
		m_dirtyLedger = ledger;
	}
	bool undo( Terrain3DGpuPageStore &store, uint64_t confirmedCompletedEpoch );
	bool redo( Terrain3DGpuPageStore &store, uint64_t confirmedCompletedEpoch );
	void clear( Terrain3DGpuPageStore &store );
	// Call after device loss. All prior physical handles are invalid.
	void abandonDevice() {
		m_active = Edit{};
		m_pending = false;
		m_dispatched = false;
		m_count = 0u;
		m_cursor = 0u;
		m_lastInvalidations = 0u;
	}
	uint32_t historySize() const {
		return m_count;
	}
	uint32_t undoCount() const {
		return m_cursor;
	}
	bool pending() const {
		return m_pending;
	}
	uint8_t lastInvalidatedChannels() const {
		return m_lastInvalidations;
	}
	static uint8_t invalidationsFor( uint8_t sourceChannel );
	static bool
	validRect( uint32_t textureWidth, uint32_t textureHeight, uint32_t x, uint32_t y, uint32_t width, uint32_t height );

private:
	bool					 restore( Terrain3DGpuPageStore &store, bool redo, uint64_t confirmedCompletedEpoch );
	void					 release( Terrain3DGpuPageStore &store, Edit &edit );
	Edit					 m_history[HISTORY_LIMIT]{};
	Edit					 m_active{};
	uint64_t				 m_submissionEpoch = 0u;
	uint32_t				 m_count = 0u, m_cursor = 0u;
	uint8_t					 m_lastInvalidations = 0u;
	bool					 m_pending = false, m_dispatched = false;
	Terrain3DGpuDirtyLedger *m_dirtyLedger = nullptr;
};
#endif
