// SHD-03 compile-only pass routing and append-safe shader ID contracts.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static_assert( t3dRdCanonicalWorldRoute( false, T3D_RD_FORWARD_SHADER_DEPTH,
    false, false, false ).m_id == T3D_RD_SHADER_WORLD_RASTER_MESH_BASE + T3D_RD_FORWARD_SHADER_DEPTH,
    "WORLD mesh depth route must retain the production shader" );
static_assert( t3dRdCanonicalWorldRoute( false, T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS,
    false, false, true ).m_id == T3D_RD_SHADER_WORLD_MESH_ENV_BASE + T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS,
    "WORLD ENV DNR route must retain material and receiver support" );
static_assert( t3dRdCanonicalWorldShadowId( false, false ) == T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW,
    "WORLD mesh shadow shader ID changed" );
static_assert( t3dRdCanonicalWorldShadowId( false, true ) == T3D_RD_SHADER_WORLD_MESH_ENV_SHADOW,
    "WORLD ENV mesh shadow shader ID changed" );
static_assert( T3D_RD_SHADER_OCEAN_COLOR != T3D_RD_SHADER_RETIRED_OCEAN_PREPARED_COLOR,
    "Ocean compatibility ID removed before all consumers migrated" );
static_assert( T3D_RD_SHADER_OCEAN_ENV_COLOR != T3D_RD_SHADER_RETIRED_OCEAN_ENV_PREPARED_COLOR,
    "Ocean ENV compatibility ID removed before SHD-05" );
