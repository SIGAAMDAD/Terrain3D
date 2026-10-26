// SHD-01 compile-time verification for stable catalog routes and descriptor ABI.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static_assert( t3dRdCanonicalTerrainRoute( 0u, 0u, 0u, false, false, false ).m_id ==
    T3D_RD_SHADER_TERRAIN_FORWARD_BASE, "Near terrain shader ID changed" );
static_assert( t3dRdCanonicalTerrainRoute( 0u, 1u, 8u, false, false, false ).m_id ==
    t3dRdTerrainForwardId( 0u, 1u, 8u ), "Middle terrain depth shader ID changed" );
static_assert( t3dRdCanonicalTerrainRoute( 2u, 2u, 9u, false, false, false ).m_id ==
    t3dRdTerrainForwardId( 2u, 2u, 9u ), "Editor far DNR shader ID changed" );
static_assert( t3dRdCanonicalTerrainRoute( 0u, 2u, 0u, true, true, false ).m_id ==
    t3dRdTerrainFeedbackId( 2u, 0u, true ), "Far feedback one-sun shader ID changed" );
static_assert( t3dRdCanonicalTerrainRoute( 1u, 1u, 0u, false, false, true ).m_id ==
    T3D_RD_SHADER_TERRAIN_COMPACT_BASE + 4u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT,
    "Compatibility middle compact shader ID changed" );
static_assert( t3dRdCanonicalTerrainRoute( 0u, 1u, 0u, true, false, true ).m_id ==
    t3dRdTerrainCompactFeedbackId( 1u, 0u ),
    "SHD-02 compact+feedback shader route missing" );
static_assert( !t3dRdCanonicalTerrainRoute( 1u, 1u, 0u, true, false, false ).m_bAvailable,
    "Compatibility feedback shader incorrectly exposed" );
static_assert( !t3dRdCanonicalTerrainRoute( 0u, 1u, 8u, true, false, false ).m_bAvailable,
    "Depth shader incorrectly exposed as feedback color" );

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
static_assert( t3dRdCanonicalTerrainRoute( 0u, 0u, 0u, true, false, false ).m_nFeedbackSet ==
    T3D_RD_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_SET, "Terrain feedback descriptor set changed" );
static_assert( t3dRdCanonicalWorldShadowId( true, true ) == T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW,
    "WORLD terrain strip shadow shader ID changed" );
static_assert( t3dRdCanonicalWorldShadowId( false, true ) == T3D_RD_SHADER_WORLD_MESH_ENV_SHADOW,
    "WORLD environment mesh shadow shader ID changed" );
