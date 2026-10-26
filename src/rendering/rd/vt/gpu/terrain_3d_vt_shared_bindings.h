// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_SHARED_BINDINGS_H
#define TERRAIN3D_VT_SHARED_BINDINGS_H
#include "Terrain3DConstants.h"

#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"

#include <godot_cpp/variant/rid.hpp>
#include <stdint.h>

// Cross-renderer VT publication contract. The physical VT is shared by terrain,
// unified-world, instances, previews, and companion World consumers; no
// raster family owns this ABI.


typedef struct t3dSharedVTBindings_s {

	godot::RID hAllocationTable;
	godot::RID hPageTable;
	godot::RID hMipLayoutTable;
	godot::RID hTailMetadata;
	godot::RID hTail[T3D_SHARED_VT_CHANNEL_COUNT];
	godot::RID hPhysical[T3D_SHARED_VT_CHANNEL_COUNT][T3D_SHARED_VT_MAX_TEXTURE_BANKS];
	uint32_t nBankCount[T3D_SHARED_VT_CHANNEL_COUNT];
	uint32_t nBankLayers[T3D_SHARED_VT_CHANNEL_COUNT][T3D_SHARED_VT_MAX_TEXTURE_BANKS];
	uint32_t nAllocationCount;
	uint32_t nVirtualPageCount;
	uint64_t nVTPageTableGeneration;
	uint64_t nResourceGeneration;
	uint64_t nBindingGeneration;
} t3dSharedVTBindings_t;


// Transitional source compatibility for the World Stage-A bridge. New
// cross-renderer consumers should use t3dSharedVTBindings_t directly.


#endif // TERRAIN3D_VT_SHARED_BINDINGS_H
