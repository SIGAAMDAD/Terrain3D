// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_RENDER_AUTHORITY_H
#define TERRAIN3D_WORLD_RENDER_AUTHORITY_H

#include "Terrain3DTypes.h"

// Unified World owns native depth/opaque/motion/shadow rendering. The editor
// preview path may still render terrain and instances while publication loads.
// t3dWorldRenderAuthorityPolicy_s centralized in Terrain3DTypes.h.


static inline t3dWorldRenderAuthorityPolicy_t t3dResolveWorldRenderAuthority( const bool bWorldPassKind,
        const bool bEditorAuthoringMode,
        const bool bWorldPublicationAvailable,
        const bool bTransparentPass ) {
	t3dWorldRenderAuthorityPolicy_t policy = {};
	policy.bShippingUnifiedRequired = bWorldPassKind && !bEditorAuthoringMode;
	policy.bUnifiedWorldPass = bWorldPassKind && !bTransparentPass && bWorldPublicationAvailable;
	policy.bEditorPreviewPass = bWorldPassKind && bEditorAuthoringMode && !policy.bUnifiedWorldPass;
	return policy;
}

#endif // TERRAIN3D_WORLD_RENDER_AUTHORITY_H
