#ifndef TERRAIN3D_RD_PREVIEW_RESULT_H
#define TERRAIN3D_RD_PREVIEW_RESULT_H

// Internal result passed with the image; batch policy never parses diagnostics.
enum t3dRdPreviewFailure_t {
	T3D_RD_PREVIEW_NONE,
	T3D_RD_PREVIEW_ASSET_SOURCE,
	T3D_RD_PREVIEW_TRANSIENT_DEFERRED,
	T3D_RD_PREVIEW_SYSTEMIC_RD,
};

#endif // TERRAIN3D_RD_PREVIEW_RESULT_H
