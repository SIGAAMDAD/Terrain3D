// RET-07: UCMP native compile-time shader ABI contracts.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static_assert( t3dRdUnifiedTerrainId( 0u, 0u, false, false, false ) ==
    T3D_RD_SHADER_UCMP_FORWARD_BASE, "UCMP shipping forward ID changed" );
static_assert( t3dRdUnifiedTerrainId( 2u, 9u, false, false, false ) ==
    T3D_RD_SHADER_UCMP_FORWARD_BASE + 2u * T3D_RD_FORWARD_SHADER_VARIANT_MAX + 9u,
    "UCMP editor DNR ID changed" );
static_assert( t3dRdUnifiedTerrainId( 0u, 0u, true, true, false ) ==
    T3D_RD_SHADER_UCMP_FEEDBACK_ONE_SUN_BASE, "UCMP feedback one-sun ID changed" );
static_assert( t3dRdUnifiedTerrainId( 0u, 0u, true, false, true ) ==
    T3D_RD_SHADER_UCMP_FEEDBACK_COMPACT_BASE, "UCMP compact feedback ID changed" );
static_assert( T3D_RD_TERRAIN_VT_DESCRIPTOR_SET == 0u, "VT set ABI changed" );
static_assert( T3D_RD_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_SET == 3u,
    "Feedback set ABI changed" );
static_assert( t3dRdCanonicalWorldRoute( true, 0u, false, false, false ).m_id ==
    T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE, "WORLD terrain strip color ID changed" );
static_assert( t3dRdCanonicalWorldRoute( true, 8u, false, false, true ).m_id ==
    T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE + 8u, "WORLD environment strip depth ID changed" );
static_assert( t3dRdCanonicalWorldRoute( true, 0u, false, true, true ).m_id ==
    T3D_RD_SHADER_WORLD_STRIP_ENV_COMPACT_BASE, "WORLD environment compact ID changed" );
static_assert( t3dRdCanonicalWorldRoute( false, 0u, true, false, false ).m_id ==
    T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE, "WORLD mesh feedback ID changed" );
static_assert( t3dRdCanonicalWorldRoute( false, 9u, false, false, true ).m_id ==
    T3D_RD_SHADER_WORLD_MESH_ENV_BASE + 9u, "WORLD environment DNR ID changed" );
static_assert( !t3dRdCanonicalWorldRoute( false, 0u, true, true, true ).m_bAvailable,
    "Unimplemented WORLD compact+feedback shader incorrectly exposed" );
static_assert( !t3dRdCanonicalWorldRoute( true, 0u, true, false, false ).m_bAvailable,
    "Unimplemented WORLD terrain feedback shader incorrectly exposed" );
static_assert( t3dRdCanonicalWorldRoute( false, 0u, true, false, true ).m_nMaterialVtSet ==
    T3D_RD_WORLD_VT_DESCRIPTOR_SET, "WORLD material VT descriptor set changed" );
static_assert( t3dRdCanonicalWorldShadowId( true, true ) == T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW,
    "WORLD terrain strip shadow shader ID changed" );
static_assert( t3dRdCanonicalWorldShadowId( false, true ) == T3D_RD_SHADER_WORLD_MESH_ENV_SHADOW,
    "WORLD environment mesh shadow shader ID changed" );
