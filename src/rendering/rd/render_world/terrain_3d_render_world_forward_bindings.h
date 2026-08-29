// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_FORWARD_BINDINGS_H
#define TERRAIN3D_RENDER_WORLD_FORWARD_BINDINGS_H

#include <godot_cpp/variant/rid.hpp>
#include <stdint.h>

enum T3DForwardBorrowedCapabilityBits : uint32_t {
	T3D_RW_FORWARD_DIRECTIONAL_LIGHTS = 1u << 0u,
	T3D_RW_FORWARD_LOCAL_LIGHTS = 1u << 1u,
	T3D_RW_FORWARD_CLUSTER_DATA = 1u << 2u,
	T3D_RW_FORWARD_SHADOW_ATLAS = 1u << 3u,
	T3D_RW_FORWARD_REFLECTIONS = 1u << 4u,
	T3D_RW_FORWARD_DECALS = 1u << 5u,
	T3D_RW_FORWARD_SKY_RADIANCE = 1u << 6u,
	T3D_RW_FORWARD_SCENE_SAMPLERS = 1u << 7u,
	T3D_RW_FORWARD_ENVIRONMENT = 1u << 8u
};

struct T3DForwardResolvedEnvironment {
	float vAmbientColorEnergy[4];
	float vBackgroundColorEnergy[4];
	float fExposure;
	float fIBLNormalization;
	float fSkyContribution;
	float fReserved;
};

struct T3DForwardBorrowedBindings {
	godot::RID hDirectionalLightBuffer;
	godot::RID hLocalLightBuffer;
	godot::RID hClusterBuffer;
	godot::RID hShadowAtlas;
	godot::RID hReflectionAtlas;
	godot::RID hDecalAtlas;
	godot::RID hSkyRadiance;
	godot::RID hSceneSamplers;
	T3DForwardResolvedEnvironment Environment;
	uint64_t nLightingGeneration;
	uint64_t nEnvironmentGeneration;
	uint64_t nBindingGeneration;
	uint32_t nCapabilityBits;
	uint32_t nReserved;
};

struct T3DForwardBorrowedBindingState {
	T3DForwardBorrowedBindings Bindings;
	uint64_t nPublishCount;
	uint64_t nRejectedCount;
	bool bInitialized;
};

bool t3d_render_world_forward_bindings_initialize_cold(
        const T3DForwardBorrowedBindings &pInitial, T3DForwardBorrowedBindingState &rState );
bool t3d_render_world_forward_bindings_publish_hot( T3DForwardBorrowedBindingState &rState, const T3DForwardBorrowedBindings &pBindings );
bool t3d_render_world_forward_bindings_validate_reference_debug(
        const T3DForwardBorrowedBindingState &pState, uint32_t pRequiredCapabilities );
void t3d_render_world_forward_bindings_shutdown_cold( T3DForwardBorrowedBindingState &rState );

#endif // TERRAIN3D_RENDER_WORLD_FORWARD_BINDINGS_H
