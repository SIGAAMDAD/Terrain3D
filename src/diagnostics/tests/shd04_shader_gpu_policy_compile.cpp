// SHD-04 compile-only append-safe shader ID and GPU dispatch ABI fixtures.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static_assert( T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_FINALIZE -
               T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_CLASSIFY == 3u,
               "WORLD scan shader order/ABI changed" );
static_assert( T3D_RD_SHADER_WORLD_VISIBILITY_CULL_SMALL + 1u ==
               T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_CLASSIFY,
               "WORLD bounded cull shader order changed" );
static_assert( T3D_RD_SHADER_PAGE_COMPUTE_R32UI_REDUCE -
               T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE == 3u,
               "Page small reducer must have one version per channel format" );
static_assert( T3D_RD_SHADER_PAGE_COMPUTE_R32UI_PARTIAL -
               T3D_RD_SHADER_PAGE_COMPUTE_R32F_PARTIAL == 3u,
               "Page partial reducer must have one version per channel format" );
static_assert( T3D_RD_SHADER_PAGE_COMPUTE_REDUCE_FINAL ==
               T3D_RD_SHADER_PAGE_COMPUTE_R32UI_PARTIAL + 1u,
               "Shared page finalizer ID was moved" );
static_assert( t3dRdTerrainCompactFeedbackId( 2u, 7u ) < T3D_RD_SHADER_COUNT,
               "Compact/VT feedback production shader is outside catalog" );
