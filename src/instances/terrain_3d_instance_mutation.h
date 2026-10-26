// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_MUTATION_H
#define TERRAIN3D_INSTANCE_MUTATION_H

#include <stdint.h>
#include <godot_cpp/variant/vector2i.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef enum t3dInstanceMutationFlags_e : uint32_t {
	T3D_INSTANCE_MUTATION_NONE = 0u,
	T3D_INSTANCE_MUTATION_TRANSFORM = 1u << 0u,
	T3D_INSTANCE_MUTATION_TOPOLOGY = 1u << 1u,
	T3D_INSTANCE_MUTATION_POLICY = 1u << 2u,
	T3D_INSTANCE_MUTATION_REGION_UNLOAD = 1u << 3u,
	T3D_INSTANCE_MUTATION_BULK = 1u << 4u,
} t3dInstanceMutationFlags_t;

typedef struct t3dInstanceMutationEvent_s {
	Vector2i m_region = Vector2i( INT32_MAX, INT32_MAX );
	Vector2i m_cell = Vector2i( INT32_MAX, INT32_MAX );
	int32_t m_nMeshId = -1;
	uint32_t m_nSourceRevision = 0u;
	uint32_t m_nFlags = T3D_INSTANCE_MUTATION_NONE;
	uint64_t m_nGeneration = 0u;
} t3dInstanceMutationEvent_t;

#endif // TERRAIN3D_INSTANCE_MUTATION_H
