// SHD-02: canonical terrain pass, compact-feedback and append-only ABI gates.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static_assert( T3D_RD_SHADER_TERRAIN_COMPACT_FEEDBACK_BASE ==
    T3D_RD_SHADER_ENVIRONMENT_HEIGHT + 1u, "Existing shader IDs shifted" );
static_assert( T3D_RD_SHADER_UCMP_ALIAS == T3D_RD_SHADER_TERRAIN_COMPACT_FEEDBACK_BASE +
    3u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT, "Compact-feedback shader count is incomplete" );
static_assert( t3dRdCanonicalTerrainRoute( 0u, 2u, 7u, true, false, true ).m_id ==
    t3dRdTerrainCompactFeedbackId( 2u, 7u ), "Far compact feedback routing failed" );
static_assert( t3dRdCanonicalTerrainRoute( 0u, 0u, 0u, true, false, true ).m_nFeedbackSet ==
    T3D_RD_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_SET, "Compact feedback set changed" );
static_assert( !t3dRdCanonicalTerrainRoute( 1u, 0u, 0u, true, false, true ).m_bAvailable,
    "Compatibility shader must not claim canonical feedback" );
static_assert( !t3dRdCanonicalTerrainRoute( 0u, 0u, 8u, true, false, true ).m_bAvailable,
    "Depth shader must not produce VT feedback" );
static_assert( t3dRdCanonicalVtFeedbackHintId( 0u ) ==
    T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE + 1u, "Feedback hint ABI changed" );
static_assert( t3dRdCanonicalVtColorHintId( 2u ) ==
    T3D_RD_SHADER_TERRAIN_VT_COLOR_HINT_BASE + 2u * T3D_RD_VT_COLOR_HINT_ID_STRIDE + 2u,
    "Canonical non-feedback hint ID changed" );
