// RET-07: deprecated shader IDs remain tombstones, not valid routes.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static_assert( T3D_RD_SHADER_TERRAIN_COMPACT_FEEDBACK_BASE ==
    T3D_RD_SHADER_ENVIRONMENT_HEIGHT + 1u, "Stable numeric shader ABI changed" );
static_assert( T3D_RD_SHADER_UCMP_ALIAS == T3D_RD_SHADER_TERRAIN_COMPACT_FEEDBACK_BASE +
    3u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT, "UCMP alias offset changed" );
static_assert( t3dRdUnifiedTerrainId( 0u, 7u, true, false, true ) ==
    T3D_RD_SHADER_UCMP_FEEDBACK_COMPACT_BASE + 7u,
    "UCMP compact feedback routing changed" );
static_assert( t3dRdUnifiedTerrainId( 2u, 7u, false, false, true ) ==
    T3D_RD_SHADER_UCMP_COMPACT_BASE + 2u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + 7u,
    "UCMP editor compact routing changed" );
static_assert( T3D_RD_SHADER_BRUSH_COMPUTE != T3D_RD_SHADER_EDITOR_BRUSH_TILES,
    "Retired local brush shader and active BRP shader share an ID" );
