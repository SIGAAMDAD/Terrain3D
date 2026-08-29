// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_forward_bindings.h"

static bool t3d_rw_forward_generation_valid( const T3DForwardBorrowedBindings &pBindings ) {
	return pBindings.nLightingGeneration != 0u && pBindings.nEnvironmentGeneration != 0u && pBindings.nBindingGeneration != 0u;
}

bool t3d_render_world_forward_bindings_initialize_cold(
        const T3DForwardBorrowedBindings &pInitial, T3DForwardBorrowedBindingState &rState ) {
	rState = T3DForwardBorrowedBindingState();
	if ( !t3d_rw_forward_generation_valid( pInitial ) ) {
		return false;
	}
	rState.Bindings = pInitial;
	rState.bInitialized = true;
	return true;
}

bool t3d_render_world_forward_bindings_publish_hot( T3DForwardBorrowedBindingState &rState, const T3DForwardBorrowedBindings &pBindings ) {
	if ( !rState.bInitialized || !t3d_rw_forward_generation_valid( pBindings ) ) {
		++rState.nRejectedCount;
		return false;
	}
	if ( pBindings.nBindingGeneration < rState.Bindings.nBindingGeneration ) {
		++rState.nRejectedCount;
		return false;
	}
	rState.Bindings = pBindings;
	++rState.nPublishCount;
	return true;
}

bool t3d_render_world_forward_bindings_validate_reference_debug(
        const T3DForwardBorrowedBindingState &pState, uint32_t pRequiredCapabilities ) {
	if ( !pState.bInitialized || !t3d_rw_forward_generation_valid( pState.Bindings ) ) {
		return false;
	}
	return ( pState.Bindings.nCapabilityBits & pRequiredCapabilities ) == pRequiredCapabilities;
}

void t3d_render_world_forward_bindings_shutdown_cold( T3DForwardBorrowedBindingState &rState ) {
	// All RIDs in this structure are BORROWED_GODOT. Clearing handles is the only legal shutdown action.
	rState = T3DForwardBorrowedBindingState();
}
