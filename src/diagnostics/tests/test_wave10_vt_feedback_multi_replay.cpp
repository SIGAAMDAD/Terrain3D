#include "rendering/rd/vt/core/terrain_3d_vt_feedback_multi_capture.h"
#include <assert.h>
#include <vector>

static uint32_t t3dNext( uint32_t &rState )
{
	rState ^= rState << 13u;
	rState ^= rState >> 17u;
	rState ^= rState << 5u;
	return rState;
}

int main()
{
	uint64_t pixelBytes = 0u, totalBytes = 0u;
	assert( !t3d_vt_feedback_multi_capture_bytes( 0u, 100u, 8u, pixelBytes, totalBytes ) );
	assert( !t3d_vt_feedback_multi_capture_bytes( 64u, 64u, 9u, pixelBytes, totalBytes ) );
	assert( !t3d_vt_feedback_multi_capture_bytes( 65535u, 65535u, 8u, pixelBytes, totalBytes ) );
	assert( t3d_vt_feedback_multi_capture_bytes( 1920u, 1080u, 8u, pixelBytes, totalBytes ) );
	assert( pixelBytes == 1920ull * 1080ull * 4ull );
	assert( totalBytes == pixelBytes * 9ull );

	t3dVTFeedbackGridLayout_t layout{};
	assert( t3d_vt_feedback_multi_layout( 1200000u, layout ) );
	t3dVTFeedbackMultiParams_t params{};
	assert( t3d_vt_feedback_multi_params( layout, 48u, 40u, 0u, 1024u, 8u, params ) );
	const uint32_t pixels = 48u * 40u;
	std::vector< uint32_t > pages( pixels * 8u, UINT32_MAX );
	std::vector< uint32_t > counts( pixels, 0u );
	t3dVTFeedbackMultiCaptureView_t capture{ pages.data(), counts.data(), 48u, 40u, 8u, 1024u };
	assert( t3d_vt_feedback_multi_capture_valid( capture, params ) );
	uint32_t state = 0xbadc0ffeu;
	for ( uint32_t run = 0u; run < 1200u; ++run ) {
		for ( uint32_t pixel = 0u; pixel < pixels; ++pixel ) {
			const uint32_t count = t3dNext( state ) % 9u;
			counts[pixel] = count;
			for ( uint32_t layer = 0u; layer < 8u; ++layer )
				pages[layer * pixels + pixel] = layer < count ? t3dNext( state ) % 1024u : UINT32_MAX;
		}
		assert( t3d_vt_feedback_multi_capture_valid( capture, params ) );
		const uint32_t chosenPixel = t3dNext( state ) % pixels;
		const uint32_t chosenLayer = t3dNext( state ) % 8u;
		const uint32_t old = pages[chosenLayer * pixels + chosenPixel];
		pages[chosenLayer * pixels + chosenPixel] = chosenLayer < counts[chosenPixel] ?
			UINT32_MAX : 123u;
		assert( !t3d_vt_feedback_multi_capture_valid( capture, params ) );
		pages[chosenLayer * pixels + chosenPixel] = old;
		counts[chosenPixel] = 9u;
		assert( !t3d_vt_feedback_multi_capture_valid( capture, params ) );
	}
	capture.m_pCounts = nullptr;
	assert( !t3d_vt_feedback_multi_capture_valid( capture, params ) );
	capture.m_pCounts = counts.data();
	capture.m_nLayers = 7u;
	assert( !t3d_vt_feedback_multi_capture_valid( capture, params ) );

	t3dVTFeedbackMultiSourceProof_t proof{};
	proof.m_nViewport = 1123u;
	proof.m_nFrame = 2u;
	proof.m_nFeedbackEpoch = 8u;
	proof.m_nDatabaseGeneration = 10u;
	proof.m_nDeviceGeneration = 3u;
	proof.m_nViewportWidth = 48u;
	proof.m_nViewportHeight = 40u;
	proof.m_nLayerCount = 8u;
	proof.m_nRequiredFamilies = T3D_VT_MULTI_SOURCE_TERRAIN | T3D_VT_MULTI_SOURCE_WORLD |
		T3D_VT_MULTI_SOURCE_INSTANCES;
	proof.m_nPublishedFamilies = proof.m_nRequiredFamilies;
	proof.m_bDepthTested = true;
	proof.m_bVisibilityComplete = true;
	proof.m_bSamplerInputsPublished = true;
	assert( t3d_vt_feedback_multi_same_source_frame( proof, proof ) );
	t3dVTFeedbackMultiSourceProof_t changed = proof;
	changed.m_nFrame++;
	assert( !t3d_vt_feedback_multi_same_source_frame( proof, changed ) );
	changed = proof;
	changed.m_nPublishedFamilies = 0u;
	assert( !t3d_vt_feedback_multi_same_source_frame( proof, changed ) );
	changed = proof;
	changed.m_bDepthTested = false;
	assert( !t3d_vt_feedback_multi_same_source_frame( proof, changed ) );
	return 0;
}
