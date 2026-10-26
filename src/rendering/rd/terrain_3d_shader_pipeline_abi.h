// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_SHADER_PIPELINE_ABI_H
#define TERRAIN3D_SHADER_PIPELINE_ABI_H

#include <stdint.h>

// Pipeline-facing descriptor contracts. Shared implementation includes may use
// these resources, but they do not own these layouts; stage roots/builders do.
#define T3D_RD_DESCRIPTOR_BIT( binding ) ( 1ULL << ( binding ) )
#define T3D_RD_DESCRIPTOR_RANGE_MASK( first, last ) ( ( ( 1ULL << ( ( last ) - ( first ) + 1u ) ) - 1ULL ) << ( first ) )

static const uint64_t T3D_RD_TERRAIN_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 38u );
static const uint64_t T3D_RD_TERRAIN_SET2_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 15u );
static const uint32_t T3D_RD_TERRAIN_PUSH_BYTES = 16u;
static const uint64_t T3D_RD_TERRAIN_VERTEX_INPUT_MASK = 0u;

static const uint64_t T3D_RD_WORLD_TERRAIN_SET0_MASK =
        T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 25u ) | T3D_RD_DESCRIPTOR_BIT( 49u ) | T3D_RD_DESCRIPTOR_BIT( 50u );
static const uint64_t T3D_RD_WORLD_TERRAIN_SET1_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 1u, 12u );
static const uint64_t T3D_RD_WORLD_MESH_SET0_MASK =
        T3D_RD_DESCRIPTOR_BIT( 0u ) | T3D_RD_DESCRIPTOR_RANGE_MASK( 39u, 42u ) | T3D_RD_DESCRIPTOR_RANGE_MASK( 46u, 49u );
static const uint64_t T3D_RD_WORLD_MESH_SET1_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 12u );
static const uint32_t T3D_RD_WORLD_RASTER_PUSH_BYTES = 0u;
static const uint64_t T3D_RD_WORLD_RASTER_VERTEX_INPUT_MASK = 0u;

static const uint64_t T3D_RD_INSTANCE_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 6u );
static const uint64_t T3D_RD_INSTANCE_SET1_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 10u );
static const uint32_t T3D_RD_INSTANCE_PUSH_BYTES = 32u;
static const uint64_t T3D_RD_INSTANCE_VERTEX_INPUT_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 5u );

static const uint64_t T3D_RD_OCEAN_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 2u );
static const uint64_t T3D_RD_OCEAN_VERTEX_INPUT_MASK = 0u;

static const uint64_t T3D_RD_RENDER_WORLD_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 19u );
static const uint64_t T3D_RD_RENDER_WORLD_SET2_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 15u );
static const uint32_t T3D_RD_RENDER_WORLD_RASTER_PUSH_BYTES = 0u;
static const uint64_t T3D_RD_RENDER_WORLD_VERTEX_INPUT_MASK = 0u;

// Authoritative unified-world compute families.
static const uint64_t T3D_RD_WORLD_VIS_RESET_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 2u );
static const uint64_t T3D_RD_WORLD_VIS_CULL_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 12u );
static const uint64_t T3D_RD_WORLD_VIS_FINALIZE_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 2u );
static const uint32_t T3D_RD_WORLD_VIS_PUSH_BYTES = 48u;
static const uint64_t T3D_RD_WORLD_HIZ_SET0_MASK = T3D_RD_DESCRIPTOR_RANGE_MASK( 0u, 1u );
static const uint32_t T3D_RD_WORLD_HIZ_PUSH_BYTES = 16u;

#undef T3D_RD_DESCRIPTOR_RANGE_MASK
#undef T3D_RD_DESCRIPTOR_BIT

#endif // TERRAIN3D_SHADER_PIPELINE_ABI_H
