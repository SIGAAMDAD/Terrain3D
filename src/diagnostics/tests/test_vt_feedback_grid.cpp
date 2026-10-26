#include "rendering/rd/vt/core/terrain_3d_vt_feedback_grid.h"
#include <assert.h>
#include <string.h>

int main() {
	t3dVTFeedbackGridLayout_t small{}, runtime{}, high{};
	assert( !t3d_vt_feedback_grid_layout( 0u, small ) );
	assert( !t3d_vt_feedback_grid_layout( 3096u, small ) );
	assert( t3d_vt_feedback_grid_layout( 4096u, small ) );
	assert( small.m_nWidth == 32u && small.m_nHeight == 24u );
	assert( small.m_nSlotBytes <= 4096u );
	assert( t3d_vt_feedback_grid_layout( 65536u, runtime ) );
	assert( runtime.m_nWidth == 160u && runtime.m_nHeight == 90u );
	assert( runtime.m_nSlotBytes <= 65536u );
	assert( t3d_vt_feedback_grid_layout( 131072u, high ) );
	assert( high.m_nWidth == 240u && high.m_nHeight == 136u );
	assert( high.m_nSlotBytes <= 131072u );
	assert( high.m_nTotalBytes == high.m_nSlotBytes * T3D_VT_FEEDBACK_GRID_SLOT_COUNT );
	t3dVTFeedbackGridParams_t params{};
	assert( t3d_vt_feedback_grid_make_params( runtime, 200u, 10000u, 1920u, 1080u, 2u, 0x1234u, params ) );
	assert( sizeof( params ) == 32u && params.m_nAllocationCount == 200u && params.m_nPageCount == 10000u );
	assert( params.m_nGridWidth == 160u && ( params.m_nGridHeightAndViewportWidth & 0xffffu ) == 90u );
	assert( ( params.m_nGridHeightAndViewportWidth >> 16u ) == 1920u );
	assert( ( params.m_nWriteSlotAndViewportHeight & 0xffffu ) == 2u );
	assert( ( params.m_nWriteSlotAndViewportHeight >> 16u ) == 1080u );
	assert( params.m_nSlotStrideWords == runtime.m_nSlotBytes / 4u );
	assert( !t3d_vt_feedback_grid_make_params( runtime, 200u, 10000u, 65536u, 1080u, 2u, 0u, params ) );
	assert( !t3d_vt_feedback_grid_make_params( runtime, 200u, 10000u, 1920u, 0u, 2u, 0u, params ) );
	t3dVTFeedbackGridSlot_t slots[T3D_VT_FEEDBACK_GRID_SLOT_COUNT]{};
	for ( uint32_t i = 0u; i < T3D_VT_FEEDBACK_GRID_SLOT_COUNT; ++i ) {
		t3dVTFeedbackGridHeader_t header{ T3D_VT_FEEDBACK_GRID_MAGIC,
			T3D_VT_FEEDBACK_GRID_CONTRACT_VERSION, i, 123u + i, 44u, 7u, 9u, 768u,
			0x1122334455667788ull };
		assert( slots[i].prepare( header ) );
		assert( !slots[i].prepare( header ) );
		assert( slots[i].markDrawn() );
		assert( !slots[i].markDrawn() );
		assert( slots[i].submit( true ) );
		assert( !slots[i].submit( true ) );
		t3dVTFeedbackGridHeader_t stale = header;
		stale.m_nFeedbackEpoch++;
		assert( !slots[i].complete( stale ) );
		stale = header;
		stale.m_nViewportKey++;
		assert( !slots[i].complete( stale ) );
		stale = header;
		stale.m_nDatabaseGeneration++;
		assert( !slots[i].complete( stale ) );
		if ( i == 0u ) {
			slots[i].invalidate();
			assert( !slots[i].complete( header ) );
			assert( slots[i].m_eStatus == T3D_VT_FEEDBACK_SLOT_FREE );
		} else {
			assert( slots[i].complete( header ) );
			assert( !slots[i].complete( header ) );
			slots[i].reset();
		}
		assert( slots[i].prepare( header ) );
		assert( slots[i].markDrawn() );
		assert( !slots[i].submit( false ) );
		assert( slots[i].m_eStatus == T3D_VT_FEEDBACK_SLOT_FREE );
	}
	return 0;
}
