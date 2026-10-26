#include "rendering/rd/vt/core/terrain_3d_vt_feedback_multi_source.h"
#include <assert.h>

int main()
{
	t3dVTFeedbackGridLayout_t layout{};
	assert( t3d_vt_feedback_multi_layout( 1200000u, layout ) );
	t3dVTFeedbackMultiParams_t params{};
	assert( t3d_vt_feedback_multi_params( layout, 1920u, 1080u, 0u, 1048576u, 8u, params ) );
	const uint32_t epoch = 9u, database = 11u, device = 17u;
	const uint64_t viewport = 0x1234567890abcdefull;
	t3dVTFeedbackMultiSourceProof_t proof{};
	proof.m_nViewport = viewport;
	proof.m_nFrame = 24u;
	proof.m_nFeedbackEpoch = epoch;
	proof.m_nDatabaseGeneration = database;
	proof.m_nDeviceGeneration = device;
	proof.m_nViewportWidth = 1920u;
	proof.m_nViewportHeight = 1080u;
	proof.m_nLayerCount = 8u;
	proof.m_nRequiredFamilies = T3D_VT_MULTI_SOURCE_TERRAIN | T3D_VT_MULTI_SOURCE_WORLD |
		T3D_VT_MULTI_SOURCE_INSTANCES;
	proof.m_nPublishedFamilies = proof.m_nRequiredFamilies;
	proof.m_bDepthTested = true;
	proof.m_bVisibilityComplete = true;
	proof.m_bSamplerInputsPublished = true;
	assert( t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch+1u, database, device, viewport ) );
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database+1u, device, viewport ) );
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device+1u, viewport ) );
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport+1u ) );
	proof.m_bDepthTested = false;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_bDepthTested = true;
	proof.m_bVisibilityComplete = false;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_bVisibilityComplete = true;
	proof.m_bSamplerInputsPublished = false;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_bSamplerInputsPublished = true;
	proof.m_nPublishedFamilies ^= T3D_VT_MULTI_SOURCE_INSTANCES;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_nPublishedFamilies |= T3D_VT_MULTI_SOURCE_INSTANCES;
	proof.m_nRequiredFamilies |= T3D_VT_MULTI_SOURCE_TRANSPARENT;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_nPublishedFamilies |= T3D_VT_MULTI_SOURCE_TRANSPARENT;
	assert( t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_nRequiredFamilies = T3D_VT_MULTI_SOURCE_TERRAIN;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_nRequiredFamilies = T3D_VT_MULTI_SOURCE_ALL;
	proof.m_nLayerCount = 7u;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_nLayerCount = 8u;
	proof.m_nViewportWidth = 1919u;
	assert( !t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );
	proof.m_nViewportWidth = 1920u;
	assert( t3d_vt_feedback_multi_source_complete( proof, params, epoch, database, device, viewport ) );

	t3dVTFeedbackMultiParams_t slotParams = params;
	slotParams.m_nSlot = 2u;
	const t3dVTFeedbackGridHeader_t header = t3d_vt_feedback_multi_header(
		slotParams, 24u, epoch, database, device, viewport );
	t3dVTFeedbackMultiSlot_t slot{};
	assert( !slot.markDrawn() && !slot.submit( true ) );
	assert( slot.prepare( header ) );
	assert( !slot.prepare( header ) );
	assert( slot.markDrawn() );
	assert( slot.submit( true ) );
	assert( slot.m_eStatus == T3D_VT_FEEDBACK_SLOT_READBACK_SUBMITTED );
	t3dVTFeedbackGridHeader_t stale = header;
	stale.m_nFrame++;
	assert( !slot.complete( stale, slotParams ) );
	stale = header;
	stale.m_nVersion = T3D_VT_FEEDBACK_GRID_CONTRACT_VERSION;
	assert( !slot.complete( stale, slotParams ) );
	assert( slot.complete( header, slotParams ) );
	assert( slot.m_eStatus == T3D_VT_FEEDBACK_SLOT_READY );
	assert( !slot.complete( header, slotParams ) );
	slot.reset();
	assert( slot.prepare( header ) && slot.markDrawn() && slot.submit( true ) );
	slot.invalidate();
	assert( !slot.complete( header, slotParams ) );
	assert( slot.m_eStatus == T3D_VT_FEEDBACK_SLOT_FREE );
	assert( slot.prepare( header ) && slot.markDrawn() );
	assert( !slot.submit( false ) );
	assert( slot.m_eStatus == T3D_VT_FEEDBACK_SLOT_FREE );
	return 0;
}
