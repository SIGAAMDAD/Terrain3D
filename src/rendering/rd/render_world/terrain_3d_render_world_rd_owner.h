// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_RD_OWNER_H
#define TERRAIN3D_RENDER_WORLD_RD_OWNER_H

#include "rendering/rd/terrain_3d_rd_context.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

EA_FORCE_INLINE bool t3dRenderWorldRegisterRid( Terrain3DRdContext *const pContext,
        const RID &rRid,
        const String &rName,
        const Terrain3DRdContext::ResourceType type,
        const Terrain3DRdContext::ResourceOwner owner,
        const Terrain3DRdContext::MemoryCategory memoryCategory = Terrain3DRdContext::MEMORY_NONE,
        const uint64_t nBytes = 0u ) {
	return pContext && pContext->registerResource( rRid, rName, type, memoryCategory, nBytes, owner );
}

EA_FORCE_INLINE void t3dRenderWorldFreeRid( Terrain3DRdContext *const pContext, RID &rRid, const uint64_t nBytes = 0u ) {
	if ( pContext && rRid.is_valid() ) {
		pContext->freeResource( rRid, nBytes );
	}
}

// Creation can fail before a RID has been transferred to Terrain3DRdContext.
// Only that unregistered rollback case may free directly through RenderingDevice.
EA_FORCE_INLINE void t3dRenderWorldDiscardUnregisteredRid( RenderingDevice *const pRd, RID &rRid ) {
	if ( pRd && rRid.is_valid() ) {
		pRd->free_rid( rRid );
		rRid = RID();
	}
}

#endif // TERRAIN3D_RENDER_WORLD_RD_OWNER_H
